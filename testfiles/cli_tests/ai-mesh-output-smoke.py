"""Editable mesh output: RGB and CMYK grids, alpha, curved boundaries, holes, transforms,
and masks; unsafe translucent overlaps refuse without replacing a destination.
Synthetic controls only; sends no desktop input. Illustrator qualification is separate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

SVG = '{http://www.w3.org/2000/svg}'
WIDTH, HEIGHT = 480, 320
NAMES = ['grid with corner opacity', 'ellipse', 'compound hole', 'curved patch',
         'reflected and transformed', 'masked mesh']
# Exclude the outer two pixels, where two independently antialiased clips differ.
REGIONS = {name: (22 + i % 3 * 150, 22 + i // 3 * 150,
                  118 + i % 3 * 150, 118 + i // 3 * 150) for i, name in enumerate(NAMES)}


def control(mode):
    rgb = ['#ff0000', '#00ff00', '#0000ff', '#ffffff', '#ffff00', '#00ffff', '#ff00ff', '#808080', '#000000']
    cmyk = ['0 1 1 0', '1 0 1 0', '1 1 0 0', '0 0 0 0', '0 0 1 0', '1 0 0 0',
            '0 1 0 0', '0 0 0 0.5', '0 0 0 1']
    colors = rgb if mode == 'RGB' else [f'device-cmyk({c})' for c in cmyk]

    def stop(path, color=None):
        style = '' if color is None else f' style="stop-color:{colors[color]};stop-opacity:{0.5 if color == 2 else 1}"'
        return f'<stop path="{path}"{style}/>'

    grid = ('<meshgradient id="grid" x="0" y="0" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>' +
            stop('l 50,0', 0) + stop('l 0,50', 1) + stop('l -50,0', 2) + stop('l 0,-50', 3) +
            '</meshpatch><meshpatch>' + stop('l 50,0') + stop('l 0,50', 4) + stop('l -50,0', 5) +
            '</meshpatch></meshrow><meshrow><meshpatch>' + stop('l 0,50') + stop('l -50,0', 6) +
            stop('l 0,-50', 7) + '</meshpatch><meshpatch>' + stop('l 0,50') + stop('l -50,0', 8) +
            '</meshpatch></meshrow></meshgradient>')
    sides = ['C 30,-8 70,8 100,0', 'C 95,30 105,70 100,100',
             'C 70,95 30,105 0,100', 'C -5,70 5,30 0,0']
    curved = ('<meshgradient id="curve" x="0" y="0" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>' +
              ''.join(stop(p, i) for i, p in enumerate(sides)) + '</meshpatch></meshrow></meshgradient>')
    boxed = grid.replace('id="grid"', 'id="box"').replace('userSpaceOnUse', 'objectBoundingBox').replace('50', '0.5')
    defs = (grid + curved + boxed + '<meshgradient id="turned" href="#grid" gradientUnits="userSpaceOnUse" '
            'gradientTransform="matrix(1 .1 .1 .9 -5 0)"/>'
            '<linearGradient id="fade"><stop stop-color="#000000"/><stop offset="1" stop-color="#ffffff"/>'
            '</linearGradient><mask id="mask" maskUnits="userSpaceOnUse" x="0" y="0" width="100" height="100">'
            '<rect width="100" height="100" fill="url(#fade)"/></mask>')
    cells = ['<rect width="100" height="100" fill="url(#grid)"/>',
             '<ellipse cx="50" cy="50" rx="50" ry="50" fill="url(#grid)" opacity=".6"/>',
             '<path d="M0 0 H100 V100 H0 Z M30 30 H70 V70 H30 Z" fill-rule="evenodd" fill="url(#grid)"/>',
             '<path d="M0 0 ' + ' '.join(sides) + ' Z" fill="url(#curve)"/>',
             '<g transform="translate(100 0) scale(-1 1)"><rect width="100" height="100" '
             'fill="url(#turned)" fill-opacity=".7" opacity=".8"/></g>',
             '<rect width="100" height="100" fill="url(#box)" mask="url(#mask)" opacity=".8"/>']
    art = ''.join(f'<g transform="translate({20 + i % 3 * 150} {20 + i // 3 * 150})">{cell}</g>'
                  for i, cell in enumerate(cells))
    # A final ordinary paint also catches a leaked clip, opacity, or mask.
    color = '#ff8000' if mode == 'RGB' else 'device-cmyk(0 .5 1 0)'
    art += f'<rect x="450" y="290" width="20" height="20" fill="{color}"/>'
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="{mode}">'
            f'<defs>{defs}</defs>{art}</svg>')


def compare(a_png, b_png):
    with Image.open(a_png) as a, Image.open(b_png) as b:
        a, b = np.asarray(a.convert('RGB'), dtype=np.int16), np.asarray(b.convert('RGB'), dtype=np.int16)
    if a.shape != b.shape:
        return {'size': dict(over24=1, max=255)}
    regions = dict(REGIONS, after=(450, 290, 470, 310))
    return {name: dict(max=int(d.max()), over24=float((d > 24).mean()))
            for name, (x0, y0, x1, y1) in regions.items()
            for d in [np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).max(axis=2)]}


def folded_control():
    # Four half-transparent patches cover the same square. Painting them independently
    # changes both colour and coverage. This is intentionally outside the writer subset.
    sides = [[['l 100,0', 'l 0,100', 'l -100,0', 'l 0,-100'],
              ['l -100,0', 'l 0,100', 'l 100,0']],
             [['l 0,-100', 'l -100,0', 'l 0,100'], ['l 0,-100', 'l 100,0']]]
    colors = iter(['#ff0000', '#00ff00', '#0000ff', '#ffff00',
                   '#00ffff', '#ff00ff', '#ff8000', '#808080', '#000000'])
    rows = []
    for r, row in enumerate(sides):
        patches = []
        for c, patch in enumerate(row):
            stops = []
            for i, path in enumerate(patch):
                inherited = (r or c) and i == 0
                style = '' if inherited else f' style="stop-color:{next(colors)};stop-opacity:.5"'
                stops.append(f'<stop path="{path}"{style}/>')
            patches.append('<meshpatch>' + ''.join(stops) + '</meshpatch>')
        rows.append('<meshrow>' + ''.join(patches) + '</meshrow>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">'
            '<defs><meshgradient id="m" x="10" y="10" gradientUnits="userSpaceOnUse">' +
            ''.join(rows) + '</meshgradient></defs>'
            '<rect x="10" y="10" width="100" height="100" fill="url(#m)"/></svg>')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', required=True, type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[name] = str(out / name.lower())
    env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    exe = args.inkscape.resolve()
    checks = []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name + ' ' + str(detail), flush=True)

    def run(label, *command, ok=True):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        if ok and p.returncode:
            raise RuntimeError(f'{label}: exit {p.returncode}; see log')
        return p

    def png(label, source):
        target = out / (label + '.png')
        p = run(label + '-png', exe, source, '--export-type=png', f'--export-filename={target}',
                '--export-area-page', '--export-dpi=96', '--export-background=white')
        return target, (p.stdout + p.stderr).decode('utf-8', 'replace')

    def records_of(ai):
        from pypdf import PdfReader
        private = PdfReader(ai, strict=True).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        return subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'],
                              input=packed[len(b'%AI24_ZStandard_Data'):], capture_output=True,
                              timeout=60, check=True).stdout.decode('latin-1')

    def close(results):
        return all(r['over24'] <= .02 for r in results.values())

    for mode in ('RGB', 'CMYK'):
        stem = mode.lower()
        source = out / f'{stem}-control.svg'
        source.write_text(control(mode), encoding='utf-8')
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        original, _ = png(stem + '-control', source)
        ai = out / f'{stem}-control.ai'
        run(stem + '-save', exe, source, f'--export-filename={ai}', '--export-extension=org.inkscape.output.ai.native')
        records = records_of(ai)
        (out / f'{stem}-records.ps').write_text(records, encoding='latin-1')
        check(f'{mode}: 21 editable patches', records.count('/Mesh X!') == 21 and records.count('%_[1 1] /Size X#') == 21)
        check(f'{mode}: the corners keep their colour model', records.count(f'/Device{mode} /CS X#') == 21)
        check(f'{mode}: the compound clipping rule is written', '1 XR' in records and '*u\n' in records)
        check(f'{mode}: the mask is attached', '(AI9_Mask)' in records)
        reopened, log = png(stem + '-reopened', ai)
        check(f'{mode}: opens natively', 'opens from its PDF page' not in log, log.strip()[-200:])
        result = compare(original, reopened)
        check(f'{mode}: native reopen matches', close(result), result)
        svg = out / f'{stem}-reopened.svg'
        run(stem + '-svg', exe, ai, f'--export-filename={svg}')
        tree = ET.parse(svg)
        meshes = list(tree.iter(SVG + 'meshgradient'))
        check(f'{mode}: 21 editable patches reopen', len(meshes) == 21 and
              sorted(len(list(m.iter(SVG + 'meshpatch'))) for m in meshes) == [1] * 21)
        again = out / f'{stem}-again.ai'
        run(stem + '-again', exe, ai, f'--export-filename={again}', '--export-extension=org.inkscape.output.ai.native')
        repeated, log = png(stem + '-again', again)
        check(f'{mode}: repeated save keeps 21 native meshes', records_of(again).count('/Mesh X!') == 21 and
              'opens from its PDF page' not in log and close(compare(original, repeated)))
        # A CMYK document's canvas blends mesh corners in CMYK, as Illustrator does and as a CMYK
        # shading in the PDF is blended (PROOF-document-colors.md, "A CMYK document's gradients
        # and meshes blended in CMYK on the screen"); Poppler converts the corners to RGB first
        # and blends there (10 to 21 levels from Illustrator on average, the canvas under 3). So
        # what Poppler can show, the same patches and corners in the same places, is checked
        # against the canvas's RGB blend of the same corners: the control as an RGB document,
        # whose CMYK paint shows through the same working profile.
        reference = original
        if mode == 'CMYK':
            rgb_blend = out / 'cmyk-rgb-blend.svg'
            rgb_blend.write_text(control(mode).replace('proof:color-mode="CMYK"', 'proof:color-mode="RGB"'),
                                 encoding='utf-8')
            reference, _ = png('cmyk-rgb-blend', rgb_blend)
        for label, document in [('ai-page', ai), ('pdf', out / f'{stem}-control.pdf')]:
            if label == 'pdf':
                run(stem + '-pdf', exe, source, f'--export-filename={document}')
            target = out / f'{stem}-{label}'
            # Splash misdraws translucent mesh soft masks. Cairo and Illustrator qualify their
            # appearance; opaque meshes are checked separately with Splash below.
            renderer = (args.pdftoppm or args.dll_dir / 'pdftoppm.exe').with_name('pdftocairo.exe')
            run(stem + '-' + label, renderer, '-r', '96', '-png',
                '-singlefile', document, target)
            result = compare(reference, target.with_suffix('.png'))
            check(f'{mode}: {label} matches the canvas' + (' blended in RGB' if mode == 'CMYK' else ''),
                  close(result), result)
        opaque = out / f'{stem}-opaque.svg'
        opaque.write_text(control(mode).replace('stop-opacity:0.5', 'stop-opacity:1'), encoding='utf-8')
        opaque_png, _ = png(stem + '-opaque', opaque)
        opaque_pdf = out / f'{stem}-opaque.pdf'
        run(stem + '-opaque-pdf', exe, opaque, f'--export-filename={opaque_pdf}')
        target = out / f'{stem}-opaque-splash'
        run(stem + '-opaque-splash', args.pdftoppm or args.dll_dir / 'pdftoppm.exe', '-r', '96', '-png',
            '-singlefile', opaque_pdf, target)
        result = compare(opaque_png, target.with_suffix('.png'))
        (out / f'{stem}-splash-diagnostic.json').write_text(json.dumps(result, indent=2))
        if mode == 'RGB':
            check('RGB: opaque untransformed meshes match in Splash',
                  all(result[name]['over24'] <= .02 for name in NAMES[:4]), result)
        legacy = out / f'{stem}-legacy.ai'
        p = run(stem + '-legacy', exe, source, f'--export-filename={legacy}',
                '--export-extension=org.inkscape.output.ai.legacy7', ok=False)
        check(f'{mode}: Illustrator 7 refuses meshes by name', p.returncode == 1 and not legacy.exists() and
              b'mesh gradients (Illustrator 7)' in p.stderr)
        check(f'{mode}: source unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)

    source = out / 'folded.svg'
    source.write_text(folded_control(), encoding='utf-8')
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    destination = out / 'folded.ai'
    previous = b'Existing destination must survive a refused native save.\n'
    destination.write_bytes(previous)
    p = run('folded-existing', exe, source, f'--export-filename={destination}',
            '--export-extension=org.inkscape.output.ai.native', ok=False)
    check('folded: native save refuses by name', p.returncode == 1 and
          b'translucent mesh gradients outside the non-overlapping patch subset' in p.stderr)
    check('folded: existing destination is untouched', destination.read_bytes() == previous)
    destination = out / 'folded-new.ai'
    p = run('folded-new', exe, source, f'--export-filename={destination}',
            '--export-extension=org.inkscape.output.ai.native', ok=False)
    check('folded: no partial new output', p.returncode == 1 and not destination.exists())
    check('folded: source unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)

    report = dict(checks=checks, passed=sum(c['passed'] for c in checks), total=len(checks))
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'{report["passed"]}/{report["total"]} checks passed. {out / "report.json"}')
    return int(report['passed'] != report['total'])


if __name__ == '__main__':
    raise SystemExit(main())
