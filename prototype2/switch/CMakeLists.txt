# Prototype 2 - Nintendo Switch build (ReXGlue Horizon fork SDK + static NVK).
# Generated code and app sources live in ../project (shared with the Windows build); codegen is a
# separate host step (tools/codegen.ps1) because a cross-built rexglue cannot run here.
# Scheme follows Mercenaries2-NX app/CMakeLists.txt (see X360_TO_SWITCH_PORTING_GUIDE.md stage 4).
cmake_minimum_required(VERSION 3.25)
project(prototype2 VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(P2_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/../project")
set(REXSDK_DIR "" CACHE PATH "ReXGlue SDK source (Horizon fork)")
if(NOT REXSDK_DIR)
    message(FATAL_ERROR "Set REXSDK_DIR to the patched Horizon-fork SDK")
endif()
# WORKLOG #107: the presenter's FSR 1 (EASU + RCAS) and CAS paths only need their SPIR-V, which the SDK ships
# built; the macro also gates members of rex/ui/presenter.h, so it is set for every target (one class layout).
add_compile_definitions(REX_HAS_FIDELITYFX_SDK=1)
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
option(P2_RELEASE_DIAGNOSTICS_OFF "Use basic FPS integration without SDK profiling" ON)
if(P2_RELEASE_DIAGNOSTICS_OFF)
    set_source_files_properties("${REXSDK_DIR}/src/ui/switch_perf.cpp" DIRECTORY "${REXSDK_DIR}"
        PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(rexglue_switch_startup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/release_perf.cpp")
    set_source_files_properties("${REXSDK_DIR}/src/ui/vulkan/vulkan_presenter.cpp" DIRECTORY "${REXSDK_DIR}/src/ui"
        PROPERTIES COMPILE_DEFINITIONS "REX_SWITCH_RELEASE_DIAGNOSTICS=1")
endif()

if(NOT EXISTS "${P2_PROJECT}/generated/default/sources.cmake")
    message(FATAL_ERROR "Run tools/codegen.ps1 first")
endif()
# sources.cmake lists paths relative to the project directory.
set(_saved_dir "${CMAKE_CURRENT_SOURCE_DIR}")
include("${P2_PROJECT}/generated/default/sources.cmake")
set(P2_GENERATED "")
foreach(src ${GENERATED_SOURCES})
    if(IS_ABSOLUTE "${src}")
        list(APPEND P2_GENERATED "${src}")
    else()
        list(APPEND P2_GENERATED "${P2_PROJECT}/${src}")
    endif()
endforeach()
add_library(prototype2_recomp OBJECT ${P2_GENERATED})
target_include_directories(prototype2_recomp PRIVATE "${P2_PROJECT}" "${P2_PROJECT}/src" "${P2_PROJECT}/generated/default" "${CMAKE_CURRENT_SOURCE_DIR}/../build/switch-hot")
target_link_libraries(prototype2_recomp PRIVATE rex::runtime)
rexglue_apply_target_settings(prototype2_recomp)
target_precompile_headers(prototype2_recomp PRIVATE "${P2_PROJECT}/generated/default/prototype2_pch.h")

set(P2_APP_SOURCES
    "${P2_PROJECT}/src/main.cpp"
    "${P2_PROJECT}/src/switch_screenshot.cpp")
file(GLOB P2_SWITCH_EXTRA CONFIGURE_DEPENDS "${P2_PROJECT}/src/switch_*.cpp" "${P2_PROJECT}/src/native/*.cpp")
list(APPEND P2_APP_SOURCES ${P2_SWITCH_EXTRA})
list(REMOVE_DUPLICATES P2_APP_SOURCES)
add_executable(prototype2 ${P2_APP_SOURCES})
target_include_directories(prototype2 PRIVATE "${P2_PROJECT}" "${P2_PROJECT}/src" "${P2_PROJECT}/generated/default" "${CMAKE_CURRENT_SOURCE_DIR}/../build/switch-hot"
    "${REXSDK_DIR}/src/core" "${REXSDK_DIR}/src/graphics" "${REXSDK_DIR}/thirdparty/glslang")
target_link_libraries(prototype2 PRIVATE prototype2_recomp rex::runtime)
rexglue_configure_target(prototype2 GPU_PLUGINS xenos)

set_property(TARGET prototype2 APPEND PROPERTY LINK_DEPENDS "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a")
set(P2_SWITCH_HEAP_MB 2048 CACHE STRING "Capped Switch heap size in MiB")
set_source_files_properties("${REXSDK_DIR}/src/ui/runtime_switch.cpp" DIRECTORY "${REXSDK_DIR}"
    PROPERTIES COMPILE_DEFINITIONS "REX_SWITCH_HEAP_MB=${P2_SWITCH_HEAP_MB}")
set(P2_SWITCH_HOST_STACK_MB 8 CACHE STRING "Host stack budget per Xbox thread in MiB")
set_source_files_properties("${REXSDK_DIR}/src/system/xthread.cpp" DIRECTORY "${REXSDK_DIR}/src/system"
    PROPERTIES COMPILE_DEFINITIONS "REX_SWITCH_HOST_STACK_MB=${P2_SWITCH_HOST_STACK_MB}")
# Tens of thousands of registrations; -O3 takes many minutes and it runs once.
set_source_files_properties("${P2_PROJECT}/generated/default/prototype2_register.cpp"
    PROPERTIES COMPILE_OPTIONS "-O0" SKIP_PRECOMPILE_HEADERS ON)
foreach(t prototype2 prototype2_recomp)
    target_compile_options(${t} PRIVATE -ffp-contract=off -fno-strict-aliasing)
endforeach()
rexglue_switch_add_nro(prototype2 NAME "Prototype 2" AUTHOR "penettrator, fungalspores" VERSION "1.1.0" ICON "${CMAKE_CURRENT_SOURCE_DIR}/../extras/icon_fs.jpg")

option(P2_GUEST_STACK_AUDIT "Instrument generated function returns (diagnostic overhead)" OFF)
foreach(t prototype2 prototype2_recomp)
    target_compile_definitions(${t} PRIVATE P2_GUEST_STACK_AUDIT=$<BOOL:${P2_GUEST_STACK_AUDIT}>)
endforeach()

option(P2_TASK_LIFETIME_AUDIT "Record targeted task lifecycle events (diagnostic)" OFF)
foreach(t prototype2 prototype2_recomp)
    target_compile_definitions(${t} PRIVATE P2_TASK_LIFETIME_AUDIT=$<BOOL:${P2_TASK_LIFETIME_AUDIT}>)
endforeach()

option(P2_DIRECT_CALLS "Allow inlining of unhooked generated calls" OFF)
foreach(t prototype2 prototype2_recomp)
    target_compile_definitions(${t} PRIVATE P2_DIRECT_CALLS=$<BOOL:${P2_DIRECT_CALLS}>)
endforeach()
if(P2_DIRECT_CALLS)
    set_property(SOURCE "${P2_PROJECT}/generated/default/prototype2_recomp.0.cpp" APPEND PROPERTY
      COMPILE_OPTIONS "-fopt-info-inline-optimized=${CMAKE_BINARY_DIR}/direct-call-inlining.0.log")
endif()

# First LTO stage is limited to guest code. Native renderer/xxHash/SDK keep their
# validated compilation settings; do not couple driver/cache changes to this test.
option(P2_GUEST_LTO "Optimize generated guest code across translation units" OFF)
if(P2_GUEST_LTO)
    if(NOT P2_DIRECT_CALLS)
        message(FATAL_ERROR "Guest LTO experiment requires protected direct calls")
    endif()
    target_compile_options(prototype2_recomp PRIVATE -flto -fno-fat-lto-objects -frounding-math)
    target_link_options(prototype2 PRIVATE -flto=2 -flto-partition=balanced -frounding-math
      -ffp-contract=off -fno-strict-aliasing)
endif()

option(P2_LEAF_LOCALS "Cache complete register images in callback-free guest leaf functions" OFF)
option(P2_ARM_VMX "Lower integer VMX permutes and partial stores directly to ARM64" OFF)
foreach(t prototype2 prototype2_recomp)
 target_compile_definitions(${t} PRIVATE P2_LEAF_LOCALS=$<BOOL:${P2_LEAF_LOCALS}> P2_ARM_VMX=$<BOOL:${P2_ARM_VMX}>)
endforeach()
if(P2_LEAF_LOCALS AND P2_GUEST_STACK_AUDIT)
 message(FATAL_ERROR "Leaf register cache requires context-reading stack diagnostics off")
endif()
# Diagnostic wrappers preserve all original NVK/WSI/libnx arguments and returns.
target_link_options(prototype2 PRIVATE "LINKER:--wrap=bqQueueBuffer")

# WORKLOG #126: profile-guided optimization of the generated guest code (prototype2_recomp only).
# GEN: instrumented build, the game writes /pgo/*.gcda on the SD card (switch_pgo.cpp); USE: optimized with them.
# Both stages must use the same build directory layout (-fprofile-prefix-path strips it from the file names).
set(P2_PGO "OFF" CACHE STRING "Application and guest code PGO stage: OFF, GEN or USE")
set(P2_PGO_DATA "${CMAKE_CURRENT_SOURCE_DIR}/../config/pgo-data" CACHE STRING "Directory with the .gcda files for P2_PGO=USE")
if(P2_PGO STREQUAL "GEN")
    # run97: the indirect-call value profiler uses a thread-local libgcov variable, null on guest threads (crash at
    # start): arc counts only.
    target_compile_options(prototype2_recomp PRIVATE -fprofile-generate=/pgo -fno-profile-values -fprofile-update=single
      "-fprofile-prefix-path=${CMAKE_BINARY_DIR}")
    target_link_options(prototype2 PRIVATE -fprofile-generate=/pgo)
    target_compile_definitions(prototype2 PRIVATE P2_PGO_GEN=1)
    # WORKLOG #127: the port's own code too (native renderer, skinning, culling: ring and sim thread work).
    target_compile_options(prototype2 PRIVATE -fprofile-generate=/pgo -fno-profile-values -fprofile-update=single
      "-fprofile-prefix-path=${CMAKE_BINARY_DIR}")
elseif(P2_PGO STREQUAL "USE")
    target_compile_options(prototype2_recomp PRIVATE "-fprofile-use=${P2_PGO_DATA}" -fprofile-partial-training -fprofile-correction
      "-fprofile-prefix-path=${CMAKE_BINARY_DIR}" -Wno-missing-profile -Wno-coverage-mismatch)
    target_link_options(prototype2 PRIVATE "-fprofile-use=${P2_PGO_DATA}" -fprofile-partial-training)
    target_compile_options(prototype2 PRIVATE "-fprofile-use=${P2_PGO_DATA}" -fprofile-partial-training
      -fprofile-correction "-fprofile-prefix-path=${CMAKE_BINARY_DIR}" -Wno-missing-profile -Wno-coverage-mismatch)
endif()
