#ifndef MICRO_FEATURES_H
#define MICRO_FEATURES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 6.5 — training-matched audio front end for the wake-word path.

   Thin wrapper around TFLM's audio microfrontend
   (Middlewares/Third_Party/tflm/tensorflow/lite/experimental/microfrontend/lib),
   the exact int16 fixed-point pipeline microWakeWord models are trained
   against: Hann window -> int16 kissfft -> mel filterbank -> noise reduction
   (exponential smoothing, separate even/odd coefficients) -> PCAN gain
   control -> log with shift. Output is one uint16[40] feature frame per
   10 ms step, values in roughly [0, 670].

   The config constants below are microWakeWord's training settings (which
   are also the microfrontend defaults, micro_speech lineage). A future model
   trained with different settings would change them here and re-verify with
   tools/compare_features.py.

   Replaces the Phase 6 mel_fbank.c float front end, whose log-mel output
   distribution the model had never seen (confidence pinned at 0.000). */

#define MICRO_FEATURES_SAMPLE_RATE   16000u
#define MICRO_FEATURES_WINDOW_MS     30u
#define MICRO_FEATURES_STEP_MS       10u
#define MICRO_FEATURES_HOP_SAMPLES   160u    /* STEP_MS * 16 samples/ms */
#define MICRO_FEATURES_N_CHANNELS    40u

/* One-time setup. Allocates ~10 KB of frontend state (window/FFT/filterbank
   tables) from the newlib heap via malloc — call once from main() before the
   scheduler starts, so the one-time allocation needs no malloc locking.
   Returns 0 on success, -1 on failure (mirrored in the status global). */
int micro_features_init(void);

/* Clears all streaming state (window overlap, noise-reduction estimate,
   PCAN). Call only from the task that owns the process calls — the harness
   uses this so a feature-dump capture starts from the same fresh state the
   Python reference does. */
void micro_features_reset(void);

/* Feed exactly MICRO_FEATURES_HOP_SAMPLES of 16 kHz mono q15. Returns 1 and
   fills `out` when a feature frame was produced, 0 while the 30 ms window is
   still priming (the first two hops after init/reset). `out` is the caller's
   copy — safe to hold across subsequent calls. */
int micro_features_process_hop(const int16_t *samples,
                               uint16_t out[MICRO_FEATURES_N_CHANNELS]);

/* 0 = init never ran, 1 = ready, -1 = FrontendPopulateState failed (malloc
   exhausted — see _sbrk in sysmem.c). */
extern volatile int32_t micro_features_init_status;

#ifdef __cplusplus
}
#endif

#endif /* MICRO_FEATURES_H */
