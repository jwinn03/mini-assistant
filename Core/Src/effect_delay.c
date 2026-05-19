#include "effect_delay.h"
#include <string.h>

/* ----- Ring buffer layout ------------------------------------------------
   2^17 samples per channel = 131072 samples = ~2.73 sec @ 48 kHz, 256 KB
   per channel q15. Power-of-two so the wrap is `(idx + 1) & MASK` — a
   single AND instruction, no compare/subtract. SDRAM placement via the
   linker's .sdram section (added in Chunk 1); cache is write-through so
   no manual maintenance is required. */

#define BUF_LOG2     17
#define BUF_SIZE     (1u << BUF_LOG2)
#define BUF_MASK     (BUF_SIZE - 1u)

#define FS_HZ        48000u
#define MAX_DELAY_SAMPLES   ((FS_HZ * EFFECT_DELAY_MS_MAX) / 1000u)

static q15_t s_buf_L[BUF_SIZE] __attribute__((section(".sdram")));
static q15_t s_buf_R[BUF_SIZE] __attribute__((section(".sdram")));

/* ----- Parameter state -------------------------------------------------- */

/* Target values written from the UI task. The audio task snapshots them
   once per block and smooths the multiplicative ones with a one-pole IIR. */
static volatile uint32_t s_delay_target_samples;
static volatile q15_t    s_fb_target_q15;
static volatile q15_t    s_dry_target_q15;
static volatile q15_t    s_wet_target_q15;

/* Smoothed values used inside process(). Re-init from targets on effect_init
   so the first block doesn't audibly ramp from zero. */
static uint32_t s_delay_samples;
static q15_t    s_fb_q15;
static q15_t    s_dry_q15;
static q15_t    s_wet_q15;

static uint32_t s_write_idx;

/* alpha = 75/256 ≈ 0.293 per block. block period = 128/48000 ≈ 2.67 ms,
   so tau ≈ block_period / alpha ≈ 9 ms — within the 10 ms target. */
#define SMOOTH_ALPHA_NUM  75
#define SMOOTH_ALPHA_DEN  256

static inline q15_t smooth_q15(q15_t cur, q15_t tgt)
{
    int32_t diff = (int32_t)tgt - (int32_t)cur;
    return (q15_t)((int32_t)cur + (diff * SMOOTH_ALPHA_NUM) / SMOOTH_ALPHA_DEN);
}

/* ----- Init ------------------------------------------------------------- */

void effect_delay_init(void)
{
    /* SDRAM at reset holds undefined content. Zero both ring buffers up
       front; ~512 KB of writes which the cache absorbs in burst lines. */
    memset(s_buf_L, 0, sizeof(s_buf_L));
    memset(s_buf_R, 0, sizeof(s_buf_R));

    s_write_idx = 0;

    s_delay_target_samples = (FS_HZ * 480u) / 1000u;   /* 480 ms */
    s_fb_target_q15  = (q15_t)((40 * 32767) / 100);     /* 40 % */
    /* Mix defaults to 50 % so engaging the effect produces an immediately
       audible echo without a second slider tweak. */
    s_dry_target_q15 = 16384;
    s_wet_target_q15 = 16384;

    /* Skip the smoothing ramp on first block — start at the targets. */
    s_delay_samples = s_delay_target_samples;
    s_fb_q15  = s_fb_target_q15;
    s_dry_q15 = s_dry_target_q15;
    s_wet_q15 = s_wet_target_q15;
}

/* ----- Process ---------------------------------------------------------- */

void effect_delay_process(q15_t *L, q15_t *R, uint32_t n)
{
    /* Smooth multiplicative params toward target (per-block IIR). Time
       snaps — interpolating it would shift pitch on every adjustment. */
    s_fb_q15  = smooth_q15(s_fb_q15,  s_fb_target_q15);
    s_dry_q15 = smooth_q15(s_dry_q15, s_dry_target_q15);
    s_wet_q15 = smooth_q15(s_wet_q15, s_wet_target_q15);
    s_delay_samples = s_delay_target_samples;

    uint32_t w  = s_write_idx;
    uint32_t r  = (w - s_delay_samples) & BUF_MASK;
    int32_t  fb = (int32_t)s_fb_q15;
    int32_t  dr = (int32_t)s_dry_q15;
    int32_t  we = (int32_t)s_wet_q15;

    for (uint32_t i = 0; i < n; i++) {
        /* L channel: SDRAM read at r, SDRAM write at w. Sequential access
           on both sides keeps cache-line fills warm. */
        int32_t in_l  = (int32_t)L[i];
        int32_t tap_l = (int32_t)s_buf_L[r];

        /* Write: in + feedback*tap, saturate q15. */
        int32_t bw_l  = in_l + ((fb * tap_l) >> 15);
        if (bw_l >  32767) bw_l =  32767;
        if (bw_l < -32768) bw_l = -32768;
        s_buf_L[w] = (q15_t)bw_l;

        /* Output: dry*in + wet*tap. Each q15 product, shift first then sum
           (sum bound = 2 * q15-max ≈ 65534, safe in int32). */
        int32_t out_l = ((dr * in_l) >> 15) + ((we * tap_l) >> 15);
        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        L[i] = (q15_t)out_l;

        /* R channel — identical math, separate buffer. */
        int32_t in_r  = (int32_t)R[i];
        int32_t tap_r = (int32_t)s_buf_R[r];

        int32_t bw_r  = in_r + ((fb * tap_r) >> 15);
        if (bw_r >  32767) bw_r =  32767;
        if (bw_r < -32768) bw_r = -32768;
        s_buf_R[w] = (q15_t)bw_r;

        int32_t out_r = ((dr * in_r) >> 15) + ((we * tap_r) >> 15);
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;
        R[i] = (q15_t)out_r;

        w = (w + 1u) & BUF_MASK;
        r = (r + 1u) & BUF_MASK;
    }

    s_write_idx = w;
}

/* ----- Setters ---------------------------------------------------------- */

void effect_delay_set_time_ms(float ms)
{
    if (ms < (float)EFFECT_DELAY_MS_MIN) ms = (float)EFFECT_DELAY_MS_MIN;
    if (ms > (float)EFFECT_DELAY_MS_MAX) ms = (float)EFFECT_DELAY_MS_MAX;
    uint32_t samples = (uint32_t)(ms * (float)FS_HZ / 1000.0f + 0.5f);
    if (samples > MAX_DELAY_SAMPLES) samples = MAX_DELAY_SAMPLES;
    s_delay_target_samples = samples;
}

void effect_delay_set_feedback_pct(float pct)
{
    if (pct < 0.0f)                            pct = 0.0f;
    if (pct > (float)EFFECT_DELAY_FB_MAX)      pct = (float)EFFECT_DELAY_FB_MAX;
    int32_t q = (int32_t)((pct * 32767.0f) / 100.0f + 0.5f);
    s_fb_target_q15 = (q15_t)q;
}

void effect_delay_set_mix_pct(float pct)
{
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    float w = pct / 100.0f;
    float d = 1.0f - w;
    s_dry_target_q15 = (q15_t)(d * 32767.0f + 0.5f);
    s_wet_target_q15 = (q15_t)(w * 32767.0f + 0.5f);
}
