#!/bin/sh
# Builds the rutracker release folder: both NROs + toml, both NSP forwarders, README, licenses. No game data, no keys.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/release/Prototype-1-2-Switch-1.0.0}
rm -rf "$OUT"
mkdir -p "$OUT/switch/prototype-nx" "$OUT/switch/prototype2-nx" "$OUT/nsp" "$OUT/licenses"
P1=$ROOT/prototype
P2=$ROOT/prototype2
cp $P1/build/switch/prototype.nro "$OUT/switch/prototype-nx/"
cp $P1/prototype.toml "$OUT/switch/prototype-nx/prototype.toml"
cp $P2/build/switch/prototype2.nro "$OUT/switch/prototype2-nx/"
cp $P2/config/prototype2.toml "$OUT/switch/prototype2-nx/prototype2.toml"
cp "${NSP_DIR:?set NSP_DIR to the folder with the built NSPs}"/*.nsp "$OUT/nsp/"
cp $ROOT/release/README.txt "$OUT/README.txt"
cp $ROOT/LICENSE "$OUT/LICENSE"
cp $P2/THIRD_PARTY_NOTICES.md "$OUT/licenses/THIRD_PARTY_NOTICES_Prototype2.md"
cp "${SDK_DIR:?set SDK_DIR}"/LICENSE "$OUT/licenses/LICENSE-ReXGlue-SDK.txt"
cp $ROOT/forwarder/LICENSE-Forwarder-Mod.md "$OUT/licenses/LICENSE-Forwarder-Mod-nx-hbloader.md"
cp $ROOT/art/icon_prototype.jpg $ROOT/art/icon_prototype2.jpg "$OUT/"
# Guard: no game data or console keys.
if find "$OUT" -iname '*.xex' -o -iname '*.rcf' -o -iname 'prod.keys' -o -iname 'title.keys' | grep -q .; then
  echo "FORBIDDEN FILES IN RELEASE"; exit 1
fi
cd "$OUT" && find . -type f -not -name SHA256SUMS.txt | sort | while IFS= read -r f; do sha256sum "$f"; done > SHA256SUMS.txt
du -sh "$OUT"; find "$OUT" -type f | sort
