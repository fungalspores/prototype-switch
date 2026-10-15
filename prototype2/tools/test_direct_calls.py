import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_direct_calls import convert,protected_targets
class DirectCalls(unittest.TestCase):
 def test_hook_protected(self):
  s='sub_822DA288(ctx, base);'; self.assertEqual(convert(s,{'sub_822DA288'}),(s,set()))
 def test_added_hook_restores_public_call(self):
  s='P2_DIRECT_CALL(sub_822DA288)(ctx, base);'; self.assertEqual(convert(s,{'sub_822DA288'}),('sub_822DA288(ctx, base);',set()))
 def test_unhooked(self):
  self.assertEqual(convert('sub_82000100(ctx, base);',set())[0],'P2_DIRECT_CALL(sub_82000100)(ctx, base);')
 def test_idempotent(self):
  once,_=convert('sub_82000100(ctx, base);',set()); self.assertEqual(convert(once,set())[0],once)
 def test_original_and_indirect_not_changed(self):
  s='__imp__sub_82000100(ctx, base); REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);'; self.assertEqual(convert(s,set())[0],s)
 def test_all_actual_hooks_protected(self):
  p,strong=protected_targets(); self.assertGreaterEqual(len(strong),5); self.assertFalse(strong-p)
  for name in ['sub_82C49260','sub_829F5978','sub_829F4C58','sub_822BAA78','sub_822DA288','sub_82C2CC60']:
   self.assertIn(name,p)
if __name__=='__main__': unittest.main()
