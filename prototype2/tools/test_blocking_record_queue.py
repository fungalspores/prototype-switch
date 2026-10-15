from pathlib import Path
import sys,subprocess,re
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
out=ROOT/'build/switch-hot/blocking_record_queue_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/I'+str(ROOT/'project'),str(ROOT/'tests/blocking_record_queue_test.cpp'),'/Fe:'+str(out),'/Fo:'+str(out.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(out)],check=True)
s=(ROOT/'project/src/native/native_renderer.cpp').read_text()
start=s.index('  static constexpr uint32_t kRecordQueueSize');end=s.index('  RendererStats& stats()',start)
block=s[start:end]
assert 'wait_for(' not in block and 'yield(' not in block and 'record_tail' not in block
assert 'record_work.CompleteFront();' in block and block.index('ExecuteDrawCommand(*draw);')<block.index('record_work.CompleteFront();')
assert 'record_work.Drain();' in block and 'record_work.Close();' in s
assert not any(x in s for x in ['record_mutex','record_sleeping','record_stop','record_queue =','record_cv'])
print('PASS actual renderer executes command before release/completion, uses blocking queue, retains drain/close, no legacy polling fields')