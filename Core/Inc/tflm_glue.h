#ifndef TFLM_GLUE_H
#define TFLM_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 6 step 1: C++-link canary. Calling this from C confirms the toolchain
   accepts a .cc translation unit and that the C++ runtime (operator new/delete,
   __cxa_*) links cleanly under --specs=nano.specs. Once the canary passes,
   this file will grow to expose the TFLite Micro interpreter API to C. */
void tflm_glue_nop(void);

#ifdef __cplusplus
}
#endif

#endif /* TFLM_GLUE_H */
