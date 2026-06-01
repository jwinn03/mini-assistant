/* Replacement for upstream tensorflow/lite/micro/cortex_m_generic/micro_time.cc.
   Excluded from the tflm static library glob (see
   Middlewares/Third_Party/tflm/CMakeLists.txt) because the upstream file
   references CMSIS-Core symbols (DCB, DCB_DEMCR_TRCENA_Msk) that exist only in
   newer CMSIS revisions than the project ships.

   DWT->CYCCNT is already initialized by dsp_init() before any TFLM code runs
   — see the LAR-unlock sequence noted in CLAUDE.md guidelines and implemented
   in Core/Src/dsp.c. No init logic is needed here. */

#include "tensorflow/lite/micro/micro_time.h"
#include "stm32f7xx.h"

namespace tflite {

uint32_t ticks_per_second(void)
{
    /* DWT->CYCCNT ticks once per core clock. */
    return SystemCoreClock;
}

uint32_t GetCurrentTimeTicks(void)
{
    return DWT->CYCCNT;
}

}  // namespace tflite
