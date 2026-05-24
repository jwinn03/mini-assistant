cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# C++ is needed for the TFLite Micro runtime in Phase 6. Toolchain already sets
# -fno-rtti / -fno-exceptions / -fno-threadsafe-statics in gcc-arm-none-eabi.cmake,
# so the C++ blast radius stays small (tflm_glue.cc only).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME mini-assistant)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM, C, and C++ languages.
# C++ is enabled for the TFLite Micro integration in Phase 6 (see tflm_glue.cc).
enable_language(C CXX ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# CMSIS-DSP — vendored from ARM-software/CMSIS-DSP.
# Source/CMakeLists.txt builds a STATIC library target named CMSISDSP and
# propagates its Include/ path PUBLIClly. Cortex-M7 flags are inherited from
# the project toolchain (-mcpu / -mfpu / -mfloat-abi).
# CMSISCORE points CMSIS-DSP at our CMSIS-Core headers (cmsis_compiler.h, etc.)
# already vendored under Drivers/CMSIS/Include.
set(CMSISCORE "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS")
add_subdirectory(Middlewares/Third_Party/CMSIS-DSP/Source)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/wm8994.c
    Core/Src/audio.c
    Core/Src/dsp.c
    Core/Src/dsp_chain.c
    Core/Src/dsp_util.c
    Core/Src/effect_gain.c
    Core/Src/effect_clip.c
    Core/Src/effect_fir.c
    Core/Src/effect_eq.c
    Core/Src/effect_delay.c
    Core/Src/effect_chorus.c
    Core/Src/effect_reverb.c
    Core/Src/sdram.c
    Core/Src/lcd.c
    Core/Src/i2c3_bus.c
    Core/Src/ft5336.c
    Core/Src/ui.c
    Core/Src/wav.c
    Core/Src/sd_card.c
    Core/Src/recorder.c
    Core/Src/player.c
    Core/Src/ui_page_record.c
    Core/Src/tflm_glue.cc
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    ARM_MATH_CM7
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    CMSISDSP
)

# Force-keep symbols that exist only as debugger entry points.
# --gc-sections would otherwise discard them since no source file calls them.
# Single dispatcher dsp_set_param() anchors all per-effect setters by reference,
# so one --undefined entry covers every effect added in Phase 4.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,--undefined=dsp_set_param
)
