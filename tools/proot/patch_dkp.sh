R=/opt/dka64-rootfs
LD=$R/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
RP=$R/lib/aarch64-linux-gnu:$R/usr/lib/aarch64-linux-gnu
n=0
for f in $(rg --files -uuu $R/opt/devkitpro/devkitA64/bin $R/opt/devkitpro/devkitA64/libexec $R/opt/devkitpro/devkitA64/aarch64-none-elf/bin $R/opt/devkitpro/tools/bin 2>/dev/null); do
  [ -L "$f" ] && continue
  head -c 4 "$f" | rg -q 'ELF' || continue
  patchelf --print-interpreter "$f" >/dev/null 2>&1 || continue
  patchelf --set-interpreter $LD --set-rpath $RP "$f" 2>/dev/null && n=$((n+1))
done
echo "patched $n"
