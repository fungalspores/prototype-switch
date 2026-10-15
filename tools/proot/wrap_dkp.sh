R=/opt/dka64-rootfs
LD=$R/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
RP=$R/lib/aarch64-linux-gnu:$R/usr/lib/aarch64-linux-gnu
D=$R/opt/devkitpro/devkitA64/libexec/gcc/aarch64-none-elf/15.2.0
for n in cc1 cc1plus cc1obj; do
  cp "${ORIG_DIR:-/tmp}/orig_$n" $D/$n.real && chmod 755 $D/$n.real
  printf '#!/bin/sh\nexec %s --library-path %s "$0.real" "$@"\n' "$LD" "$RP" > $D/$n
  chmod 755 $D/$n
done
# other big libexec binaries (lto1, collect2, lto-wrapper): check they still run
for f in lto1 collect2 lto-wrapper; do [ -f $D/$f ] && { $D/$f --version >/dev/null 2>&1; echo "$f rc=$?"; }; done
