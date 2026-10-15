"""Post-codegen fixes for ReXGlue output that the prebuilt SDK gets wrong.

dcbz: ReXGlue clears 32 bytes (the PowerPC short cache line), but the Xbox 360
has only 128-byte lines and dcbz always clears 128. Prototype zeroes buffers
with dcbz in 128-byte steps, so 3/4 of each block kept stale data; xenia-canary
72ce130 fixed the same bug, which made Alex fall through the map.

Idempotent; run after every codegen (CMakeLists appends it to the codegen step).

    python patch_generated.py generated/default
"""
import glob
import os
import re
import sys

DCBZ = re.compile(
    r"(// dcbz [^\n]*\n\s*ea = [^\n;]*& )~31;(\n\s*memset\(\(void\*\)REX_RAW_ADDR\(ea\), 0, )32\);")


def main():
    gen = sys.argv[1]
    total = 0
    for path in sorted(glob.glob(os.path.join(gen, "*_recomp.*.cpp"))):
        with open(path, newline="") as f:
            text = f.read()
        new, n = DCBZ.subn(r"\1~127;\g<2>128);", text)
        if n:
            with open(path, "w", newline="") as f:
                f.write(new)
            total += n
    print(f"patch_generated: dcbz -> 128 bytes at {total} sites")


if __name__ == "__main__":
    main()
