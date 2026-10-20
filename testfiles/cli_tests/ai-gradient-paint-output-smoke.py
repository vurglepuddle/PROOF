"""Gradient paint beyond plain fills through both .ai formats: gradients on strokes, gradient
fills with plain strokes, and tints of spot inks in gradients. Synthetic artwork only.

Illustrator writes a stroke's gradient as the fill's instance opened with `1 Bb`, about an S or
s, and ends any instance with what its operator does to the stroke: `0 BB` after f, `1 BB` after
S or B, `2 BB` after s or b (Adobe's 1998 specification, table 10). Every gradient stroke in
Illustrator's files is on an unfilled path; a gradient fill with a plain stroke is a b or B in
the fill's instance. A spot stop is its ink's CMYK, name and tint (from 1), colour style 3.

The modern control has linear and radial strokes, open and closed, a CMYK one, a compound path
and a turned elliptical radial, two gradient fills with plain strokes, and a spot ink's tints
in a fill and a stroke. Its file reopens from its own records and matches the canvas; its PDF
page is PROOF's own PDF export, and is compared with the canvas at the RGB shapes
(PDF_TOLERANCE, as in the radial smoke). Illustrator 7 has no gradient strokes: its control
has the two fills with plain strokes and the spot fill, and the modern control is refused by
name. Refusals keep the previous file.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops
from pypdf import PdfReader

HEAD = '''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
  xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:proof="urn:x-proof:document:1"
  width="430" height="400" viewBox="0 0 430 400">
  <defs><linearGradient id="rgb"><stop stop-color="#ff0000"/><stop offset="0.45" stop-color="#ffd000"/>
  <stop offset="1" stop-color="#0030ff"/></linearGradient>
  <linearGradient id="cmyk"><stop style="stop-color:device-cmyk(0 0 1 0)"/>
  <stop offset="0.5" style="stop-color:device-cmyk(0 1 1 0)"/>
  <stop offset="1" style="stop-color:device-cmyk(1 0.2 0 0.3)"/></linearGradient>
  <linearGradient id="la" xlink:href="#rgb" gradientUnits="userSpaceOnUse" x1="30" y1="0" x2="190" y2="0"/>
  <radialGradient id="rb" xlink:href="#rgb" gradientUnits="userSpaceOnUse" cx="310" cy="70" r="90" fx="330" fy="50"/>
  <linearGradient id="lc" xlink:href="#cmyk"/>
  <linearGradient id="ld" xlink:href="#rgb" gradientUnits="userSpaceOnUse" x1="200" y1="160" x2="300" y2="260"/>
  <linearGradient id="le" xlink:href="#rgb"/>
  <linearGradient id="lf" xlink:href="#rgb" x1="0" y1="0" x2="0" y2="1"/>
  <radialGradient id="rt" xlink:href="#rgb" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="60"
    gradientTransform="translate(110 290) rotate(15) scale(1 0.4)"/>
  <linearGradient id="spot" gradientUnits="userSpaceOnUse" x1="20" y1="0" x2="200" y2="0">
  <stop style="stop-color:device-cmyk(0 1 0.8 0.1)" proof:ink="Brand Red" proof:ink-alternate="device-cmyk(0 1 0.8 0.1)"/>
  <stop offset="0.5" style="stop-color:device-cmyk(0 0.2 0.16 0.02)" proof:ink="Brand Red"
    proof:ink-alternate="device-cmyk(0 1 0.8 0.1)" proof:ink-tint="0.2"/>
  <stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/></linearGradient>
  <linearGradient id="spot2" xlink:href="#spot" gradientUnits="userSpaceOnUse" x1="230" y1="0" x2="410" y2="0"/>
  </defs><g inkscape:groupmode="layer" inkscape:label="Paint">
'''
SPOT_FILL = '''  <rect x="20" y="320" width="180" height="60" fill="url(#spot)"/>
'''
SPOT_STROKE = '''  <path d="M230 350 L410 350" fill="none" stroke="url(#spot2)" stroke-width="20"/>
'''
STROKES = '''  <path d="M30 40 L190 40" fill="none" stroke="url(#la)" stroke-width="24"/>
  <rect x="230" y="20" width="160" height="100" fill="none" stroke="url(#rb)" stroke-width="16"/>
  <circle cx="90" cy="200" r="55" fill="none" stroke="url(#lc)" stroke-width="14"/>
  <path d="M200 160 H300 V260 H200 Z M225 185 V235 H275 V185 Z" fill="none" stroke="url(#ld)" stroke-width="10"/>
  <path d="M20 290 L200 290" fill="none" stroke="url(#rt)" stroke-width="20"/>
'''
FILLS = '''  <rect x="330" y="160" width="80" height="60" fill="url(#le)" stroke="#000000" stroke-width="4"/>
  <path d="M330 250 L410 250 L370 300" fill="url(#lf)" stroke="#202020" stroke-width="3"/>
'''
TAIL = '</g></svg>'
# Well inside each stroke, along its gradient, and inside the filled shapes and on their strokes.
STROKE_POINTS = [(40, 40), (80, 35), (110, 45), (150, 40), (185, 40),             # open linear
                 (230, 70), (310, 20), (390, 70), (310, 120), (385, 118),          # closed radial
                 (200, 210), (250, 160), (300, 210), (250, 260),                   # compound, outer
                 (225, 210), (250, 185), (275, 210), (250, 235),                   # compound, inner
                 (40, 290), (90, 285), (110, 290), (130, 295), (180, 290)]         # turned ellipse
CMYK_POINTS = [(35, 200), (145, 200), (90, 145), (90, 255), (129, 161)]
FILL_POINTS = [(345, 190), (370, 190), (395, 190), (330, 190), (370, 262), (350, 256), (390, 256), (360, 250)]
SPOT_FILL_POINTS = [(30, 350), (80, 340), (110, 350), (150, 360), (190, 350)]
SPOT_STROKE_POINTS = [(240, 350), (290, 345), (320, 350), (360, 355), (400, 350)]
# A CMYK document in a profile of its own, as the import leaves a CMYK .ai that has one: its
# process paints, stops too, are icc-color() in that profile (PROFILE: the test data's).
ICC = '''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
  xmlns:proof="urn:x-proof:document:1" proof:color-mode="CMYK" proof:color-profile="Press"
  width="300" height="160" viewBox="0 0 300 160"><defs>
  <color-profile id="p" name="Press" xlink:href="PROFILE" rendering-intent="relative-colorimetric"/>
  <linearGradient id="g" gradientUnits="userSpaceOnUse" x1="20" y1="0" x2="200" y2="0">
  <stop style="stop-color:#cd2468 icc-color(Press, 0.05, 1, 0.45, 0.22)"/>
  <stop offset="1" style="stop-color:#00a0e0 icc-color(Press, 1, 0, 0, 0)"/></linearGradient></defs>
  <rect x="20" y="20" width="180" height="120" fill="url(#g)"/>
  <rect x="220" y="20" width="60" height="120" style="fill:#cd2468 icc-color(Press, 0.05, 1, 0.45, 0.22)"/>
</svg>'''
ICC_POINTS = [(30, 80), (70, 60), (110, 80), (150, 100), (190, 80), (250, 80)]
# Poppler draws radial shadings in steps and the canvas dithers its gradients: up to 11 apart on
# the same drawing inside RGB radials at 96 dpi (the radial smoke, 2026-10-20).
PDF_TOLERANCE = 12


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

    def records(ai, modern):
        """The file's records: a modern file's are Zstandard in its private data, decoded with the
        toolchain's zstd beside the DLLs."""
        if not modern:
            return ai.read_bytes()
        private = PdfReader(ai).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        prefix = b'%AI24_ZStandard_Data'
        zstd = (args.dll_dir.resolve() if args.dll_dir else Path()) / 'zstd.exe'
        p = subprocess.run([str(zstd), '-d', '-c'], input=packed[len(prefix):], capture_output=True, timeout=60)
        return p.stdout.replace(b'\r\n', b'\n').replace(b'\r', b'\n') if not p.returncode else b''

    def paint(element, name):
        style = dict(kv.split(':', 1) for kv in element.get('style', '').split(';') if ':' in kv)
        return style.get(name, element.get(name, ''))

    exe = args.inkscape.resolve()
    svg_ns = '{http://www.w3.org/2000/svg}'
    controls = {
        'modern': (HEAD + STROKES + FILLS + SPOT_FILL + SPOT_STROKE + TAIL, 'native',
                   STROKE_POINTS + CMYK_POINTS + FILL_POINTS + SPOT_FILL_POINTS + SPOT_STROKE_POINTS),
        'legacy': (HEAD + FILLS + SPOT_FILL + TAIL, 'legacy7', FILL_POINTS + SPOT_FILL_POINTS),
    }
    for name, (svg, extension, points) in controls.items():
        modern = name == 'modern'
        source = out / (name + '-control.svg')
        source.write_text(svg)
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        canvas = out / (name + '-canvas.png')
        run(name + '-canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
        ai = out / (name + '.ai')
        option = '--export-extension=org.inkscape.output.ai.' + extension
        run(name + '-save', exe, source, option, f'--export-filename={ai}')
        check(name + ': file saved', ai.is_file() and ai.stat().st_size > 100)
        data = records(ai, modern)
        strokes = 6 if modern else 0
        ends = {b'\nS\n1 BB\n': 3 if modern else 0, b'\ns\n2 BB\n': 3 if modern else 0,
                b'\nb\n2 BB\n': 1, b'\nB\n1 BB\n': 1, b'\nf\n0 BB\n': 1}
        counts = {k.decode().replace('\n', ' ').strip(): data.count(k) for k in ends}
        check(name + ': instances end as Illustrator ends them', data.count(b'\n1 Bb\n') == strokes
              and all(data.count(k) == v for k, v in ends.items()), counts)
        # Each spot gradient: the ink at full strength and at 20% (written 0.8), declared once.
        spot_stops = (data.count(b'0 1 0.8 0.1 (Brand Red) 0 3 50 0 %_Bs\n'),
                      data.count(b'0 1 0.8 0.1 (Brand Red) 0.8 3 50 50 %_Bs\n'))
        check(name + ': spot stops as tints of their ink', spot_stops == ((2, 2) if modern else (1, 1))
              and data.count(b'%%CMYKCustomColor: 0 1 0.8 0.1 (Brand Red)\n') == 1, spot_stops)
        reopened = out / (name + '.svg')
        p = run(name + '-reopen', exe, ai, f'--export-filename={reopened}')
        tree = ET.parse(reopened).getroot()
        paths = list(tree.iter(svg_ns + 'path'))
        check(name + ': reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr
              and not list(tree.iter(svg_ns + 'image')) and len(paths) == strokes + 3, len(paths))
        stroked = sum(paint(e, 'stroke').startswith('url(') and paint(e, 'fill') in ('none', '') for e in paths)
        filled = sum(paint(e, 'fill').startswith('url(') and paint(e, 'stroke').startswith('#') for e in paths)
        check(name + ': gradient strokes and stroked gradient fills', stroked == strokes and filled == 2,
              (stroked, filled))
        proof_ns = '{urn:x-proof:document:1}'
        inked = [s for s in tree.iter(svg_ns + 'stop') if s.get(proof_ns + 'ink') == 'Brand Red']
        tints = sorted(s.get(proof_ns + 'ink-tint', '1') for s in inked)
        check(name + ': spot stops reopen as tints of their ink', tints == (['0.2', '0.2', '1', '1'] if modern
                                                                            else ['0.2', '1']), tints)
        if modern:
            stops = sum('device-cmyk' in s.get('style', '') for s in tree.iter(svg_ns + 'stop'))
            check('modern: CMYK stroke stops retained', stops >= 3, stops)
        preview = out / (name + '-reopened.png')
        run(name + '-preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
        d = deltas(canvas, preview, points)
        check(name + ': placement after reopen', max(d) <= 4, d)
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
            d = deltas(canvas, prefix.with_suffix('.png'), STROKE_POINTS + FILL_POINTS)
            check('modern: PDF page matches canvas (RGB shapes)', max(d) <= PDF_TOLERANCE, d)
            refusals = [
                ('both', FILLS.replace('stroke="#000000"', 'stroke="url(#la)"'),
                 b'gradients on both the fill and the stroke'),
                ('filled', STROKES.replace('fill="none" stroke="url(#la)"', 'fill="#00ff00" stroke="url(#la)"'),
                 b'gradient strokes on filled objects'),
                ('alpha', STROKES.replace('stroke-width="24"', 'stroke-width="24" stroke-opacity="0.5"'),
                 b'gradient transparency'),
            ]
        else:
            refusals = [('strokes', STROKES + FILLS, b'gradients on strokes (Illustrator 7)')]
        for label, body, reason in refusals:
            path = out / (name + '-' + label + '.svg')
            path.write_text(HEAD + body + TAIL)
            kept = ai.read_bytes()
            p = run(name + '-' + label, exe, path, option, f'--export-filename={ai}', success=False)
            check(name + ': ' + label + ' refused safely', p.returncode == 1 and reason in p.stderr
                  and ai.read_bytes() == kept)
        check(name + ': working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == before)

    # ICC-tagged stops are written as their numbers, as solid paint is. The profile goes with the
    # PDF half, where the reader finds a file's own profile, and the file reopens in it.
    profile = Path(__file__).resolve().parent.parent / 'data' / 'colors' / 'default_cmyk.icc'
    source = out / 'icc-control.svg'
    source.write_text(ICC.replace('PROFILE', profile.as_uri()))
    canvas = out / 'icc-canvas.png'
    run('icc-canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    ai = out / 'icc.ai'
    run('icc-save', exe, source, '--export-extension=org.inkscape.output.ai.native', f'--export-filename={ai}')
    data = records(ai, True)
    check('icc: stops written as their numbers', b'0.05 1 0.45 0.22 1 50 0 %_Bs\n' in data
          and b'1 0 0 0 1 50 100 %_Bs\n' in data and b'%AI9_ColorModel: 2\n' in data)
    reopened = out / 'icc.svg'
    p = run('icc-reopen', exe, ai, f'--export-filename={reopened}')
    tree = ET.parse(reopened).getroot()
    check('icc: reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr
          and not list(tree.iter(svg_ns + 'image')) and len(list(tree.iter(svg_ns + 'path'))) == 2)
    def tags(icc):
        """An ICC profile's tags but its description, which LittleCMS writes anew when it saves one."""
        count = int.from_bytes(icc[128:132], 'big')
        entries = [icc[132 + 12 * i:144 + 12 * i] for i in range(count)]
        return {e[:4]: icc[int.from_bytes(e[4:8], 'big'):][:int.from_bytes(e[8:12], 'big')]
                for e in entries if e[:4] != b'desc'}

    embedded = []
    for element in tree.iter(svg_ns + 'color-profile'):
        href = element.get('{http://www.w3.org/1999/xlink}href', '')
        if href.startswith('data:') and ',' in href:
            embedded.append(tags(base64.b64decode(href.split(',', 1)[1])))
    check('icc: reopens in its own profile', tree.get('{urn:x-proof:document:1}color-mode') == 'CMYK'
          and tags(profile.read_bytes()) in embedded, len(embedded))
    stops = [s.get('style', '') for s in tree.iter(svg_ns + 'stop')]
    check('icc: stop numbers retained', any('0.05, 1, 0.45, 0.22)' in s or '0.05,1,0.45,0.22)' in s for s in stops)
          and any(', 1, 0, 0, 0)' in s or ',1,0,0,0)' in s for s in stops), stops)
    preview = out / 'icc-reopened.png'
    run('icc-preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
    d = deltas(canvas, preview, ICC_POINTS)
    check('icc: drawn as before after reopen', max(d) <= 4, d)
    (out / 'report.json').write_text(json.dumps(checks, indent=2))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
