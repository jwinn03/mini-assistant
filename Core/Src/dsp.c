#include "dsp.h"
#include "stm32f7xx.h"

static volatile q15_t  s_gain_scale = 0x7FFF;
static volatile int8_t s_gain_shift = 0;

volatile uint32_t dsp_cycles_last;
volatile uint32_t dsp_cycles_max;

/* dB -> (scale, shift) lookup table for arm_scale_q15.
   arm_scale_q15 computes out = sat(((scale * in) >> 15) << shift),
   so the effective linear gain is (scale / 32768) * 2^shift.
   Each entry is precomputed: gain = 10^(dB/20), factored into a shift
   that brings the mantissa into [0.5, 1.0), then quantized to q15.
   Precomputed (not derived via libm at runtime) so the build has zero
   floating-point library dependency. */
#define GAIN_LUT_SIZE (DSP_GAIN_DB_MAX - DSP_GAIN_DB_MIN + 1)

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

void dsp_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    dsp_cycles_last = 0;
    dsp_cycles_max  = 0;
}

__attribute__((used, retain))
void dsp_set_gain(q15_t scale, int8_t shift)
{
    s_gain_scale = scale;
    s_gain_shift = shift;
}

void dsp_set_gain_db(float db)
{
    int db_int = (int)(db + (db >= 0.0f ? 0.5f : -0.5f));
    if (db_int < DSP_GAIN_DB_MIN) db_int = DSP_GAIN_DB_MIN;
    if (db_int > DSP_GAIN_DB_MAX) db_int = DSP_GAIN_DB_MAX;
    int idx = db_int - DSP_GAIN_DB_MIN;
    dsp_set_gain(gain_lut[idx].scale, gain_lut[idx].shift);
}

void process_audio(int16_t *in, int16_t *out, uint32_t len)
{
    arm_scale_q15((q15_t *)in, s_gain_scale, s_gain_shift, (q15_t *)out, len);
}
