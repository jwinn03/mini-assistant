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

#include <string.h>

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

/* Diagnostic globals (see wake_word.h). */
extern "C" volatile uint8_t  wake_word_model_op_count   = 0;
extern "C" volatile uint16_t wake_word_model_ops[WAKE_WORD_MAX_OPS] = {};
extern "C" volatile uint8_t  wake_word_model_has_custom = 0;
extern "C" volatile uint32_t wake_word_arena_used       = 0;

extern "C" volatile uint8_t  wake_word_input_dims_count = 0;
extern "C" volatile int32_t  wake_word_input_dims[WAKE_WORD_MAX_DIMS] = {};
extern "C" volatile uint32_t wake_word_input_elements   = 0;
extern "C" volatile uint8_t  wake_word_input_type       = 0;
extern "C" volatile float    wake_word_input_scale      = 0.0f;
extern "C" volatile int32_t  wake_word_input_zero_point = 0;

extern "C" volatile uint8_t  wake_word_output_dims_count = 0;
extern "C" volatile int32_t  wake_word_output_dims[WAKE_WORD_MAX_DIMS] = {};
extern "C" volatile uint32_t wake_word_output_elements   = 0;
extern "C" volatile uint8_t  wake_word_output_type       = 0;
extern "C" volatile float    wake_word_output_scale      = 0.0f;
extern "C" volatile int32_t  wake_word_output_zero_point = 0;

namespace {

/* Trigger policy — keep close to the plan's 0.95 + ≥3 frames + 200 ms
   cooldown. Confidence floats and the threshold is a float so we can tune
   per model. */
constexpr float    kThreshold       = 0.95f;
constexpr uint32_t kMinConsecutive  = 3u;
constexpr uint32_t kCooldownFrames  = 10u;   /* 10 * 20 ms = 200 ms */

/* TFLM arena lives in SDRAM (.sdram section, MPU region 1 = write-through
   cacheable, no maintenance needed). 80 KB gives generous slack so we can
   rule out arena-too-small as a cause of AllocateTensors failure when
   diagnosing missing ops. */
constexpr uint32_t kArenaSize       = 80u * 1024u;
__attribute__((section(".sdram"), aligned(16)))
static uint8_t s_arena[kArenaSize];

/* Resource variables. Streaming TFLM models often back their hidden state
   with TF Lite resource variables; reserve a generous slot count up front
   (the arena absorbs unused ones for free). */
constexpr int kMaxResourceVariables = 24;

/* The V2 hey_jarvis model expects a stack of the 3 most-recent mel feature
   frames as input (3 * 40 = 120 elements). The model also maintains
   longer-term state via TF Lite resource variables (CALL_ONCE / VAR_HANDLE
   etc. in the op list). If the chosen wake-word model uses a different
   stack depth, init bails with -6 and you set kFrameHistory accordingly. */
constexpr uint32_t kFrameHistory   = 3u;
constexpr uint32_t kInputElements  = kFrameHistory * MEL_FBANK_N_MELS;

/* Op resolver — superset of what microWakeWord V2 models are known to use.
   Each registration is ~1 KB of FLASH that --gc-sections cannot strip; if
   tightening FLASH later, prune to only the ops the model actually exercises.
   Includes resource-variable ops (VAR_HANDLE, READ/ASSIGN_VARIABLE, CALL_ONCE)
   because streaming TFLM models often use them for persistent state. */
using WakeOpResolver = tflite::MicroMutableOpResolver<26>;

tflite::MicroAllocator*          s_allocator   = nullptr;
tflite::MicroResourceVariables*  s_resource    = nullptr;
tflite::MicroInterpreter*        s_interpreter = nullptr;
WakeOpResolver                   s_resolver;
alignas(tflite::MicroInterpreter)
static uint8_t s_interp_storage[sizeof(tflite::MicroInterpreter)];

/* Sliding state for the task loop. */
int16_t s_window[MEL_FBANK_WIN_SIZE];
float   s_features[MEL_FBANK_N_MELS];
/* Stack of the most-recent kFrameHistory mel frames. Layout: oldest at
   offset 0, newest at offset (kFrameHistory - 1) * MEL_FBANK_N_MELS. */
__attribute__((section(".audio_buffers")))
float    s_feature_stack[kInputElements];
uint32_t s_consecutive_above  = 0;
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
    /* Streaming model state machinery. */
    TF_LITE_ENSURE_STATUS(r.AddVarHandle());
    TF_LITE_ENSURE_STATUS(r.AddReadVariable());
    TF_LITE_ENSURE_STATUS(r.AddAssignVariable());
    TF_LITE_ENSURE_STATUS(r.AddCallOnce());
    /* Misc that some KWS models pull in. */
    TF_LITE_ENSURE_STATUS(r.AddLeakyRelu());
    TF_LITE_ENSURE_STATUS(r.AddHardSwish());
    TF_LITE_ENSURE_STATUS(r.AddMean());
    TF_LITE_ENSURE_STATUS(r.AddSub());
    return kTfLiteOk;
}

/* Walk the model's operator_codes table and copy the builtin code of each
   unique op into wake_word_model_ops[]. Lets the debugger see the model's
   ops without needing TFLM's ErrorReporter to be wired up. */
static void dump_model_ops(const tflite::Model* model)
{
    auto* op_codes = model->operator_codes();
    if (op_codes == nullptr) return;

    uint32_t out = 0;
    for (uint32_t i = 0;
         i < op_codes->size() && out < WAKE_WORD_MAX_OPS;
         i++)
    {
        auto* oc = op_codes->Get(i);
        if (oc == nullptr) continue;
        if (oc->custom_code() != nullptr) {
            wake_word_model_has_custom = 1;
        }
        wake_word_model_ops[out++] = (uint16_t)oc->builtin_code();
    }
    wake_word_model_op_count = (uint8_t)out;
}

void feed_features_to_input()
{
    auto* input = s_interpreter->input(0);
    if (input->type == kTfLiteInt8) {
        const float scale = input->params.scale;
        const int   zp    = input->params.zero_point;
        const float inv_s = (scale != 0.0f) ? (1.0f / scale) : 0.0f;
        for (uint32_t i = 0; i < kInputElements; i++) {
            int32_t q = (int32_t)(s_feature_stack[i] * inv_s) + zp;
            if (q < -128) q = -128;
            if (q >  127) q =  127;
            input->data.int8[i] = (int8_t)q;
        }
    } else if (input->type == kTfLiteFloat32) {
        for (uint32_t i = 0; i < kInputElements; i++) {
            input->data.f[i] = s_feature_stack[i];
        }
    }
    /* If a future model wants uint8 input it would need its own branch here;
       the V2 microWakeWord models are int8 quantised so we expect that path. */
}

float read_output_confidence()
{
    auto* output = s_interpreter->output(0);
    if (output->type == kTfLiteInt8) {
        const int8_t q = output->data.int8[0];
        return ((float)q - (float)output->params.zero_point) * output->params.scale;
    }
    if (output->type == kTfLiteUInt8) {
        const uint8_t q = output->data.uint8[0];
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

        /* Crude bridge between our log-mel features and what microWakeWord's
           audio_microfrontend would produce. The model expects features in
           ~[0, 26]; ours land in ~[-14, +8]. A constant offset of +14 makes
           the floor non-negative; the clip keeps quiet bins from going
           further below. This does NOT match the training-time front end
           (PCAN + smoothing + log-shift) and exists only to prove the model
           responds to input. Replacing this with a faithful audio_microfrontend
           port is the proper Phase-6-completion task. */
        for (uint32_t i = 0; i < MEL_FBANK_N_MELS; i++) {
            float v = s_features[i] + 14.0f;
            if (v < 0.0f) v = 0.0f;
            s_features[i] = v;
        }

        /* Shift the feature stack one frame older and append the new one at
           the end. Net effect: stack always holds the most-recent
           kFrameHistory frames, oldest at offset 0. Until kFrameHistory
           frames have actually been produced (~60 ms after boot), the
           leading frames are zero — accepted as a brief warm-up artefact. */
        memmove(&s_feature_stack[0],
                &s_feature_stack[MEL_FBANK_N_MELS],
                (kInputElements - MEL_FBANK_N_MELS) * sizeof(float));
        memcpy(&s_feature_stack[kInputElements - MEL_FBANK_N_MELS],
               s_features,
               MEL_FBANK_N_MELS * sizeof(float));

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

    /* Diagnostic: surface the model's op list before anything else can fail.
       Even if init bails later, the user can read wake_word_model_ops[]. */
    dump_model_ops(model);

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

    /* Surface actual arena usage so we can tighten kArenaSize later if we
       want to reclaim SDRAM. */
    wake_word_arena_used = (uint32_t)s_interpreter->arena_used_bytes();

    /* Dump input + output tensor geometry so we can adapt the feature
       pipeline once we know what the model actually expects. */
    {
        auto* in = s_interpreter->input(0);
        if (in != nullptr && in->dims != nullptr) {
            const int n = in->dims->size;
            wake_word_input_dims_count = (uint8_t)(n > (int)WAKE_WORD_MAX_DIMS ? WAKE_WORD_MAX_DIMS : n);
            uint32_t elems = 1;
            for (int d = 0; d < n; d++) {
                if (d < (int)WAKE_WORD_MAX_DIMS) {
                    wake_word_input_dims[d] = in->dims->data[d];
                }
                elems *= (uint32_t)in->dims->data[d];
            }
            wake_word_input_elements   = elems;
            wake_word_input_type       = (uint8_t)in->type;
            wake_word_input_scale      = in->params.scale;
            wake_word_input_zero_point = in->params.zero_point;
        }
        auto* out = s_interpreter->output(0);
        if (out != nullptr && out->dims != nullptr) {
            const int n = out->dims->size;
            wake_word_output_dims_count = (uint8_t)(n > (int)WAKE_WORD_MAX_DIMS ? WAKE_WORD_MAX_DIMS : n);
            uint32_t elems = 1;
            for (int d = 0; d < n; d++) {
                if (d < (int)WAKE_WORD_MAX_DIMS) {
                    wake_word_output_dims[d] = out->dims->data[d];
                }
                elems *= (uint32_t)out->dims->data[d];
            }
            wake_word_output_elements   = elems;
            wake_word_output_type       = (uint8_t)out->type;
            wake_word_output_scale      = out->params.scale;
            wake_word_output_zero_point = out->params.zero_point;
        }
    }

    /* Smoke-check: confirm input geometry matches our kFrameHistory * 40
       stacked feature vector. */
    if (s_interpreter->input(0)->dims->size > 0) {
        uint32_t in_elems = 1;
        for (int d = 0; d < s_interpreter->input(0)->dims->size; d++) {
            in_elems *= (uint32_t)s_interpreter->input(0)->dims->data[d];
        }
        if (in_elems != kInputElements) {
            wake_word_init_status = -6;
            return;
        }
    }

    /* Zero the feature stack so the first kFrameHistory invocations see
       silence padding ahead of any real audio. */
    memset(s_feature_stack, 0, sizeof(s_feature_stack));

    /* Init succeeded. wake_word_init_status will tick up to mirror
       wake_word_total_inferences as soon as the task starts producing. */

    BaseType_t ok = xTaskCreate(wake_word_task, "WakeWord", 1024, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        wake_word_init_status = -7;
    }
}
