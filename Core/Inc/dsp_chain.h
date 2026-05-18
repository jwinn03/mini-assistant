#ifndef DSP_CHAIN_H
#define DSP_CHAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"

/* Fixed effect order in the audio chain. The enum value is also the index
   into effect_cycles[] and the effect_id passed to dsp_set_param. Adding
   a new effect = append before EFFECT_COUNT and add the dispatch slot in
   dsp_chain_process(). */
typedef enum {
    EFFECT_ID_GAIN = 0,
    EFFECT_ID_CLIP,
    EFFECT_ID_FIR,
    EFFECT_ID_EQ,
    EFFECT_ID_DELAY,
    EFFECT_ID_CHORUS,
    EFFECT_ID_REVERB,
    EFFECT_COUNT
} effect_id_t;

/* Per-effect cycle counts measured in dsp_chain_process(). Written by the
   audio task, read by the UI; 32-bit aligned so the read is atomic on M7. */
extern volatile uint32_t effect_cycles[EFFECT_COUNT];

/* Display names — used by the UI page selector. */
extern const char * const effect_names[EFFECT_COUNT];

void dsp_chain_init(void);
void dsp_chain_process(int16_t *in_interleaved, int16_t *out_interleaved, uint32_t frames);
void dsp_chain_set_enabled(uint8_t effect_id, bool on);
bool dsp_chain_get_enabled(uint8_t effect_id);

#endif
