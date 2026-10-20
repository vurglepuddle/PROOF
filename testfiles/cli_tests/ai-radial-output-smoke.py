"""Opaque radial gradient writing through both .ai formats; synthetic artwork only.

Circles with and without a focal point go to both formats; ellipses (a gradient transform,
an object-bounding-box gradient on an oblong, a skewed object) to the modern one, and the
Illustrator 7 form refuses them by name. Each file is reopened and drawn, and compared with
the canvas. The modern file's PDF page must be PROOF's own PDF export of the same drawing, and
is compared with the canvas too, more loosely (PDF_TOLERANCE).

Without a profile the canvas and Poppler convert DeviceCMYK differently, whatever the paint:
pure cyan is 0 255 255 on the canvas and 0 173 239 in Poppler, and these stops differ by a
median of 23 inside a radial (2026-10-20). So the canvas is compared with the PDF page at the
RGB shapes only; the CMYK ones are checked through their records and by the page being
PROOF's own export. (With large cyan areas the difference alone sends a file written from an
RGB document to its page on reopening: todo.md, 2026-10-20.)
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops
from pypdf import PdfReader

HEAD = '''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
  xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="430" height="320" viewBox="0 0 430 320">
  <defs><linearGradient id="rgb"><stop stop-color="#ff0000"/><stop offset="0.45" stop-color="#ffd000"/>
  <stop offset="1" stop-color="#0030ff"/></linearGradient>
  <linearGradient id="cmyk"><stop style="stop-color:device-cmyk(0 0 1 0)"/>
  <stop offset="0.5" style="stop-color:device-cmyk(0 1 1 0)"/>
  <stop offset="1" style="stop-color:device-cmyk(1 0.2 0 0.3)"/></linearGradient>
  <radialGradient id="a" xlink:href="#rgb" gradientUnits="userSpaceOnUse" cx="80" cy="80" r="60"/>
  <radialGradient id="b" xlink:href="#cmyk" gradientUnits="userSpaceOnUse" cx="230" cy="80" r="60" fx="255" fy="60"/>
  <radialGradient id="t" xlink:href="#rgb" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="50" fx="12" fy="-20"
    gradientTransform="translate(80 240) rotate(20)"/>
'''
# Ellipses: a gradient transform, an object-bounding-box gradient on an oblong, and a circle on
# a skewed object.
ELLIPSES = '''  <radialGradient id="c" xlink:href="#rgb" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="60" fx="25" fy="5"
    gradientTransform="translate(360 80) rotate(30) scale(1 0.5)"/>
  <radialGradient id="d" xlink:href="#cmyk" fx="0.35" fy="0.4"/>
  <radialGradient id="e" xlink:href="#rgb" gradientUnits="userSpaceOnUse" cx="320" cy="240" r="45"/>
'''
BODY = '''</defs><g inkscape:groupmode="layer" inkscape:label="Radial">
  <rect x="20" y="20" width="120" height="120" fill="url(#a)"/>
  <rect x="170" y="20" width="120" height="120" fill="url(#b)"/>
  <rect x="25" y="185" width="110" height="110" fill="url(#t)"/>
'''
ELLIPSE_BODY = '''  <rect x="300" y="20" width="120" height="120" fill="url(#c)"/>
  <rect x="150" y="180" width="110" height="60" fill="url(#d)"/>
  <rect x="280" y="200" width="80" height="80" transform="matrix(1 0 0.3 1 -72 0)" fill="url(#e)"/>
'''
TAIL = '</g></svg>'
# Inside each shape, along its gradient: centres, focal sides and outer bands.
CMYK_POINTS = [(230, 80), (255, 62), (205, 110), (280, 80)]
CIRCLE_POINTS = [(80, 80), (100, 80), (80, 120), (50, 55)] + CMYK_POINTS + [(80, 240), (95, 225), (60, 270),
                                                                         (115, 255)]
# Poppler draws a radial shading in steps and the canvas dithers its gradients: on the same
# drawing they differ by up to 11 inside RGB radials at 96 dpi, evenly about the gradient's own
# ellipse (2026-10-20). A misplaced axis or focal point differs by tens on one side.
PDF_TOLERANCE = 12
ELLIPSE_CMYK_POINTS = [(205, 210), (175, 195), (240, 230), (248, 232), (230, 215)]
ELLIPSE_POINTS = [(360, 80), (385, 85), (330, 60), (400, 110)] + ELLIPSE_CMYK_POINTS + [(320, 240), (300, 220),
                                                                                     (340, 265), (300, 270)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        folder = out / name.lower()
        folder.mkdir()
        env[name] = str(folder)
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    checks = []

    def check(name, ok, detail=''):
        checks.append(dict(name=name, passed=bool(ok), detail=detail))
        print(('PASS ' if ok else 'FAIL ') + name + (f' {detail}' if detail != '' else ''), flush=True)

    def run(label, *command, success=True):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        if success and p.returncode:
            raise RuntimeError(f'{label}: exit {p.returncode}; see log')
        return p

    def deltas(a_png, b_png, points):
        with Image.open(a_png) as a, Image.open(b_png) as b:
            a, b = a.convert('RGB'), b.convert('RGB')
            return [max(abs(x - y) for x, y in zip(a.getpixel(p), b.getpixel(p))) for p in points]

    exe = args.inkscape.resolve()
    svg_ns = '{http://www.w3.org/2000/svg}'
    controls = {
        'modern': (HEAD + ELLIPSES + BODY + ELLIPSE_BODY + TAIL, 'native', 6, CIRCLE_POINTS + ELLIPSE_POINTS),
        'legacy': (HEAD + BODY + TAIL, 'legacy7', 3, CIRCLE_POINTS),
    }
    for name, (svg, extension, count, points) in controls.items():
        source = out / (name + '-control.svg')
        source.write_text(svg)
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        canvas = out / (name + '-canvas.png')
        run(name + '-canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
        ai = out / (name + '.ai')
        option = '--export-extension=org.inkscape.output.ai.' + extension
        run(name + '-save', exe, source, option, f'--export-filename={ai}')
        check(name + ': file saved', ai.is_file() and ai.stat().st_size > 100)
        reopened = out / (name + '.svg')
        run(name + '-reopen', exe, ai, f'--export-filename={reopened}')
        tree = ET.parse(reopened).getroot()
        radials = list(tree.iter(svg_ns + 'radialGradient'))
        check(name + ': native shapes with radial fills', len(list(tree.iter(svg_ns + 'path'))) == count
              and not list(tree.iter(svg_ns + 'image')) and len(radials) == count, len(radials))
        focal = sum(r.get('fx') is not None for r in radials)
        check(name + ': focal points kept', focal == (4 if name == 'modern' else 2), focal)
        stops = list(tree.iter(svg_ns + 'stop'))
        cmyk = sum('device-cmyk' in s.get('style', '') for s in stops)
        check(name + ': CMYK stops retained', cmyk == (6 if name == 'modern' else 3), cmyk)
        preview = out / (name + '-reopened.png')
        run(name + '-preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
        d = deltas(canvas, preview, points)
        check(name + ': radial placement after reopen', max(d) <= 4, d)
        if name == 'modern':
            check('modern: PDF compatible', PdfReader(ai).pdf_header == '%PDF-1.7')
            prefix = out / 'modern-pdf'
            run('modern-pdf', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', ai, prefix)
            direct = out / 'modern-direct.pdf'
            run('modern-direct', exe, source, f'--export-filename={direct}')
            run('modern-direct-png', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', direct,
                out / 'modern-direct')
            with Image.open(prefix.with_suffix('.png')) as a, Image.open(out / 'modern-direct.png') as b:
                same = max(max(px) for px in ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getdata())
            check("modern: PDF page is PROOF's own PDF export", same <= 1, same)
            rgb = [p for p in points if p not in CMYK_POINTS + ELLIPSE_CMYK_POINTS]
            d = deltas(canvas, prefix.with_suffix('.png'), rgb)
            check('modern: PDF radial placement matches canvas (RGB shapes)', max(d) <= PDF_TOLERANCE, d)
        else:
            data = ai.read_bytes()
            check('legacy: editable radial instances', data.count(b' 1 3 Bd\n') == 3 and data.count(b' Bh\n') == 3
                  and data.count(b' Bg\n') == 3 and b' Bm\n' not in data)
            # An ellipse is refused by name, and the file saved before is kept.
            refused = out / 'legacy-ellipse.svg'
            refused.write_text(HEAD + ELLIPSES + BODY + ELLIPSE_BODY + TAIL)
            kept = ai.read_bytes()
            p = run('legacy-ellipse', exe, refused, option, f'--export-filename={ai}', success=False)
            check('legacy: ellipses refused safely', p.returncode == 1
                  and b'elliptical radial gradients (Illustrator 7)' in p.stderr and ai.read_bytes() == kept)
        focal_radius = out / (name + '-focal-radius.svg')
        focal_radius.write_text(svg.replace('r="60"/>', 'r="60" fr="10"/>', 1))
        kept = ai.read_bytes()
        p = run(name + '-focal-radius', exe, focal_radius, option, f'--export-filename={ai}', success=False)
        check(name + ': focal radius refused safely', p.returncode == 1
              and b'radial gradients with a focal radius' in p.stderr and ai.read_bytes() == kept)
        check(name + ': working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == before)
    (out / 'report.json').write_text(json.dumps(checks, indent=2))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
