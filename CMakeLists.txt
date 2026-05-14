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

# Enable CMake support for ASM and C languages
enable_language(C ASM)

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
    Core/Src/sdram.c
    Core/Src/lcd.c
    Core/Src/i2c3_bus.c
    Core/Src/ft5336.c
    Core/Src/ui.c
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
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,--undefined=dsp_set_gain
)
