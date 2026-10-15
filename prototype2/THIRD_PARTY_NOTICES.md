# Prototype 2 — 0.1.0 / Сторонние компоненты



# Third-party notices



This port is built on the work of other projects. Their licenses apply to their code and to the parts derived from it.

The port's own code is under GPL-3.0 (`LICENSE`).



## Projects this port is derived from



| Project | Used for | License |

|---|---|---|

| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | Code generator, Xbox 360 kernel and runtime, GPU command processing, XMA audio, virtual file system | BSD-3-Clause |

| [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by NebadasSwifty | SDK snapshot with the Horizon (Switch) layer and its fixes | BSD-3-Clause (SDK), GPL-3.0 (application code) |

| Horizon (Switch) layer of the SDK by [StevensND](https://github.com/StevensND) ([NFSMW-NX](https://github.com/StevensND/NFSMW-NX)) | Guest memory, exception handling, threading, input, audio, presentation | BSD-3-Clause (as part of the SDK) |

| [project8-sw](https://github.com/R-drg/project8-sw) and [the-simpsons-game-nx](https://github.com/R-drg/the-simpsons-game-nx) by R-drg | Base of the native renderer (Xbox 360 GPU commands to Vulkan) | GPL-3.0 |

| [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico, on top of [Mesa](https://mesa3d.org) | Vulkan driver (NVK) and shader compiler (NAK) for Horizon, linked into the NRO | MIT (Mesa's licenses per file) |

| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | EASU/RCAS upscale shader | MIT |



## Libraries in the NRO



| Library | License |

|---|---|

| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: XMA audio decoder) | LGPL-2.1-or-later (configured without GPL parts) |

| [libmspack](https://www.cabextract.org.uk/libmspack/) (LZX decompression) | LGPL-2.1 |

| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others |

| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |

| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |

| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |

| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |

| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |

| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |

| [toml++](https://github.com/marzer/tomlplusplus) | MIT |

| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |

| [libnx](https://github.com/switchbrew/libnx) and the devkitA64 runtime (newlib, libstdc++) | ISC; newlib and GCC runtime licenses |



## Trademarks



Prototype 2 is a trademark of its respective owners. Xbox 360 is a trademark of Microsoft.

Nintendo Switch is a trademark of Nintendo. This project is not affiliated with or endorsed by any of them and

does not contain any of their files. A legally owned copy of the game is required.



nx-hbloader references: ISC, Copyright 2017-2018 nx-hbloader Authors; see extras/fourth-core/LICENSE-hbloader.txt.


## NSP forwarder / NSP-форвардер

Forwarder-Mod (nx-hbloader derivative), ISC; pinned commit `9853bd9167d5a7f0c5fdf935864245c1fbcf8282`: https://github.com/Skywalker25/Forwarder-Mod . License: `extras/forwarder/LICENSE-Forwarder-Mod.txt`. Changes: Prototype2 target path/Title ID,39-bit address space, cores0–3, debug flags and compatibility adjustment to the noreturn declaration.

hacBrewPack is a build-time packaging tool, not included in the NSP: https://github.com/rlaphoenix/hacBrewPack , commit `745b16ecfc9ce055743067d200572204cb2aac6c`. Reproduction script: `tools/build_forwarder.sh`. No console keys are distributed. / Ключи консоли не распространяются.
