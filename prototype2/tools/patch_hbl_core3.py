"""Prototype 2 package 0.2.0: patch ONLY the verified hbloader binary."""
from pathlib import Path
import argparse,hashlib
ap=argparse.ArgumentParser();ap.add_argument('input',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args()
b=a.input.read_bytes()
if hashlib.sha256(b).hexdigest()!='853df8526557b82c6b2018a8a102d22e8306687415e49653530dfd9a1d603b32':raise SystemExit('Unsupported hbl.nsp: use your hbloader source and highest_cpu_id=3; do not patch unknown offsets')
if a.input.resolve()==a.output.resolve():raise SystemExit('Use a different output file; keep your original backup')
c=bytearray(b)
for offset in (0x9efd,0x9fbd):
 if c[offset]!=2:raise SystemExit('Unexpected ThreadInfo')
 c[offset]=3
if hashlib.sha256(c).hexdigest()!='3d1540fbba92ce9efd9d56273782f05cfda77867441ab4e53ee84da4c90735ac':raise SystemExit('Patch verification failed')
if a.output.exists():raise SystemExit('Output already exists')
a.output.write_bytes(c);print('PASS: max core 2 -> 3 in ACID and ACI0; only 2 bytes changed')
