"""Focused serializer launch diagnostics; no game or authoring files are written."""
import subprocess
from pathlib import Path
from unittest.mock import patch
from product_backend import Backends

root=Path(__file__).resolve().parents[1]
backend=Backends(root/'build-fast/dist/ReloadMakerV2',root).adapter('plugin')
with patch.object(backend.subprocess,'run',return_value=subprocess.CompletedProcess([],0xC0000005,'','')):
    ok,message=backend._run_spriggit(['version'])
    assert not ok and '0xC0000005' in message and 'No diagnostic output' in message
with patch.object(backend.subprocess,'run',return_value=subprocess.CompletedProcess([],1,'','Invalid YAML record')):
    ok,message=backend._run_spriggit(['version'])
    assert not ok and 'Invalid YAML record' in message and '0x00000001' in message
with patch.object(backend.subprocess,'run',side_effect=subprocess.TimeoutExpired('serializer',180)):
    ok,message=backend._run_spriggit(['version'])
    assert not ok and 'timed out' in message
print('PASS serializer crash code, native error text and timeout reporting')
