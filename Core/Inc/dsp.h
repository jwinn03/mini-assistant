#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include "arm_math.h"

#define DSP_GAIN_DB_MIN -24
#define DSP_GAIN_DB_MAX  24

void dsp_init(void);
void process_audio(int16_t *in, int16_t *out, uint32_t len);
void dsp_set_gain(q15_t scale, int8_t shift);
void dsp_set_gain_db(float db);

extern volatile uint32_t dsp_cycles_last;
extern volatile uint32_t dsp_cycles_max;

#endif
