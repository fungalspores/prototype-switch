#!/bin/sh
# Builds the rutracker release folder: both NROs + toml, both NSP forwarders, README, licenses. No game data, no keys.
set -e
OUT=${OUT:-/var/minis/mounts/Projects/prototype_x360_transfer/RELEASE/Prototype-1-2-Switch-${VER:-1.4.0}}
rm -rf "$OUT"
mkdir -p "$OUT/switch/prototype-nx" "$OUT/switch/prototype2-nx" "$OUT/nsp" "$OUT/licenses"
P1=/root/proto/x360/prototype_nx
P2=/work
# The PGO builds (build/pgo with PROTO_PGO=USE, /work/build/switch with P2_PGO=USE). 1.2.0 and 1.3.0 took
# P1 from build/switch, configured without the profile: check the cache before copying.
grep -q "^PROTO_PGO:STRING=USE" $P1/build/pgo/CMakeCache.txt || { echo "Prototype: build/pgo is not a PGO build"; exit 1; }
grep -q "^P2_PGO:STRING=USE" $P2/build/switch/CMakeCache.txt || { echo "Prototype 2: not a PGO build"; exit 1; }
cp $P1/build/pgo/prototype.nro "$OUT/switch/prototype-nx/"
cp $P1/prototype.toml.release "$OUT/switch/prototype-nx/prototype.toml"
cp $P2/build/switch/prototype2.nro "$OUT/switch/prototype2-nx/"
cp $P2/config/prototype2.toml.fspores "$OUT/switch/prototype2-nx/prototype2.toml"
cp /root/proto/release_nsp/*.nsp "$OUT/nsp/"
cp "$(dirname "$0")/../release/README.txt" "$OUT/README.txt"
cp /root/proto/gh/prototype-switch/rex-start/rex-start.nro "$OUT/switch/rex-start.nro"
cp /root/proto/p2nx/LICENSE "$OUT/LICENSE"
cp /root/proto/p2nx/THIRD_PARTY_NOTICES.md "$OUT/licenses/THIRD_PARTY_NOTICES_Prototype2.md"
cp /root/proto/menx/sdk/LICENSE "$OUT/licenses/LICENSE-ReXGlue-SDK.txt"
cp /root/proto/fwd/loader/LICENSE.md "$OUT/licenses/LICENSE-Forwarder-Mod-nx-hbloader.md"
cp /root/proto/release_art/icon_p1.jpg /root/proto/release_art/icon_p2.jpg "$OUT/licenses/" 2>/dev/null || true
mv "$OUT/licenses/icon_p1.jpg" "$OUT/icon_prototype.jpg"; mv "$OUT/licenses/icon_prototype2.jpg" "$OUT/" 2>/dev/null || mv "$OUT/licenses/icon_p2.jpg" "$OUT/icon_prototype2.jpg"
# Guard: no game data or console keys.
if find "$OUT" -iname '*.xex' -o -iname '*.rcf' -o -iname 'prod.keys' -o -iname 'title.keys' | grep -q .; then
  echo "FORBIDDEN FILES IN RELEASE"; exit 1
fi
cd "$OUT" && find . -type f -not -name SHA256SUMS.txt | sort | while IFS= read -r f; do sha256sum "$f"; done > SHA256SUMS.txt
du -sh "$OUT"; find "$OUT" -type f | sort
