# Native renderer (Prototype 2)

Taken from the Mercenaries 2 Switch port (D:\Mercenaries2-switch\Mercenaries2-NX, app/src/native), which copied it
from project8-sw (https://github.com/R-drg/project8-sw, commit 3393f261, `src/game/src/native/`), an adaptation of the
native renderer of nfsmw-nx (https://github.com/StevensND/nfsmw-nx). **GPL-3.0-or-later**: keep the SPDX headers;
a build containing it is GPL-3.0.

Renamed `mercs2_*` -> `p2_*` (files, namespace, cvars). Selected with `p2_renderer = "native"` in `prototype2.toml`;
the default stays the SDK's FSI emulation. Mercenaries 2 shader-hash specific code paths (water, shadows, motion blur)
match only Mercs2 ucode hashes and are inert here; comments citing "HANDOFF section NN" refer to the Mercs2 HANDOFF.
