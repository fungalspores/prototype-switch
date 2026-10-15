"""NCA / PFS0 / RomFS / CNMT writer, from fungalspores/usm-winenx nsp/build_nsp.py."""
import hashlib, struct, sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

LANGUAGES = ["AmericanEnglish", "BritishEnglish", "Japanese", "French", "German",
             "LatinAmericanSpanish", "Spanish", "Italian", "Dutch", "CanadianFrench",
             "Portuguese", "Russian"]

IVFC_BLOCK = 0x4000
EXEFS_BLOCK, META_BLOCK = 0x10000, 0x1000
CONTENT_PROGRAM, CONTENT_META, CONTENT_CONTROL = 0, 1, 2      # NCA header content types
NCM_PROGRAM, NCM_CONTROL = 1, 3                               # CNMT content record types (NcmContentType)


def sha256(data):
    return hashlib.sha256(data).digest()


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def padded(data, alignment):
    return data + bytes(align(len(data), alignment) - len(data))


def header_key(path):
    for line in open(path, encoding="utf-8", errors="replace"):
        name, _, value = line.partition("=")
        if name.strip().lower() == "header_key":
            key = bytes.fromhex(value.strip())
            if len(key) == 32:
                return key
    sys.exit(f"{path}: нет header_key")



def make_nacp(tid, title, author, version):
    nacp = bytearray(0x4000)
    for i in range(16):
        entry = i * 0x300
        name, by = title.encode(), author.encode()
        nacp[entry:entry + len(name)] = name
        nacp[entry + 0x200:entry + 0x200 + len(by)] = by
    struct.pack_into("<I", nacp, 0x302c, (1 << len(LANGUAGES)) - 1)   # supported languages
    nacp[0x3060:0x3070] = version.encode().ljust(0x10, b"\0")
    nacp[0x3025] = 0          # no user selection: the game saves on the SD card
    nacp[0x3034] = 0          # screenshots allowed
    nacp[0x3035] = 2          # video capture: automatic
    nacp[0x30f0] = 2          # logo type: Nintendo
    struct.pack_into("<Q", nacp, 0x3038, tid)                         # presence group
    struct.pack_into("<QQ", nacp, 0x3070, tid ^ 0x1000, tid)          # add-on base, save owner
    struct.pack_into("<8Q", nacp, 0x30b0, *([tid] * 8))               # local communication
    struct.pack_into("<Q", nacp, 0x30f8, tid)                         # pseudo device id seed
    return bytes(nacp)

# ---- containers ----

def pfs0(files):
    names = b""
    table = b""
    offset = 0
    for name, data in files:
        table += struct.pack("<QQII", offset, len(data), len(names), 0)
        names += name.encode() + b"\0"
        offset += len(data)
    names = padded(names, 0x20)
    return (struct.pack("<4sIII", b"PFS0", len(files), len(names), 0) + table + names +
            b"".join(data for _, data in files))


def romfs_hash(parent, name):
    h = parent ^ 123456789
    for byte in name:
        h = ((h >> 5) | (h << 27)) & 0xffffffff
        h ^= byte
    return h


def romfs_table_count(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


def romfs(files):
    """A RomFS with every file in the root directory."""
    data, file_table, offsets = b"", b"", []
    entry = 0
    for name, content in files:
        data = padded(data, 0x10)
        offsets.append((entry, len(data), len(content)))
        data += content
        entry += 0x20 + align(len(name), 4)
    file_hash = [0xffffffff] * romfs_table_count(len(files))
    for i, ((name, _), (entry, offset, size)) in enumerate(zip(files, offsets)):
        raw = name.encode()
        bucket = romfs_hash(0, raw) % len(file_hash)
        sibling = offsets[i + 1][0] if i + 1 < len(files) else 0xffffffff
        file_table += struct.pack("<IIQQII", 0, sibling, offset, size, file_hash[bucket], len(raw))
        file_table += padded(raw, 4)
        file_hash[bucket] = entry
    dir_hash = [0xffffffff] * 3
    dir_hash[romfs_hash(0, b"") % 3] = 0
    dir_table = struct.pack("<6I", 0, 0xffffffff, 0xffffffff, 0 if files else 0xffffffff, 0xffffffff, 0)

    dir_hash_off = align(0x200 + len(data), 4)
    tables = [struct.pack(f"<{len(dir_hash)}I", *dir_hash), dir_table,
              struct.pack(f"<{len(file_hash)}I", *file_hash), file_table]
    header, pos = [0x50], dir_hash_off
    for t in tables:
        header += [pos, len(t)]
        pos += len(t)
    header.append(0x200)
    image = struct.pack("<10Q", *header).ljust(0x200, b"\0") + data
    image = image.ljust(dir_hash_off, b"\0") + b"".join(tables)
    return image


class Nca:
    def __init__(self, content_type, tid):
        self.content_type, self.tid = content_type, tid
        self.body = bytearray(0xc00)
        self.sections = []           # (start, end, fs header)

    def _section(self, data, fs_header):
        start = len(self.body)
        self.body += padded(data, 0x200)
        self.sections.append((start, len(self.body), fs_header))

    def add_pfs0(self, files, block):
        image = pfs0(files)
        hashes = b"".join(sha256(image[i:i + block]) for i in range(0, len(image), block))
        layer = align(len(hashes), 0x200)
        fs = bytearray(0x200)
        struct.pack_into("<HBBB", fs, 0, 2, 1, 2, 1)             # version, PFS0, SHA-256 layers, not encrypted
        fs[8:0x28] = sha256(hashes)
        struct.pack_into("<IIQQQQ", fs, 0x28, block, 2, 0, len(hashes), layer, len(image))
        self._section(padded(hashes, 0x200) + image, fs)

    def add_romfs(self, files):
        image = romfs(files)
        romfs_size = len(image)
        levels = [padded(image, IVFC_BLOCK)]
        for _ in range(5):
            below = levels[0]
            levels.insert(0, padded(b"".join(sha256(below[i:i + IVFC_BLOCK])
                                              for i in range(0, len(below), IVFC_BLOCK)), IVFC_BLOCK))
        fs = bytearray(0x200)
        struct.pack_into("<HBBB", fs, 0, 2, 0, 3, 1)             # version, RomFS, IVFC, not encrypted
        struct.pack_into("<4sIII", fs, 8, b"IVFC", 0x20000, 0x20, 7)
        offset = 0
        for i, level in enumerate(levels):
            size = romfs_size if i == 5 else len(level)
            struct.pack_into("<QQII", fs, 0x18 + 0x18 * i, offset, size, 14, 0)
            offset += size
        fs[0xc8:0xe8] = sha256(levels[0])
        self._section(b"".join(levels), fs)

    def build(self, key):
        header = bytearray(self.body[:0xc00])
        struct.pack_into("<4sBBBBQQII", header, 0x200, b"NCA3", 0, self.content_type, 0, 0,
                         len(self.body), self.tid, 0, 0x000C1100)
        for i, (start, end, fs) in enumerate(self.sections):
            struct.pack_into("<IIB", header, 0x240 + 0x10 * i, start // 0x200, end // 0x200, 1)
            header[0x280 + 0x20 * i:0x2a0 + 0x20 * i] = sha256(fs)
            header[0x400 + 0x200 * i:0x600 + 0x200 * i] = fs
        encrypted = b""
        for sector in range(6):   # AES-128-XTS, 0x200-byte sectors, big-endian sector tweak
            enc = Cipher(algorithms.AES(key), modes.XTS(sector.to_bytes(16, "big"))).encryptor()
            encrypted += enc.update(bytes(header[0x200 * sector:0x200 * (sector + 1)])) + enc.finalize()
        return encrypted + bytes(self.body[0xc00:])


def cnmt(tid, contents):
    data = struct.pack("<QIBBHHHBBBBI4x", tid, 0, 0x80, 0, 0x10, len(contents), 0, 0, 0, 0, 0, 0)
    data += struct.pack("<QII", tid | 0x800, 0, 0)               # patch id, required versions
    for content_type, nca in contents:
        digest = sha256(nca)
        size = len(nca)
        data += digest + digest[:16] + struct.pack("<IBBBB", size & 0xffffffff, size >> 32, 0, content_type, 0)
    return data + bytes(0x20)


