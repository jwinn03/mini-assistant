#ifndef EFFECT_EQ_H
#define EFFECT_EQ_H

#include <stdint.h>
#include "arm_math.h"

/* Three-band parametric EQ as a cascade of three biquads (low shelf,
   peaking mid, high shelf). Uses arm_biquad_cas_df1_32x64_q31 — q31
   coefficients with q63 internal state — for ~30 dB more headroom in
   the state pathway than the q15 variant, which matters when poles
   sit near the unit circle on a low-shelf boost.

   Per-band gain range is ±12 dB in 1 dB steps. Coefficients for every
   (band, gain) pair are precomputed at init from the RBJ cookbook
   formulas. Setter call = 5-q31 memcpy into the inactive bank of a
   double-buffered cascade, then an atomic pointer swap. State is
   preserved across changes (no arm_biquad_*_init from the audio task). */

#define EFFECT_EQ_DB_MIN -12
#define EFFECT_EQ_DB_MAX  12

void effect_eq_init(void);
void effect_eq_process(q15_t *L, q15_t *R, uint32_t n);

void effect_eq_set_low_db(float db);
void effect_eq_set_mid_db(float db);
void effect_eq_set_high_db(float db);

#endif
