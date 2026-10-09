"""Focused prepared-export reuse, change and repair checks."""
import tempfile
from pathlib import Path
from product_grouped import remember_export, restore_export

root = Path(__file__).resolve().parents[1]/'scratchbook/export-cache-tests'
root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=root) as job:
    job = Path(job)
    output = job/'prepared'
    output.mkdir()
    (output/'test.esp').write_bytes(b'prepared plugin')
    inputs = {'weapons': {'test.esp:000800': {'guide': [1, 2, 3]}}}
    cache = job/'cache'
    remember_export(cache, inputs, output, {'objects': 2})
    assert restore_export(cache, inputs, job/'same') == {'objects': 2}
    assert (job/'same/test.esp').read_bytes() == b'prepared plugin'
    assert restore_export(cache, {'weapons': {}}, job/'removed') is None
    assert restore_export(cache, {'weapons': {'test.esp:000800': {'guide': [1, 2, 4]}}}, job/'changed') is None
    (cache/'output/test.esp').unlink()
    assert restore_export(cache, inputs, job/'missing') is None
    remember_export(cache, inputs, output, {'objects': 2})
    assert restore_export(cache, inputs, job/'repaired') == {'objects': 2}
print('PASS unchanged reuse, changed guide, removed weapon and missing-output repair')
