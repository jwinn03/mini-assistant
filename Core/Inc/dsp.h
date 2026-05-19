#ifndef DSP_H
#define DSP_H

#include <stdint.h>

void dsp_init(void);
void process_audio(int16_t *in, int16_t *out, uint32_t len);

/* Single debugger entry point — replaces per-effect setters that would each
   need their own -Wl,--undefined entry in CMakeLists.txt. Effect-specific
   param IDs are documented next to each effect's setters. */
void dsp_set_param(uint8_t effect_id, uint8_t param_id, int32_t raw);

/* Total chain cycles, measured in audio.c around process_audio(). Per-effect
   breakdown lives in dsp_chain's effect_cycles[]. */
extern volatile uint32_t dsp_cycles_last;
extern volatile uint32_t dsp_cycles_max;

#endif
