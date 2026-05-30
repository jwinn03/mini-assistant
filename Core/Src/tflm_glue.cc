/* Phase 6 step 3: TFLM canary.
   Wires up the MicroInterpreter against the hello_world float model and runs
   one inference. Proves the toolchain, the linker discipline under
   --specs=nano.specs + --gc-sections, and our integration of TFLM + CMSIS-NN
   all work end-to-end on hardware before we attempt the real wake-word path. */

#include "tflm_glue.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/examples/hello_world/models/hello_world_float_model_data.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

/* The hello_world model has a tiny computational graph (one FullyConnected op,
   single float in / float out). 3 KB of arena is overkill but matches the
   upstream example so behavior is comparable. */
constexpr int kArenaSize = 3000;
alignas(16) uint8_t s_arena[kArenaSize];

bool ApproxEq(float a, float b, float eps = 0.05f)
{
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < eps;
}

}  // namespace

/* Mirror of tflm_glue_canary()'s return so it's inspectable in a debugger
   without needing to break on the function. Initialized to a sentinel that
   means "canary never ran"; a clean run leaves 0. */
extern "C" volatile int tflm_canary_result = -100;

extern "C" int tflm_glue_canary(void)
{
    using OpResolver = tflite::MicroMutableOpResolver<1>;
    OpResolver op_resolver;
    if (op_resolver.AddFullyConnected() != kTfLiteOk) return -1;

    const tflite::Model* model =
        ::tflite::GetModel(g_hello_world_float_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) return -2;

    tflite::MicroInterpreter interpreter(
        model, op_resolver, s_arena, kArenaSize);

    if (interpreter.AllocateTensors() != kTfLiteOk) return -3;

    /* sin(1.0) ≈ 0.841471. The model is a sin() regression trained to within
       the 0.05 epsilon used by the upstream test. */
    interpreter.input(0)->data.f[0] = 1.0f;
    if (interpreter.Invoke() != kTfLiteOk) return -4;
    const float y_pred = interpreter.output(0)->data.f[0];
    if (!ApproxEq(y_pred, 0.841471f)) return -5;

    return 0;
}

extern "C" void tflm_glue_nop(void)
{
    /* Step-1 canary kept as a smoke test. tflm_glue_canary() is the step-3 one. */
}
