#include "effect_eq.h"
#include "dsp_util.h"
#include "audio.h"
#include <string.h>

/* ----- Constants --------------------------------------------------------- */

#define FS_HZ           48000.0f
#define LOW_FC_HZ          200.0f
#define MID_FC_HZ         1000.0f
#define HIGH_FC_HZ        5000.0f
#define SHELF_Q              0.707f      /* sqrt(2)/2 — Butterworth-like */
#define PEAK_Q               1.414f      /* sqrt(2) — moderate bandwidth */
#define TWO_PI               6.28318530f

#define NUM_STAGES           3           /* low + mid + high */
#define EQ_GAINS            (EFFECT_EQ_DB_MAX - EFFECT_EQ_DB_MIN + 1)  /* 25 */
#define EQ_POSTSHIFT         2           /* coeffs ≤ ±4 -> stored as ±1 q31 */
#define EQ_COEF_SCALE_DIV    0.25f       /* 1 / 2^postShift */

/* A_lut[i] = 10^((i + EFFECT_EQ_DB_MIN) / 40) — scalar in RBJ cookbook. */
static const float A_lut[EQ_GAINS] = {
    0.25119f, 0.28184f, 0.31623f, 0.35481f, 0.39811f, 0.44668f, 0.50119f,
    0.56234f, 0.63096f, 0.70795f, 0.79433f, 0.89125f, 1.00000f, 1.12202f,
    1.25893f, 1.41254f, 1.58489f, 1.77828f, 1.99526f, 2.23872f, 2.51189f,
    2.81838f, 3.16228f, 3.54813f, 3.98107f
};

typedef enum { BAND_LOW = 0, BAND_MID, BAND_HIGH } band_t;

/* ----- Storage ----------------------------------------------------------- */

/* Precomputed coefficients per (band, gain index). 3 × 25 × 5 q31 = 1500 B.
   .rodata after init, but it's writable here because we fill it at boot. */
static q31_t s_band_coeffs[NUM_STAGES][EQ_GAINS][5];

/* Live cascade — two banks so the UI task can build the next coefficient
   set without disturbing the audio task's current read. The active pointer
   is a 32-bit aligned word swap, atomic on M7. */
static q31_t s_cascade[2][NUM_STAGES * 5];
static volatile uint8_t s_active_bank;
static volatile uint8_t s_gain_idx[NUM_STAGES];

/* Per-channel state (q63 to preserve precision through the cascade).
   4 q63 per stage × 3 stages = 96 B per channel; both fit in DTCM. */
static q63_t s_state_L[NUM_STAGES * 4] __attribute__((section(".audio_buffers")));
static q63_t s_state_R[NUM_STAGES * 4] __attribute__((section(".audio_buffers")));

/* q15 ↔ q31 conversion scratch — single buffer reused for L then R. */
static q31_t s_scratch_q31[AUDIO_HALF_FRAMES] __attribute__((section(".audio_buffers")));

static arm_biquad_cas_df1_32x64_ins_q31 s_inst_L;
static arm_biquad_cas_df1_32x64_ins_q31 s_inst_R;

/* ----- Coefficient generation (RBJ cookbook) ----------------------------- */

static void compute_band_coeffs(band_t band, int gain_idx, q31_t *out5)
{
    float A = A_lut[gain_idx];
    float sqrtA;
    arm_sqrt_f32(A, &sqrtA);   /* one VSQRT.F32 instruction on M7+FPU */

    float fc, Q;
    switch (band) {
        case BAND_LOW:  fc = LOW_FC_HZ;  Q = SHELF_Q; break;
        case BAND_MID:  fc = MID_FC_HZ;  Q = PEAK_Q;  break;
        default:        fc = HIGH_FC_HZ; Q = SHELF_Q; break;
    }

    float w0    = TWO_PI * fc / FS_HZ;
    float cw    = dsp_util_cosf(w0);
    float sw    = dsp_util_sinf(w0);
    float alpha = sw / (2.0f * Q);

    float b0, b1, b2, a0, a1, a2;
    switch (band) {
        case BAND_LOW: {
            float t = 2.0f * sqrtA * alpha;
            b0 =     A * ((A + 1.0f) - (A - 1.0f) * cw + t);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
            b2 =     A * ((A + 1.0f) - (A - 1.0f) * cw - t);
            a0 =          (A + 1.0f) + (A - 1.0f) * cw + t;
            a1 = -2.0f *  ((A - 1.0f) + (A + 1.0f) * cw);
            a2 =          (A + 1.0f) + (A - 1.0f) * cw - t;
            break;
        }
        case BAND_MID: {
            b0 =  1.0f + alpha * A;
            b1 = -2.0f * cw;
            b2 =  1.0f - alpha * A;
            a0 =  1.0f + alpha / A;
            a1 = -2.0f * cw;
            a2 =  1.0f - alpha / A;
            break;
        }
        default: { /* BAND_HIGH */
            float t = 2.0f * sqrtA * alpha;
            b0 =      A * ((A + 1.0f) + (A - 1.0f) * cw + t);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
            b2 =      A * ((A + 1.0f) + (A - 1.0f) * cw - t);
            a0 =           (A + 1.0f) - (A - 1.0f) * cw + t;
            a1 =  2.0f *   ((A - 1.0f) - (A + 1.0f) * cw);
            a2 =           (A + 1.0f) - (A - 1.0f) * cw - t;
            break;
        }
    }

    /* Normalize by a0; CMSIS expects {b0, b1, b2, -a1, -a2}. Then scale by
       1/2^postShift so the stored q31 stays in [-1, 1). */
    float inv = EQ_COEF_SCALE_DIV / a0;
    float B0    =  b0 * inv;
    float B1    =  b1 * inv;
    float B2    =  b2 * inv;
    float A1_n  = -a1 * inv;
    float A2_n  = -a2 * inv;

    /* Quantize. q31_t max representable = 2^31 - 1; saturate just in case. */
    const float SCALE = 2147483648.0f;
    float vals[5] = { B0, B1, B2, A1_n, A2_n };
    for (int k = 0; k < 5; k++) {
        float v = vals[k] * SCALE;
        if (v >  2147483647.0f) v =  2147483647.0f;
        if (v < -2147483648.0f) v = -2147483648.0f;
        out5[k] = (q31_t)v;
    }
}

/* ----- Public API -------------------------------------------------------- */

void effect_eq_init(void)
{
    for (int g = 0; g < EQ_GAINS; g++) {
        compute_band_coeffs(BAND_LOW,  g, s_band_coeffs[BAND_LOW][g]);
        compute_band_coeffs(BAND_MID,  g, s_band_coeffs[BAND_MID][g]);
        compute_band_coeffs(BAND_HIGH, g, s_band_coeffs[BAND_HIGH][g]);
    }

    /* Default: all bands at 0 dB. At A=1 the b and a coefficients are
       identical, so H(z) = 1 — mathematically transparent. */
    const int idx_0db = -EFFECT_EQ_DB_MIN;   /* 12 */
    s_gain_idx[0] = s_gain_idx[1] = s_gain_idx[2] = (uint8_t)idx_0db;
    s_active_bank = 0;
    for (int st = 0; st < NUM_STAGES; st++) {
        memcpy(&s_cascade[0][st * 5],
               s_band_coeffs[st][idx_0db],
               5 * sizeof(q31_t));
    }

    arm_biquad_cas_df1_32x64_init_q31(&s_inst_L, NUM_STAGES,
                                      s_cascade[0], s_state_L, EQ_POSTSHIFT);
    arm_biquad_cas_df1_32x64_init_q31(&s_inst_R, NUM_STAGES,
                                      s_cascade[0], s_state_R, EQ_POSTSHIFT);
}

void effect_eq_process(q15_t *L, q15_t *R, uint32_t n)
{
    /* L channel: q15 -> q31 (shift left 16 is lossless), filter, convert
       back. Saturation at the q15 boundary is implicit in the >> 16 of a
       q31 value already clamped to [-2^31, 2^31-1] by arm_biquad. */
    for (uint32_t i = 0; i < n; i++) s_scratch_q31[i] = ((q31_t)L[i]) << 16;
    arm_biquad_cas_df1_32x64_q31(&s_inst_L, s_scratch_q31, s_scratch_q31, n);
    for (uint32_t i = 0; i < n; i++) L[i] = (q15_t)(s_scratch_q31[i] >> 16);

    for (uint32_t i = 0; i < n; i++) s_scratch_q31[i] = ((q31_t)R[i]) << 16;
    arm_biquad_cas_df1_32x64_q31(&s_inst_R, s_scratch_q31, s_scratch_q31, n);
    for (uint32_t i = 0; i < n; i++) R[i] = (q15_t)(s_scratch_q31[i] >> 16);
}

/* ----- Setters ----------------------------------------------------------- */

static void rebuild_band(int band, int gain_idx)
{
    if (band < 0 || band >= NUM_STAGES) return;
    if (gain_idx < 0) gain_idx = 0;
    if (gain_idx >= EQ_GAINS) gain_idx = EQ_GAINS - 1;

    s_gain_idx[band] = (uint8_t)gain_idx;

    uint8_t cur = s_active_bank;
    uint8_t nxt = (uint8_t)(cur ^ 1);

    /* Build the next cascade in the inactive bank: clone the active set,
       then overwrite only the band that changed. */
    memcpy(s_cascade[nxt], s_cascade[cur], sizeof(s_cascade[0]));
    memcpy(&s_cascade[nxt][band * 5],
           s_band_coeffs[band][gain_idx],
           5 * sizeof(q31_t));

    /* Two pointer writes + one byte flip. pCoeffs is read once at the top
       of arm_biquad_cas_df1_32x64_q31; a torn read at worst processes one
       block (~2.7 ms) with mixed coefficients across stages — sub-threshold. */
    s_inst_L.pCoeffs = s_cascade[nxt];
    s_inst_R.pCoeffs = s_cascade[nxt];
    s_active_bank = nxt;
}

static int round_db_to_idx(float db)
{
    int v = (int)(db + (db >= 0.0f ? 0.5f : -0.5f));
    return v - EFFECT_EQ_DB_MIN;
}

void effect_eq_set_low_db (float db) { rebuild_band(BAND_LOW,  round_db_to_idx(db)); }
void effect_eq_set_mid_db (float db) { rebuild_band(BAND_MID,  round_db_to_idx(db)); }
void effect_eq_set_high_db(float db) { rebuild_band(BAND_HIGH, round_db_to_idx(db)); }
