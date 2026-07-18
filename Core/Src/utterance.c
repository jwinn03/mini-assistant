#include "utterance.h"
#include "vad.h"
#include "decimator.h"
#include "wake_word.h"
#include "tts_player.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ===== Geometry ========================================================== */

/* The capture works on the same 20 ms / 320-sample frame the VAD uses, read
   from the 16 kHz mono decimator ring. */
#define UTT_FRAME_LEN        VAD_FRAME_LEN        /* 320 samples (20 ms) */
#define UTT_FRAME_MS         VAD_FRAME_MS         /* 20 ms */
#define UTT_SAMPLES_PER_MS   16u                  /* 16 kHz mono */

/* Capture buffer: 8.192 s of headroom, 8.0 s hard cap. 131072 × 2 B = 256 KB.
   A single utterance is a linear fill (write index), not a wrap-around stream. */
#define UTT_CAP_SAMPLES      131072u              /* 256 KB in .sdram */
#define UTT_MAX_SAMPLES      (8000u * UTT_SAMPLES_PER_MS)   /* 8 s = 128000 */

/* Pre-roll ring: always-on capture of recent audio so the first phoneme of
   the command survives even if the user speaks the instant the wake fires.
   8192 samples (512 ms, 16 KB) power-of-two; we prepend the most recent
   300 ms on wake fire. */
#define PREROLL_RING_SAMPLES 8192u
#define PREROLL_RING_MASK    (PREROLL_RING_SAMPLES - 1u)
#define PREROLL_SAMPLES      (300u * UTT_SAMPLES_PER_MS)    /* 300 ms = 4800 */

/* Hangover: keep capturing for 900 ms after the last speech-positive frame,
   resetting whenever speech resumes. */
#define HANGOVER_FRAMES      (900u / UTT_FRAME_MS)          /* 30 frames */

/* Reject the capture if total speech is under 100 ms — almost certainly a
   spurious wake fire rather than a real command. */
#define UTT_MIN_SPEECH_MS    100u

/* How long an accepted (but un-taken) capture lingers in ENDED before the
   task auto-re-arms, so standalone testing can fire repeatedly without a
   Phase 8 consumer to call utterance_release(). 1.5 s at 50 frames/s. */
#define ENDED_HOLD_FRAMES    (1500u / UTT_FRAME_MS)         /* 75 frames */

/* ===== Buffers =========================================================== */

static int16_t s_cap[UTT_CAP_SAMPLES]          __attribute__((section(".sdram")));
static int16_t s_preroll[PREROLL_RING_SAMPLES] __attribute__((section(".sdram")));

/* One frame of scratch, kept off the 512-word task stack (matches the
   wake-word task's static s_hop). */
static int16_t s_frame[UTT_FRAME_LEN];

/* ===== State ============================================================= */

volatile uint8_t  utterance_state          = UTTERANCE_STATE_ARMED;
volatile uint32_t utterance_capture_ms     = 0;
volatile uint32_t utterance_speech_ms      = 0;
volatile q15_t    utterance_peak           = 0;
volatile uint32_t utterance_total_captures = 0;
volatile uint32_t utterance_total_rejects  = 0;
volatile uint32_t utterance_ring_overruns  = 0;

static uint32_t s_pre_head;          /* monotonic pre-roll write index */
static uint32_t s_cap_len;           /* samples in s_cap for the current utterance */
static uint32_t s_speech_frames;     /* speech-positive frames seen this capture */
static uint32_t s_hang;              /* remaining hangover frames */
static uint32_t s_ended_ticks;       /* frames spent in ENDED */
static uint32_t s_last_seen_fires;   /* wake-fire edge tracking */
static uint32_t s_ready_len;         /* length of the accepted capture */
static bool     s_taken;             /* capture pinned by utterance_take() */
static volatile bool s_release_req;  /* utterance_release() handshake */

static TaskHandle_t s_task_handle;

/* ===== Pre-roll ring ===================================================== */

static void preroll_push(const int16_t *frame)
{
    uint32_t h = s_pre_head & PREROLL_RING_MASK;
    uint32_t first = PREROLL_RING_SAMPLES - h;
    if (first > UTT_FRAME_LEN) first = UTT_FRAME_LEN;
    memcpy(&s_preroll[h], frame, first * sizeof(int16_t));
    if (UTT_FRAME_LEN > first) {
        memcpy(&s_preroll[0], frame + first,
               (UTT_FRAME_LEN - first) * sizeof(int16_t));
    }
    s_pre_head += UTT_FRAME_LEN;
}

/* Seed the capture buffer with the most recent PREROLL_SAMPLES of pre-roll
   audio (or fewer if we haven't been listening that long yet). */
static void preroll_snapshot_into_cap(void)
{
    uint32_t n = PREROLL_SAMPLES;
    if (n > s_pre_head) n = s_pre_head;          /* early boot: less available */
    uint32_t start = s_pre_head - n;
    for (uint32_t i = 0; i < n; i++) {
        s_cap[i] = s_preroll[(start + i) & PREROLL_RING_MASK];
    }
    s_cap_len = n;
}

/* ===== Capture ========================================================== */

static void cap_append(const int16_t *frame)
{
    uint32_t n = UTT_FRAME_LEN;
    if (s_cap_len + n > UTT_CAP_SAMPLES) n = UTT_CAP_SAMPLES - s_cap_len;
    if (n == 0) return;

    memcpy(&s_cap[s_cap_len], frame, n * sizeof(int16_t));

    q15_t pk = utterance_peak;
    for (uint32_t i = 0; i < n; i++) {
        int16_t v = frame[i];
        int16_t a = (v < 0) ? (int16_t)-v : v;
        if (a > pk) pk = a;
    }
    utterance_peak = pk;

    s_cap_len += n;
}

static void enter_armed(void)
{
    s_cap_len       = 0;
    s_speech_frames = 0;
    s_hang          = 0;
    s_ended_ticks   = 0;
    s_taken         = false;
    s_release_req   = false;
    /* Resync the fire counter so a fire that happened while we were busy
       doesn't immediately re-trigger. Keep the noise floor (no vad_reset). */
    s_last_seen_fires = wake_word_total_fires;
    utterance_state = UTTERANCE_STATE_ARMED;
}

static void begin_capture(void)
{
    preroll_snapshot_into_cap();     /* prepend ~300 ms of lead-in */
    s_speech_frames = 0;
    s_hang          = HANGOVER_FRAMES;   /* a silent capture ends in 600 ms */
    utterance_peak  = 0;
    utterance_capture_ms = s_cap_len / UTT_SAMPLES_PER_MS;
    utterance_state = UTTERANCE_STATE_ACTIVE;
}

/* Decide accept vs. reject at end of an active capture. */
static void end_capture(void)
{
    uint32_t speech_ms = s_speech_frames * UTT_FRAME_MS;
    utterance_speech_ms  = speech_ms;
    utterance_capture_ms = s_cap_len / UTT_SAMPLES_PER_MS;

    if (speech_ms < UTT_MIN_SPEECH_MS) {
        utterance_total_rejects++;
        enter_armed();
        return;
    }

    s_ready_len = s_cap_len;
    utterance_total_captures++;
    s_ended_ticks = 0;
    s_taken       = false;
    s_release_req = false;
    utterance_state = UTTERANCE_STATE_ENDED;
}

/* ===== Task ============================================================== */

static void process_armed(void)
{
    preroll_push(s_frame);
    (void)vad_is_speech(s_frame, true);          /* adapt floor while listening */

    /* Phase 10 self-trigger gate: the speakers are audible to the MEMS mic,
       so the wake model can fire on the board's own TTS voice. While playback
       is active (plus its short tail) swallow fires with the same resync
       idiom process_ended uses — the model keeps running, captures just
       can't start. Barge-in is deferred (Future tasks). */
    if (tts_player_active()) {
        s_last_seen_fires = wake_word_total_fires;
        return;
    }

    uint32_t fires = wake_word_total_fires;
    if (fires != s_last_seen_fires) {
        s_last_seen_fires = fires;
        begin_capture();
    }
}

static void process_active(void)
{
    /* Ignore wake fires that land mid-capture. */
    s_last_seen_fires = wake_word_total_fires;

    bool speech = vad_is_speech(s_frame, false); /* floor frozen during capture */
    cap_append(s_frame);
    utterance_capture_ms = s_cap_len / UTT_SAMPLES_PER_MS;

    if (speech) {
        s_speech_frames++;
        s_hang = HANGOVER_FRAMES;
    } else if (s_hang > 0) {
        s_hang--;
    }

    if (s_cap_len >= UTT_MAX_SAMPLES) {          /* 8 s hard cap */
        end_capture();
    } else if (s_hang == 0) {                    /* hangover elapsed */
        end_capture();
    }
}

static void process_ended(void)
{
    /* Keep the pre-roll fresh and ignore fires while a capture is held. */
    preroll_push(s_frame);
    s_last_seen_fires = wake_word_total_fires;

    if (s_release_req) {                         /* Phase 8 finished with it */
        enter_armed();
        return;
    }
    if (!s_taken && ++s_ended_ticks >= ENDED_HOLD_FRAMES) {
        enter_armed();                           /* standalone auto-re-arm */
    }
}

static void utterance_task(void *arg)
{
    (void)arg;

    uint32_t read_pos = decimator_head;
    vad_reset();
    enter_armed();

    for (;;) {
        /* Block until a full 20 ms frame of new samples is available. */
        while ((int32_t)(decimator_head - (read_pos + UTT_FRAME_LEN)) < 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* Overrun: fell more than a ring (~64 ms) behind — skip to the latest
           frame. Affects only this consumer; the wake-word task is independent. */
        if (decimator_head - read_pos > DECIMATOR_RING_SIZE) {
            read_pos = decimator_head - UTT_FRAME_LEN;
            utterance_ring_overruns++;
        }

        for (uint32_t i = 0; i < UTT_FRAME_LEN; i++) {
            s_frame[i] = decimator_ring[(read_pos + i) & DECIMATOR_RING_MASK];
        }
        read_pos += UTT_FRAME_LEN;

        switch (utterance_state) {
        case UTTERANCE_STATE_ARMED:  process_armed();  break;
        case UTTERANCE_STATE_ACTIVE: process_active(); break;
        case UTTERANCE_STATE_ENDED:  process_ended();  break;
        default:                     enter_armed();    break;
        }
    }
}

/* ===== Public API ======================================================== */

void utterance_init(void)
{
    /* .sdram is NOLOAD — zero before first use so a stale read never pumps
       garbage into a capture. */
    memset(s_cap, 0, sizeof(s_cap));
    memset(s_preroll, 0, sizeof(s_preroll));
    s_pre_head = 0;

    enter_armed();

    /* 512 words / 2 KB stack: the per-frame work is one arm_power_q15 plus a
       memcpy; the big buffers live in .sdram, the frame scratch is static.
       Same priority class as the wake-word / recorder / player tasks. */
    xTaskCreate(utterance_task, "Utter", 512, NULL,
                tskIDLE_PRIORITY + 2, &s_task_handle);
}

utterance_state_t utterance_get_state(void)
{
    return (utterance_state_t)utterance_state;
}

bool utterance_take(int16_t **buf, uint32_t *len_samples)
{
    if (utterance_state != UTTERANCE_STATE_ENDED) return false;
    if (buf)         *buf         = s_cap;
    if (len_samples) *len_samples = s_ready_len;
    s_taken = true;          /* pin: task won't re-arm until released */
    return true;
}

void utterance_release(void)
{
    if (s_taken) {
        s_taken = false;
        s_release_req = true;
    }
}
