#include "effect_chorus.h"
#include "dsp_util.h"
#include <string.h>

/* ----- Buffer & LFO constants ------------------------------------------- */

#define BUF_LOG2              12
#define BUF_SIZE              (1u << BUF_LOG2)     /* 4096 samples = ~85 ms */
#define BUF_MASK              (BUF_SIZE - 1u)

#define FS_HZ                 48000.0f
#define CENTER_DELAY_SAMPLES  960.0f               /* 20 ms */

/* phase_inc = rate_hz * 2^32 / fs. Precompute the per-Hz constant. */
#define PHASE_INC_PER_HZ      89478.485f

#define SINE_LUT_SIZE         256
#define LFO_R_OFFSET          0x40000000u           /* 90° in q32 phase */

/* alpha = 75/256 ≈ 0.293 per block → ~9 ms tau, matches the other smoothers. */
#define SMOOTH_ALPHA_NUM      75
#define SMOOTH_ALPHA_DEN      256
#define SMOOTH_F_ALPHA        0.293f

/* ----- Buffers in DTCM -------------------------------------------------- */

static q15_t s_buf_L[BUF_SIZE] __attribute__((section(".audio_buffers")));
static q15_t s_buf_R[BUF_SIZE] __attribute__((section(".audio_buffers")));
static q15_t s_sine_lut[SINE_LUT_SIZE];   /* .bss — fits in DTCM lookups via cache */

/* ----- Parameter state -------------------------------------------------- */

static volatile uint32_t s_phase_inc_target;
static volatile q15_t    s_dry_target_q15;
static volatile q15_t    s_wet_target_q15;
static volatile float    s_depth_target_samples;

static uint32_t s_phase;
static uint32_t s_phase_inc;
static q15_t    s_dry_q15;
static q15_t    s_wet_q15;
static float    s_depth_samples;
static uint32_t s_write_idx;

/* ----- Smoothers -------------------------------------------------------- */

static inline q15_t smooth_q15(q15_t cur, q15_t tgt)
{
    int32_t diff = (int32_t)tgt - (int32_t)cur;
    return (q15_t)((int32_t)cur + (diff * SMOOTH_ALPHA_NUM) / SMOOTH_ALPHA_DEN);
}

static inline float smooth_f(float cur, float tgt)
{
    return cur + SMOOTH_F_ALPHA * (tgt - cur);
}

/* ----- Init ------------------------------------------------------------- */

void effect_chorus_init(void)
{
    memset(s_buf_L, 0, sizeof(s_buf_L));
    memset(s_buf_R, 0, sizeof(s_buf_R));

    /* Build the sine LUT once. 256-entry covers one full period; the audio
       loop interpolates between entries for smoothness. */
    for (int i = 0; i < SINE_LUT_SIZE; i++) {
        float a = (6.28318530f * (float)i) / (float)SINE_LUT_SIZE;
        float s = dsp_util_sinf(a);
        int32_t q = (int32_t)(s * 32767.0f + (s >= 0.0f ? 0.5f : -0.5f));
        if (q >  32767) q =  32767;
        if (q < -32768) q = -32768;
        s_sine_lut[i] = (q15_t)q;
    }

    s_phase = 0;
    s_write_idx = 0;

    s_phase_inc_target = (uint32_t)(1.0f * PHASE_INC_PER_HZ);   /* 1 Hz */
    s_depth_target_samples = 3.0f * FS_HZ / 1000.0f;            /* 3 ms = 144 samples */
    s_dry_target_q15 = 16384;
    s_wet_target_q15 = 16384;                                    /* mix = 50 % */

    s_phase_inc       = s_phase_inc_target;
    s_depth_samples   = s_depth_target_samples;
    s_dry_q15         = s_dry_target_q15;
    s_wet_q15         = s_wet_target_q15;
}

/* ----- Process ---------------------------------------------------------- */

static inline q15_t lfo_lookup(uint32_t phase)
{
    uint32_t idx   = phase >> 24;
    uint32_t frac  = (phase >> 8) & 0xFFFFu;     /* q16 fractional position */
    int32_t  a     = (int32_t)s_sine_lut[idx];
    int32_t  b     = (int32_t)s_sine_lut[(idx + 1u) & 0xFFu];
    int32_t  delta = b - a;
    int32_t  adj   = (int32_t)(((int64_t)delta * (int64_t)frac) >> 16);
    return (q15_t)(a + adj);
}

static inline q15_t tap_interp(const q15_t *buf, uint32_t w, float offset_samples)
{
    uint32_t io   = (uint32_t)offset_samples;
    float    fo   = offset_samples - (float)io;
    uint32_t r0   = (w - io) & BUF_MASK;
    uint32_t r1   = (r0 - 1u) & BUF_MASK;            /* one sample further back */
    float    y0   = (float)buf[r0];
    float    y1   = (float)buf[r1];
    return (q15_t)(y0 + fo * (y1 - y0));
}

void effect_chorus_process(q15_t *L, q15_t *R, uint32_t n)
{
    /* Per-block smoothing of multiplicative params; rate snaps (a tiny pitch
       jump is inaudible amongst the modulation itself). */
    s_dry_q15        = smooth_q15(s_dry_q15, s_dry_target_q15);
    s_wet_q15        = smooth_q15(s_wet_q15, s_wet_target_q15);
    s_depth_samples  = smooth_f  (s_depth_samples, s_depth_target_samples);
    s_phase_inc      = s_phase_inc_target;

    uint32_t w       = s_write_idx;
    uint32_t phase   = s_phase;
    uint32_t pinc    = s_phase_inc;
    int32_t  dr      = (int32_t)s_dry_q15;
    int32_t  we      = (int32_t)s_wet_q15;
    float    depth_s = s_depth_samples;

    for (uint32_t i = 0; i < n; i++) {
        /* LFO values for L and R, 90° apart for a wide stereo image. */
        int32_t lfo_l = (int32_t)lfo_lookup(phase);
        int32_t lfo_r = (int32_t)lfo_lookup(phase + LFO_R_OFFSET);

        /* Fractional read offsets in samples. lfo / 32768 in [-1, 1]. */
        float off_l = CENTER_DELAY_SAMPLES + ((float)lfo_l * (1.0f/32768.0f)) * depth_s;
        float off_r = CENTER_DELAY_SAMPLES + ((float)lfo_r * (1.0f/32768.0f)) * depth_s;

        q15_t tap_l = tap_interp(s_buf_L, w, off_l);
        q15_t tap_r = tap_interp(s_buf_R, w, off_r);

        /* Write current input to the buffer. No feedback in chorus — would
           push it toward flanger territory; the standalone Delay effect
           covers that combination of long-delay + high-feedback already. */
        s_buf_L[w] = L[i];
        s_buf_R[w] = R[i];

        /* Mix dry + wet. Shift each product to q15 before summing so the
           sum never overflows int32. */
        int32_t in_l  = (int32_t)L[i];
        int32_t in_r  = (int32_t)R[i];
        int32_t out_l = ((dr * in_l) >> 15) + ((we * (int32_t)tap_l) >> 15);
        int32_t out_r = ((dr * in_r) >> 15) + ((we * (int32_t)tap_r) >> 15);
        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;
        L[i] = (q15_t)out_l;
        R[i] = (q15_t)out_r;

        w     = (w + 1u) & BUF_MASK;
        phase += pinc;
    }

    s_write_idx = w;
    s_phase     = phase;
}

/* ----- Setters ---------------------------------------------------------- */

void effect_chorus_set_rate_hz(float hz)
{
    if (hz < EFFECT_CHORUS_RATE_HZ_MIN) hz = EFFECT_CHORUS_RATE_HZ_MIN;
    if (hz > EFFECT_CHORUS_RATE_HZ_MAX) hz = EFFECT_CHORUS_RATE_HZ_MAX;
    s_phase_inc_target = (uint32_t)(hz * PHASE_INC_PER_HZ);
}

void effect_chorus_set_depth_ms(float ms)
{
    if (ms < (float)EFFECT_CHORUS_DEPTH_MS_MIN) ms = (float)EFFECT_CHORUS_DEPTH_MS_MIN;
    if (ms > (float)EFFECT_CHORUS_DEPTH_MS_MAX) ms = (float)EFFECT_CHORUS_DEPTH_MS_MAX;
    s_depth_target_samples = ms * FS_HZ / 1000.0f;
}

void effect_chorus_set_mix_pct(float pct)
{
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    float w = pct / 100.0f;
    s_dry_target_q15 = (q15_t)((1.0f - w) * 32767.0f + 0.5f);
    s_wet_target_q15 = (q15_t)(w * 32767.0f + 0.5f);
}
