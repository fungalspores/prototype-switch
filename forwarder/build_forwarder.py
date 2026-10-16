#!/usr/bin/env python3
"""HOME-menu NSP forwarder for a ReXGlue Switch port NRO.

    build_forwarder.py --nro sdmc:/switch/prototype-nx/prototype.nro --icon icon.jpg --title "Prototype"
                       --author fungalspores --version 1.0.0 --tid 0150524F54310000 --out out/

Program NCA: Forwarder-Mod (nx-hbloader, Skywalker25, commit 9853bd9) built with devkitA64; its NPDM gets
the title ID, the 39-bit address space (the full application memory a port needs), CPU cores 0-3 and
ForceDebug (Atmosphere 1.8.0+ bit 19). RomFS: nextNroPath / nextArgv = the NRO on the SD card.
Control NCA: NACP (title, author, version, no user selection, no save data) + the icon for every language.
NCA/PFS0/RomFS/CNMT writing is from fungalspores/usm-winenx nsp/build_nsp.py. Sections are not encrypted,
only NCA headers (header_key from the player's own prod.keys); the NSP needs sigpatches like any homebrew NSP.
"""
import argparse, json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nsp_container as nc  # NCA/PFS0/RomFS/CNMT from usm-winenx build_nsp.py

LOADER = os.path.join(HERE, "loader")
DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")


def build_loader(tid, name):
    j = json.load(open(os.path.join(LOADER, "hbl.json.orig")))
    j["name"] = name
    for k in ("title_id", "title_id_range_min", "title_id_range_max"):
        j[k] = f"0x{tid:016x}"
    j["program_id"] = j["program_id_range_min"] = j["program_id_range_max"] = j["title_id"]
    j["address_space_type"] = 3  # 39-bit
    for c in j["kernel_capabilities"]:
        if c["type"] == "kernel_flags":
            c["value"].update(highest_cpu_id=3, lowest_cpu_id=0, lowest_thread_priority=28,
                              highest_thread_priority=63)
        if c["type"] == "debug_flags":
            c["value"] = {"allow_debug": False, "force_debug": True, "force_debug_prod": False}
    json.dump(j, open(os.path.join(LOADER, "hbl.json"), "w"), indent=2)
    env = dict(os.environ, DEVKITPRO=DKP,
               PATH=f"{DKP}/tools/bin:{DKP}/devkitA64/bin:" + os.environ["PATH"])
    for f in ("hbl.npdm", "hbl.nso", "hbl.elf", "hbl.nsp"):
        try: os.remove(os.path.join(LOADER, f))
        except FileNotFoundError: pass
    r = subprocess.run(["make", "-j6", "RELEASE=1", "ROMFS="], cwd=LOADER, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit("forwarder build failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    nso = open(os.path.join(LOADER, "hbl.nso"), "rb").read()
    npdm = open(os.path.join(LOADER, "hbl.npdm"), "rb").read()
    assert npdm[:4] == b"META" and ((npdm[0xC] >> 1) & 7) == 3, "NPDM must be 39-bit"
    return nso, npdm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nro", required=True, help="NRO path on the SD card (sdmc:/...)")
    ap.add_argument("--icon", required=True, help="256x256 JPEG")
    ap.add_argument("--title", required=True)
    ap.add_argument("--author", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--tid", required=True)
    ap.add_argument("--keys", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--file-name")
    ap.add_argument("--launcher", help="NRO started first with the game NRO as argv[1] and the title as argv[2] "
                                         "(the settings screen, e.g. sdmc:/switch/rex-start.nro)")
    a = ap.parse_args()

    tid = int(a.tid, 16)
    icon = open(a.icon, "rb").read()
    if icon[:2] != b"\xff\xd8" or len(icon) > 0x20000:
        sys.exit("icon must be a JPEG up to 128 KB")
    key = nc.header_key(a.keys)
    nso, npdm = build_loader(tid, a.title)

    program = nc.Nca(nc.CONTENT_PROGRAM, tid)
    program.add_pfs0([("main", nso), ("main.npdm", npdm)], nc.EXEFS_BLOCK)
    if a.launcher:
        argv = f'{a.launcher} {a.nro} "{a.title}"'
        program.add_romfs([("nextArgv", argv.encode()), ("nextNroPath", a.launcher.encode())])
    else:
        program.add_romfs([("nextArgv", a.nro.encode()), ("nextNroPath", a.nro.encode())])
    program = program.build(key)

    nacp = nc.make_nacp(tid, a.title, a.author, a.version)
    control = nc.Nca(nc.CONTENT_CONTROL, tid)
    control.add_romfs([("control.nacp", nacp)] + [(f"icon_{lang}.dat", icon) for lang in nc.LANGUAGES])
    control = control.build(key)

    meta = nc.Nca(nc.CONTENT_META, tid)
    meta.add_pfs0([(f"Application_{tid:016x}.cnmt",
                    nc.cnmt(tid, [(nc.NCM_PROGRAM, program), (nc.NCM_CONTROL, control)]))], nc.META_BLOCK)
    meta = meta.build(key)

    files = [(nc.sha256(program)[:16].hex() + ".nca", program), (nc.sha256(control)[:16].hex() + ".nca", control),
             (nc.sha256(meta)[:16].hex() + ".cnmt.nca", meta)]
    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, f"{a.file_name or a.title} [{tid:016X}][v0].nsp")
    with open(out, "wb") as f:
        f.write(nc.pfs0(files))
    print(f"NSP: {out} ({os.path.getsize(out)} bytes), title id {tid:016X}, starts "
          f"{a.launcher + ' -> ' if a.launcher else ''}{a.nro}")


if __name__ == "__main__":
    main()
