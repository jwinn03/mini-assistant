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

/* Diagnostic: at init we walk the model's operator_codes table and copy each
   builtin code into wake_word_model_ops[]. Cross-reference with the
   BuiltinOperator enum in tensorflow/lite/schema/schema_generated.h to see
   which ops the model expects vs. what we registered in the resolver.
   Useful when AllocateTensors fails with -5 to spot the missing op. */
#define WAKE_WORD_MAX_OPS 48u
extern volatile uint8_t  wake_word_model_op_count;
extern volatile uint16_t wake_word_model_ops[WAKE_WORD_MAX_OPS];
extern volatile uint8_t  wake_word_model_has_custom;
extern volatile uint32_t wake_word_arena_used;     /* AllocateTensors result */

/* Tensor geometry diagnostics. Filled after AllocateTensors() so we can
   read off the shape / type / quantization params without instrumenting
   TFLM internals. Dim arrays hold up to 8 dimensions; entries past
   *_dims_count are undefined. Types use TfLiteType enum values
   (kTfLiteFloat32 = 1, kTfLiteInt8 = 9, etc.). */
#define WAKE_WORD_MAX_DIMS 8u
extern volatile uint8_t  wake_word_input_dims_count;
extern volatile int32_t  wake_word_input_dims[WAKE_WORD_MAX_DIMS];
extern volatile uint32_t wake_word_input_elements;
extern volatile uint8_t  wake_word_input_type;
extern volatile float    wake_word_input_scale;
extern volatile int32_t  wake_word_input_zero_point;

extern volatile uint8_t  wake_word_output_dims_count;
extern volatile int32_t  wake_word_output_dims[WAKE_WORD_MAX_DIMS];
extern volatile uint32_t wake_word_output_elements;
extern volatile uint8_t  wake_word_output_type;
extern volatile float    wake_word_output_scale;
extern volatile int32_t  wake_word_output_zero_point;

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_H */
