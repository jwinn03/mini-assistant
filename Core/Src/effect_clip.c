#include "effect_clip.h"

/* ---- Threshold LUT -----------------------------------------------------
   Index = -dB (0..30); value = q15 saturation level for arm_clip_q15.
   Precomputed (no libm at runtime).

   Earlier revisions paired each threshold with a makeup-gain tuple intended
   to keep perceived loudness roughly constant. In practice the makeup
   amplified the noise floor whenever the signal wasn't actually peaking at
   full-scale — i.e. essentially always for mic input — which is louder, not
   level-matched. A hard clipper's natural behavior is for loudness to drop
   as peaks are sliced off; users compensate with the upstream Gain stage. */
#define CLIP_LUT_SIZE (EFFECT_CLIP_DB_MAX - EFFECT_CLIP_DB_MIN + 1)

static const q15_t clip_threshold_lut[CLIP_LUT_SIZE] = {
    /*  0 dB */ 32767,
    /* -1 dB */ 29205,
    /* -2 dB */ 26030,
    /* -3 dB */ 23199,
    /* -4 dB */ 20675,
    /* -5 dB */ 18428,
    /* -6 dB */ 16427,
    /* -7 dB */ 14637,
    /* -8 dB */ 13046,
    /* -9 dB */ 11628,
    /*-10 dB */ 10362,
    /*-11 dB */  9238,
    /*-12 dB */  8233,
    /*-13 dB */  7337,
    /*-14 dB */  6539,
    /*-15 dB */  5829,
    /*-16 dB */  5194,
    /*-17 dB */  4630,
    /*-18 dB */  4126,
    /*-19 dB */  3677,
    /*-20 dB */  3277,
    /*-21 dB */  2920,
    /*-22 dB */  2603,
    /*-23 dB */  2320,
    /*-24 dB */  2068,
    /*-25 dB */  1843,
    /*-26 dB */  1642,
    /*-27 dB */  1464,
    /*-28 dB */  1305,
    /*-29 dB */  1163,
    /*-30 dB */  1036,
};

/* Live threshold. 32-bit aligned volatile write/read = atomic on M7. */
static volatile q15_t s_threshold = 32767;

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
    s_threshold = 32767;
    s_dc_x_prev_L = s_dc_y_prev_L = 0.0f;
    s_dc_x_prev_R = s_dc_y_prev_R = 0.0f;
}

void effect_clip_process(q15_t *L, q15_t *R, uint32_t n)
{
    /* 1. DC blocker — always runs while the chain slot is enabled, so a
          threshold change never reveals a DC pedestal. */
    dc_block_inplace(L, n, &s_dc_x_prev_L, &s_dc_y_prev_L);
    dc_block_inplace(R, n, &s_dc_x_prev_R, &s_dc_y_prev_R);

    /* 2. Hard clip with symmetric thresholds. arm_clip_q15 is SIMD on M7.
          No output makeup — that role belongs to the Gain stage upstream. */
    q15_t thresh = s_threshold;
    arm_clip_q15(L, L, (q15_t)(-thresh), thresh, n);
    arm_clip_q15(R, R, (q15_t)(-thresh), thresh, n);
}

void effect_clip_set_threshold_db(float db)
{
    int db_int = (int)(db + (db >= 0.0f ? 0.5f : -0.5f));
    if (db_int < EFFECT_CLIP_DB_MIN) db_int = EFFECT_CLIP_DB_MIN;
    if (db_int > EFFECT_CLIP_DB_MAX) db_int = EFFECT_CLIP_DB_MAX;
    int idx = -db_int;     /* 0 dB -> 0, -30 dB -> 30 */
    s_threshold = clip_threshold_lut[idx];
}
