#!/usr/bin/env bash
# Prototype 2 0.2.0; Windows Git Bash + Docker.
set -euo pipefail
cd "$(dirname "$0")/.."
python tools/prepare_switch_codegen.py
python tools/prepare_no_volatile.py
python tools/prepare_switch_hot.py
MSYS_NO_PATHCONV=1 docker run --rm --mount "type=bind,source=$(pwd -W),target=/work" -w /work devkitpro/devkita64:latest bash -lc "cmake -S switch -B build/switch -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/work/switch/cmake/switch-devkitA64.cmake -DREXSDK_DIR=/work/reference/masseffect-nx/sdk -DREXGLUE_SWITCH_NVK_SDK=/work/out/mesa-sdk/opt/devkitpro/portlibs/switch -DP2_PGO=${PGO:-USE} -DP2_PGO_DATA=/work/config/pgo-data -DP2_RELEASE_DIAGNOSTICS_OFF=ON -DP2_GUEST_STACK_AUDIT=OFF -DP2_TASK_LIFETIME_AUDIT=OFF -DP2_DIRECT_CALLS=ON -DP2_GUEST_LTO=ON -DP2_LEAF_LOCALS=ON -DP2_ARM_VMX=ON && cmake --build build/switch --target prototype2 -j ${JOBS:-8}"
