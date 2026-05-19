#ifndef EFFECT_CLIP_H
#define EFFECT_CLIP_H

#include <stdint.h>
#include "arm_math.h"

/* Hard clipper with a one-pole DC blocker on each channel.
   - DC blocker runs every block to keep any DC offset out of the clip stage;
     without it, threshold sweeps thump as the DC tail gets sliced asymmetrically.
   - Clip uses arm_clip_q15 on the threshold from a 31-entry dB->q15 LUT.
   - Output makeup gain compensates the loss in level so threshold drags don't
     also act as a 30 dB attenuator.
   Threshold range: 0 dB (transparent) down to -30 dB (aggressive). */

#define EFFECT_CLIP_DB_MIN -30
#define EFFECT_CLIP_DB_MAX   0

void effect_clip_init(void);
void effect_clip_process(q15_t *L, q15_t *R, uint32_t n);
void effect_clip_set_threshold_db(float db);

#endif
