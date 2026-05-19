#ifndef EFFECT_FIR_H
#define EFFECT_FIR_H

#include <stdint.h>
#include "arm_math.h"

/* 32-tap Hamming-windowed-sinc FIR with four hardwired coefficient banks.
   Coefficients are computed once at boot from dsp_util_sinf/cosf — the same
   approach as a precomputed LUT, just lifted into init() so changing a
   cutoff doesn't require regenerating a header. Banks live in .bss; the
   active bank is selected via an atomic pointer swap into the
   arm_fir_instance_q15.pCoeffs field (no arm_fir_init from the audio task,
   which would zero state and click). */

#define EFFECT_FIR_TAPS      32
#define EFFECT_FIR_BANKS      4
#define EFFECT_FIR_BANK_MIN   0
#define EFFECT_FIR_BANK_MAX   3

extern const char * const effect_fir_bank_names[EFFECT_FIR_BANKS];

void effect_fir_init(void);
void effect_fir_process(q15_t *L, q15_t *R, uint32_t n);
void effect_fir_set_bank_f(float bank);   /* slider-friendly: rounds to nearest int */

#endif
