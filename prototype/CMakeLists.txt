# Prototype (Xbox 360, 4156084E) for Nintendo Switch.
# SDK: masseffect-nx a85b0b4 + penettrator/prototype2-nx SDK patch (Horizon layer by StevensND),
# renderer: prototype2-nx native renderer (nfsmw-nx -> project8-sw -> Mercs2-NX -> Prototype 2).
# GPL-3.0-or-later (the native renderer is GPL-3.0).
cmake_minimum_required(VERSION 3.25)
project(prototype VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "" CACHE PATH "ReXGlue SDK source (masseffect-nx Horizon fork + P2 patch)")
if(NOT REXSDK_DIR)
    message(FATAL_ERROR "Set REXSDK_DIR")
endif()
# The presenter's FSR 1 path; gates members of rex/ui/presenter.h, so it is set for every target.
add_compile_definitions(REX_HAS_FIDELITYFX_SDK=1)
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
# Release: basic FPS integration only (from Prototype 2 NX switch/release_perf.cpp), no SDK profiler polling the
# clock and temperature services (it coincided with Horizon OC's hoc:clk crashing, see ХОД_РАБОТЫ.md).
option(PROTO_RELEASE "Release build without the SDK profiler" ON)
if(PROTO_RELEASE)
    set_source_files_properties("${REXSDK_DIR}/src/ui/switch_perf.cpp" DIRECTORY "${REXSDK_DIR}"
        PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(rexglue_switch_startup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/switch_release_perf.cpp")
    set_source_files_properties("${REXSDK_DIR}/src/ui/vulkan/vulkan_presenter.cpp" DIRECTORY "${REXSDK_DIR}/src/ui"
        PROPERTIES COMPILE_DEFINITIONS "REX_SWITCH_RELEASE_DIAGNOSTICS=1")
endif()

set(GEN "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
if(NOT EXISTS "${GEN}/sources.cmake")
    message(FATAL_ERROR "Run rexglue codegen prototype_manifest.toml first")
endif()
include("${GEN}/sources.cmake")
set(PROTO_GENERATED "")
foreach(src ${GENERATED_SOURCES})
    if(IS_ABSOLUTE "${src}")
        list(APPEND PROTO_GENERATED "${src}")
    else()
        list(APPEND PROTO_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
    endif()
endforeach()
add_library(prototype_recomp OBJECT ${PROTO_GENERATED})
target_include_directories(prototype_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GEN}")
target_link_libraries(prototype_recomp PRIVATE rex::runtime)
rexglue_apply_target_settings(prototype_recomp)
target_precompile_headers(prototype_recomp PRIVATE "${GEN}/prototype_pch.h")
target_compile_options(prototype_recomp PRIVATE -fno-tree-slp-vectorize -fno-tree-vectorize)

file(GLOB PROTO_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/native/*.cpp")
add_executable(prototype ${PROTO_APP_SOURCES})
target_include_directories(prototype PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GEN}"
    "${REXSDK_DIR}/src/core" "${REXSDK_DIR}/src/graphics" "${REXSDK_DIR}/thirdparty/glslang")
target_link_libraries(prototype PRIVATE prototype_recomp rex::runtime)
rexglue_configure_target(prototype GPU_PLUGINS xenos)

set_property(TARGET prototype APPEND PROPERTY LINK_DEPENDS "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a")
set(PROTO_SWITCH_HEAP_MB 2048 CACHE STRING "Capped Switch heap size in MiB")
set_source_files_properties("${REXSDK_DIR}/src/ui/runtime_switch.cpp" DIRECTORY "${REXSDK_DIR}"
    PROPERTIES COMPILE_DEFINITIONS "REX_SWITCH_HEAP_MB=${PROTO_SWITCH_HEAP_MB}")
set(PROTO_SWITCH_HOST_STACK_MB 8 CACHE STRING "Host stack budget per Xbox thread in MiB")
set_source_files_properties("${REXSDK_DIR}/src/system/xthread.cpp" DIRECTORY "${REXSDK_DIR}/src/system"
    PROPERTIES COMPILE_DEFINITIONS "REX_SWITCH_HOST_STACK_MB=${PROTO_SWITCH_HOST_STACK_MB}")
set_source_files_properties("${GEN}/prototype_register.cpp"
    PROPERTIES COMPILE_OPTIONS "-O0" SKIP_PRECOMPILE_HEADERS ON)
foreach(t prototype prototype_recomp)
    target_compile_options(${t} PRIVATE -ffp-contract=off -fno-strict-aliasing)
endforeach()
rexglue_switch_add_nro(prototype NAME "Prototype" AUTHOR "fungalspores" VERSION "1.1.0" ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
target_link_options(prototype PRIVATE "LINKER:--wrap=bqQueueBuffer")

# Profile-guided optimization (after Prototype 2 NX switch/CMakeLists.txt, WORKLOG #126/#127).
# GEN: instrumented build, the game writes /pgo-proto/*.gcda on the SD card (src/proto_pgo.cpp); USE: optimized
# with them. Both stages must use the same build directory (-fprofile-prefix-path strips it from the file names).
set(PROTO_PGO "OFF" CACHE STRING "PGO stage: OFF, GEN or USE")
set(PROTO_PGO_DATA "${CMAKE_CURRENT_SOURCE_DIR}/pgo-data" CACHE STRING "Directory with the .gcda files for PROTO_PGO=USE")
if(PROTO_PGO STREQUAL "GEN")
    # Indirect-call value profiling uses a thread-local libgcov variable that is null on guest threads: arcs only.
    foreach(t prototype_recomp prototype)
        target_compile_options(${t} PRIVATE -fprofile-generate=/pgo-proto -fno-profile-values -fprofile-update=single
          "-fprofile-prefix-path=${CMAKE_BINARY_DIR}")
    endforeach()
    target_link_options(prototype PRIVATE -fprofile-generate=/pgo-proto)
    target_compile_definitions(prototype PRIVATE PROTO_PGO_GEN=1)
elseif(PROTO_PGO STREQUAL "USE")
    foreach(t prototype_recomp prototype)
        target_compile_options(${t} PRIVATE "-fprofile-use=${PROTO_PGO_DATA}" -fprofile-partial-training -fprofile-correction
          "-fprofile-prefix-path=${CMAKE_BINARY_DIR}" -Wno-missing-profile -Wno-coverage-mismatch)
    endforeach()
    target_link_options(prototype PRIVATE "-fprofile-use=${PROTO_PGO_DATA}" -fprofile-partial-training)
endif()
