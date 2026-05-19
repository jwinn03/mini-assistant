#ifndef EFFECT_DELAY_H
#define EFFECT_DELAY_H

#include <stdint.h>
#include "arm_math.h"

/* Stereo digital delay backed by two 128 K-sample ring buffers in SDRAM
   (256 KB per channel, 2.73 sec capacity, power-of-two sized so the wrap
   is a single & mask). Cache policy on SDRAM is write-through (MPU
   region 1), so no manual SCB_*DCache calls are needed.

   Parameters: time (10..2000 ms), feedback (0..95 %), mix (0..100 %).
   Feedback and mix are smoothed one-pole per block; delay time snaps
   to the new sample on change (smoothing it would pitch-shift, which
   is a chorus/flanger behavior — separate effect). */

#define EFFECT_DELAY_MS_MIN    10
#define EFFECT_DELAY_MS_MAX    2000
#define EFFECT_DELAY_FB_MIN    0
#define EFFECT_DELAY_FB_MAX    95
#define EFFECT_DELAY_MIX_MIN   0
#define EFFECT_DELAY_MIX_MAX   100

void effect_delay_init(void);
void effect_delay_process(q15_t *L, q15_t *R, uint32_t n);
void effect_delay_set_time_ms(float ms);
void effect_delay_set_feedback_pct(float pct);
void effect_delay_set_mix_pct(float pct);

#endif
