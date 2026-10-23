#!/bin/sh
# Prototype (Xbox 360, 4156084E) -> Switch NRO. Run from this folder.
#   REXGLUE=<host rexglue from the patched SDK>  REXSDK_DIR=<patched SDK>  NVK_SDK=<folder with lib/libvulkan.a>
#   game/ = your extracted disc (default.xex inside).
set -e
cd "$(dirname "$0")"
: "${REXGLUE:?}" "${REXSDK_DIR:?}" "${NVK_SDK:?}"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
[ -f generated/default/sources.cmake ] || "$REXGLUE" -f codegen prototype_manifest.toml
python3 tools/patch_generated.py generated/default
python3 tools/prepare_proto_codegen.py generated/default
python3 tools/prepare_no_volatile.py generated/default
# Speed patches from mgr-nx-deko (tuancapoo, GPL-3.0): guest CAS without the trailing dmb, vector byte
# reversal without a table lookup, constant shuffles as one permutation.
python3 ../tools/mgr/guest_cas.py --gen generated/default
python3 ../tools/mgr/vec_byteswap.py --gen generated/default
python3 ../tools/mgr/pch_neon_shuffles.py generated/default/prototype_pch.h
cmake -S . -B build/switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/switch-devkitA64.cmake" \
  -DREXSDK_DIR="$REXSDK_DIR" -DREXGLUE_SWITCH_NVK_SDK="$NVK_SDK"
cmake --build build/switch -j"${JOBS:-6}"
ls -la build/switch/prototype.nro
