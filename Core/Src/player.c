#include "player.h"
#include "wav.h"
#include "sd_card.h"
#include "audio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include <string.h>

/* ===== Ring buffer ======================================================= */

/* 256 KB power-of-two, matching the recorder. Same wrap math, same
   reasoning. Direction is inverted: the player task pushes (from SD), the
   audio task pops (consume). */
#define RING_LOG2   18
#define RING_BYTES  (1u << RING_LOG2)
#define RING_MASK   (RING_BYTES - 1u)

static uint8_t s_ring[RING_BYTES] __attribute__((section(".sdram")));

static volatile uint32_t s_head;       /* written by player task */
static volatile uint32_t s_tail;       /* written by audio task */

/* ===== State + commands ================================================== */

#define CMD_NONE   0u
#define CMD_START  1u
#define CMD_STOP   2u

static volatile uint8_t  s_state;      /* player_state_t */
static volatile uint8_t  s_cmd;
static volatile uint32_t s_frames_played;     /* incremented by audio task */
static volatile uint32_t s_duration_frames;   /* set by player task when LOADING */
static volatile uint32_t s_total_data_bytes;  /* total PCM payload (from header) */

/* The audio task notifies the player whenever fill drops to a level that
   leaves room for one full READ_CHUNK_BYTES. Symmetric with the recorder's
   drain threshold — same chunk size, same trade-off. */
#define READ_CHUNK_BYTES         (32u * 1024u)
#define REFILL_THRESHOLD_BYTES   (RING_BYTES - READ_CHUNK_BYTES)   /* 224 KB */

/* Begin playback only once the ring has accumulated at least this much —
   gives the audio task a healthy buffer before it first consumes. */
#define PREFILL_BYTES            (RING_BYTES / 2u)

static TaskHandle_t s_task_handle;
static FIL          s_file;
static char         s_filename[SD_REC_FILENAME_MAX];
static uint32_t     s_bytes_read;             /* how much of the file's data chunk we've read */
static bool         s_eof_reached;

/* ===== Audio task: pop from ring ========================================= */

static inline uint32_t ring_fill(void)
{
    return s_head - s_tail;
}

/* Pop `bytes` from the ring into dst, advancing s_tail. Caller verified
   bytes <= ring_fill(). Handles wrap. */
static inline void ring_pop(uint8_t *dst, uint32_t bytes)
{
    uint32_t tail_phys = s_tail & RING_MASK;
    uint32_t first = RING_BYTES - tail_phys;
    if (first > bytes) first = bytes;
    memcpy(dst, &s_ring[tail_phys], first);
    if (bytes > first) {
        memcpy(dst + first, &s_ring[0], bytes - first);
    }
    s_tail += bytes;
}

void player_inject_post(int16_t *buf, uint32_t frames)
{
    if (s_state != PLAYER_STATE_PLAYING) return;

    uint32_t bytes = frames * 4u;
    uint32_t fill  = ring_fill();

    if (fill >= bytes) {
        ring_pop((uint8_t *)buf, bytes);
        s_frames_played += frames;
    } else if (fill > 0) {
        /* Partial: pop what we have, zero-fill the rest. Happens only at
           EOF (player task signals completion by stopping refill once
           s_eof_reached is set, then the ring naturally drains). */
        ring_pop((uint8_t *)buf, fill);
        memset((uint8_t *)buf + fill, 0, bytes - fill);
        s_frames_played += fill / 4u;
    } else {
        /* Total underrun (SD stall or already-EOF). Output silence. */
        memset(buf, 0, bytes);
    }

    /* Nudge the player task whenever there's room for one full chunk AND
       we still have file content left to read. eNoAction notifies coalesce
       cheaply when the task is already running. */
    if (!s_eof_reached && ring_fill() <= REFILL_THRESHOLD_BYTES) {
        if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
    }

    /* Transition out of PLAYING when both the file is fully read and the
       ring is drained. Player task is the canonical state writer, but it
       is asleep at that point; doing the transition here keeps latency
       low and is safe because the player task only writes state when it
       holds the "I'm awake" notification — it'll observe IDLE on its next
       wake and self-clean. */
    if (s_eof_reached && ring_fill() == 0) {
        s_state = PLAYER_STATE_IDLE;
        if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
    }
}

/* ===== Player task ======================================================= */

/* Push up to READ_CHUNK_BYTES from the file into the ring. Splits across
   ring wraparound into at most two f_read calls. Returns FR_OK on success.
   Sets s_eof_reached when the data chunk is exhausted. Takes FATFS mutex
   so an unmount path can't yank state mid-read. */
static FRESULT refill_chunk(void)
{
    if (s_eof_reached) return FR_OK;

    uint32_t free_bytes = RING_BYTES - ring_fill();
    if (free_bytes < READ_CHUNK_BYTES) return FR_OK;     /* not enough room yet */

    uint32_t remaining = s_total_data_bytes - s_bytes_read;
    uint32_t chunk = READ_CHUNK_BYTES;
    if (chunk > remaining) chunk = remaining;
    if (chunk == 0) {
        s_eof_reached = true;
        return FR_OK;
    }

    uint32_t head_phys = s_head & RING_MASK;
    uint32_t first = RING_BYTES - head_phys;
    if (first > chunk) first = chunk;

    sd_card_lock();
    if (!sd_card_mounted()) {
        sd_card_unlock();
        return FR_DISK_ERR;
    }

    UINT read = 0;
    FRESULT r = f_read(&s_file, &s_ring[head_phys], first, &read);
    if (r != FR_OK) { sd_card_unlock(); return r; }
    if (read < first) {
        /* Partial read — file shorter than header advertised. Treat as EOF. */
        s_eof_reached = true;
        s_head        += read;
        s_bytes_read  += read;
        sd_card_unlock();
        return FR_OK;
    }

    if (chunk > first) {
        uint32_t rem = chunk - first;
        r = f_read(&s_file, &s_ring[0], rem, &read);
        if (r != FR_OK) { sd_card_unlock(); return r; }
        if (read < rem) {
            s_eof_reached = true;
            s_head        += first + read;
            s_bytes_read  += first + read;
            sd_card_unlock();
            return FR_OK;
        }
    }
    sd_card_unlock();

    s_head       += chunk;
    s_bytes_read += chunk;
    if (s_bytes_read >= s_total_data_bytes) s_eof_reached = true;
    return FR_OK;
}

/* Open + parse + prefill. Returns FR_OK on success and leaves state =
   PLAYER_STATE_PLAYING. On failure state is set to ERROR by the caller. */
static FRESULT begin_playback(void)
{
    s_state = PLAYER_STATE_LOADING;

    sd_card_lock();
    if (!sd_card_mounted()) { sd_card_unlock(); return FR_DISK_ERR; }
    wav_info_t info;
    FRESULT r = wav_read_open(&s_file, s_filename, &info);
    sd_card_unlock();
    if (r != FR_OK) return r;

    s_total_data_bytes  = info.data_bytes;
    s_duration_frames   = info.data_bytes / WAV_BYTES_PER_FRAME;
    s_bytes_read        = 0;
    s_eof_reached       = false;
    s_head              = 0;
    s_tail              = 0;
    s_frames_played     = 0;

    /* Prefill before the audio task starts consuming. refill_chunk takes the
       FATFS mutex per call — fine to loop here without holding it across
       calls since the file handle state is owned by this task. */
    while (!s_eof_reached && ring_fill() < PREFILL_BYTES) {
        r = refill_chunk();
        if (r != FR_OK) {
            sd_card_lock();
            f_close(&s_file);
            sd_card_unlock();
            return r;
        }
    }

    s_state = PLAYER_STATE_PLAYING;
    return FR_OK;
}

static void end_playback(void)
{
    /* Audio task already observed the IDLE transition; we just close. */
    sd_card_lock();
    f_close(&s_file);
    sd_card_unlock();
    s_eof_reached       = true;
    s_total_data_bytes  = 0;
    s_bytes_read        = 0;
    /* Leave s_frames_played / s_duration_frames for UI to display final pos. */
}

static void player_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        uint8_t cmd   = s_cmd;
        uint8_t state = s_state;

        if (state == PLAYER_STATE_IDLE) {
            if (cmd == CMD_START) {
                s_cmd = CMD_NONE;
                FRESULT r = begin_playback();
                if (r != FR_OK) {
                    s_state = PLAYER_STATE_ERROR;
                }
            } else if (s_total_data_bytes != 0) {
                /* Audio task transitioned us to IDLE at end-of-stream; finish
                   the close. The total_data_bytes != 0 guard makes this a
                   one-shot — wouldn't fire on a fresh boot or after stop. */
                end_playback();
            }
        } else if (state == PLAYER_STATE_PLAYING) {
            if (cmd == CMD_STOP) {
                s_cmd = CMD_NONE;
                /* Hard stop: empty the ring so the audio task hears silence
                   on its next inject. Order matters: set state first so the
                   audio task's inject-time underrun branch zero-fills. */
                s_state = PLAYER_STATE_IDLE;
                s_eof_reached = true;
                /* Drain ring atomically by snapping tail to head. */
                s_tail = s_head;
                end_playback();
            } else {
                FRESULT r = refill_chunk();
                if (r != FR_OK) {
                    s_state = PLAYER_STATE_ERROR;
                    sd_card_lock();
                    f_close(&s_file);
                    sd_card_unlock();
                }
            }
        } else if (state == PLAYER_STATE_ERROR) {
            if (cmd == CMD_STOP) {
                s_cmd = CMD_NONE;
                sd_card_lock();
                f_close(&s_file);
                sd_card_unlock();
                s_state = PLAYER_STATE_IDLE;
                s_total_data_bytes = 0;
            }
        }
    }
}

/* ===== Public API ======================================================== */

void player_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_head            = 0;
    s_tail            = 0;
    s_state           = PLAYER_STATE_IDLE;
    s_cmd             = CMD_NONE;
    s_frames_played   = 0;
    s_duration_frames = 0;
    s_total_data_bytes = 0;
    s_eof_reached     = true;

    xTaskCreate(player_task, "Player", 1024, NULL,
                tskIDLE_PRIORITY + 2, &s_task_handle);
}

bool player_start(const char *filename)
{
    if (s_state != PLAYER_STATE_IDLE) return false;
    if (!filename) return false;
    /* Snapshot filename — caller's buffer may be reused by the UI. */
    size_t len = 0;
    while (filename[len] && len < SD_REC_FILENAME_MAX - 1) {
        s_filename[len] = filename[len];
        len++;
    }
    s_filename[len] = '\0';

    s_cmd = CMD_START;
    if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
    return true;
}

bool player_stop(void)
{
    if (s_state == PLAYER_STATE_IDLE) return false;
    s_cmd = CMD_STOP;
    if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
    return true;
}

player_state_t player_get_state(void)
{
    return (player_state_t)s_state;
}

uint32_t player_get_position_ms(void)
{
    return s_frames_played / 48u;
}

uint32_t player_get_duration_ms(void)
{
    return s_duration_frames / 48u;
}
