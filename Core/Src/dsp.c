#include "dsp.h"
#include "dsp_chain.h"
#include "effect_gain.h"
#include "effect_clip.h"
#include "effect_fir.h"
#include "effect_eq.h"
#include "effect_delay.h"
#include "effect_chorus.h"
#include "effect_reverb.h"
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
        case EFFECT_ID_CLIP:
            /* param 0 = threshold dB (0..-30; raw is integer dB). */
            if (param_id == 0) {
                effect_clip_set_threshold_db((float)raw);
            }
            break;
        case EFFECT_ID_FIR:
            /* param 0 = bank index (0..3). */
            if (param_id == 0) {
                effect_fir_set_bank_f((float)raw);
            }
            break;
        case EFFECT_ID_EQ:
            /* param 0 = low dB, 1 = mid dB, 2 = high dB. raw is integer dB. */
            if      (param_id == 0) effect_eq_set_low_db ((float)raw);
            else if (param_id == 1) effect_eq_set_mid_db ((float)raw);
            else if (param_id == 2) effect_eq_set_high_db((float)raw);
            break;
        case EFFECT_ID_DELAY:
            /* param 0 = time ms, 1 = feedback %, 2 = mix %. raw is integer. */
            if      (param_id == 0) effect_delay_set_time_ms     ((float)raw);
            else if (param_id == 1) effect_delay_set_feedback_pct((float)raw);
            else if (param_id == 2) effect_delay_set_mix_pct     ((float)raw);
            break;
        case EFFECT_ID_CHORUS:
            /* param 0 = rate (tenths of Hz: raw=10 -> 1.0 Hz),
               param 1 = depth ms, param 2 = mix %. */
            if      (param_id == 0) effect_chorus_set_rate_hz ((float)raw * 0.1f);
            else if (param_id == 1) effect_chorus_set_depth_ms((float)raw);
            else if (param_id == 2) effect_chorus_set_mix_pct ((float)raw);
            break;
        case EFFECT_ID_REVERB:
            /* param 0 = size %, 1 = damping %, 2 = mix %. */
            if      (param_id == 0) effect_reverb_set_size_pct   ((float)raw);
            else if (param_id == 1) effect_reverb_set_damping_pct((float)raw);
            else if (param_id == 2) effect_reverb_set_mix_pct    ((float)raw);
            break;
        default:
            break;
    }
}
