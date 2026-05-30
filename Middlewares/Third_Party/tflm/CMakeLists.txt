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

target_sources(tflm PRIVATE ${TFLM_SOURCES})

# Public include directories. Consumers of `tflm` get all of these for free.
target_include_directories(tflm PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/flatbuffers/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmlowp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ruy"
)

# TFLM-required compile defines.
#   TF_LITE_STATIC_MEMORY    : no malloc/free in the runtime
#   TF_LITE_DISABLE_X86_NEON : forbid x86-only SIMD intrinsics
#   CMSIS_NN                 : enables the cmsis_nn kernel adapters
target_compile_definitions(tflm PUBLIC
    TF_LITE_STATIC_MEMORY
    TF_LITE_DISABLE_X86_NEON
    CMSIS_NN
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
