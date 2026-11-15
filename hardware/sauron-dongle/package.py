#!/usr/bin/env python3
"""Bundle only user-facing print/CAD files; omit intermediate preview meshes."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

root = Path(__file__).resolve().parent
files = [root/name for name in ('README.md','requirements.txt','build_case.py',
         'render_preview.py','package.py','validation.json','preview.png','orthographic.png')]
files += sorted((root/'stl').glob('*.stl'))
files += sorted((root/'cad').glob('*.step'))
with ZipFile(root/'sauron-dongle-print-pack.zip','w',ZIP_DEFLATED) as pack:
    for path in files:
        pack.write(path,Path('sauron-dongle')/path.relative_to(root))
print('Packaged sauron-dongle-print-pack.zip')
