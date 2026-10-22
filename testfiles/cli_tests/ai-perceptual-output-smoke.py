"""Editable RGB Oklab linear/radial gradients, native AI and vector PDF. Synthetic only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image
from pypdf import PdfReader


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', type=Path, required=True)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', type=Path, help='accepted for the shared smoke-runner interface')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[name] = str(out / name.lower())
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    checks = []

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)

    def run(label, source, target, extension=None):
        command = [str(args.inkscape.resolve()), str(source), f'--export-filename={target}']
        if target.suffix == '.png':
            command += ['--export-background=white', '--export-area-page', '--export-dpi=96']
        if extension:
            command.append('--export-extension=org.inkscape.output.ai.' + extension)
        result = subprocess.run(command, env=env, capture_output=True, timeout=180)
        log = result.stdout + result.stderr
        (out / (label + '.log')).write_bytes(log)
        return result.returncode, log.decode('utf-8', 'replace')

    source = out / 'control.svg'
    source.write_text('''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
      xmlns:proof="urn:x-proof:document:1" width="240" height="100" viewBox="0 0 240 100">
      <defs><linearGradient id="g" proof:gradient-method="oklab">
        <stop stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/>
      </linearGradient><radialGradient id="r" xlink:href="#g"/></defs>
      <rect x="10" y="10" width="100" height="80" fill="url(#g)"/>
      <rect x="130" y="10" width="100" height="80" fill="url(#r)"/>
      </svg>''', encoding='utf-8')
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    before, ai, svg, after = [out / name for name in ('before.png', 'written.ai', 'reopened.svg', 'after.png')]
    check('source rendered', run('source', source, before)[0] == 0)
    check('modern saved', run('write', source, ai, 'native')[0] == 0 and ai.exists())
    code, log = run('reopen', ai, svg)
    check('native reopen', code == 0 and 'opens from its PDF page' not in log)
    root = ET.parse(svg).getroot()
    method = '{urn:x-proof:document:1}gradient-method'
    vectors = [v for v in root.iter() if v.get(method) == 'oklab']
    check('two Oklab methods retained', len(vectors) == 2)
    check('original stops stay editable', all(len(list(v)) == 2 for v in vectors))
    check('reopen rendered', run('after', svg, after)[0] == 0)
    a, b = [np.asarray(Image.open(p).convert('RGB'), dtype=np.int16) for p in (before, after)]
    check('render dimensions retained', a.shape == b.shape)
    check('Oklab canvas survives reopen', a.shape == b.shape and np.max(np.abs(a - b)) <= 3)
    # Independent numerical reference for the red/blue Oklab midpoint. Classic has G=0.
    check('canvas draws Oklab rather than Classic', np.max(np.abs(a[50, 60] - [140, 83, 162])) <= 4)

    pdf = PdfReader(ai)
    seen, shadings, images = set(), [], []

    def walk(obj):
        obj = obj.get_object() if hasattr(obj, 'get_object') else obj
        if not isinstance(obj, (dict, list)) or id(obj) in seen:
            return
        seen.add(id(obj))
        if isinstance(obj, dict):
            if '/ShadingType' in obj:
                shadings.append(obj)
            if obj.get('/Subtype') == '/Image':
                images.append(obj)
            for value in obj.values():
                walk(value)
        else:
            for value in obj:
                walk(value)

    walk(pdf.pages[0]['/Resources'])
    check('PDF contains linear and radial vector shading', {s['/ShadingType'] for s in shadings} == {2, 3})
    check('PDF gradients contain no raster images', not images)
    again = out / 'second.ai'
    check('second save', run('second', svg, again, 'native')[0] == 0)
    second_svg = out / 'second.svg'
    code, log = run('second-reopen', again, second_svg)
    check('second native reopen', code == 0 and 'opens from its PDF page' not in log)
    check('second save keeps both Oklab methods', sum(v.get(method) == 'oklab' for v in ET.parse(second_svg).getroot().iter()) == 2)
    legacy = out / 'legacy.ai'
    _, log = run('legacy', source, legacy, 'legacy7')
    check('Illustrator 7 refuses Perceptual by name', 'Perceptual gradients (Illustrator 7)' in log and not legacy.exists())
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)
    (out / 'report.json').write_text(json.dumps(dict(checks=checks), indent=2), encoding='utf-8')
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out}')
    return int(any(not c['passed'] for c in checks))


if __name__ == '__main__':
    raise SystemExit(main())
