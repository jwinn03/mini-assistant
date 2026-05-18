#ifndef EFFECT_GAIN_H
#define EFFECT_GAIN_H

#include <stdint.h>
#include "arm_math.h"

#define EFFECT_GAIN_DB_MIN -24
#define EFFECT_GAIN_DB_MAX  24

void effect_gain_init(void);
void effect_gain_process(q15_t *L, q15_t *R, uint32_t n);
void effect_gain_set_db(float db);

/* Direct scale/shift setter — primarily a debugger entry point.
   Reachable via dsp_set_param(EFFECT_ID_GAIN, ...). */
void effect_gain_set_scale_shift(q15_t scale, int8_t shift);

#endif
