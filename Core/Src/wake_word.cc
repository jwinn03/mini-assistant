/* Phase 6.5 — wake-word recognizer task, training-matched front end.

   Pulls 10 ms hops (160 samples) from decimator_ring, feeds them through the
   TFLM audio microfrontend (micro_features.c — the exact int16 pipeline
   microWakeWord trains against), and stages each resulting uint16[40] frame
   into the model input. The V2 hey_jarvis streaming model takes 3 FRESH
   frames per Invoke (input (1, 3, 40)) and runs every 30 ms — its initial
   conv layer strides over the 3 slices while resource variables carry the
   longer-term context. Feeding overlapping (sliding) frames corrupts that
   internal state; that and the unmatched float log-mel front end were why
   the Phase 6 version never fired.

   Feature scaling: the model was trained on microfrontend output / 25.6
   (float range ~[0, 26]); ESPHome ships the same arithmetic fused into int8
   space as (x * 256) / 666 - 128. We scale to float and let
   feed_features_to_input() quantize with the input tensor's published
   scale/zero-point.

   Trigger policy follows the hey_jarvis V2 manifest: mean of the last 5
   inference probabilities > 0.97, then a ~1 s refractory. */

#include "wake_word.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f7xx.h"        /* DWT for cycle measurement of Invoke() */

#include "decimator.h"
#include "micro_features.h"
#include "feature_dump.h"
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
extern "C" volatile uint32_t wake_word_ring_overruns         = 0;

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

/* Trigger policy — from the hey_jarvis V2 manifest (probability_cutoff 0.97,
   sliding_window_size 5). Fire when the mean of the last 5 inference
   probabilities exceeds the cutoff, then hold off ~1 s (refractory) so one
   utterance can't double-fire. All tunable per model. */
constexpr float    kProbabilityCutoff    = 0.97f;
constexpr uint32_t kSlidingWindow        = 5u;
constexpr uint32_t kRefractoryInferences = 33u;   /* 33 * 30 ms ≈ 1 s */

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

/* The V2 hey_jarvis model takes 3 FRESH feature frames per Invoke — input
   shape (1, 3, 40), 120 int8 elements, one inference per 30 ms of audio.
   Longer-term context lives inside the model in TF Lite resource variables
   (CALL_ONCE / VAR_HANDLE etc. in the op list). If a future model uses a
   different stride, init bails with -6 and kStride changes here. */
constexpr uint32_t kStride         = 3u;
constexpr uint32_t kInputElements  = kStride * MICRO_FEATURES_N_CHANNELS;

/* uint16 microfrontend output -> the float feature domain the model was
   trained on. See the file header for the ESPHome int8 equivalence. */
constexpr float    kFeatureScale   = 1.0f / 25.6f;

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

/* Task-loop state. One hop of decimator output, the most recent feature
   frame, and the 3-frame staging buffer the input tensor is filled from
   (slot s_stride_step * 40; oldest frame at offset 0). */
int16_t  s_hop[MICRO_FEATURES_HOP_SAMPLES];
uint16_t s_frame[MICRO_FEATURES_N_CHANNELS];
__attribute__((section(".audio_buffers")))
float    s_feature_stack[kInputElements];
uint32_t s_stride_step = 0;

/* Trigger state: ring of the last kSlidingWindow probabilities. */
float    s_recent[kSlidingWindow];
uint32_t s_recent_idx          = 0;
uint32_t s_recent_count        = 0;
uint32_t s_refractory_remaining = 0;

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
    if (s_refractory_remaining > 0) {
        s_refractory_remaining--;
        return;
    }

    s_recent[s_recent_idx] = confidence;
    s_recent_idx = (s_recent_idx + 1u) % kSlidingWindow;
    if (s_recent_count < kSlidingWindow) {
        s_recent_count++;
        return;   /* don't evaluate until the window is full */
    }

    float sum = 0.0f;
    for (uint32_t i = 0; i < kSlidingWindow; i++) {
        sum += s_recent[i];
    }
    if (sum > kProbabilityCutoff * (float)kSlidingWindow) {
        wake_word_total_fires++;
        s_recent_idx           = 0;
        s_recent_count         = 0;
        s_refractory_remaining = kRefractoryInferences;
    }
}

void wake_word_task(void* /*arg*/)
{
    /* Start at "now" — the frontend buffers its own 30 ms window internally,
       so the first feature frame appears after three hops. */
    uint32_t read_pos = decimator_head;

    for (;;) {
        /* Debugger-requested feature-dump recapture: reset the frontend and
           all staging/trigger state so the device and the Python reference
           both start from frame 0 of a fresh pipeline. */
        if (feature_dump_take_rearm()) {
            micro_features_reset();
            s_stride_step  = 0;
            s_recent_idx   = 0;
            s_recent_count = 0;
        }

        /* Block until a full hop of new samples is available. */
        while ((int32_t)(decimator_head - (read_pos + MICRO_FEATURES_HOP_SAMPLES)) < 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* If we fell more than a ring behind (UI hogging the CPU, debugger
           halt), the unread samples are already overwritten. Jump to the
           freshest full hop. The frontend sees a seam — harmless for
           detection, fatal for a feature-dump capture in flight. */
        if (decimator_head - read_pos > DECIMATOR_RING_SIZE) {
            read_pos = decimator_head - MICRO_FEATURES_HOP_SAMPLES;
            wake_word_ring_overruns++;
            feature_dump_mark_invalid();
        }

        for (uint32_t i = 0; i < MICRO_FEATURES_HOP_SAMPLES; i++) {
            s_hop[i] = decimator_ring[(read_pos + i) & DECIMATOR_RING_MASK];
        }
        /* Re-check after the copy: if the audio task lapped us mid-copy,
           part of s_hop is freshly overwritten data. Detection just hears
           a glitch; a feature-dump capture in flight must be discarded. */
        if (decimator_head - read_pos > DECIMATOR_RING_SIZE) {
            wake_word_ring_overruns++;
            feature_dump_mark_invalid();
        }
        read_pos += MICRO_FEATURES_HOP_SAMPLES;

        feature_dump_pcm(s_hop, MICRO_FEATURES_HOP_SAMPLES);

        if (!micro_features_process_hop(s_hop, s_frame)) {
            continue;   /* 30 ms window still priming (first two hops) */
        }
        feature_dump_frame(s_frame);

        /* Stage the frame into the next stride slot, scaled into the float
           feature domain the model was trained on. */
        float *slot = &s_feature_stack[s_stride_step * MICRO_FEATURES_N_CHANNELS];
        for (uint32_t i = 0; i < MICRO_FEATURES_N_CHANNELS; i++) {
            slot[i] = (float)s_frame[i] * kFeatureScale;
        }
        if (++s_stride_step < kStride) {
            continue;   /* model wants kStride fresh frames per Invoke */
        }
        s_stride_step = 0;

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
    }
}

}  // namespace

extern "C" void wake_word_init(void)
{
    /* The microfrontend is initialised from main() before the scheduler
       starts (its state tables come from the newlib heap). Without it the
       task has nothing to feed the model. */
    if (micro_features_init_status != 1) {
        wake_word_init_status = -8;
        return;
    }

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

    /* Smoke-check: confirm input geometry matches our kStride * 40
       staged feature buffer. */
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

    /* Hygiene only — all kStride slots are refilled with fresh frames
       before every Invoke, so no zero-padding is ever fed to the model. */
    memset(s_feature_stack, 0, sizeof(s_feature_stack));

    /* Init succeeded. wake_word_init_status will tick up to mirror
       wake_word_total_inferences as soon as the task starts producing. */

    /* 512 words = 2 KB stack. TFLM's working memory lives in the SDRAM arena
       (s_arena), not on the task stack; Invoke's stack usage is dominated by
       a handful of conv kernel locals plus the recursive interpreter
       dispatch. 2 KB has measured fine; heap_4 is too tight to budget more. */
    BaseType_t ok = xTaskCreate(wake_word_task, "WakeWord", 512, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        wake_word_init_status = -7;
    }
}
