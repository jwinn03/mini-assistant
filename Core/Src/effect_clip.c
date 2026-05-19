#include "effect_clip.h"

/* ---- LUT ---------------------------------------------------------------
   Index = -dB (0..30). Each entry holds:
     threshold:    q15 saturation level for arm_clip_q15 (±threshold)
     makeup_scale: q15 multiplier for arm_scale_q15 (applies +|dB| dB makeup)
     makeup_shift: arm_scale_q15 shift exponent
   The makeup tuple at index i is the inverse of the threshold attenuation,
   identical in structure to effect_gain's LUT but pre-paired here so a single
   lookup yields both values. Precomputed (no libm at runtime). */
#define CLIP_LUT_SIZE (EFFECT_CLIP_DB_MAX - EFFECT_CLIP_DB_MIN + 1)

typedef struct {
    q15_t  threshold;
    q15_t  makeup_scale;
    int8_t makeup_shift;
} clip_entry_t;

static const clip_entry_t clip_lut[CLIP_LUT_SIZE] = {
    /*  0 dB */ {32767, 16384, 1},
    /* -1 dB */ {29205, 18386, 1},
    /* -2 dB */ {26030, 20628, 1},
    /* -3 dB */ {23199, 23145, 1},
    /* -4 dB */ {20675, 25975, 1},
    /* -5 dB */ {18428, 29136, 1},
    /* -6 dB */ {16427, 32690, 1},
    /* -7 dB */ {14637, 18340, 2},
    /* -8 dB */ {13046, 20578, 2},
    /* -9 dB */ {11628, 23089, 2},
    /*-10 dB */ {10362, 25906, 2},
    /*-11 dB */ { 9238, 29065, 2},
    /*-12 dB */ { 8233, 32613, 2},
    /*-13 dB */ { 7337, 18300, 3},
    /*-14 dB */ { 6539, 20533, 3},
    /*-15 dB */ { 5829, 23035, 3},
    /*-16 dB */ { 5194, 25845, 3},
    /*-17 dB */ { 4630, 29004, 3},
    /*-18 dB */ { 4126, 32538, 3},
    /*-19 dB */ { 3677, 18255, 4},
    /*-20 dB */ { 3277, 20480, 4},
    /*-21 dB */ { 2920, 22981, 4},
    /*-22 dB */ { 2603, 25786, 4},
    /*-23 dB */ { 2320, 28929, 4},
    /*-24 dB */ { 2068, 32459, 4},
    /*-25 dB */ { 1843, 18207, 5},
    /*-26 dB */ { 1642, 20431, 5},
    /*-27 dB */ { 1464, 22928, 5},
    /*-28 dB */ { 1305, 25723, 5},
    /*-29 dB */ { 1163, 28857, 5},
    /*-30 dB */ { 1036, 32381, 5},
};

/* Live parameter state. 32-bit aligned, written from the UI task as one of:
     - threshold (16-bit) + scale (16-bit) packed (atomic 32-bit write)
     - shift     (8-bit) — separate volatile, 32-bit aligned word, atomic
   Reads from the audio task happen once per process() block; a torn read
   would only affect one block (~2.7 ms) which is below human detection. */
static volatile q15_t  s_threshold    = 32767;
static volatile q15_t  s_makeup_scale = 16384;
static volatile int8_t s_makeup_shift = 1;

/* DC blocker state — one pole HP per channel. R = 0.999 -> ~7.6 Hz cutoff,
   ~21 ms settle. State lives in DTCM by virtue of being .bss in SRAM, but
   it's only touched from the audio task so cache misses are rare. Float is
   used because the FPU is single-precision hard, and a q15 state with the
   coefficient that close to 1.0 underflows. */
#define DC_R   0.999f
static float s_dc_x_prev_L, s_dc_y_prev_L;
static float s_dc_x_prev_R, s_dc_y_prev_R;

static void dc_block_inplace(q15_t *buf, uint32_t n, float *x_prev, float *y_prev)
{
    float xp = *x_prev;
    float yp = *y_prev;
    for (uint32_t i = 0; i < n; i++) {
        float x = (float)buf[i];
        float y = x - xp + DC_R * yp;
        xp = x;
        yp = y;
        /* Q15 saturation guard. With DC removed, |y| <= |x| for typical signals,
           but a worst-case transient on a DC-offset input can briefly overshoot. */
        if (y >  32767.0f) y =  32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        buf[i] = (q15_t)y;
    }
    *x_prev = xp;
    *y_prev = yp;
}

void effect_clip_init(void)
{
    s_threshold    = 32767;
    s_makeup_scale = 16384;
    s_makeup_shift = 1;
    s_dc_x_prev_L = s_dc_y_prev_L = 0.0f;
    s_dc_x_prev_R = s_dc_y_prev_R = 0.0f;
}

void effect_clip_process(q15_t *L, q15_t *R, uint32_t n)
{
    /* 1. DC blocker — always runs while the chain slot is enabled, so a
          threshold change never reveals a DC pedestal. */
    dc_block_inplace(L, n, &s_dc_x_prev_L, &s_dc_y_prev_L);
    dc_block_inplace(R, n, &s_dc_x_prev_R, &s_dc_y_prev_R);

    /* 2. Snapshot the LUT tuple. The three volatiles are written together
          by set_threshold_db so a torn read at worst pairs an old shift with
          a new scale for a single block — inaudible at 30 Hz UI rate. */
    q15_t  thresh = s_threshold;
    q15_t  scale  = s_makeup_scale;
    int8_t shift  = s_makeup_shift;

    /* 3. Hard clip with symmetric thresholds. arm_clip_q15 is SIMD on M7. */
    arm_clip_q15(L, L, (q15_t)(-thresh), thresh, n);
    arm_clip_q15(R, R, (q15_t)(-thresh), thresh, n);

    /* 4. Makeup gain to restore loudness lost to the threshold. */
    arm_scale_q15(L, scale, shift, L, n);
    arm_scale_q15(R, scale, shift, R, n);
}

void effect_clip_set_threshold_db(float db)
{
    int db_int = (int)(db + (db >= 0.0f ? 0.5f : -0.5f));
    if (db_int < EFFECT_CLIP_DB_MIN) db_int = EFFECT_CLIP_DB_MIN;
    if (db_int > EFFECT_CLIP_DB_MAX) db_int = EFFECT_CLIP_DB_MAX;
    int idx = -db_int;     /* 0 dB -> 0, -30 dB -> 30 */
    s_threshold    = clip_lut[idx].threshold;
    s_makeup_scale = clip_lut[idx].makeup_scale;
    s_makeup_shift = clip_lut[idx].makeup_shift;
}
