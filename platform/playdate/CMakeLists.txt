cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})

if (NOT ${ENVSDK} STREQUAL "")
    file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
    execute_process(
        COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
        COMMAND head -n 1
        COMMAND cut -c9-
        OUTPUT_VARIABLE SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

if (NOT EXISTS ${SDK})
    message(FATAL_ERROR "SDK Path not found; set ENV value PLAYDATE_SDK_PATH")
    return()
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(PLAYDATE_GAME_NAME PokeMini)
# Device build also produces PokeMini.pdx (no _DEVICE suffix). The two cmake
# branches (sim/device) are mutually exclusive per build dir, so reusing the
# same target name is fine.
set(PLAYDATE_GAME_DEVICE PokeMini)

project(${PLAYDATE_GAME_NAME} C ASM)

set(POKEMINI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(GAME_SOURCES
    PokeMini_Playdate.c
    ${POKEMINI_ROOT}/freebios/freebios.c
    ${POKEMINI_ROOT}/source/PMCommon.c
    ${POKEMINI_ROOT}/source/PokeMini.c
    ${POKEMINI_ROOT}/source/Multicart.c
    ${POKEMINI_ROOT}/source/Hardware.c
    ${POKEMINI_ROOT}/source/Video.c
    ${POKEMINI_ROOT}/source/Video_x1.c
    ${POKEMINI_ROOT}/source/CommandLine.c
    ${POKEMINI_ROOT}/source/MinxCPU.c
    ${POKEMINI_ROOT}/source/MinxCPU_XX.c
    ${POKEMINI_ROOT}/source/MinxCPU_CE.c
    ${POKEMINI_ROOT}/source/MinxCPU_CF.c
    ${POKEMINI_ROOT}/source/MinxCPU_SP.c
    ${POKEMINI_ROOT}/source/MinxTimers.c
    ${POKEMINI_ROOT}/source/MinxIO.c
    ${POKEMINI_ROOT}/source/MinxIRQ.c
    ${POKEMINI_ROOT}/source/MinxPRC.c
    ${POKEMINI_ROOT}/source/MinxColorPRC.c
    ${POKEMINI_ROOT}/source/MinxLCD.c
    ${POKEMINI_ROOT}/source/MinxAudio.c
    ${POKEMINI_ROOT}/source/UI.c
    ${POKEMINI_ROOT}/source/Joystick.c
    ${POKEMINI_ROOT}/resource/PokeMini_ColorPal.c
    ${POKEMINI_ROOT}/resource/PokeMini_Font12.c
    ${POKEMINI_ROOT}/resource/PokeMini_Icons12.c
)

if (TOOLCHAIN STREQUAL "armgcc")
    add_executable(${PLAYDATE_GAME_DEVICE} ${GAME_SOURCES})
else()
    add_library(${PLAYDATE_GAME_NAME} SHARED ${GAME_SOURCES})
endif()

include(${SDK}/C_API/buildsupport/playdate_game.cmake)

if (TOOLCHAIN STREQUAL "armgcc")
    set(TARGET_NAME ${PLAYDATE_GAME_DEVICE})
else()
    set(TARGET_NAME ${PLAYDATE_GAME_NAME})
endif()

target_include_directories(${TARGET_NAME} PRIVATE
    ${POKEMINI_ROOT}/source
    ${POKEMINI_ROOT}/resource
    ${POKEMINI_ROOT}/freebios
)

target_compile_definitions(${TARGET_NAME} PRIVATE
    NO_DIRS
    NO_ZIP
    PERFORMANCE
    NO_SCANDIRS
    SIMPLE_CONTROLS
)

if (TOOLCHAIN STREQUAL "armgcc")
    target_link_options(${TARGET_NAME} PRIVATE --specs=nosys.specs)
    # -Os over -O3: the M7 Rev A has only a 4 KB I-cache, so PokeMini's
    # 256-case MinxCPU_Exec dispatcher likely thrashes when compiled with
    # -O3's aggressive inlining. -Os trades inlining for code density and
    # may keep more of the hot loop resident. -flto still does cross-TU
    # inlining at link time. Suggested in the Playdate "dirty optimization
    # secrets" thread.
    # -falign-loops=32 aligns inner-loop entry points to 32-byte (cache-
    # line) boundaries on the M7.
    target_compile_options(${TARGET_NAME} PRIVATE -Os -flto -falign-loops=32)
    target_link_options(${TARGET_NAME} PRIVATE -Os -flto)
endif()
