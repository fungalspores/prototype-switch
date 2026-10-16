# prototype2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(prototype2 VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(PROTOTYPE2_SOURCES
    src/main.cpp
)
if(WIN32)
    # Windows UCRT lacks C23 roundevenf (emitted by Clang for PPC rounding). Same shim as Mercs2.
    list(APPEND PROTOTYPE2_SOURCES src/math_compat_windows.cpp src/crash_report_windows.cpp)
endif()

if(WIN32)
    add_executable(prototype2 WIN32 ${PROTOTYPE2_SOURCES})
else()
    add_executable(prototype2 ${PROTOTYPE2_SOURCES})
endif()

rexglue_setup_target(prototype2)

if(WIN32)
    # Map file lets tools/symbolize_crash.py turn crash_report.txt RVAs into sub_XXXXXXXX names.
    target_link_options(prototype2 PRIVATE "LINKER:/MAP:${CMAKE_BINARY_DIR}/prototype2.map")
endif()
