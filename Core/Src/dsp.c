#include "dsp.h"
#include "dsp_chain.h"
#include "effect_gain.h"
#include "stm32f7xx.h"

volatile uint32_t dsp_cycles_last;
volatile uint32_t dsp_cycles_max;

void dsp_init(void)
{
    /* DWT cycle counter — needs DEMCR.TRCENA + DWT.LAR unlock or CYCCNT
       silently stays at 0. See CLAUDE.md Guidelines for the canonical sequence. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    dsp_cycles_last = 0;
    dsp_cycles_max  = 0;

    dsp_chain_init();
}

void process_audio(int16_t *in, int16_t *out, uint32_t len)
{
    /* len is interleaved q15 samples (stereo). Frame count = len / 2. */
    dsp_chain_process(in, out, len >> 1);
}

void dsp_set_gain_db(float db)
{
    effect_gain_set_db(db);
}

/* `used` keeps the symbol in its object file; `-Wl,--undefined=dsp_set_param`
   in CMakeLists.txt forces the linker to keep it through --gc-sections.
   By dispatching to each effect's setters, dsp_set_param indirectly anchors
   them too — one --undefined entry covers every effect's debugger surface. */
__attribute__((used))
void dsp_set_param(uint8_t effect_id, uint8_t param_id, int32_t raw)
{
    switch (effect_id) {
        case EFFECT_ID_GAIN:
            /* param 0 = dB (raw is integer dB; GDB-friendly).
               param 1 = direct (raw is q15 scale << 8 | shift). */
            if (param_id == 0) {
                effect_gain_set_db((float)raw);
            } else if (param_id == 1) {
                effect_gain_set_scale_shift((q15_t)(raw >> 8), (int8_t)(raw & 0xFF));
            }
            break;
        /* Future effects (CLIP, FIR, EQ, DELAY, CHORUS, REVERB) extend this switch. */
        default:
            break;
    }
}
