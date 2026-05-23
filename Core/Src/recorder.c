#include "recorder.h"
#include "wav.h"
#include "sd_card.h"
#include "audio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include <string.h>

/* ===== Ring buffer ======================================================= */

/* 2^18 = 256 KB, power-of-two so the wrap is a single AND. Sized to ~1.36 s
   of audio at 192 KB/s — comfortable headroom over the worst-case ~400 ms
   SD erase stall observed on consumer class-10 cards.

   Stored as a byte ring (not a sample ring) because f_write takes UINT
   bytes; computing wraps on the byte index avoids a multiply at the
   write site. */
#define RING_LOG2   18
#define RING_BYTES  (1u << RING_LOG2)
#define RING_MASK   (RING_BYTES - 1u)

static uint8_t s_ring[RING_BYTES] __attribute__((section(".sdram")));

/* Monotonic byte counters. head - tail = current fill (correct as long
   as fill stays below 2^31, which holds: ring is 256 KB << 2 GB). The
   physical index in the ring is (counter & RING_MASK). 32-bit aligned
   single-writer-per-variable → atomic on M7 without barriers. */
static volatile uint32_t s_head;       /* written by audio task */
static volatile uint32_t s_tail;       /* written by recorder task */

/* ===== State + commands ================================================== */

/* State is written only by the recorder task; the audio task and UI read
   it. UI requests transitions by writing s_cmd. Decoupling command from
   state keeps state changes monotonic per-writer (the audio task's tap
   check is then trivially race-free). */
#define CMD_NONE   0u
#define CMD_START  1u
#define CMD_STOP   2u

static volatile uint8_t  s_state;      /* recorder_state_t */
static volatile uint8_t  s_cmd;
static volatile uint8_t  s_tap;        /* recorder_tap_t */
static volatile uint32_t s_frames_written;   /* monotonic, for elapsed_ms */
static volatile q15_t    s_peak;             /* read-and-reset by UI */

static TaskHandle_t s_task_handle;
static FIL          s_file;
static uint32_t     s_data_bytes;            /* PCM bytes written to file so far */
static uint32_t     s_last_sync_data_bytes;  /* threshold for periodic header resync */

/* Resync the on-disk header every 10 s of audio so a yanked card still
   leaves a playable file (RIFF/data sizes patched to the last sync point).
   192000 B/s × 10 s = 1.92 MB. */
#define HEADER_SYNC_INTERVAL_BYTES   (1920000u)

/* The recorder task wakes whenever the fill crosses 32 KB. Smaller and
   we'd write less than a full erase block, paying full erase overhead
   per write; larger and the ring runs hot during back-to-back stalls. */
#define DRAIN_THRESHOLD_BYTES        (32u * 1024u)

/* Cap one f_write call to one drain threshold worth so a single write
   never starves the recorder task for the player or vice-versa. */
#define WRITE_CHUNK_BYTES            DRAIN_THRESHOLD_BYTES

/* ===== Helpers =========================================================== */

static inline uint32_t ring_fill(void)
{
    return s_head - s_tail;
}

/* Update the running peak (q15 abs). Decays the previous value per block
   so the meter naturally falls back to 0 when audio goes quiet — keeps the
   UI's read path single-load (no read-and-reset race against this writer).
   Decay constant 250/256 ≈ 0.977 per block; at 375 blocks/s that reaches
   ~10 % in ~267 ms, fast enough that the meter tracks transients without
   feeling sticky. */
static inline void update_peak(const int16_t *buf, uint32_t samples)
{
    q15_t pk = (q15_t)(((int32_t)s_peak * 250) >> 8);
    for (uint32_t i = 0; i < samples; i++) {
        int16_t v = buf[i];
        int16_t a = (v < 0) ? (int16_t)-v : v;     /* INT16_MIN saturates to INT16_MAX after negate-cast — acceptable */
        if (a > pk) pk = a;
    }
    s_peak = pk;
}

/* Push `bytes` from src into the ring, advancing s_head. Caller must have
   verified ring_fill() + bytes <= RING_BYTES (otherwise data is dropped
   silently — see recorder_tap_*). Handles the wraparound by splitting the
   copy into two memcpy calls. */
static inline void ring_push(const uint8_t *src, uint32_t bytes)
{
    uint32_t head_phys = s_head & RING_MASK;
    uint32_t first = RING_BYTES - head_phys;
    if (first > bytes) first = bytes;
    memcpy(&s_ring[head_phys], src, first);
    if (bytes > first) {
        memcpy(&s_ring[0], src + first, bytes - first);
    }
    /* head advance is the publication; do it AFTER the writes so the
       recorder task never sees uninitialized ring slots. The compiler
       can't reorder writes past this volatile store. */
    s_head += bytes;
}

/* Notify the recorder task that drainable data is available. Safe to call
   from task context (this is in the audio task, not an ISR). */
static inline void wake_recorder(void)
{
    if (s_task_handle) {
        xTaskNotify(s_task_handle, 0, eNoAction);
    }
}

/* ===== Tap hooks (audio task) ============================================ */

/* Common tap path for both PRE and POST. Branchless-fast when not recording
   (single volatile load + early return — ~6 cycles in the common case). */
static void tap_common(const int16_t *buf, uint32_t frames)
{
    /* AUDIO_HALF_FRAMES * 2 channels * 2 bytes/sample = 512 bytes per half. */
    uint32_t bytes = frames * 4u;

    /* Free space check. If the ring is full (file task can't keep up), we
       drop this half — better than corrupting the head/tail counters. The
       drop will produce a click in the recording; surface it via an error
       counter in a future revision. */
    if (RING_BYTES - ring_fill() < bytes) {
        return;
    }

    ring_push((const uint8_t *)buf, bytes);
    update_peak(buf, frames * 2u);

    s_frames_written += frames;

    /* Notify the recorder task only when we cross the drain threshold —
       avoids hammering the scheduler 375 times/sec when the ring is mostly
       empty and the file task is sleeping. */
    if (ring_fill() >= DRAIN_THRESHOLD_BYTES) {
        wake_recorder();
    }
}

void recorder_tap_pre(const int16_t *buf, uint32_t frames)
{
    if (s_state != RECORDER_STATE_RECORDING) return;
    if (s_tap   != RECORDER_TAP_PRE)         return;
    tap_common(buf, frames);
}

void recorder_tap_post(const int16_t *buf, uint32_t frames)
{
    if (s_state != RECORDER_STATE_RECORDING) return;
    if (s_tap   != RECORDER_TAP_POST)        return;
    tap_common(buf, frames);
}

/* ===== File task ========================================================= */

/* Drain up to WRITE_CHUNK_BYTES from the ring to the file. Splits across
   the ring boundary into at most two f_write calls. Returns FR_OK on
   success — on error, the caller transitions to ERROR. */
static FRESULT drain_chunk(void)
{
    uint32_t fill = ring_fill();
    if (fill == 0) return FR_OK;

    uint32_t chunk = fill;
    if (chunk > WRITE_CHUNK_BYTES) chunk = WRITE_CHUNK_BYTES;

    uint32_t tail_phys = s_tail & RING_MASK;
    uint32_t first = RING_BYTES - tail_phys;
    if (first > chunk) first = chunk;

    UINT wrote = 0;
    FRESULT r = f_write(&s_file, &s_ring[tail_phys], first, &wrote);
    if (r != FR_OK || wrote != first) return (r != FR_OK) ? r : FR_DISK_ERR;

    if (chunk > first) {
        uint32_t rem = chunk - first;
        r = f_write(&s_file, &s_ring[0], rem, &wrote);
        if (r != FR_OK || wrote != rem) return (r != FR_OK) ? r : FR_DISK_ERR;
    }

    s_data_bytes += chunk;
    s_tail       += chunk;

    /* Periodic header resync — if power dies between syncs, the file is
       still valid up to the last sync. wav_finalize leaves the cursor at
       the patched location, so we have to f_lseek back to end-of-data. */
    if (s_data_bytes - s_last_sync_data_bytes >= HEADER_SYNC_INTERVAL_BYTES) {
        FRESULT fr = wav_finalize(&s_file, s_data_bytes);
        if (fr != FR_OK) return fr;
        /* Restore cursor to end of data so further writes append. */
        fr = f_lseek(&s_file, WAV_HEADER_BYTES + s_data_bytes);
        if (fr != FR_OK) return fr;
        s_last_sync_data_bytes = s_data_bytes;
    }

    return FR_OK;
}

/* IDLE + CMD_START → open file, advance to ARMED then RECORDING.
   Returns FR_OK on success; on failure state goes to ERROR. */
static FRESULT begin_recording(void)
{
    char name[SD_REC_FILENAME_MAX];
    if (!sd_card_next_filename(name)) return FR_NO_PATH;

    s_state = RECORDER_STATE_ARMED;

    FRESULT r = f_open(&s_file, name, FA_CREATE_NEW | FA_WRITE);
    if (r != FR_OK) return r;

    r = wav_write_header(&s_file);
    if (r != FR_OK) {
        f_close(&s_file);
        return r;
    }

    /* Reset ring + counters. Audio task is not yet tapping (state still ARMED),
       so head/tail mutations here are race-free. */
    s_head = 0;
    s_tail = 0;
    s_data_bytes           = 0;
    s_last_sync_data_bytes = 0;
    s_frames_written       = 0;
    s_peak                 = 0;

    /* Publish RECORDING. Audio task starts tapping on the next half-buffer. */
    s_state = RECORDER_STATE_RECORDING;
    return FR_OK;
}

/* RECORDING + CMD_STOP → STOPPING → IDLE. Drains residual ring, finalizes
   the header, closes the file. Audio task stopped tapping the moment we
   wrote STOPPING (its check is state == RECORDING). */
static FRESULT end_recording(void)
{
    s_state = RECORDER_STATE_STOPPING;

    /* Drain whatever is left. The audio task has stopped tapping so head
       is stable. One pass is enough — fill is bounded by RING_BYTES. */
    while (ring_fill() > 0) {
        FRESULT r = drain_chunk();
        if (r != FR_OK) return r;
    }

    FRESULT r = wav_finalize(&s_file, s_data_bytes);
    if (r != FR_OK) { f_close(&s_file); return r; }

    r = f_close(&s_file);
    if (r != FR_OK) return r;

    s_state = RECORDER_STATE_IDLE;
    return FR_OK;
}

static void recorder_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Wake either on notification or on the 50 ms heartbeat (catches
           missed notifies, polls for STOP commands while idle, gives the
           card-detect a chance to flap). */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        uint8_t cmd   = s_cmd;
        uint8_t state = s_state;

        if (state == RECORDER_STATE_IDLE) {
            if (cmd == CMD_START) {
                s_cmd = CMD_NONE;
                FRESULT r = begin_recording();
                if (r != FR_OK) {
                    s_state = RECORDER_STATE_ERROR;
                }
            }
        } else if (state == RECORDER_STATE_RECORDING) {
            if (cmd == CMD_STOP) {
                s_cmd = CMD_NONE;
                FRESULT r = end_recording();
                if (r != FR_OK) {
                    /* Best-effort close on error so the FIL handle is freed. */
                    f_close(&s_file);
                    s_state = RECORDER_STATE_ERROR;
                }
            } else {
                /* Drain whatever's available since the last wake. The audio
                   task continues to push during the drain — that's fine,
                   head moves forward and we'll catch up on the next pass. */
                FRESULT r = drain_chunk();
                if (r != FR_OK) {
                    f_close(&s_file);
                    s_state = RECORDER_STATE_ERROR;
                }
            }
        } else if (state == RECORDER_STATE_ERROR) {
            if (cmd == CMD_STOP) {
                s_cmd = CMD_NONE;
                s_state = RECORDER_STATE_IDLE;
            }
        }
        /* ARMED, STOPPING are transient and handled inside begin_/end_recording
           — they should not be observed at the top of the loop. */
    }
}

/* ===== Public API ======================================================== */

void recorder_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_head  = 0;
    s_tail  = 0;
    s_state = RECORDER_STATE_IDLE;
    s_cmd   = CMD_NONE;
    s_tap   = RECORDER_TAP_POST;        /* default per CLAUDE.md "processed samples" */
    s_frames_written = 0;
    s_peak  = 0;

    xTaskCreate(recorder_task, "Recorder", 1024, NULL,
                tskIDLE_PRIORITY + 2, &s_task_handle);
}

bool recorder_start(void)
{
    if (s_state != RECORDER_STATE_IDLE) return false;
    s_cmd = CMD_START;
    if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
    return true;
}

bool recorder_stop(void)
{
    if (s_state == RECORDER_STATE_IDLE) return false;
    s_cmd = CMD_STOP;
    if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
    return true;
}

bool recorder_set_tap(recorder_tap_t tap)
{
    if (s_state != RECORDER_STATE_IDLE) return false;
    s_tap = (uint8_t)tap;
    return true;
}

recorder_tap_t recorder_get_tap(void)
{
    return (recorder_tap_t)s_tap;
}

recorder_state_t recorder_get_state(void)
{
    return (recorder_state_t)s_state;
}

uint32_t recorder_get_elapsed_ms(void)
{
    /* frames at 48 kHz → ms: ms = frames * 1000 / 48000 = frames / 48.
       Using integer division of frames/48 is exact (no FP, no overflow
       since frames stays below 4 billion ≈ 24 hours of audio). */
    return s_frames_written / 48u;
}

q15_t recorder_get_peak(void)
{
    /* Pure read — audio task owns decay. UI handles its own visual smoothing
       via the bar's delta-paint logic in ui_page_record.c. */
    return s_peak;
}
