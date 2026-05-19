#ifndef EFFECT_REVERB_H
#define EFFECT_REVERB_H

#include <stdint.h>
#include "arm_math.h"

/* Schroeder reverberator: 4 parallel damped comb filters (prime-length
   delays in SDRAM) summed into 2 cascaded allpass filters (short-delay,
   DTCM-resident). Memory split keeps the high-rate allpass traffic on the
   single-cycle bus while the longer comb taps live in cacheable SDRAM —
   the headline experiment of Phase 4 in terms of SDRAM ↔ DTCM trade-offs.

   Params:
     - Size:    0..100 % -> comb feedback g 0.70..0.90 (longer tail)
     - Damping: 0..100 % -> one-pole LP in each comb's feedback (0.0..0.7
                            of HF rolloff; brighter on low values, darker
                            on high values)
     - Mix:     0..100 % -> wet/dry crossfade

   Allpass coefficient is fixed at 0.5 (typical Schroeder choice). */

#define EFFECT_REVERB_SIZE_MIN     0
#define EFFECT_REVERB_SIZE_MAX     100
#define EFFECT_REVERB_DAMP_MIN     0
#define EFFECT_REVERB_DAMP_MAX     100
#define EFFECT_REVERB_MIX_MIN      0
#define EFFECT_REVERB_MIX_MAX      100

void effect_reverb_init(void);
void effect_reverb_process(q15_t *L, q15_t *R, uint32_t n);
void effect_reverb_set_size_pct   (float pct);
void effect_reverb_set_damping_pct(float pct);
void effect_reverb_set_mix_pct    (float pct);

#endif
