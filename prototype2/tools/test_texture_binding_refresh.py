import sys,subprocess,re
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
source=(ROOT/'project/src/native/native_renderer.cpp').read_text()
slot=source[source.index('  VkImageView SlotView('):source.index('  VkSampler SlotSampler(')]
condition=re.search(r'if \((e.frame == frame.*?)\) \{',slot,re.S)[1]
assert 'e.answers_epoch == textures.answers_epoch()' in condition
out=ROOT/'build/switch-hot'
(out/'slot_view_predicate_under_test.inc').write_text('bool SlotPredicate(const Entry& e,const Epochs& textures,uint64_t frame,uint64_t submission_number,uint32_t variant,const uint32_t* words) { return '+condition+'; }\n')
exe=out/'texture_binding_refresh_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++17','/O2','/EHsc','/MD','/I'+str(ROOT/'project/src'),'/I'+str(out),str(ROOT/'tests/texture_binding_refresh_test.cpp'),'/Fe:'+str(exe),'/Fo:'+str(exe.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(exe)],check=True)