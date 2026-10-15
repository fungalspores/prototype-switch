set -euo pipefail
echo "Mesa build stage 1/9"

mkdir -p /usr/local/libexec
cp /project/bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp /project/rustc-switch-wrapper.sh /usr/local/libexec/rustc
# bindgen-switch-wrapper.sh resolves the shim as $SCRIPT_DIR/bindgen-atomic-shim.h,
# so it has to live next to the installed wrapper.
cp /project/bindgen-atomic-shim.h /usr/local/libexec/bindgen-atomic-shim.h
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc

echo "Mesa build stage 2/9"

MESON_BIN="$(command -v meson || true)"
if [ -z "$MESON_BIN" ]; then
    echo "ERROR: meson not found in container PATH" >&2
    exit 1
fi
mkdir -p /usr/local/bin
if [ "$MESON_BIN" != "/usr/local/bin/meson" ]; then
    ln -sf "$MESON_BIN" /usr/local/bin/meson
fi

echo "Mesa build stage 3/9"

cp /project/src/gallium/winsys/nouveau/drm/nouveau.h \
    /opt/devkitpro/portlibs/switch/include/
cp /project/src/nouveau/headers/nv_device_info.h \
    /opt/devkitpro/portlibs/switch/include/

echo "Mesa build stage 4/9"

if [ ! -d /usr/lib/llvm-15/lib/clang/15/include ]; then
    ln -sf /usr/lib/llvm-15/lib/clang/15.0.6/include /usr/lib/llvm-15/lib/clang/15/include
fi

echo "Mesa build stage 5/9"

if [ ! -f /opt/devkitpro/portlibs/switch/lib/libdl.a ]; then
    cd /tmp
    echo "void __mesa_switch_stub_lib(void) {}" > stub_posix.c
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -c stub_posix.c -o stub_posix.o
    for L in dl rt util; do
        /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs \
            /opt/devkitpro/portlibs/switch/lib/lib${L}.a stub_posix.o
    done
fi

echo "Mesa build stage 6/9"

cd /project
native_setup=()
[ ! -f builddir-native/meson-private/coredata.dat ] || native_setup=(--reconfigure)
meson setup builddir-native "${native_setup[@]}" \
    --native-file /work/tools/switch/mesa-native.ini \
    -Dvulkan-drivers= \
    -Dgallium-drivers= \
    -Dshader-cache=true \
    -Dplatforms= \
    -Dglx=disabled \
    -Degl=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dtools=[] \
    -Dllvm=enabled \
    -Dshared-llvm=enabled \
    -Dmesa-clc=enabled \
    -Dprecomp-compiler=enabled \
    -Dinstall-mesa-clc=true

echo "Mesa build stage 7/9"
ninja -j4 -C /project/builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
echo "Mesa build stage 8/9"
# The cross wrapper requires an explicit AArch64 Rust target on Linux.
rustup target add aarch64-unknown-linux-gnu
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu

export PATH="/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"
cd /project
cross_setup=()
[ ! -f builddir-switch/meson-private/coredata.dat ] || cross_setup=(--reconfigure)
# Without dl_iterate_phdr NVK keys the on-disk shader cache on "nvk-<version>"
# alone, so binaries compiled by an older NAK on the SD card would be reused
# after compiler changes. Key it on the compiler/driver sources instead.
NVK_BUILD_ID="$(find src/nouveau/compiler src/nouveau/vulkan src/nouveau/nil     src/compiler/nir src/compiler/spirv -type f     \( -name '*.c' -o -name '*.h' -o -name '*.rs' -o -name '*.py' \)     -print0 | LC_ALL=C sort -z | xargs -0 cat | sha1sum | cut -c1-40)"
echo "NVK build id (shader cache key): $NVK_BUILD_ID"
meson setup builddir-switch "${cross_setup[@]}" \
    --cross-file switch_cross_file.txt \
    --native-file /work/tools/switch/mesa-native.ini \
    --buildtype=release \
    -Doptimization=1 \
    -Db_lto=false \
    -Db_ndebug=true \
    -Dvulkan-drivers=nouveau \
    -Dgallium-drivers=nouveau \
    -Dshader-cache=true \
    -Dgallium-rusticl=false \
    -Dplatforms=switch \
    -Dglx=disabled \
    -Degl=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dllvm=disabled \
    -Dshared-glapi=disabled \
    -Dshared-llvm=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dcpp_rtti=false     -Dnvk-build-id="$NVK_BUILD_ID"

echo "Mesa build stage 9/9"

export PATH="/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"
ninja -j4 -C /project/builddir-switch \
    src/nouveau/vulkan/libnvk.a \
    src/nouveau/vulkan/libvulkan.a \
    src/vulkan/util/libvulkan_util.a \
    src/compiler/nir/libnir.a \
    src/compiler/libcompiler.a \
    src/compiler/spirv/libvtn.a \
    src/util/libxmlconfig.a \
    src/nouveau/compiler/libnak.a \
    src/nouveau/compiler/libnak_rs.a \
    src/nouveau/nil/libnil.a \
    src/nouveau/nil/liblibnil_format_table.a \
    src/nouveau/mme/libnouveau_mme.a \
    src/nouveau/winsys/libnouveau_ws.a \
    src/nouveau/headers/libnvidia_headers_c.a \
    src/compiler/rust/libcompiler_c_helpers.a \
    src/util/libmesa_util.a \
    src/util/libmesa_util_simd.a \
    src/util/blake3/libblake3.a \
    src/c11/impl/libmesa_util_c11.a


# Use the AArch64 archiver and include the complete Rust staticlib runtime.
# The host ar used by Mesa does not reliably index the bundled Rust objects.
echo "Merging the complete NVK and Rust runtime archives"
mkdir -p /work/out/mesa-sdk/opt/devkitpro/portlibs/switch/lib
cd /project/builddir-switch
printf "CREATE /work/out/mesa-sdk/opt/devkitpro/portlibs/switch/lib/libvulkan.a\nADDLIB src/nouveau/vulkan/libvulkan.a\nADDLIB src/nouveau/compiler/libnak_rs.a\nSAVE\nEND\n" | /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar -M
echo "Mesa Switch archive ready"
