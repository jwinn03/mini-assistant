#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include "arm_math.h"
#include "effect_gain.h"

/* Back-compat aliases for the Phase 3 gain UI. The current ui.c still uses
   these names; they go away when the UI is refactored to the page widget. */
#define DSP_GAIN_DB_MIN EFFECT_GAIN_DB_MIN
#define DSP_GAIN_DB_MAX EFFECT_GAIN_DB_MAX

void dsp_init(void);
void process_audio(int16_t *in, int16_t *out, uint32_t len);

/* Back-compat shim — delegates to effect_gain_set_db(). */
void dsp_set_gain_db(float db);

/* Single debugger entry point — replaces per-effect setters that would each
   need their own -Wl,--undefined entry in CMakeLists.txt. Effect-specific
   param IDs are documented next to each effect's setters. */
void dsp_set_param(uint8_t effect_id, uint8_t param_id, int32_t raw);

/* Total chain cycles, measured in audio.c around process_audio(). Per-effect
   breakdown lives in dsp_chain's effect_cycles[]. */
extern volatile uint32_t dsp_cycles_last;
extern volatile uint32_t dsp_cycles_max;

#endif
