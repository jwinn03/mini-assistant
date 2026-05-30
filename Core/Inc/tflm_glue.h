#ifndef TFLM_GLUE_H
#define TFLM_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 6 step 1 canary: empty C++ TU. Calling this from C confirmed the
   toolchain accepts a .cc unit and the C++ ABI links under --specs=nano.specs.
   Retained because it costs nothing and is still useful as a smoke test. */
void tflm_glue_nop(void);

/* Phase 6 step 3 canary: instantiate the TFLM MicroInterpreter, load the
   hello_world float model, run one inference with input = 1.0f, check the
   output against sin(1.0).

   Returns 0 on success, negative on failure (see tflm_glue.cc for codes). The
   global `tflm_canary_result` mirrors the return value so it can be inspected
   in a debugger after main() runs. */
int tflm_glue_canary(void);
extern volatile int tflm_canary_result;

#ifdef __cplusplus
}
#endif

#endif /* TFLM_GLUE_H */
