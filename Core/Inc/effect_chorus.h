#ifndef EFFECT_CHORUS_H
#define EFFECT_CHORUS_H

#include <stdint.h>
#include "arm_math.h"

/* Stereo chorus: short modulated delay around a ~20 ms center, two LFOs
   90° apart for a wide stereo image. Backed by 4 K-sample DTCM ring
   buffers (single-cycle access — keeps the high-rate read/write traffic
   off the FMC bus, leaving SDRAM bandwidth for delay+reverb).

   LFO is a 256-entry q15 sine LUT built at boot from dsp_util_sinf,
   driven by a 32-bit phase accumulator with linear interpolation between
   adjacent LUT entries. Fractional delay reads also interpolate linearly.

   Params: rate 0.1..8 Hz, depth 0..10 ms, mix 0..100 %. */

#define EFFECT_CHORUS_RATE_HZ_MIN   0.1f
#define EFFECT_CHORUS_RATE_HZ_MAX   8.0f
#define EFFECT_CHORUS_DEPTH_MS_MIN  0
#define EFFECT_CHORUS_DEPTH_MS_MAX  10
#define EFFECT_CHORUS_MIX_MIN       0
#define EFFECT_CHORUS_MIX_MAX       100

void effect_chorus_init(void);
void effect_chorus_process(q15_t *L, q15_t *R, uint32_t n);
void effect_chorus_set_rate_hz (float hz);
void effect_chorus_set_depth_ms(float ms);
void effect_chorus_set_mix_pct (float pct);

#endif
