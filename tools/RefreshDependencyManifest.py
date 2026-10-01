"""Refresh the staged-file hashes after copying the current product adapters."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
file = root/'dependency-manifest.json'
manifest = json.loads(file.read_text())
manifest['files'] = {}
for folder in ('runtime', 'backends', 'mcp', 'tools', 'skills'):
    for path in sorted((root/folder).rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            with path.open('rb') as stream:
                manifest['files'][path.relative_to(root).as_posix()] = hashlib.file_digest(stream, 'sha256').hexdigest()
file.write_text(json.dumps(manifest, indent=2))
