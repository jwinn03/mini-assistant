#include "tts_player.h"
#include "dsp_util.h"
#include "arm_math.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ===== Sizing ============================================================ */

/* 16 kHz inbound ring: 2^18 samples = 512 KB = ~16.4 s of speech. Power of
   two so wrap is one AND (player.c pattern). Written by the assistant task,
   drained by the TTSPlay task. */
#define RING16_LOG2      18u
#define RING16_SAMPLES   (1u << RING16_LOG2)
#define RING16_MASK      (RING16_SAMPLES - 1u)

/* 48 kHz staging ring: 4096 samples (~85 ms) — enough to absorb scheduling
   jitter between the TTSPlay task and the audio task's 2.67 ms cadence.
   Written by the TTSPlay task, drained by the audio-task inject hooks. */
#define STAGE_LOG2       12u
#define STAGE_SAMPLES    (1u << STAGE_LOG2)
#define STAGE_MASK       (STAGE_SAMPLES - 1u)

/* Playback starts once this much 16 kHz audio has arrived (or EOS for a
   shorter reply). 8000 samples = 500 ms — two protocol-v2 8 KB chunks. */
#define PREFILL_16K      8000u

/* Interpolator: x3, 48 taps (numTaps % L == 0 as arm_fir_interpolate_q15
   requires), fixed 160-in / 480-out chunks. 160*3 = 480 sidesteps the
   non-integer 128-frames-per-half / 3 problem entirely. */
#define UP_L             3u
#define NUM_TAPS         48u
#define IN_CHUNK         160u
#define OUT_CHUNK        (IN_CHUNK * UP_L)

/* FX-ON tail: after speech ends, keep feeding zeros through the effect chain
   so delay/reverb tails decay before the mic returns. */
#define TTS_FX_TAIL_MS   1500u
#define TAIL_FRAMES      (48u * TTS_FX_TAIL_MS)

/* Wake-gate window after the last injected frame (utterance.c polls this
   via tts_player_active). */
#define ACTIVE_TAIL_MS   300u

#define PI_F             3.14159265358979f
#define TWO_PI_F         6.28318530717959f

/* ===== Buffers =========================================================== */

/* Inbound PCM in SDRAM (NOLOAD — memset in init per project convention). */
static int16_t s_ring16[RING16_SAMPLES] __attribute__((section(".sdram")));

/* Staging + filter working set in .bss (SRAM). ~10 KB total; the audio hook
   touches at most 128 samples per 2.67 ms — no need to spend DTCM. */
static q15_t s_stage[STAGE_SAMPLES];
static q15_t s_coeffs[NUM_TAPS];
static q15_t s_fir_state[(NUM_TAPS / UP_L) + IN_CHUNK - 1u];
static q15_t s_in[IN_CHUNK];
static q15_t s_out[OUT_CHUNK];
static arm_fir_interpolate_instance_q15 s_inst;

/* Monotonic indices — strict single-writer each (player.c pattern):
   h16: assistant task   t16: TTSPlay task
   hs:  TTSPlay task     ts:  audio task (inject hooks)                     */
static volatile uint32_t s_h16, s_t16;
static volatile uint32_t s_hs,  s_ts;

/* Stream flags. eos/abort written by the assistant task; flushed by the
   TTSPlay task (final partial chunk interpolated). */
static volatile bool s_eos;
static volatile bool s_abort;
static volatile bool s_flushed;

static volatile int32_t    s_tail_frames_left;
static volatile TickType_t s_last_inject_tick;
static TaskHandle_t        s_task_handle;

/* ===== Public state ====================================================== */

volatile uint8_t  tts_player_fx_enabled = 0;    /* default: clean voice */
volatile uint8_t  tts_player_state      = TTS_IDLE;
volatile uint32_t tts_player_underruns  = 0;
volatile uint32_t tts_player_ms_played  = 0;

/* ===== Filter design (mirrors decimator.c compute_coeffs) ================ */

static void compute_coeffs(void)
{
    /* Hamming-windowed sinc lowpass at 7 kHz (48 kHz rate): passes the full
       0-8 kHz speech band of the 16 kHz source, kills the zero-stuffing
       images at 16 k +/- and 32 k +/-.

       THE ONE DIFFERENCE from the decimator: after normalising to unity DC
       gain, scale by L (=3). Zero-stuffing spreads each input sample's energy
       over L output slots, so a unity-gain filter would play back at 1/3
       amplitude. Peak coefficient ~= 3 * 2*7000/48000 ~= 0.88 — still inside
       q15's [-1, 1). */
    const float omega_c = TWO_PI_F * 7000.0f / 48000.0f;
    const float M_off   = (NUM_TAPS - 1u) * 0.5f;

    float h[NUM_TAPS];
    float sum = 0.0f;
    for (uint32_t n = 0; n < NUM_TAPS; n++) {
        float m    = (float)n - M_off;
        float h_lp = dsp_util_sinf(omega_c * m) / (PI_F * m);
        float w    = 0.54f - 0.46f *
                     dsp_util_cosf(TWO_PI_F * (float)n / (float)(NUM_TAPS - 1u));
        h[n] = h_lp * w;
        sum += h[n];
    }

    const float k = (sum != 0.0f) ? ((float)UP_L / sum) : 0.0f;
    for (uint32_t n = 0; n < NUM_TAPS; n++) {
        float   v = h[n] * k;
        int32_t q = (int32_t)(v * 32768.0f + (v >= 0.0f ? 0.5f : -0.5f));
        if (q > 32767)  q = 32767;
        if (q < -32768) q = -32768;
        s_coeffs[n] = (q15_t)q;
    }
}

/* ===== Ring helpers ====================================================== */

static inline uint32_t fill16(void)  { return s_h16 - s_t16; }
static inline uint32_t fill48(void)  { return s_hs - s_ts; }

/* ===== TTSPlay task: 16k ring -> interpolate -> staging ring ============= */

static void interpolate_available(void)
{
    for (;;) {
        uint32_t in_avail  = fill16();
        uint32_t out_space = STAGE_SAMPLES - fill48();
        if (out_space < OUT_CHUNK) return;

        if (in_avail >= IN_CHUNK) {
            uint32_t t = s_t16;
            for (uint32_t i = 0; i < IN_CHUNK; i++) {
                s_in[i] = s_ring16[(t + i) & RING16_MASK];
            }
            s_t16 = t + IN_CHUNK;
        } else if (s_eos && !s_flushed) {
            /* Final partial chunk: pad with zeros so the last <10 ms of
               speech isn't dropped, then mark the stream flushed. */
            uint32_t t = s_t16;
            for (uint32_t i = 0; i < in_avail; i++) {
                s_in[i] = s_ring16[(t + i) & RING16_MASK];
            }
            memset(&s_in[in_avail], 0, (IN_CHUNK - in_avail) * sizeof(q15_t));
            s_t16 = t + in_avail;
            s_flushed = true;
        } else {
            return;                      /* wait for more PCM (or EOS) */
        }

        arm_fir_interpolate_q15(&s_inst, s_in, s_out, IN_CHUNK);

        uint32_t hcur = s_hs;
        for (uint32_t i = 0; i < OUT_CHUNK; i++) {
            s_stage[(hcur + i) & STAGE_MASK] = s_out[i];
        }
        s_hs = hcur + OUT_CHUNK;

        if (s_flushed && fill16() == 0) return;
    }
}

static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));

        if (s_abort && tts_player_state != TTS_IDLE) {
            /* Drop unplayed 16k PCM; the <=85 ms already staged plays out.
               t16 is this task's variable, so the snap is race-free. */
            s_t16     = s_h16;
            s_eos     = true;
            s_flushed = true;
            s_abort   = false;
        }

        interpolate_available();

        if (tts_player_state == TTS_FILLING &&
            (fill16() >= PREFILL_16K || (s_eos && s_flushed))) {
            tts_player_state = TTS_PLAYING;
        }
    }
}

/* ===== Audio-task inject hooks =========================================== */

/* Pop up to `frames` mono staging samples into interleaved stereo `buf`
   (L = R). Zero-fills any shortfall. Handles end-of-speech transition. */
static void inject_block(int16_t *buf, uint32_t frames)
{
    uint32_t avail = fill48();
    uint32_t take  = (avail < frames) ? avail : frames;

    uint32_t t = s_ts;
    for (uint32_t i = 0; i < take; i++) {
        int16_t s = s_stage[(t + i) & STAGE_MASK];
        buf[2u * i]      = s;
        buf[2u * i + 1u] = s;
    }
    s_ts = t + take;

    if (take < frames) {
        memset(&buf[2u * take], 0, (frames - take) * 2u * sizeof(int16_t));
        if (!(s_eos && s_flushed)) {
            tts_player_underruns++;      /* genuine starvation, not stream end */
        }
    }

    tts_player_ms_played += frames / 48u;
    s_last_inject_tick    = xTaskGetTickCount();

    /* Stream fully played out? */
    if (s_eos && s_flushed && fill48() == 0u && fill16() == 0u) {
        if (tts_player_fx_enabled) {
            s_tail_frames_left = (int32_t)TAIL_FRAMES;
            tts_player_state   = TTS_TAIL;
        } else {
            tts_player_state = TTS_IDLE;
        }
    }
}

void tts_inject_pre(int16_t *buf, uint32_t frames)
{
    uint8_t st = tts_player_state;
    if (st == TTS_IDLE || st == TTS_FILLING) return;

    if (st == TTS_TAIL) {
        /* Feed silence through the chain (mic stays muted) until the
           delay/reverb tails have decayed. */
        memset(buf, 0, frames * 2u * sizeof(int16_t));
        s_last_inject_tick = xTaskGetTickCount();
        s_tail_frames_left -= (int32_t)frames;
        if (s_tail_frames_left <= 0) {
            tts_player_state = TTS_IDLE;
        }
        return;
    }
    /* TTS_PLAYING */
    if (!tts_player_fx_enabled) return;  /* post hook owns this block */
    inject_block(buf, frames);
}

void tts_inject_post(int16_t *buf, uint32_t frames)
{
    if (tts_player_state != TTS_PLAYING) return;
    if (tts_player_fx_enabled) return;   /* pre hook owns this block */
    inject_block(buf, frames);
}

/* ===== Assistant-task API ================================================ */

uint32_t tts_player_write(const uint8_t *data, uint32_t len)
{
    uint8_t st = tts_player_state;
    if (st == TTS_IDLE || st == TTS_TAIL) {
        /* New stream. A tap into TTS_TAIL cuts the tail short — the new
           response takes priority over a decaying reverb. Indices stay
           monotonic; only the stream flags reset. */
        s_eos     = false;
        s_flushed = false;
        s_abort   = false;
        tts_player_state = TTS_FILLING;
    }

    uint32_t samples = len / 2u;
    uint32_t space   = RING16_SAMPLES - fill16();
    if (samples > space) samples = space;

    uint32_t h = s_h16;
    for (uint32_t i = 0; i < samples; i++) {
        /* int16 LE on the wire, int16 LE in memory — direct assembly. */
        s_ring16[(h + i) & RING16_MASK] =
            (int16_t)((uint16_t)data[2u * i] | ((uint16_t)data[2u * i + 1u] << 8));
    }
    s_h16 = h + samples;

    if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
    return samples * 2u;
}

void tts_player_eos(void)
{
    s_eos = true;
    if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
}

void tts_player_abort(void)
{
    if (tts_player_state == TTS_IDLE) return;
    s_abort = true;
    if (s_task_handle) xTaskNotify(s_task_handle, 0, eNoAction);
}

bool tts_player_active(void)
{
    if (tts_player_state != TTS_IDLE) return true;
    return (xTaskGetTickCount() - s_last_inject_tick) <
           pdMS_TO_TICKS(ACTIVE_TAIL_MS);
}

void tts_player_init(void)
{
    /* .sdram is NOLOAD: contents undefined at reset. Reads are fill-bounded
       so garbage is never consumed, but the project convention is explicit
       zeroing for anything in .sdram. */
    memset(s_ring16, 0, sizeof(s_ring16));

    s_h16 = s_t16 = 0;
    s_hs  = s_ts  = 0;
    s_eos = s_abort = s_flushed = false;
    s_tail_frames_left = 0;
    s_last_inject_tick = 0;

    compute_coeffs();
    (void)arm_fir_interpolate_init_q15(&s_inst, UP_L, NUM_TAPS,
                                       s_coeffs, s_fir_state, IN_CHUNK);

    xTaskCreate(tts_task, "TTSPlay", 512, NULL,
                tskIDLE_PRIORITY + 2, &s_task_handle);
}
