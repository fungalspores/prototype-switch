#!/bin/sh
# Mesa for the Prototype 2 based SDK (penettrator patch), Alpine host, no Docker.
set -e
. $HOME/.cargo/env
export DEVKITPRO=/opt/devkitpro
P=${MESA_SRC:?set MESA_SRC to the patched mesa-switch checkout}
NATIVE=${MESA_NATIVE:-$P/builddir-native}
cd $P
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
export MESA_SWITCH_RUSTC=$HOME/.cargo/bin/rustc
export MESA_SWITCH_BINDGEN=/usr/local/bin/bindgen
cp bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp rustc-switch-wrapper.sh /usr/local/libexec/rustc
cp bindgen-atomic-shim.h /usr/local/libexec/
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
printf "[binaries]\nrust = '%s'\n" "$HOME/.cargo/bin/rustc" > $P/mesa-native.ini
PL=$DEVKITPRO/portlibs/switch
cp src/gallium/winsys/nouveau/drm/nouveau.h src/nouveau/headers/nv_device_info.h $PL/include/
export PATH="/usr/local/libexec:$NATIVE/src/compiler/clc:$NATIVE/src/compiler/spirv:$PATH"
NVK_BUILD_ID="$(find src/nouveau/compiler src/nouveau/vulkan src/nouveau/nil src/compiler/nir src/compiler/spirv -type f \( -name '*.c' -o -name '*.h' -o -name '*.rs' -o -name '*.py' \) -print0 | LC_ALL=C sort -z | xargs -0 cat | sha1sum | cut -c1-40)"
echo "NVK build id: $NVK_BUILD_ID"
meson setup builddir-switch --wipe --cross-file switch_cross_file.txt --native-file $P/mesa-native.ini \
  --buildtype=release -Doptimization=1 -Db_lto=false -Db_ndebug=true -Dvulkan-drivers=nouveau \
  -Dgallium-drivers=nouveau -Dshader-cache=true -Dgallium-rusticl=false -Dplatforms=switch -Dglx=disabled \
  -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled \
  -Dshared-llvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false -Dnvk-build-id="$NVK_BUILD_ID"
ninja -j6 -C builddir-switch src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libvulkan.a src/vulkan/util/libvulkan_util.a \
  src/compiler/nir/libnir.a src/compiler/libcompiler.a src/compiler/spirv/libvtn.a src/util/libxmlconfig.a \
  src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a src/nouveau/nil/libnil.a \
  src/nouveau/nil/liblibnil_format_table.a src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a \
  src/nouveau/headers/libnvidia_headers_c.a src/compiler/rust/libcompiler_c_helpers.a src/util/libmesa_util.a \
  src/util/libmesa_util_simd.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a || true
ls -la builddir-switch/src/nouveau/vulkan/libvulkan.a builddir-switch/src/nouveau/compiler/libnak_rs.a
OUT=${NVK_SDK:-$P/../nvk-sdk}/lib; mkdir -p $OUT; rm -f $OUT/libvulkan.a
cd builddir-switch
printf "CREATE $OUT/libvulkan.a\nADDLIB src/nouveau/vulkan/libvulkan.a\nADDLIB src/nouveau/compiler/libnak_rs.a\nSAVE\nEND\n" | $DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar -M
ls -la $OUT
echo "=== MESA P2 OK"
