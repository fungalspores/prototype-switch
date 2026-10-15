# [PROTOTYPE] and [PROTOTYPE 2] for Nintendo Switch

[Русский](README.ru.md)

Native Nintendo Switch ports of the **Xbox 360** versions of Prototype (2009) and Prototype 2 (2012), made by
**static recompilation** with [ReXGlue](https://github.com/rexglue/rexglue-sdk): the games' PowerPC code is
translated to C++ and compiled to ARM64 for Horizon OS, and their Direct3D command stream is drawn with Vulkan
(NVK/Mesa). No emulator, no Xbox 360 GPU emulation.

**Free. Not for sale.** News: [t.me/fspores](https://t.me/fspores)

This repository has **no game files**. You need your own copy of the Xbox 360 games.

![status](https://img.shields.io/badge/Prototype-playable-brightgreen) ![status](https://img.shields.io/badge/Prototype%202-playable-brightgreen)

## Download

Ready builds (NROs, settings, HOME-menu NSP forwarders): see **Releases**.

## What you get

| | Prototype | Prototype 2 |
|---|---|---|
| Port | this project (fungalspores) | [penettrator](https://github.com/penettrator/prototype2-nx), built from his sources with our fixes |
| Picture | native 1280x720 | 960x544 + FSR 1 (the original release: 640x352) |
| FPS* | menus and cutscenes 30-60, gameplay 25-30 | city 25-30 |
| Sound | yes | yes |
| Saves | yes (fixed) | yes |

\* Switch OLED (Mariko) with sys-clk / Horizon OC at CPU 1785 / GPU 768-1152 / RAM 2333 MHz. Lower at stock clocks.

## Install

Needs Atmosphère 1.8.0+ with sigpatches, DBI (or another NSP installer), ~7 GB (Prototype) and ~8 GB (Prototype 2)
on the SD card. FAT32 is fine.

1. Copy `switch/` from the release to the root of the SD card:
   ```
   sdmc:/switch/prototype-nx/prototype.nro, prototype.toml
   sdmc:/switch/prototype2-nx/prototype2.nro, prototype2.toml
   ```
2. Put your game files there:
   - Prototype → `sdmc:/switch/prototype-nx/game/` (with `default.xex` inside)
   - Prototype 2 → `sdmc:/switch/prototype2-nx/game_root/` (with `default.xex` inside)

   From an ISO: `extract-xiso -x "Prototype.iso" -d game`. `$SystemUpdate` is not needed.
3. Install both NSPs from `nsp/` with DBI and start the games from the HOME menu.
   Without an NSP: hold **R** while starting any installed game and pick the port in the Homebrew menu.
   **Not from the Album:** applet mode does not have enough memory.

### Tested editions

- Prototype: Xbox 360, title ID 4156084E (Russian edition).
- Prototype 2: `default.xex` SHA-256 `f6caf82c433b0bb20fe701912388b1e7ee8d6b529d8fc275af960ac2e9ddcab8`.

The code is recompiled from one specific `default.xex`; other editions may not start.

## Settings

`prototype.toml` / `prototype2.toml` next to the NRO. Restart the game after a change.

Prototype 2 picture (ours / the original release):

| Setting | Ours | Original | |
|---|---|---|---|
| `p2_render_width` x `p2_render_height` | 960x544 | 640x352 | internal resolution, scaled with FSR |
| `p2_native_texture_lod_bias` | 0.5 | 1.0 | lower = sharper textures |
| `p2_native_max_anisotropy` | 2 | 1 | |
| `p2_geo_lod_bias_scale` | 2.0 | 4.0 | lower = more detailed meshes |
| `p2_shadow_range_scale` | 0.5 | 0.3 | |
| `p2_ped_max_scale`, `p2_vehicle_max_scale` | 0.3, 0.15 | 0.3, 0.15 | crowds cost the most CPU |

`user_language`: 12 Russian, 1 English (depends on your copy).

Prototype 2 runs on all four CPU cores and heats the console more than the first game. With Horizon OC / sys-clk,
give it (`0150524F54320000`) its own clock profile. Do not use high overclocks on an Erista (original) console.

## What we fixed

**Prototype** (port from scratch, ReXGlue + the masseffect-nx Horizon SDK + the Prototype 2 NX native renderer):
- Hangs in cutscenes, transitions and after the pause menu: the main thread held the scene critical sections
  while waiting for the render thread to drain the full command ring, and the render thread needed them.
  The main thread now releases the critical sections it owns while it waits (`src/proto_deadlock_fix.cpp`);
  the render thread drains in place when the ring fills during its own drain.
- "Corrupted" saves: the game writes the save file's size into its first word and checks it on load; the SDK
  returned the size cached when the file was opened (0). Fixed in `XFile::Write` / `SetLength`.
- Console signature stubs (`XeKeysConsolePrivateKeySign` / `...SignatureVerification`) returned "invalid".
- Per-address locks instead of one process-wide mutex for the game's lwarx/stwcx. regions; guest memory
  accesses without `volatile` (from Prototype 2 NX); audio, 30-60 FPS menus with the native renderer.

**Prototype 2** (penettrator's port, built from his GPL-3.0 sources):
- The same SDK fixes (saves, signatures, critical sections).
- A better picture (see Settings).
- No temperature-sensor polling: an open `ts` session for the whole game coincided with Horizon OC's `hoc:clk`
  crashing under load (and `pcv`/`ptm`/`Bus` after it).

A splash with the credit is shown for 3 seconds before each game (skip with A/B/+).

## Building

Everything was built **on an Android phone** (Alpine Linux in PRoot, aarch64) without Docker; a normal Linux box
works the same way. Outline:

1. **devkitA64**: `tools/proot/pull_dka64.py` unpacks the arm64 `devkitpro/devkita64` image; on musl hosts run
   `tools/proot/patch_dkp.sh` and `wrap_dkp.sh` (cc1/cc1plus/lto1 must stay unpatched, called through ld-linux).
2. **SDK**: [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) at `a85b0b4` +
   `patches/rexglue-sdk-masseffect-nx.patch`; third-party sources with its `tools/fetch_thirdparty.py`
   (rexglue-sdk v0.10.0). Host `rexglue`:
   `cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_ENABLE_TRACY=OFF && ninja -C out/host rexglue`
   (on musl add `-lucontext_posix -lucontext`; without `/dev/shm` run it with `LD_PRELOAD` of `tools/proot/shmshim.c`).
3. **Mesa/NVK**: [mesa-switch](https://github.com/danfromtico/mesa-switch) at `d4a00ea0ab3` + `patches/mesa-switch.patch`,
   then `tools/proot/mesa_p2_build.sh` (needs meson >= 1.7, Rust nightly + rust-src + rustfmt, a dynamically linked
   bindgen 0.73+, mesa_clc/vtn_bindgen2 from a native Mesa build).
4. **Prototype**: `prototype/build.sh` (codegen ~1-2 min, NRO ~10-35 min).
5. **Prototype 2**: as in penettrator's `docs/BUILD.en.md`: codegen of `project/prototype2_manifest.toml`, then
   `tools/prepare_switch_codegen.py`, `prepare_no_volatile.py`, `prepare_switch_hot.py` and
   `cmake -S switch -B build/switch ... -DP2_DIRECT_CALLS=ON -DP2_GUEST_LTO=ON -DP2_LEAF_LOCALS=ON -DP2_ARM_VMX=ON`
   (~25 min with LTO).
6. **NSP forwarders**: `forwarder/build_forwarder.py --nro sdmc:/switch/prototype-nx/prototype.nro --icon art/icon_prototype.jpg --title Prototype --author fungalspores --version 1.0.0 --tid 0150524F54310000 --keys <your prod.keys> --out out/`
   (needs [Forwarder-Mod](https://github.com/Skywalker25/Forwarder-Mod) `9853bd9` cloned to `forwarder/loader`).
   Your own `prod.keys` only; never publish them.

## Credits

- **penettrator** — the Prototype 2 port ([prototype2-nx](https://github.com/penettrator/prototype2-nx)). Thank you!
- **NebadasSwifty** — [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx), the SDK with the Horizon layer.
- **StevensND** — the Horizon layer of ReXGlue and [nfsmw-nx](https://github.com/StevensND/nfsmw-nx).
- **R-drg** — [project8-sw](https://github.com/R-drg/project8-sw), the base of the native renderer.
- **Tom Clay** and the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk); the [Xenia](https://xenia.jp) project.
- **danfromtico** — [mesa-switch](https://github.com/danfromtico/mesa-switch); the [Mesa](https://mesa3d.org) project.
- **Skywalker25** — [Forwarder-Mod](https://github.com/Skywalker25/Forwarder-Mod); switchbrew (libnx); devkitPro.
- AMD FidelityFX Super Resolution 1.

## License

GPL-3.0 (see `LICENSE`); third-party components under their own licenses (`prototype2/THIRD_PARTY_NOTICES.md`).
Prototype is a trademark of its owners. This project is not affiliated with Radical Entertainment, Activision,
Microsoft or Nintendo and contains none of their files.
