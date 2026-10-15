import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_task_lifetime_audit import instrument,VT
class Instrumentation(unittest.TestCase):
    def source(self,extra='',high=-2113863680):
        return f'DEFINE_REX_FUNC(sub_12345678) {{\n\tctx.r11.s64 = {high};\n\tctx.r10.s64 = ctx.r11.s64 + -29616;\n'+extra+'\tREX_STORE_U32(ctx.r31.u32 + 0, ctx.r10.u32);\n}\n'
    def test_base_write_and_address(self):
        out,sites=instrument(self.source()); self.assertEqual(len(sites),1)
        self.assertIn('P2_TASK_BASE_WRITE(ctx, base, ctx.r31.u32 + 0, "sub_12345678")',out)
        self.assertLess(out.index('P2_TASK_BASE_WRITE'),out.index('REX_STORE_U32'))
    def test_other_high(self): self.assertEqual(instrument(self.source(high=-2113732608))[1],[])
    def test_overwrite(self): self.assertEqual(instrument(self.source('\tctx.r10.u64 = REX_LOAD_U32(ctx.r3.u32);\n'))[1],[])
    def test_call(self): self.assertEqual(instrument(self.source('\tsub_12345670(ctx, base);\n'))[1],[])
    def test_label(self): self.assertEqual(instrument(self.source('loc_12345678:\n'))[1],[])
    def test_idempotence(self):
        once,_=instrument(self.source()); twice,_=instrument(once); self.assertEqual(once,twice)
    def test_intervening_unrelated_write(self): self.assertEqual(len(instrument(self.source('\tctx.r9.s64 = 0;\n'))[1]),1)
if __name__=='__main__': unittest.main()
