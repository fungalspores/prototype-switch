"""Materialize Git symlink placeholders in Windows-fetched SDK dependencies."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
upstream=ROOT/'reference/rexglue-upstream'
sdk=ROOT/'reference/masseffect-nx/sdk'
paths=subprocess.check_output(['git','-C',str(upstream),'config','--file','.gitmodules','--get-regexp','path'],text=True)
count=0
for line in paths.splitlines():
 rel=line.split(' ',1)[1]
 if not rel.startswith('thirdparty/'):continue
 source_repo=upstream/rel
 entries=subprocess.check_output(['git','-C',str(source_repo),'ls-files','-s','-z']).split(b'\0')
 linkmap={}
 for entry in entries:
  if not entry.startswith(b'120000 '):continue
  name=entry.split(b'\t',1)[1].decode()
  data=subprocess.check_output(['git','-C',str(source_repo),'show',':'+name]).decode().strip()
  linkmap[(source_repo/name).resolve()]=data
 for source,data in linkmap.items():
  resolved=source
  visited=set()
  while resolved in linkmap:
   if resolved in visited:raise RuntimeError('Cyclic Git link')
   visited.add(resolved)
   resolved=(resolved.parent/linkmap[resolved]).resolve()
  if not resolved.is_relative_to(source_repo.resolve()) or not resolved.is_file():
   print('Skipped external or directory link:',source.relative_to(upstream));continue
  target=sdk/source.relative_to(upstream)
  if not target.is_file():continue
  if target.read_bytes().strip()!=data.encode():continue
  target.write_bytes(resolved.read_bytes());count+=1
print('Materialized',count,'dependency links')

config=sdk/'thirdparty/FFmpeg/config.h'
if config.is_file():
 text=config.read_text()
 if text.startswith('#if defined(WIN32)'):
  text='#if defined(__SWITCH__)\n#include "config_switch_aarch64.h"\n#elif '+text[4:]
  config.write_text(text)
  print('Enabled existing Switch FFmpeg configuration')
