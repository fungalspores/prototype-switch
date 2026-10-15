#!/usr/bin/env python3
"""Unpack an Xbox 360 "Games on Demand" (GOD) package into the disc's files.

    god_extract.py <GOD folder: .../<TitleID>/00007000> <output folder> [--iso out.iso]

A GOD package is the disc's game partition (XDVDFS, "XGD") cut into DataNNNN files
inside "<hash>.data/", next to a 44 KB header file of the same name. Each DataNNNN is
    1 master hash block, then 203 sub-parts of (1 sub hash block + 204 data blocks),
with 4 KB blocks: 4096 + 203 * (4096 + 204 * 4096) = 170,459,136 bytes for a full part.
Only the data blocks belong to the disc. This script reads them in order, finds the
XDVDFS volume ("MICROSOFT*XBOX*MEDIA" at 0x10000 of the partition), and writes every
file of the disc into the output folder (default.xex among them). --iso also writes
the bare partition as one file, which extract-xiso and similar tools read.
"""
import argparse, os, struct, sys

BLOCK = 0x1000
DATA_PER_SUB = 0xCC          # 204 data blocks per sub-part
SUBS_PER_PART = 0xCB         # 203 sub-parts per DataNNNN file
SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"


class GodImage:
    """The data blocks of all DataNNNN files as one byte range."""

    def __init__(self, data_dir):
        names = sorted(n for n in os.listdir(data_dir) if n.lower().startswith("data"))
        if not names:
            raise SystemExit(f"no DataNNNN files in {data_dir}")
        self.parts = [open(os.path.join(data_dir, n), "rb") for n in names]
        self.part_blocks = []
        for f in self.parts:
            size = os.fstat(f.fileno()).st_size
            blocks = 0
            body = size - BLOCK                            # after the master hash block
            while body > 0 and blocks < SUBS_PER_PART * DATA_PER_SUB:
                body -= BLOCK                              # the sub-part's hash block
                take = min(DATA_PER_SUB, max(0, body // BLOCK))
                blocks += take
                body -= take * BLOCK
            self.part_blocks.append(blocks)
        self.size = sum(self.part_blocks) * BLOCK

    def _block_offset(self, n):
        for part, count in enumerate(self.part_blocks):
            if n < count:
                sub, inside = divmod(n, DATA_PER_SUB)
                return part, BLOCK + sub * (BLOCK + DATA_PER_SUB * BLOCK) + BLOCK + inside * BLOCK
            n -= count
        raise ValueError("read past the end of the image")

    def read(self, offset, length):
        out = bytearray()
        while length > 0:
            n, within = divmod(offset, BLOCK)
            part, at = self._block_offset(n)
            take = min(length, BLOCK - within)
            f = self.parts[part]
            f.seek(at + within)
            chunk = f.read(take)
            if len(chunk) != take:
                raise ValueError(f"short read in part {part}")
            out += chunk
            offset += take
            length -= take
        return bytes(out)


def find_volume(img):
    # the GOD data normally starts at the game partition, so the volume is at 0x10000;
    # look a little further in case a package kept some leading sectors
    for base in (0, 0xFD90000, 0x2080000, 0x18300000):
        if base + 0x10000 + 0x800 <= img.size and img.read(base + 0x10000, len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("no XDVDFS volume (MICROSOFT*XBOX*MEDIA) found: not a GOD game package?")


def walk(img, base, dir_sector, dir_size, path, out, stats):
    table = img.read(base + dir_sector * SECTOR, dir_size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF and sector == 0xFFFFFFFF:
            continue                                       # padding
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
        full = os.path.join(path, name)
        if attr & 0x10:                                    # directory
            os.makedirs(os.path.join(out, full), exist_ok=True)
            if size:
                walk(img, base, sector, size, full, out, stats)
        else:
            dst = os.path.join(out, full)
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            with open(dst, "wb") as f:
                pos, left_bytes = base + sector * SECTOR, size
                while left_bytes:
                    take = min(left_bytes, 8 << 20)
                    f.write(img.read(pos, take))
                    pos += take
                    left_bytes -= take
            stats[0] += 1
            stats[1] += size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("god", help="the 00007000 folder (with <hash> and <hash>.data inside)")
    ap.add_argument("out", help="where to write the disc's files")
    ap.add_argument("--iso", help="also write the game partition as one image file")
    a = ap.parse_args()

    datas = [n for n in os.listdir(a.god) if n.lower().endswith(".data") and os.path.isdir(os.path.join(a.god, n))]
    if len(datas) != 1:
        raise SystemExit(f"expected one <hash>.data folder in {a.god}, found {datas}")
    img = GodImage(os.path.join(a.god, datas[0]))
    print(f"{len(img.parts)} parts, {img.size / 2**30:.2f} GB of disc data")
    base = find_volume(img)
    root_sector, root_size = struct.unpack_from("<II", img.read(base + 0x10000 + 20, 8))
    print(f"XDVDFS volume at +{base:#x}, root directory sector {root_sector}, {root_size} bytes")

    if a.iso:
        with open(a.iso, "wb") as f:
            pos = base
            while pos < img.size:
                take = min(img.size - pos, 8 << 20)
                f.write(img.read(pos, take))
                pos += take
        print(f"partition image: {a.iso}")

    os.makedirs(a.out, exist_ok=True)
    stats = [0, 0]
    walk(img, base, root_sector, root_size, "", a.out, stats)
    print(f"{stats[0]} files, {stats[1] / 2**30:.2f} GB -> {a.out}")
    xex = os.path.join(a.out, "default.xex")
    print("default.xex:", "есть" if os.path.isfile(xex) else "НЕТ")


if __name__ == "__main__":
    main()
