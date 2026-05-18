#include "effect_gain.h"

/* dB -> (scale, shift) lookup table for arm_scale_q15.
   arm_scale_q15 computes out = sat(((scale * in) >> 15) << shift),
   so the effective linear gain is (scale / 32768) * 2^shift.
   Each entry is precomputed: gain = 10^(dB/20), factored into a shift
   that brings the mantissa into [0.5, 1.0), then quantized to q15.
   Precomputed (not derived via libm at runtime) so the build has zero
   floating-point library dependency. Table unchanged from Phase 3. */
#define GAIN_LUT_SIZE (EFFECT_GAIN_DB_MAX - EFFECT_GAIN_DB_MIN + 1)

typedef struct {
    q15_t  scale;
    int8_t shift;
} gain_entry_t;

static const gain_entry_t gain_lut[GAIN_LUT_SIZE] = {
    /* -24 dB */ { 2068, 0},
    /* -23 dB */ { 2320, 0},
    /* -22 dB */ { 2603, 0},
    /* -21 dB */ { 2920, 0},
    /* -20 dB */ { 3277, 0},
    /* -19 dB */ { 3677, 0},
    /* -18 dB */ { 4126, 0},
    /* -17 dB */ { 4630, 0},
    /* -16 dB */ { 5194, 0},
    /* -15 dB */ { 5829, 0},
    /* -14 dB */ { 6539, 0},
    /* -13 dB */ { 7337, 0},
    /* -12 dB */ { 8233, 0},
    /* -11 dB */ { 9238, 0},
    /* -10 dB */ {10362, 0},
    /*  -9 dB */ {11628, 0},
    /*  -8 dB */ {13046, 0},
    /*  -7 dB */ {14637, 0},
    /*  -6 dB */ {16427, 0},
    /*  -5 dB */ {18428, 0},
    /*  -4 dB */ {20675, 0},
    /*  -3 dB */ {23199, 0},
    /*  -2 dB */ {26030, 0},
    /*  -1 dB */ {29205, 0},
    /*   0 dB */ {16384, 1},
    /*  +1 dB */ {18386, 1},
    /*  +2 dB */ {20628, 1},
    /*  +3 dB */ {23145, 1},
    /*  +4 dB */ {25975, 1},
    /*  +5 dB */ {29136, 1},
    /*  +6 dB */ {32690, 1},
    /*  +7 dB */ {18340, 2},
    /*  +8 dB */ {20578, 2},
    /*  +9 dB */ {23089, 2},
    /* +10 dB */ {25906, 2},
    /* +11 dB */ {29065, 2},
    /* +12 dB */ {32613, 2},
    /* +13 dB */ {18300, 3},
    /* +14 dB */ {20533, 3},
    /* +15 dB */ {23035, 3},
    /* +16 dB */ {25845, 3},
    /* +17 dB */ {29004, 3},
    /* +18 dB */ {32538, 3},
    /* +19 dB */ {18255, 4},
    /* +20 dB */ {20480, 4},
    /* +21 dB */ {22981, 4},
    /* +22 dB */ {25786, 4},
    /* +23 dB */ {28929, 4},
    /* +24 dB */ {32459, 4},
};

static volatile q15_t  s_scale = 0x7FFF;
static volatile int8_t s_shift = 0;

void effect_gain_init(void)
{
    s_scale = 0x7FFF;
    s_shift = 0;
}

void effect_gain_process(q15_t *L, q15_t *R, uint32_t n)
{
    /* Snapshot once so a mid-process parameter change can't split scale
       across channels. 32-bit aligned volatile reads are atomic on M7. */
    q15_t  scale = s_scale;
    int8_t shift = s_shift;
    arm_scale_q15(L, scale, shift, L, n);
    arm_scale_q15(R, scale, shift, R, n);
}

void effect_gain_set_db(float db)
{
    int db_int = (int)(db + (db >= 0.0f ? 0.5f : -0.5f));
    if (db_int < EFFECT_GAIN_DB_MIN) db_int = EFFECT_GAIN_DB_MIN;
    if (db_int > EFFECT_GAIN_DB_MAX) db_int = EFFECT_GAIN_DB_MAX;
    int idx = db_int - EFFECT_GAIN_DB_MIN;
    s_scale = gain_lut[idx].scale;
    s_shift = gain_lut[idx].shift;
}

void effect_gain_set_scale_shift(q15_t scale, int8_t shift)
{
    s_scale = scale;
    s_shift = shift;
}
