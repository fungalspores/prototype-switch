"""Find code pointers in the image that ReXGlue did not register as functions.

Every absolute pointer in the image is listed in .reloc. A pointer into code
that is neither a known function start nor a label inside a generated function
is reached only indirectly (vtable, callback, thread entry) and will hit
"Call to invalid or unregistered function" at run time.

    python pointer_scan.py image.bin generated/default [--toml]
"""
import argparse
import glob
import json
import os
import re
import struct

BASE = 0x82000000


def sections(img):
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    count = struct.unpack_from("<H", img, pe + 6)[0]
    opt = struct.unpack_from("<H", img, pe + 20)[0]
    out = []
    for i in range(count):
        s = pe + 24 + opt + i * 40
        name = img[s:s + 8].rstrip(b"\0").decode()
        vsize, va = struct.unpack_from("<II", img, s + 8)
        flags = struct.unpack_from("<I", img, s + 36)[0]
        out.append((name, BASE + va, BASE + va + vsize, flags))
    return out


def relocs(img, start, end):
    """Yield guest addresses of HIGHLOW relocations in the .reloc section."""
    off = start - BASE
    stop = end - BASE
    while off + 8 <= stop:
        page, size = struct.unpack_from("<II", img, off)
        if size < 8:
            break
        for k in range(8, size, 2):
            e = struct.unpack_from("<H", img, off + k)[0]
            if e >> 12 == 3:
                yield BASE + page + (e & 0xFFF)
        off += size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("generated")
    ap.add_argument("--toml", action="store_true", help="print [functions] lines")
    ap.add_argument("--relocs", action="store_true", help="walk .reloc instead of scanning data")
    ap.add_argument("--skip", nargs="*", default=[], help="hex addresses to leave out")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    secs = sections(img)
    code = [(n, lo, hi) for n, lo, hi, f in secs if f & 0x20]
    reloc = next((lo, hi) for n, lo, hi, f in secs if n == ".reloc")

    starts = {int(k, 16) for k in json.load(
        open(os.path.join(a.generated, "codegen.partition.json")))["assignments"]}
    labels = set()
    for path in glob.glob(os.path.join(a.generated, "*_recomp.*.cpp")):
        for m in re.finditer(r"^loc_([0-9A-F]{8}):", open(path).read(), re.M):
            labels.add(int(m.group(1), 16))

    def in_code(x):
        return any(lo <= x < hi for _, lo, hi in code)

    def after_end(prev):
        return (prev == 0x4E800020            # blr
                or prev == 0                  # padding
                or prev == 0x60000000         # nop
                or prev >> 26 == 18 and not prev & 1  # b (no link)
                or prev == 0x4E800420)        # bctr

    # The loader drops .reloc from memory, so fall back to every aligned word
    # in the data sections and keep targets that follow a function end.
    data = [(n, lo, hi) for n, lo, hi, f in secs
            if not f & 0x20 and n not in (".reloc", ".pdata", ".XBLD", ".edata", ".idata")]
    if a.relocs:
        where_list = relocs(img, *reloc)
    else:
        where_list = (w for _, lo, hi in data for w in range(lo, hi - 3, 4))

    stats = {"start": 0, "label": 0, "missing": 0, "not_after_end": 0}
    missing = {}
    for where in where_list:
        if in_code(where):
            continue
        ptr = struct.unpack_from(">I", img, where - BASE)[0]
        if not in_code(ptr) or ptr & 3:
            continue
        if ptr in starts:
            stats["start"] += 1
        elif ptr in labels:
            stats["label"] += 1
        elif not after_end(struct.unpack_from(">I", img, ptr - 4 - BASE)[0]):
            stats["not_after_end"] += 1
        else:
            stats["missing"] += 1
            missing.setdefault(ptr, []).append(where)

    # Addresses built in code: lis rX,hi then addi/ori rY,rX,lo within a few
    # instructions (callbacks, thread entries passed as arguments).
    built = {}
    for _, lo, hi in code:
        for pc in range(lo, hi - 3, 4):
            insn = struct.unpack_from(">I", img, pc - BASE)[0]
            if insn >> 26 != 15 or (insn >> 16) & 31:  # lis = addis rD,0,imm
                continue
            rd = (insn >> 21) & 31
            high = (insn & 0xFFFF) << 16
            for k in range(1, 9):
                nxt = pc + 4 * k
                if nxt >= hi:
                    break
                n = struct.unpack_from(">I", img, nxt - BASE)[0]
                op, rs = n >> 26, (n >> 16) & 31
                if op == 14 and rs == rd:  # addi
                    simm = n & 0xFFFF
                    simm -= 0x10000 if simm & 0x8000 else 0
                    target = (high + simm) & 0xFFFFFFFF
                elif op == 24 and (n >> 21) & 31 == rd:  # ori rA,rS,imm
                    target = high | (n & 0xFFFF)
                else:
                    continue
                if in_code(target) and not target & 3 and target not in starts \
                        and target not in labels \
                        and after_end(struct.unpack_from(">I", img, target - 4 - BASE)[0]):
                    insn_t = struct.unpack_from(">I", img, target - BASE)[0]
                    if insn_t and insn_t >> 16 != 0x0101:
                        built.setdefault(target, []).append(pc)
                break
    for ptr, refs in built.items():
        if ptr not in missing:
            stats["missing"] += 1
            missing[ptr] = refs

    # Drop what is not a function entry: jump tables (the "code" is a list of
    # code addresses), import thunks (0x0101xxxx ordinal words), padding, and
    # anything the caller names with --skip (e.g. SEH __except landing pads).
    skip = {int(s, 16) for s in a.skip}
    for ptr in list(missing):
        insn = struct.unpack_from(">I", img, ptr - BASE)[0]
        if ptr in skip or insn == 0 or insn >> 16 == 0x0101 or in_code(insn):
            del missing[ptr]
    stats["missing"] = len(missing)

    print("code sections:", ", ".join(f"{n} {lo:#x}-{hi:#x}" for n, lo, hi in code))
    print(f"known functions {len(starts)}, labels {len(labels)}")
    print("pointers:", stats)
    print(f"distinct missing targets: {len(missing)}")
    for ptr in sorted(missing):
        prev = struct.unpack_from(">I", img, ptr - 4 - BASE)[0]
        insn = struct.unpack_from(">I", img, ptr - BASE)[0]
        refs = missing[ptr]
        if a.toml:
            print(f"0x{ptr:08X} = {{}}  # ptr at 0x{refs[0]:08X}" + (f" (+{len(refs) - 1})" if len(refs) > 1 else ""))
        else:
            print(f"  {ptr:#010x}  prev={prev:08x} insn={insn:08x}  refs={len(refs)} first={refs[0]:#010x}")


if __name__ == "__main__":
    main()
