#ifndef UTTERANCE_H
#define UTTERANCE_H

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 7 — VAD-gated utterance capture.

   After the wake word fires (Phase 6), capture the spoken command until the
   user stops talking, then hold the buffer ready for Phase 8 (network
   handoff). A dedicated low-priority task is a *second* independent consumer
   of decimator_ring (the 16 kHz mono stream the wake-word task already reads);
   it keeps its own read cursor and never advances a shared tail, so the
   wake-word path is untouched.

   State machine:
     ARMED   — listening. Continuously fills a 300 ms pre-roll ring and lets
               the VAD adapt its noise floor. Watches wake_word_total_fires.
     ACTIVE  — capturing. On wake fire, the pre-roll is prepended to the
               capture buffer and live 20 ms frames are appended. The VAD
               floor is frozen here. Ends 600 ms after the last speech frame
               (hangover), or at the 8 s cap.
     ENDED   — capture accepted and available via utterance_take(). New wake
               fires are ignored until the buffer is released (or, with no
               Phase 8 consumer yet, after a short display hold) → ARMED.

   Captures whose speech content is under 200 ms are rejected (likely a false
   wake fire): no ENDED, the reject counter bumps, and the task re-arms. */

typedef enum {
    UTTERANCE_STATE_ARMED  = 0,
    UTTERANCE_STATE_ACTIVE = 1,
    UTTERANCE_STATE_ENDED  = 2,
} utterance_state_t;

/* Creates the SDRAM capture/pre-roll buffers and the utterance task. Must be
   called after audio_init() (the decimator must be live) and — per the heap
   ordering rule in StartDefaultTask — before wake_word_init(). */
void utterance_init(void);

utterance_state_t utterance_get_state(void);

/* Hand off the captured utterance (Phase 8 transport). Only succeeds in the
   ENDED state. On success, *buf / *len_samples point at the captured 16 kHz
   mono q15 audio and the buffer is pinned (the task will not re-arm or reuse
   it) until utterance_release() is called. Returns false if no capture is
   ready. */
bool utterance_take(int16_t **buf, uint32_t *len_samples);

/* Release a taken capture and allow the task to re-arm. No-op if nothing is
   taken. */
void utterance_release(void);

/* ----- observability globals (volatile; written by the utterance task) ----- */
extern volatile uint8_t  utterance_state;            /* utterance_state_t */
extern volatile uint32_t utterance_capture_ms;       /* total captured length */
extern volatile uint32_t utterance_speech_ms;        /* speech-positive duration */
extern volatile q15_t    utterance_peak;             /* running capture peak */
extern volatile uint32_t utterance_total_captures;   /* accepted captures */
extern volatile uint32_t utterance_total_rejects;    /* too-short rejections */
extern volatile uint32_t utterance_ring_overruns;    /* decimator-ring fall-behinds */

#ifdef __cplusplus
}
#endif

#endif /* UTTERANCE_H */
