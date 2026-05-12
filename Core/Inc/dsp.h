#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include "arm_math.h"

void dsp_init(void);
void process_audio(int16_t *in, int16_t *out, uint32_t len);
void dsp_set_gain(q15_t scale, int8_t shift);

extern volatile uint32_t dsp_cycles_last;
extern volatile uint32_t dsp_cycles_max;

#endif
