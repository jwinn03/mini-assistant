/* Phase 6 step 1: C++-link canary.
   Empty translation unit that proves the toolchain can compile and link a .cc
   file under --specs=nano.specs + --gc-sections without the C++ runtime
   pulling in libc bloat. Subsequent steps will add the TFLite Micro
   interpreter instantiation here. */

#include "tflm_glue.h"

extern "C" void tflm_glue_nop(void)
{
    /* Intentionally empty. The act of being called from C is the test. */
}
