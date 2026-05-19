#ifndef DSP_UTIL_H
#define DSP_UTIL_H

#include <stdint.h>
#include "arm_math.h"

/* Float sin/cos for boot-time math (FIR / biquad coefficient generation).
   Uses range reduction to [-π/2, π/2] + a 5-term Taylor series — accurate
   to ~5e-5, sub-q15-LSB. Pure +/-/* on the FPU; no libm dependency.
   NOT intended for audio-rate use (use dsp_util_sin_q15_phase for LFOs). */
float dsp_util_sinf(float x);
float dsp_util_cosf(float x);

#endif
