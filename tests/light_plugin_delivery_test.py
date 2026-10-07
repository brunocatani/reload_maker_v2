"""Changing a receipt-owned .esl into a light .esp preserves other groups."""
from pathlib import Path
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from product_grouped import deliver
from product_packs import write,read

def run(workspace):
    workspace.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=workspace) as temporary:
        root=Path(temporary);output=root/'output';mod=root/'mods'/'Reloads'
        mod.mkdir(parents=True);output.mkdir()
        receipt='F4SE/Plugins/DIAMOND/Generated/DIAMOND_a/files.json'
        old={'plugin':'DIAMOND_a.esl','files':['DIAMOND_a.esl'],'modelOverrides':[]}
        new={'plugin':'DIAMOND_a.esp','files':['DIAMOND_a.esp'],'modelOverrides':[]}
        write(mod/receipt,old);write(output/receipt,new)
        (mod/'DIAMOND_a.esl').write_bytes(b'old owned plugin')
        (output/'DIAMOND_a.esp').write_bytes(b'new owned plugin')
        (mod/'unrelated.esl').write_bytes(b'keep user file')
        other='F4SE/Plugins/DIAMOND/Generated/DIAMOND_b/files.json'
        write(mod/other,{'plugin':'DIAMOND_b.esl','files':['DIAMOND_b.esl']})
        (mod/'DIAMOND_b.esl').write_bytes(b'keep other group')
        for _ in range(2):
            deliver(output,mo2=mod)
            assert not (mod/'DIAMOND_a.esl').exists()
            assert (mod/'DIAMOND_a.esp').read_bytes()==b'new owned plugin'
            assert (mod/'unrelated.esl').read_bytes()==b'keep user file'
            assert (mod/'DIAMOND_b.esl').read_bytes()==b'keep other group'
            assert read(mod/receipt)==new
    print('PASS receipt-owned extension migration and repeated delivery preserve unrelated files/groups')

if __name__=='__main__':run(Path(sys.argv[1]).resolve())
