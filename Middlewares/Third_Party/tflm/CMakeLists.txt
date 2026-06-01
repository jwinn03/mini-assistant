# TFLite Micro — built from the source subset extracted by create_tflm_tree.py
# with --makefile_options="TARGET=cortex_m_generic TARGET_ARCH=cortex-m7+fp
# OPTIMIZED_KERNEL_DIR=cmsis_nn" (see Phase 6 plan in
# C:\Users\Awesome Server\.claude\plans\regarding-your-last-question-binary-acorn.md).
#
# Exposes a STATIC library target named `tflm` that the top-level CMakeLists
# links via target_link_libraries.
#
# CMSIS-NN: TFLM bundled a pinned copy under third_party/cmsis_nn/Source but we
# already build the upstream main-branch CMSIS-NN as a separate static library
# `cmsis-nn` (see top-level CMakeLists.txt). We compile TFLM's CMSIS-NN kernel
# adapters (tensorflow/lite/micro/kernels/cmsis_nn/*.cc) against our standalone
# library's headers — the public APIs match between the pinned and latest
# revisions. If TFLM updates its bundled CMSIS-NN to use new APIs and we hit a
# mismatch, the fix is to pin our standalone CMSIS-NN to the same commit
# TFLM uses (currently 6d9d61d8 per third_party_downloads).

cmake_minimum_required(VERSION 3.15)

add_library(tflm STATIC)

# Glob all TFLM core sources under tensorflow/. The extraction script already
# trimmed this to what cortex_m_generic+cmsis_nn needs, so a broad glob is safe.
file(GLOB_RECURSE TFLM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/tensorflow/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tensorflow/*.c"
)

# Drop sources we don't want in the embedded build:
#   - *_test.cc          : gtest-based unit tests
#   - fake_micro_context : test scaffolding (mocks)
#   - examples/          : example apps and models — pulled in separately for
#                          the step-3 canary, not the TFLM library proper.
list(FILTER TFLM_SOURCES EXCLUDE REGEX "_test\\.cc$")
list(FILTER TFLM_SOURCES EXCLUDE REGEX "/fake_micro_context\\.cc$")
list(FILTER TFLM_SOURCES EXCLUDE REGEX "/examples/")
# cortex_m_generic/micro_time.cc references CMSIS-Core symbols (DCB,
# DCB_DEMCR_TRCENA_Msk) that exist only in newer CMSIS revisions. Our CMSIS-Core
# tree still uses the older `CoreDebug` name for the same block. Rather than
# alias-define the new names, we exclude TFLM's version and provide
# `tflite::GetCurrentTimeTicks()` / `ticks_per_second()` in
# Core/Src/tflm_micro_time.cc using the DWT setup that dsp_init() already runs.
list(FILTER TFLM_SOURCES EXCLUDE REGEX "/cortex_m_generic/micro_time\\.cc$")

target_sources(tflm PRIVATE ${TFLM_SOURCES})

# Public include directories. Consumers of `tflm` get all of these for free.
# The two Drivers/ paths are needed because TFLM's cortex_m_generic target
# (micro_time.cc) reaches for DWT / DCB from core_cm7.h, which lives under
# our existing CMSIS-Core tree alongside the STM32F7xx device headers.
target_include_directories(tflm PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/flatbuffers/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmlowp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ruy"
    "${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Include"
    "${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F7xx/Include"
    # TFLM's cmsis_nn kernel adapters in kernels/cmsis_nn/*.cc do
    # `#include "Include/arm_nnfunctions.h"` — i.e. they expect the CMSIS-NN
    # root, not its Include/ subdir. The cmsis-nn target propagates Include/
    # already (which works for `<arm_nnfunctions.h>` style), so we add the
    # root here for the TFLM-style include. Single CMSIS-NN tree, two paths.
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/CMSIS-NN"
)

# TFLM-required compile defines.
#   TF_LITE_STATIC_MEMORY    : no malloc/free in the runtime
#   TF_LITE_DISABLE_X86_NEON : forbid x86-only SIMD intrinsics
#   CMSIS_NN                 : enables the cmsis_nn kernel adapters
#   STM32F746xx              : selects the right device variant inside stm32f7xx.h
#   ARMCM7                   : enables the DWT->LAR unlock branch in micro_time.cc
#   CMSIS_DEVICE_..._FILE    : tells TFLM's cortex_m_generic/micro_time.cc which
#                              CMSIS device header to include for DWT/DCB access.
#                              The Makefile would normally set this; we don't use it.
target_compile_definitions(tflm PUBLIC
    TF_LITE_STATIC_MEMORY
    TF_LITE_DISABLE_X86_NEON
    CMSIS_NN
    STM32F746xx
    ARMCM7
    CMSIS_DEVICE_ARM_CORTEX_M_XX_HEADER_FILE=<stm32f7xx.h>
)

# TFLM is noisy. Suppress only what's actually emitted; do not blanket-disable.
target_compile_options(tflm PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-strict-aliasing
    -Wno-type-limits
)

# tflm depends on cmsis-nn for header resolution in the optimized kernel adapters.
target_link_libraries(tflm PUBLIC cmsis-nn)
