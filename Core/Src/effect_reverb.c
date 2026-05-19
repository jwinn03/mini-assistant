#include "effect_reverb.h"
#include <string.h>

/* ----- Network topology -------------------------------------------------
   4 prime-length comb delays in parallel, each with a one-pole LP inside
   its feedback. Their sum feeds 2 short allpass delays in series.

   Comb lengths around 30/35/40/45 ms — primes prevent the four taps from
   synchronizing into a perceptible echo grid.
   Allpass lengths around 5/1.7 ms — also prime, also too short for the
   ear to resolve as discrete echoes (they smear). */

#define COMB_COUNT       4
#define ALLPASS_COUNT    2

#define COMB1_LEN        1447u    /* ~30.1 ms */
#define COMB2_LEN        1693u    /* ~35.3 ms */
#define COMB3_LEN        1931u    /* ~40.2 ms */
#define COMB4_LEN        2161u    /* ~45.0 ms */
#define TOTAL_COMB_LEN   (COMB1_LEN + COMB2_LEN + COMB3_LEN + COMB4_LEN)

#define ALLPASS1_LEN     241u     /* ~5.0 ms */
#define ALLPASS2_LEN     83u      /* ~1.7 ms */
#define TOTAL_AP_LEN     (ALLPASS1_LEN + ALLPASS2_LEN)

#define G_AP_Q15         16384    /* fixed 0.5 allpass coefficient */

#define SMOOTH_ALPHA_NUM 75
#define SMOOTH_ALPHA_DEN 256

/* ----- Storage ----------------------------------------------------------
   Comb buffers in SDRAM (write-through MPU region, no cache maintenance).
   Allpass buffers in DTCM (single-cycle access — keeps the high-rate
   smearing in fast memory). */

static q15_t s_comb_storage_L[TOTAL_COMB_LEN] __attribute__((section(".sdram")));
static q15_t s_comb_storage_R[TOTAL_COMB_LEN] __attribute__((section(".sdram")));
static q15_t s_ap_storage_L  [TOTAL_AP_LEN]   __attribute__((section(".audio_buffers")));
static q15_t s_ap_storage_R  [TOTAL_AP_LEN]   __attribute__((section(".audio_buffers")));

typedef struct {
    q15_t   *buf;
    uint32_t delay;
    uint32_t idx;
    q15_t    lp_state;     /* one-pole low-pass state inside the feedback path */
} comb_state_t;

typedef struct {
    q15_t   *buf;
    uint32_t delay;
    uint32_t idx;
} ap_state_t;

static comb_state_t s_combs_L[COMB_COUNT];
static comb_state_t s_combs_R[COMB_COUNT];
static ap_state_t   s_aps_L  [ALLPASS_COUNT];
static ap_state_t   s_aps_R  [ALLPASS_COUNT];

/* ----- Parameters (target written by UI, smoothed in process) ---------- */

static volatile q15_t s_g_comb_target_q15;     /* 0.70..0.90 */
static volatile q15_t s_alpha_target_q15;      /* 0.30..1.00 (1 = no damp) */
static volatile q15_t s_dry_target_q15;
static volatile q15_t s_wet_target_q15;

static q15_t s_g_comb_q15;
static q15_t s_alpha_q15;
static q15_t s_dry_q15;
static q15_t s_wet_q15;

static inline q15_t smooth_q15(q15_t cur, q15_t tgt)
{
    int32_t diff = (int32_t)tgt - (int32_t)cur;
    return (q15_t)((int32_t)cur + (diff * SMOOTH_ALPHA_NUM) / SMOOTH_ALPHA_DEN);
}

/* ----- Init ------------------------------------------------------------ */

static const uint32_t k_comb_lens[COMB_COUNT] = {
    COMB1_LEN, COMB2_LEN, COMB3_LEN, COMB4_LEN
};
static const uint32_t k_ap_lens[ALLPASS_COUNT] = {
    ALLPASS1_LEN, ALLPASS2_LEN
};

void effect_reverb_init(void)
{
    /* SDRAM and audio-buffer DTCM regions both have undefined content
       at reset. Zero them so the first sample doesn't read garbage as
       a delay tap and pump it back through the feedback loop. */
    memset(s_comb_storage_L, 0, sizeof(s_comb_storage_L));
    memset(s_comb_storage_R, 0, sizeof(s_comb_storage_R));
    memset(s_ap_storage_L,   0, sizeof(s_ap_storage_L));
    memset(s_ap_storage_R,   0, sizeof(s_ap_storage_R));

    uint32_t off = 0;
    for (int c = 0; c < COMB_COUNT; c++) {
        s_combs_L[c].buf      = &s_comb_storage_L[off];
        s_combs_R[c].buf      = &s_comb_storage_R[off];
        s_combs_L[c].delay    = k_comb_lens[c];
        s_combs_R[c].delay    = k_comb_lens[c];
        s_combs_L[c].idx      = 0;
        s_combs_R[c].idx      = 0;
        s_combs_L[c].lp_state = 0;
        s_combs_R[c].lp_state = 0;
        off += k_comb_lens[c];
    }

    off = 0;
    for (int a = 0; a < ALLPASS_COUNT; a++) {
        s_aps_L[a].buf   = &s_ap_storage_L[off];
        s_aps_R[a].buf   = &s_ap_storage_R[off];
        s_aps_L[a].delay = k_ap_lens[a];
        s_aps_R[a].delay = k_ap_lens[a];
        s_aps_L[a].idx   = 0;
        s_aps_R[a].idx   = 0;
        off += k_ap_lens[a];
    }

    /* Defaults: medium room (g=0.80), light damping (alpha=0.79), 30 % wet. */
    s_g_comb_target_q15 = (q15_t)((int32_t)(0.80f * 32767.0f));
    s_alpha_target_q15  = (q15_t)((int32_t)(0.79f * 32767.0f));
    float wet_pct = 30.0f;
    s_dry_target_q15 = (q15_t)((1.0f - wet_pct/100.0f) * 32767.0f);
    s_wet_target_q15 = (q15_t)((       wet_pct/100.0f) * 32767.0f);

    s_g_comb_q15 = s_g_comb_target_q15;
    s_alpha_q15  = s_alpha_target_q15;
    s_dry_q15    = s_dry_target_q15;
    s_wet_q15    = s_wet_target_q15;
}

/* ----- Per-channel process ---------------------------------------------- */

static inline int32_t sat_q15(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

static void process_channel(q15_t *io, uint32_t n,
                            comb_state_t *combs, ap_state_t *aps,
                            int32_t g, int32_t alpha,
                            int32_t dr, int32_t we)
{
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = (int32_t)io[i];

        /* --- 4 parallel damped combs --------------------------------- */
        int32_t comb_sum = 0;
        for (int c = 0; c < COMB_COUNT; c++) {
            comb_state_t *cb = &combs[c];
            int32_t y     = (int32_t)cb->buf[cb->idx];          /* SDRAM read */
            int32_t state = (int32_t)cb->lp_state;
            /* One-pole LP inside the feedback: state += alpha*(y - state).
               alpha = 1 -> bypass (bright); alpha < 1 -> rolls off HF. */
            state += ((alpha * (y - state)) >> 15);
            cb->lp_state = (q15_t)state;
            /* Write input + g·damped_feedback, saturating into q15. */
            int32_t fb = x + ((g * state) >> 15);
            cb->buf[cb->idx] = (q15_t)sat_q15(fb);              /* SDRAM write */
            /* Advance ring index, wrap at prime length. Compare+sub costs
               about the same as a power-of-two mask but lets us keep the
               actual prime delay (no padding waste) — important so the
               four taps don't lock into a perceptible echo grid. */
            cb->idx++;
            if (cb->idx >= cb->delay) cb->idx = 0;
            comb_sum += y;
        }
        /* Average of 4 — keeps the sum in q15 range. */
        int32_t comb_out = comb_sum >> 2;

        /* --- 2 cascaded Schroeder allpasses -------------------------- */
        int32_t ap_in = comb_out;
        for (int a = 0; a < ALLPASS_COUNT; a++) {
            ap_state_t *ap = &aps[a];
            int32_t delayed = (int32_t)ap->buf[ap->idx];        /* DTCM read */
            int32_t v_new   = ap_in   + ((G_AP_Q15 * delayed) >> 15);
            int32_t out     = delayed - ((G_AP_Q15 * v_new  ) >> 15);
            ap->buf[ap->idx] = (q15_t)sat_q15(v_new);
            ap->idx++;
            if (ap->idx >= ap->delay) ap->idx = 0;
            ap_in = sat_q15(out);
        }

        /* --- Wet/dry crossfade -------------------------------------- */
        int32_t wet = ap_in;
        int32_t mix = ((dr * x) >> 15) + ((we * wet) >> 15);
        io[i] = (q15_t)sat_q15(mix);
    }
}

void effect_reverb_process(q15_t *L, q15_t *R, uint32_t n)
{
    /* Per-block smoothing of all multiplicative params. Smoothing g and
       alpha matters for stability — a step from low g to 0.9 would otherwise
       click as the feedback gain abruptly changed mid-tail. */
    s_g_comb_q15 = smooth_q15(s_g_comb_q15, s_g_comb_target_q15);
    s_alpha_q15  = smooth_q15(s_alpha_q15,  s_alpha_target_q15);
    s_dry_q15    = smooth_q15(s_dry_q15,    s_dry_target_q15);
    s_wet_q15    = smooth_q15(s_wet_q15,    s_wet_target_q15);

    int32_t g     = (int32_t)s_g_comb_q15;
    int32_t alpha = (int32_t)s_alpha_q15;
    int32_t dr    = (int32_t)s_dry_q15;
    int32_t we    = (int32_t)s_wet_q15;

    process_channel(L, n, s_combs_L, s_aps_L, g, alpha, dr, we);
    process_channel(R, n, s_combs_R, s_aps_R, g, alpha, dr, we);
}

/* ----- Setters --------------------------------------------------------- */

void effect_reverb_set_size_pct(float pct)
{
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    /* Map 0..100 % -> 0.70..0.90. The upper bound stays inside stability
       margins even as the smoother briefly overshoots toward target. */
    float g = 0.70f + (pct / 100.0f) * 0.20f;
    s_g_comb_target_q15 = (q15_t)(g * 32767.0f + 0.5f);
}

void effect_reverb_set_damping_pct(float pct)
{
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    /* Map 0..100 % -> alpha 1.00..0.30. alpha is the in-feedback LP
       coefficient: 1.0 = bypass (bright), 0.30 = heavy damping (dark).
       Capping at 0.30 prevents the LP from going so close to zero that
       the tail loses all energy at high gain. */
    float alpha = 1.0f - (pct / 100.0f) * 0.70f;
    s_alpha_target_q15 = (q15_t)(alpha * 32767.0f + 0.5f);
}

void effect_reverb_set_mix_pct(float pct)
{
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    float w = pct / 100.0f;
    s_dry_target_q15 = (q15_t)((1.0f - w) * 32767.0f + 0.5f);
    s_wet_target_q15 = (q15_t)(       w  * 32767.0f + 0.5f);
}
