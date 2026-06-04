#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 6 step 6 — TFLM-driven wake-word recognizer.

   wake_word_init() spawns a low-priority FreeRTOS task that pulls 30 ms
   windows from decimator_ring at the 20 ms hop rate, runs mel_fbank_process
   for features, feeds those into a static MicroInterpreter, and looks for
   sustained high confidence on the model output. A successful wake fires
   asynchronously via the trigger globals below — Phase 7 will replace that
   poll-based signal with a FreeRTOS event group bit. */

void wake_word_init(void);

/* Status / observability globals.
   `wake_word_total_inferences`   monotonic count of Invoke() calls.
   `wake_word_last_confidence`    most-recent dequantised output in [0, 1].
   `wake_word_total_fires`        monotonic count of debounced wake triggers.
   `wake_word_last_inference_cycles`  DWT-measured cycle cost of last Invoke(). */
extern volatile uint32_t wake_word_total_inferences;
extern volatile float    wake_word_last_confidence;
extern volatile uint32_t wake_word_total_fires;
extern volatile uint32_t wake_word_last_inference_cycles;

/* Init status. 0 = never ran; positive = number of Invoke()s completed since
   boot (mirrors wake_word_total_inferences). Negative values are init
   failures with the code matching wake_word.cc internal sentinels. */
extern volatile int32_t wake_word_init_status;

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_H */
