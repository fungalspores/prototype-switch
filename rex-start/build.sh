#!/bin/sh
# Builds rex-start.nro, the settings screen shown before Prototype and Prototype 2.
# Needs devkitA64 + libnx + FreeType/HarfBuzz/libpng from devkitPro portlibs (switch).
# Output: rex-start.nro next to this script. Copy it to sdmc:/switch/rex-start.nro.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
PL="$DEVKITPRO/portlibs/switch"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
CF="$ARCH -O2 -ffunction-sections -fdata-sections -I$DEVKITPRO/libnx/include -I$PL/include -I$PL/include/freetype2 -I$HERE/source -Wno-unused-variable"
OBJ="$(mktemp -d)"
cd "$HERE"
for f in source/main.c source/draw.c; do
    aarch64-none-elf-gcc $CF -c "$f" -o "$OBJ/$(basename "$f" .c).o"
done
aarch64-none-elf-gcc -specs="$DEVKITPRO/libnx/switch.specs" $ARCH -o rex-start.elf \
    "$OBJ/main.o" "$OBJ/draw.o" -L"$PL/lib" -L"$DEVKITPRO/libnx/lib" \
    -lfreetype -lharfbuzz -lpng16 -lbz2 -lz -lnx -lm
elf2nro rex-start.elf rex-start.nro --nacp=rex-start.nacp --icon=icon.jpg
rm -rf "$OBJ"
echo "built $HERE/rex-start.nro"
