#include "dsp.h"
#include "stm32f7xx.h"

static volatile q15_t  s_gain_scale = 0x7FFF;
static volatile int8_t s_gain_shift = 0;

volatile uint32_t dsp_cycles_last;
volatile uint32_t dsp_cycles_max;

void dsp_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    dsp_cycles_last = 0;
    dsp_cycles_max  = 0;
}

void dsp_set_gain(q15_t scale, int8_t shift)
{
    s_gain_scale = scale;
    s_gain_shift = shift;
}

void process_audio(int16_t *in, int16_t *out, uint32_t len)
{
    arm_scale_q15((q15_t *)in, s_gain_scale, s_gain_shift, (q15_t *)out, len);
}
