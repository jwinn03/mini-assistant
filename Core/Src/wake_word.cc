/* Phase 6 step 6 — wake-word recognizer task.

   Pulls 30 ms windows from decimator_ring on a 20 ms hop, runs them through
   the mel-fbank front end built in step 5, and feeds the resulting 40-element
   feature vector into a microMutableOpResolver-backed MicroInterpreter loaded
   with the V2 hey_jarvis streaming model.

   The model is sensitive to its training-time feature scaling — natural log
   vs log10, dB offset, normalisation. Our front end matches the most common
   convention; the model will still run with a mismatch but produce poor
   confidences. The intended remediation is the Phase 6 plan's "step 5
   offline Python comparison" — out of scope for this step. */

#include "wake_word.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f7xx.h"        /* DWT for cycle measurement of Invoke() */

#include "decimator.h"
#include "mel_fbank.h"
#include "hey_jarvis_model_data.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* ----- public observability globals ------------------------------------- */

extern "C" volatile uint32_t wake_word_total_inferences      = 0;
extern "C" volatile float    wake_word_last_confidence       = 0.0f;
extern "C" volatile uint32_t wake_word_total_fires           = 0;
extern "C" volatile uint32_t wake_word_last_inference_cycles = 0;
extern "C" volatile int32_t  wake_word_init_status           = 0;

namespace {

/* Trigger policy — keep close to the plan's 0.95 + ≥3 frames + 200 ms
   cooldown. Confidence floats and the threshold is a float so we can tune
   per model. */
constexpr float    kThreshold       = 0.95f;
constexpr uint32_t kMinConsecutive  = 3u;
constexpr uint32_t kCooldownFrames  = 10u;   /* 10 * 20 ms = 200 ms */

/* TFLM arena lives in SDRAM (.sdram section, MPU region 1 = write-through
   cacheable, no maintenance needed). 60 KB gives the model plenty of slack;
   real usage will be ~half that. */
constexpr uint32_t kArenaSize       = 60u * 1024u;
__attribute__((section(".sdram"), aligned(16)))
static uint8_t s_arena[kArenaSize];

/* Resource variables. Streaming TFLM models often back their hidden state
   with TF Lite resource variables; reserve a generous slot count up front
   (the arena absorbs unused ones for free). */
constexpr int kMaxResourceVariables = 24;

/* Op resolver — superset of what microWakeWord V2 models are known to use.
   Each registration is ~1 KB of FLASH that --gc-sections cannot strip; if
   tightening FLASH later, prune to only the ops the model actually exercises. */
using WakeOpResolver = tflite::MicroMutableOpResolver<18>;

tflite::MicroAllocator*          s_allocator   = nullptr;
tflite::MicroResourceVariables*  s_resource    = nullptr;
tflite::MicroInterpreter*        s_interpreter = nullptr;
WakeOpResolver                   s_resolver;
alignas(tflite::MicroInterpreter)
static uint8_t s_interp_storage[sizeof(tflite::MicroInterpreter)];

/* Sliding state for the task loop. */
int16_t s_window[MEL_FBANK_WIN_SIZE];
float   s_features[MEL_FBANK_N_MELS];
uint32_t s_consecutive_above = 0;
uint32_t s_cooldown_remaining = 0;

TfLiteStatus register_ops(WakeOpResolver& r)
{
    TF_LITE_ENSURE_STATUS(r.AddConv2D());
    TF_LITE_ENSURE_STATUS(r.AddDepthwiseConv2D());
    TF_LITE_ENSURE_STATUS(r.AddAveragePool2D());
    TF_LITE_ENSURE_STATUS(r.AddMaxPool2D());
    TF_LITE_ENSURE_STATUS(r.AddFullyConnected());
    TF_LITE_ENSURE_STATUS(r.AddSoftmax());
    TF_LITE_ENSURE_STATUS(r.AddLogistic());
    TF_LITE_ENSURE_STATUS(r.AddReshape());
    TF_LITE_ENSURE_STATUS(r.AddQuantize());
    TF_LITE_ENSURE_STATUS(r.AddDequantize());
    TF_LITE_ENSURE_STATUS(r.AddStridedSlice());
    TF_LITE_ENSURE_STATUS(r.AddConcatenation());
    TF_LITE_ENSURE_STATUS(r.AddAdd());
    TF_LITE_ENSURE_STATUS(r.AddMul());
    TF_LITE_ENSURE_STATUS(r.AddPad());
    TF_LITE_ENSURE_STATUS(r.AddRelu());
    TF_LITE_ENSURE_STATUS(r.AddRelu6());
    TF_LITE_ENSURE_STATUS(r.AddTanh());
    return kTfLiteOk;
}

void feed_features_to_input()
{
    auto* input = s_interpreter->input(0);
    if (input->type == kTfLiteInt8) {
        const float scale = input->params.scale;
        const int   zp    = input->params.zero_point;
        const float inv_s = (scale != 0.0f) ? (1.0f / scale) : 0.0f;
        for (uint32_t i = 0; i < MEL_FBANK_N_MELS; i++) {
            int32_t q = (int32_t)(s_features[i] * inv_s) + zp;
            if (q < -128) q = -128;
            if (q >  127) q =  127;
            input->data.int8[i] = (int8_t)q;
        }
    } else if (input->type == kTfLiteFloat32) {
        for (uint32_t i = 0; i < MEL_FBANK_N_MELS; i++) {
            input->data.f[i] = s_features[i];
        }
    }
    /* Other input types (uint8, int16) would need additional branches; the
       V2 microWakeWord models are int8 quantised so we expect that path. */
}

float read_output_confidence()
{
    auto* output = s_interpreter->output(0);
    if (output->type == kTfLiteInt8) {
        const int8_t q = output->data.int8[0];
        return ((float)q - (float)output->params.zero_point) * output->params.scale;
    }
    if (output->type == kTfLiteFloat32) {
        return output->data.f[0];
    }
    return 0.0f;
}

void update_trigger(float confidence)
{
    if (s_cooldown_remaining > 0) {
        s_cooldown_remaining--;
        s_consecutive_above = 0;
        return;
    }
    if (confidence > kThreshold) {
        s_consecutive_above++;
        if (s_consecutive_above >= kMinConsecutive) {
            wake_word_total_fires++;
            s_consecutive_above  = 0;
            s_cooldown_remaining = kCooldownFrames;
        }
    } else {
        s_consecutive_above = 0;
    }
}

void wake_word_task(void* /*arg*/)
{
    /* Wait for enough decimator samples to form one full window. */
    while (decimator_head < MEL_FBANK_WIN_SIZE) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint32_t window_start = decimator_head - MEL_FBANK_WIN_SIZE;

    for (;;) {
        /* Block until the next window's worth of new samples is available. */
        const uint32_t window_end = window_start + MEL_FBANK_WIN_SIZE;
        while ((int32_t)(decimator_head - window_end) < 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* Copy window from the ring (handle wraparound via mask). */
        for (uint32_t i = 0; i < MEL_FBANK_WIN_SIZE; i++) {
            s_window[i] = decimator_ring[(window_start + i) & DECIMATOR_RING_MASK];
        }

        mel_fbank_process(s_window, s_features);

        feed_features_to_input();
        const uint32_t t0 = DWT->CYCCNT;
        TfLiteStatus rc = s_interpreter->Invoke();
        wake_word_last_inference_cycles = DWT->CYCCNT - t0;

        if (rc == kTfLiteOk) {
            const float c = read_output_confidence();
            wake_word_last_confidence = c;
            wake_word_total_inferences++;
            wake_word_init_status = (int32_t)wake_word_total_inferences;
            update_trigger(c);
        }

        window_start += MEL_FBANK_HOP_SIZE;
    }
}

}  // namespace

extern "C" void wake_word_init(void)
{
    const tflite::Model* model = ::tflite::GetModel(hey_jarvis_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        wake_word_init_status = -1;
        return;
    }

    if (register_ops(s_resolver) != kTfLiteOk) {
        wake_word_init_status = -2;
        return;
    }

    s_allocator = tflite::MicroAllocator::Create(s_arena, kArenaSize);
    if (s_allocator == nullptr) {
        wake_word_init_status = -3;
        return;
    }

    s_resource = tflite::MicroResourceVariables::Create(s_allocator,
                                                        kMaxResourceVariables);
    if (s_resource == nullptr) {
        wake_word_init_status = -4;
        return;
    }

    s_interpreter = new (s_interp_storage) tflite::MicroInterpreter(
        model, s_resolver, s_allocator, s_resource);

    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        wake_word_init_status = -5;
        return;
    }

    /* Smoke-check: confirm input geometry matches our feature vector. The
       model may want (1, 1, 40, 1) or (1, 40); element count is what
       matters for our memcpy-style fill. */
    if (s_interpreter->input(0)->dims->size > 0) {
        uint32_t in_elems = 1;
        for (int d = 0; d < s_interpreter->input(0)->dims->size; d++) {
            in_elems *= (uint32_t)s_interpreter->input(0)->dims->data[d];
        }
        if (in_elems != MEL_FBANK_N_MELS) {
            wake_word_init_status = -6;
            return;
        }
    }

    /* Init succeeded. wake_word_init_status will tick up to mirror
       wake_word_total_inferences as soon as the task starts producing. */

    BaseType_t ok = xTaskCreate(wake_word_task, "WakeWord", 1024, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        wake_word_init_status = -7;
    }
}
