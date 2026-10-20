# SPDX-License-Identifier: GPL-2.0-or-later
"""Check importer-generated foreign PDF vectors on canvas and in exported PDF.

Set PROOF_AI_FOREIGN_DIAG and run test_ai-native-import --gtest_filter=*Foreign*
to generate the synthetic inputs. No artwork or desktop input is used.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

from PIL import Image
from pypdf import PdfReader
from pypdf.generic import ContentStream

CASES = {
    'foreign-0': [(20, 70, 'paint'), (30, 55, 'black'), (8, 55, 'white')],
    'foreign-1': [(60, 70, 'paint'), (45, 65, 'black'), (45, 84, 'white')],
    'foreign-2': [(11, 56, 'paint'), (16.5, 39, 'black'), (42, 34, 'white')],
    'foreign-gradients': [(20, 65, 'paint'), (80, 65, 'paint'), (120, 90, 'white')],
}
# Their paint is device-cmyk(0.2 0.4 0.6 0.1) in an RGB document. The canvas shows it through
# the working CMYK profile, as Illustrator does; Poppler draws the page with its formula, fitted
# to SWOP. With GRACoL 2006 they are 14 apart, (191, 146, 103) against (185, 135, 89); with SWOP
# itself 15 (2026-10-20). Paint that missed its place is a hundred or more from white.
CMYK_CASES = ('foreign-0', 'foreign-1', 'foreign-2')
CMYK_TOLERANCE = 24


def objects(obj, seen):
    obj = obj.get_object() if hasattr(obj, 'get_object') else obj
    if id(obj) in seen:
        return
    seen.add(id(obj))
    if isinstance(obj, dict):
        yield obj
        for value in obj.values():
            yield from objects(value, seen)
    elif isinstance(obj, list):
        for value in obj:
            yield from objects(value, seen)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('fixtures', 'inkscape', 'pdftoppm', 'output'):
        ap.add_argument('--' + name, required=True, type=Path)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--case', action='append', choices=list(CASES))
    args = ap.parse_args()
    selected = args.case or list(CASES)
    for name in selected:
        if not (args.fixtures / (name + '.svg')).is_file():
            ap.error(f'missing {name}.svg; generate the native-import foreign fixtures first')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C')
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    for name in ('INKSCAPE_PROFILE_DIR', 'XDG_CACHE_HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME'):
        folder = out / name.lower()
        folder.mkdir()
        env[name] = str(folder)
    checks = []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        if not passed:
            print('FAIL', name, detail, flush=True)

    def run(label, command):
        result = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{label}: exit {result.returncode}; see log')

    for name in selected:
        source = (args.fixtures / (name + '.svg')).resolve()
        png, pdf = out / (name + '.png'), out / (name + '.pdf')
        run(name + '-canvas', [args.inkscape.resolve(), source, f'--export-filename={png}', '--export-dpi=144'])
        run(name + '-pdf', [args.inkscape.resolve(), source, f'--export-filename={pdf}'])
        run(name + '-page', [args.pdftoppm.resolve(), '-f', '1', '-singlefile', '-r', '144', '-png', pdf, out / (name + '-page')])
        pictures = []
        for path in (png, out / (name + '-page.png')):
            im = Image.open(path).convert('RGBA')
            pictures.append(Image.alpha_composite(Image.new('RGBA', im.size, 'white'), im).convert('RGB'))
        for x, y, kind in CASES[name]:
            colors = [im.getpixel((int(x / 200 * im.width), int(y / 100 * im.height))) for im in pictures]
            # RGB-document canvas black is ideal black; PDF's ICC conversion of
            # 100K is a dark ink simulation. Verify darkness and exact K below,
            # rather than demanding that a print black render as RGB zero.
            if kind != 'black':
                tolerance = CMYK_TOLERANCE if kind == 'paint' and name in CMYK_CASES else 10
                check(f'{name}: canvas/PDF agree at {x},{y}', max(abs(a - b) for a, b in zip(*colors)) <= tolerance, str(colors))
            for label, color in zip(('canvas', 'PDF'), colors):
                valid = max(color) <= 48 if kind == 'black' else min(color) >= 247 if kind == 'white' else min(color) < 220
                check(f'{name}: {label} {kind} at {x},{y}', valid, str(color))
        reader = PdfReader(pdf)
        page = reader.pages[0]
        all_objects = list(objects(page, set()))
        check(name + ': PDF remains vector', not any(obj.get('/Subtype') == '/Image' for obj in all_objects))
        if name == 'foreign-gradients':
            check(name + ': editable gradients export as shadings', sum(obj.get('/ShadingType') == 2 for obj in all_objects) >= 2)
        else:
            streams = [page.get_contents()] + [obj for obj in all_objects if obj.get('/Subtype') == '/Form']
            colors = [list(map(float, values)) for stream in streams if stream is not None
                      for values, op in ContentStream(stream, reader).operations if op in (b'k', b'scn') and len(values) == 4]
            check(name + ': CMYK channels retained', any(max(abs(a - b) for a, b in zip(v, (0.2, 0.4, 0.6, 0.1))) < 1e-4 for v in colors), str(colors))
            strokes = [list(map(float, values)) for stream in streams if stream is not None
                       for values, op in ContentStream(stream, reader).operations if op in (b'K', b'SCN') and len(values) == 4]
            check(name + ': grey stroke stays on black', [0, 0, 0, 1] in strokes, str(strokes))
    result = dict(checks=checks, passed=all(c['passed'] for c in checks))
    (out / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
