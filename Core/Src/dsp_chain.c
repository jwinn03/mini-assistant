#include "dsp_chain.h"
#include "effect_gain.h"
#include "effect_clip.h"
#include "effect_fir.h"
#include "effect_eq.h"
#include "effect_delay.h"
#include "effect_chorus.h"
#include "effect_reverb.h"
#include "audio.h"
#include "stm32f7xx.h"

/* Stereo deinterleave scratch. DTCM-resident (single-cycle access) so all
   per-effect work runs cache-free regardless of where each effect's state
   buffers live. AUDIO_HALF_FRAMES = 128 q15 per channel = 256 B per buffer. */
static q15_t s_scratch_L[AUDIO_HALF_FRAMES] __attribute__((section(".audio_buffers")));
static q15_t s_scratch_R[AUDIO_HALF_FRAMES] __attribute__((section(".audio_buffers")));

static volatile bool s_enabled[EFFECT_COUNT];

volatile uint32_t effect_cycles[EFFECT_COUNT];

const char * const effect_names[EFFECT_COUNT] = {
    "Gain", "Clip", "FIR", "EQ", "Delay", "Chorus", "Reverb"
};

static inline void deinterleave(const int16_t *src, q15_t *L, q15_t *R, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        L[i] = (q15_t)src[2*i];
        R[i] = (q15_t)src[2*i + 1];
    }
}

static inline void interleave(const q15_t *L, const q15_t *R, int16_t *dst, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        dst[2*i]     = (int16_t)L[i];
        dst[2*i + 1] = (int16_t)R[i];
    }
}

void dsp_chain_init(void)
{
    for (int i = 0; i < EFFECT_COUNT; i++) {
        s_enabled[i] = false;
        effect_cycles[i] = 0;
    }
    /* Only Gain is on at boot — preserves Phase 3 behavior so a freshly-flashed
       device sounds like a clean pass-through. Each other effect is engaged by
       the UI's per-page enable toggle (tap the area above the slider). */
    s_enabled[EFFECT_ID_GAIN] = true;

    effect_gain_init();
    effect_clip_init();
    effect_fir_init();
    effect_eq_init();
    effect_delay_init();
    effect_chorus_init();
    effect_reverb_init();
}

void dsp_chain_set_enabled(uint8_t effect_id, bool on)
{
    if (effect_id < EFFECT_COUNT) s_enabled[effect_id] = on;
}

bool dsp_chain_get_enabled(uint8_t effect_id)
{
    return (effect_id < EFFECT_COUNT) && s_enabled[effect_id];
}

void dsp_chain_process(int16_t *in, int16_t *out, uint32_t frames)
{
    deinterleave(in, s_scratch_L, s_scratch_R, frames);

    uint32_t t0;

    t0 = DWT->CYCCNT;
    if (s_enabled[EFFECT_ID_GAIN]) {
        effect_gain_process(s_scratch_L, s_scratch_R, frames);
    }
    effect_cycles[EFFECT_ID_GAIN] = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    if (s_enabled[EFFECT_ID_CLIP]) {
        effect_clip_process(s_scratch_L, s_scratch_R, frames);
    }
    effect_cycles[EFFECT_ID_CLIP] = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    if (s_enabled[EFFECT_ID_FIR]) {
        effect_fir_process(s_scratch_L, s_scratch_R, frames);
    }
    effect_cycles[EFFECT_ID_FIR] = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    if (s_enabled[EFFECT_ID_EQ]) {
        effect_eq_process(s_scratch_L, s_scratch_R, frames);
    }
    effect_cycles[EFFECT_ID_EQ] = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    if (s_enabled[EFFECT_ID_DELAY]) {
        effect_delay_process(s_scratch_L, s_scratch_R, frames);
    }
    effect_cycles[EFFECT_ID_DELAY] = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    if (s_enabled[EFFECT_ID_CHORUS]) {
        effect_chorus_process(s_scratch_L, s_scratch_R, frames);
    }
    effect_cycles[EFFECT_ID_CHORUS] = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    if (s_enabled[EFFECT_ID_REVERB]) {
        effect_reverb_process(s_scratch_L, s_scratch_R, frames);
    }
    effect_cycles[EFFECT_ID_REVERB] = DWT->CYCCNT - t0;

    interleave(s_scratch_L, s_scratch_R, out, frames);
}
