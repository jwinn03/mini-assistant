#include "vad.h"
#include "arm_math.h"

/* ===== Tuning constants ==================================================== */

/* Floor adaptation rate. The floor is a one-pole leaky integrator:
     floor += (energy - floor) >> VAD_FLOOR_SHIFT
   Shift 6 → alpha = 1/64. At 50 frames/s (20 ms frames) that is ~1.3 s of
   smoothing, close to the "1 second rolling estimate" target. Slow enough
   that a brief loud transient while listening barely moves the floor. */
#define VAD_FLOOR_SHIFT   6

/* Speech is declared when frame energy exceeds (floor × MARGIN + OFFSET).
   Energy is mean-square, so a factor of 8 is ~+9 dB over the ambient floor —
   enough to clear room noise without missing soft speech. OFFSET is an
   absolute gate so that in a near-silent room (floor ≈ 0) random low-level
   noise still can't trip the detector. ~RMS 173 of 32767 full-scale — a
   deliberately forgiving starting point. Watch vad_last_energy on the Assist
   tab while speaking to calibrate: set OFFSET between the quiet-room energy
   and your speaking-energy. */
#define VAD_MARGIN_NUM    2u
#define VAD_ABS_OFFSET    4500u

/* Zero-crossing upper bound (out of VAD_FRAME_LEN-1 = 319 possible). Voiced
   and most unvoiced speech sit well under this; near-saturating broadband
   hiss runs higher. A permissive guard, not a primary discriminator. */
#define VAD_ZCR_MAX       300u

/* ===== State ============================================================== */

volatile uint32_t vad_last_energy = 0;
volatile uint32_t vad_noise_floor = 0;
volatile uint16_t vad_last_zcr    = 0;

static uint32_t s_floor;            /* working copy of the adaptive floor */

/* ===== Implementation ===================================================== */

void vad_reset(void)
{
    /* Start the floor at 0 so the cold-start threshold is just VAD_ABS_OFFSET
       (a clean fixed gate). Seeding it at the offset instead made the initial
       threshold floor×MARGIN + OFFSET = 9×OFFSET — far too high. */
    s_floor          = 0;
    vad_noise_floor  = 0;
    vad_last_energy  = 0;
    vad_last_zcr     = 0;
}

static uint32_t frame_energy(const int16_t *frame)
{
    /* Sum of squares over the frame (q63, raw integer accumulator). Dividing
       by the frame length yields mean-square energy that fits comfortably in
       32 bits: max = 32768^2 ≈ 1.07e9. */
    q63_t sumsq = 0;
    arm_power_q15((const q15_t *)frame, VAD_FRAME_LEN, &sumsq);
    return (uint32_t)(sumsq / (q63_t)VAD_FRAME_LEN);
}

static uint16_t frame_zcr(const int16_t *frame)
{
    uint16_t zc = 0;
    bool prev_neg = (frame[0] < 0);
    for (uint32_t i = 1; i < VAD_FRAME_LEN; i++) {
        bool neg = (frame[i] < 0);
        if (neg != prev_neg) zc++;
        prev_neg = neg;
    }
    return zc;
}

bool vad_is_speech(const int16_t *frame, bool adapt_floor)
{
    uint32_t energy = frame_energy(frame);
    uint16_t zcr    = frame_zcr(frame);

    vad_last_energy = energy;
    vad_last_zcr    = zcr;

    /* Threshold computed in 64-bit space — floor × MARGIN can exceed 32 bits. */
    uint64_t thresh = (uint64_t)s_floor * VAD_MARGIN_NUM + VAD_ABS_OFFSET;
    bool loud   = ((uint64_t)energy > thresh);
    bool speech = loud && (zcr <= VAD_ZCR_MAX);

    /* Adapt the floor only while the caller permits it (ARMED). The update is
       asymmetric so that speech — including the wake word itself — can never
       inflate the floor:
         - energy below the floor: always track down (a sub-floor frame can't
           be speech, so the talker can't fool this), finding true ambient.
         - energy above the floor: only fold in NON-speech frames. Letting
           loud speech raise the floor was the bug — an inflated floor × MARGIN
           pushed the ACTIVE threshold above the command's own level, so every
           capture saw zero speech and got rejected. */
    if (adapt_floor) {
        if (energy < s_floor) {
            s_floor -= (s_floor - energy) >> VAD_FLOOR_SHIFT;
        } else if (!speech) {
            s_floor += (energy - s_floor) >> VAD_FLOOR_SHIFT;
        }
        vad_noise_floor = s_floor;
    }

    return speech;
}
