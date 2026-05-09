#ifndef AUDIO_H
#define AUDIO_H

#include "stm32f7xx_hal.h"

#define AUDIO_HALF_FRAMES     128
#define AUDIO_HALF_SAMPLES    (AUDIO_HALF_FRAMES * 2)
#define AUDIO_DMA_SAMPLES     (AUDIO_HALF_SAMPLES * 2)

void audio_init(void);

#endif
