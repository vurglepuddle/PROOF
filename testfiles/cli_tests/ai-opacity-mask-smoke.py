# SPDX-License-Identifier: GPL-2.0-or-later
"""Check imported native opacity masks on the canvas and in vector PDF output.

Generate these hand-written fixtures by setting PROOF_AI_MASK_DIAG to a local
directory and running test_ai-native-import --gtest_filter=*OpacityMask*.
No production artwork, Illustrator or desktop input is used.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

from PIL import Image
from pypdf import PdfReader

# Name, probes in the 200 x 100 point document, expected grey (on white).
CASES = [
    ('mask-0', [(30, 60, 0), (70, 60, 0)]),
    ('mask-1', [(30, 60, 128), (70, 60, 0)]),
    ('mask-2', [(30, 60, 64), (70, 60, 0)]),
    ('mask-3', [(30, 60, 128), (70, 60, 0)]),
    ('mask-4', [(50, 50, 0), (50, 60, 255)]),
    ('mask-5', [(30, 60, 191), (70, 60, 128)]),
    ('gradient-0', [(30, 60, 128), (70, 60, 0)]),
    ('gradient-1', [(30, 60, 128), (70, 60, 255)]),
]


def image_objects(obj, seen):
    if hasattr(obj, 'get_object'):
        obj = obj.get_object()
    if id(obj) in seen:
        return 0
    seen.add(id(obj))
    if isinstance(obj, dict):
        return int(obj.get('/Subtype') == '/Image') + sum(image_objects(v, seen) for v in obj.values())
    if isinstance(obj, list):
        return sum(image_objects(v, seen) for v in obj)
    return 0


def pdf_objects(obj, seen):
    if hasattr(obj, 'get_object'):
        obj = obj.get_object()
    if id(obj) in seen:
        return
    seen.add(id(obj))
    if isinstance(obj, dict):
        yield obj
        for value in obj.values():
            yield from pdf_objects(value, seen)
    elif isinstance(obj, list):
        for value in obj:
            yield from pdf_objects(value, seen)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fixtures', required=True, type=Path)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--pdftoppm', required=True, type=Path)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--case', action='append', choices=[name for name, _ in CASES] + ['cmyk-gradient'],
                    help='run only this synthetic case (repeatable); default: all cases')
    args = ap.parse_args()
    selected = set(args.case or [name for name, _ in CASES] + ['cmyk-gradient'])
    for name in [name for name, _ in CASES] + ['cmyk-gradient']:
        if name not in selected:
            continue
        if not (args.fixtures / (name + '.svg')).is_file():
            ap.error(f'missing generated fixture {name}.svg; run the native-import mask tests first')
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
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{label}: exit {result.returncode}; see log')

    for name, probes in CASES:
        if name not in selected:
            continue
        source = (args.fixtures / (name + '.svg')).resolve()
        png, pdf = out / (name + '.png'), out / (name + '.pdf')
        run(name + '-canvas', [args.inkscape.resolve(), source, f'--export-filename={png}', '--export-dpi=144'])
        run(name + '-pdf', [args.inkscape.resolve(), source, f'--export-filename={pdf}'])
        run(name + '-page', [args.pdftoppm.resolve(), '-f', '1', '-singlefile', '-r', '144', '-png', pdf, out / (name + '-page')])
        for what, path in (('canvas', png), ('PDF', out / (name + '-page.png'))):
            picture = Image.open(path).convert('RGBA')
            picture = Image.alpha_composite(Image.new('RGBA', picture.size, 'white'), picture).convert('RGB')
            for x, y, expected in probes + [(120, 90, 255)]:
                pixel = picture.getpixel((int(x / 200 * picture.width), int(y / 100 * picture.height)))
                check(f'{name}: {what} at {x},{y}', all(abs(c - expected) <= 4 for c in pixel),
                      f'{pixel}, expected {expected}')
        images = image_objects(PdfReader(pdf).pages[0], set())
        check(name + ': PDF remains vector', images == 0, f'{images} image objects')
    # Ordinary CMYK gradients must keep their four channels alongside RGB mask
    # coverage, including when the document assigns a CMYK ICC profile.
    if 'cmyk-gradient' in selected:
        pdf = out / 'cmyk-gradient.pdf'
        run('cmyk-gradient-pdf', [args.inkscape.resolve(), (args.fixtures / 'cmyk-gradient.svg').resolve(),
                                  f'--export-filename={pdf}'])
        page = PdfReader(pdf).pages[0]
        shadings = [obj for obj in pdf_objects(page, set())
                    if obj.get('/ShadingType') == 2 and obj.get('/ColorSpace') == '/DeviceCMYK']
        check('CMYK gradient: device space', bool(shadings))
        endpoints = []
        for shading in shadings:
            function = shading['/Function'].get_object()
            endpoints += [list(map(float, function.get(key, []))) for key in ('/C0', '/C1')]
        check('CMYK gradient: exact process channels', [0, 0, 0, 1] in endpoints and [0, 0, 0, 0] in endpoints,
              str(endpoints))
        check('CMYK gradient: PDF remains vector', image_objects(page, set()) == 0)
    result = dict(checks=checks, passed=all(c['passed'] for c in checks))
    (out / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
