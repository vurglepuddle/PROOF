"""Filled and dual-gradient strokes through native AI, SVG reopen and PDF.

Synthetic art only. Independent fill/stroke ramps must stay on one editable path,
including open paths, holes, transforms and clips. Existing unsupported placements,
transparency, paint order and Illustrator 7 must preserve a prior destination.
Actual Illustrator qualification is separate from these headless checks.
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
  width="720" height="580" viewBox="0 0 720 580"><defs>
  <linearGradient id="base"><stop stop-color="#ff3000"/><stop offset=".4" stop-color="#ffd000"/>
    <stop offset="1" stop-color="#0030ff"/></linearGradient>
  <linearGradient id="line" xlink:href="#base"/>
  <linearGradient id="other-colours">
    <stop stop-color="#00b060"/><stop offset="1" stop-color="#c000b0"/></linearGradient>
  <linearGradient id="other" xlink:href="#other-colours" x1="0" y1="1" x2="1" y2="0"/>
  <radialGradient id="radial" xlink:href="#base" r=".7"/>
  <radialGradient id="focus" xlink:href="#other-colours" r=".7" fx=".35" fy=".4"/>
  <clipPath id="clip"><circle cx="600" cy="450" r="65"/></clipPath>
  </defs>'''
BODY = '''
  <rect x="30" y="30" width="160" height="90" fill="#336699" stroke="url(#line)" stroke-width="18"/>
  <rect x="260" y="30" width="160" height="90" fill="#66aa33" stroke="url(#radial)" stroke-width="18"/>
  <rect x="490" y="30" width="160" height="90" fill="url(#line)" stroke="url(#other)" stroke-width="18"/>
  <rect x="30" y="200" width="160" height="90" fill="url(#line)" stroke="url(#radial)" stroke-width="18"/>
  <path d="M260 200L420 200L340 290" fill="url(#focus)" stroke="url(#line)" stroke-width="14"/>
  <rect x="490" y="200" width="160" height="90" fill="url(#focus)" stroke="url(#radial)" stroke-width="18"/>
  <path d="M30 380H210V530H30Z M70 420V490H170V420Z" fill="url(#other)" fill-rule="evenodd"
    stroke="url(#radial)" stroke-width="12"/>
  <g transform="translate(360 450) rotate(18) scale(.9)">
    <rect x="-70" y="-45" width="140" height="90" fill="url(#focus)" stroke="url(#line)" stroke-width="16"/>
  </g>
  <g clip-path="url(#clip)"><rect x="520" y="370" width="160" height="160"
    fill="url(#other)" stroke="url(#radial)" stroke-width="40"/></g>
  <rect x="600" y="550" width="80" height="18" fill="#000000"/>
'''
TAIL = '</svg>'
# Interior samples from each paint, plus the hole, clipping boundary and following solid.
POINTS = [(x, y) for x, y in [
    (60, 70), (30, 75), (110, 30), (190, 75),
    (300, 70), (260, 75), (340, 30), (420, 75),
    (510, 70), (570, 70), (630, 70), (490, 75), (570, 30), (650, 75),
    (50, 240), (110, 240), (170, 240), (30, 245), (110, 200), (190, 245),
    (320, 220), (350, 250), (340, 200), (320, 245),
    (510, 240), (570, 240), (630, 240), (490, 245), (570, 200), (650, 245),
    (50, 450), (120, 400), (120, 450), (120, 380), (70, 450), (170, 450), (210, 450),
    (330, 440), (360, 450), (390, 460), (360, 408), (360, 492),
    (550, 450), (600, 450), (650, 450), (600, 388), (600, 510), (530, 450),
    (640, 560),
]]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', type=Path, required=True)
    ap.add_argument('--dll-dir', type=Path, required=True)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    for key in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[key] = str(out / key.lower())
        Path(env[key]).mkdir()
    checks = []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name + (' ' + str(detail) if detail != '' else ''), flush=True)

    def run(name, *command, success=True):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (name + '.log')).write_bytes(p.stdout + p.stderr)
        if success and p.returncode:
            raise RuntimeError(f'{name}: exit {p.returncode}; see log')
        return p

    def paint(element, key):
        style = dict(kv.split(':', 1) for kv in element.get('style', '').split(';') if ':' in kv)
        return style.get(key, element.get(key, ''))

    def deltas(a, b):
        with Image.open(a) as x, Image.open(b) as y:
            x, y = x.convert('RGB'), y.convert('RGB')
            return [max(abs(c - d) for c, d in zip(x.getpixel(p), y.getpixel(p))) for p in POINTS]

    exe = args.inkscape.resolve()
    native = '--export-extension=org.inkscape.output.ai.native'
    source = out / 'control.svg'
    source.write_text(HEAD + BODY + TAIL, encoding='utf-8')
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    canvas, ai = out / 'canvas.png', out / 'control.ai'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    run('save', exe, source, native, f'--export-filename={ai}')
    private = PdfReader(ai).pages[0]['/PieceInfo']['/Illustrator']['/Private']
    packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
    marker = b'%AI24_ZStandard_Data'
    assert packed.startswith(marker)
    records = subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'],
                             input=packed[len(marker):], capture_output=True, check=True, timeout=60).stdout
    check('independent fill and stroke instances', records.count(b'\nBb\n') == 7
          and records.count(b'\n1 Bb\n') == 9 and records.count(b'%AI5_BeginGradient:') == 16)
    check('consecutive dual instances on one path', records.count(b'\n2 BB\n1 Bb\n') == 6
          and records.count(b'\n1 BB\n1 Bb\n') == 1)
    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    tree = ET.parse(reopened).getroot()
    ns = '{http://www.w3.org/2000/svg}'
    paths = list(tree.iter(ns + 'path'))
    strokes = [e for e in paths if paint(e, 'stroke').startswith('url(')]
    check('native reopen, nine single paths with both paints', b'opens from its PDF page' not in p.stdout + p.stderr
          and not list(tree.iter(ns + 'image')) and len(strokes) == 9
          and sum(paint(e, 'fill').startswith('url(') for e in strokes) == 7)
    check('open path and compound hole retained', sum(not e.get('d', '').rstrip().lower().endswith('z') for e in strokes) == 1
          and sum(paint(e, 'fill-rule') == 'evenodd' for e in strokes) == 1)
    preview = out / 'reopened.png'
    run('preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
    with Image.open(canvas) as a, Image.open(preview) as b:
        maximum = max(max(pixel) for pixel in ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getdata())
    check('native reopen whole-page pixels', maximum <= 3, maximum)
    page = out / 'page'
    run('page', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', ai, page)
    delta = deltas(canvas, page.with_suffix('.png'))
    # Existing radial-output qualification permits 12 at 96 dpi: Poppler shading steps.
    check('PDF fill, stroke, holes, clips and following shape', max(delta) <= 12, delta)
    for label, body, option, reason in [
        ('legacy', BODY, '--export-extension=org.inkscape.output.ai.legacy7', b'gradients on strokes (Illustrator 7)'),
        ('paint-order', BODY.replace('stroke-width="18"', 'stroke-width="18" paint-order="stroke fill"'), native,
         b'strokes painted under their fill'),
        ('stroke-alpha', BODY.replace('stroke-width="18"', 'stroke-width="18" stroke-opacity=".5"'), native, b'gradient transparency'),
        ('fill-alpha', BODY.replace('fill="url(#line)"', 'fill="url(#line)" fill-opacity=".5"'), native, b'gradient transparency'),
        ('radial-focus', BODY.replace('stroke="url(#radial)"', 'stroke="url(#focus)"'), native,
         b'radial gradient strokes with an offset focal point'),
        ('stretch', '<g transform="scale(1 .8)">' + BODY + '</g>', native, b'strokes on stretched or skewed objects'),
    ]:
        path = out / (label + '.svg')
        path.write_text(HEAD + body + TAIL, encoding='utf-8')
        kept = ai.read_bytes()
        p = run(label, exe, path, option, f'--export-filename={ai}', success=False)
        check(label + ' refused with destination preserved', p.returncode == 1 and reason in p.stderr and ai.read_bytes() == kept)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == before)
    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
