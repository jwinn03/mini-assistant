#include "dsp_chain.h"
#include "effect_gain.h"
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
    /* Gain is the only Phase 4 effect on at boot — preserves Phase 3 behavior. */
    s_enabled[EFFECT_ID_GAIN] = true;

    effect_gain_init();
    /* Future: effect_clip_init(); effect_fir_init(); ... */
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

    /* Slots reserved — Phase 4 adds one effect per step in this order:
         CLIP, FIR, EQ, DELAY, CHORUS, REVERB.
       Each adds a t0/process/delta block here in chain order. */

    interleave(s_scratch_L, s_scratch_R, out, frames);
}
