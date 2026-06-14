#ifndef VAD_H
#define VAD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Energy + zero-crossing voice-activity detector (Phase 7).

   Operates on 20 ms frames (320 q15 samples) of the 16 kHz mono stream the
   decimator already produces. The detector is intentionally simple — short-
   term mean-square energy gated against an adaptively-estimated noise floor,
   with a zero-crossing sanity bound to suppress broadband hiss. No libm: the
   floor is a one-pole leaky integrator over integer energy and the energy
   itself comes from CMSIS-DSP's arm_power_q15.

   The module is pure: it owns only the noise-floor estimate, not the capture
   ring or the utterance state machine (those live in utterance.c). The caller
   decides when to let the floor adapt (during ARMED) vs. freeze it (during an
   active capture, so it doesn't track the speaker's own voice).

   These constants are first-pass lab values; expect to re-tune them against a
   real room. Energy is in units of mean-square q15 amplitude (0 .. ~1.07e9). */

#define VAD_FRAME_LEN     320u    /* 20 ms @ 16 kHz = two 10 ms decimator hops */
#define VAD_FRAME_MS      20u

/* Reset the noise-floor estimate (called when the pipeline re-arms). */
void vad_reset(void);

/* Decide whether one 20 ms frame is speech.
     frame       : VAD_FRAME_LEN q15 samples (16 kHz mono).
     adapt_floor : true  → fold this frame into the rolling noise floor
                           (use while ARMED / listening).
                   false → leave the floor frozen (use during ACTIVE capture).
   Returns true if the frame is judged to contain speech. */
bool vad_is_speech(const int16_t *frame, bool adapt_floor);

/* Last-frame diagnostics, for the UI / tuning. Updated on each call. */
extern volatile uint32_t vad_last_energy;   /* mean-square energy of last frame */
extern volatile uint32_t vad_noise_floor;   /* current adaptive floor estimate */
extern volatile uint16_t vad_last_zcr;      /* zero crossings in last frame */

#ifdef __cplusplus
}
#endif

#endif /* VAD_H */
