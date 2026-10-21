"""Opaque raster pictures through Illustrator 7 records; synthetic inputs only.

Check ASCII raster fields and samples, geometry after native reopen, CMYK print
samples, document colour mode, and named refusals preserving a prior destination.
Actual Illustrator conversion and an installed version 7 application are separate
qualification; this script sends no mouse or keyboard input.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image
from pypdf import PdfReader


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', type=Path, required=True)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    m = runpy.run_path(str(Path(__file__).with_name('ai-picture-output-smoke.py')))
    rgba, rgb, cmyk = m['pictures']()
    opaque = rgba.convert('RGB')
    gray = Image.new('L', (20, 30))
    gray.putdata([x * 8 + y * 3 for y in range(30) for x in range(20)])
    uri = m['data_uri']
    source_text = m['svg'](opaque, rgb, cmyk).replace('width="430"', 'width="640"')
    source_text = source_text.replace('height="320"', 'height="440"').replace('viewBox="0 0 430 320"', 'viewBox="0 0 640 440"')
    source_text = source_text.replace('</g></svg>', f'''
      <image x="450" y="30" width="100" height="180" preserveAspectRatio="none" href="{uri(gray, 'TIFF')}"/>
      <image x="440" y="240" width="120" height="120" href="{uri(opaque, 'PNG')}"/>
      <g transform="translate(590 370) scale(-1 1)"><image width="100" height="60"
        preserveAspectRatio="none" href="{uri(rgb, 'PNG')}"/></g>
      <rect x="440" y="380" width="30" height="20" fill="#2060c0"/>
      </g></svg>''')
    source = out / 'control.svg'
    source.write_text(source_text, encoding='utf-8')
    linked = out / 'sample.png'
    opaque.save(linked)
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, linked)}
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    if args.dll_dir:
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

    def differences(a, b, points):
        with Image.open(a) as x, Image.open(b) as y:
            x, y = x.convert('RGB'), y.convert('RGB')
            return [max(abs(c - d) for c, d in zip(x.getpixel(p), y.getpixel(p))) for p in points]

    exe = args.inkscape.resolve()
    legacy = '--export-extension=org.inkscape.output.ai.legacy7'
    ai, canvas = out / 'control.ai', out / 'canvas.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    run('save', exe, source, legacy, f'--export-filename={ai}')
    data = ai.read_bytes()
    check('standalone version 7, ASCII and RGB conversion hint', data.startswith(b'%!PS-Adobe-3.0\n')
          and b'%AI5_FileFormat 3\n' in data and b'%AI9_ColorModel: 1\n' in data and all(c < 128 for c in data)
          and b'AIPrivateData' not in data and b'%PDF-' not in data)
    pattern = rb'\[[^\n]*\] ([^\n]+)\n%%BeginData: (\d+) ASCII Bytes\nXI\n((?:%[0-9A-F]+\n)+)%%EndData\n%AI5_EndRaster\nN\n'
    blocks = re.findall(pattern, data)
    valid, samples = len(blocks) == 7, {}
    for fields, count, lines in blocks:
        f = list(map(int, fields.split()))
        pixels = bytes.fromhex(lines.replace(b'%', b'').decode())
        valid &= (len(f) == 12 and f[6] == 8 and f[7] in (1, 3, 4) and f[8:] == [0, 0, 0, 0]
                  and len(pixels) == f[4] * f[5] * f[7] and int(count) == len(lines) + 3)
        samples.setdefault(f[7], []).append(pixels)
    check('seven opaque rasters, exact field and byte counts', valid, len(blocks))
    check('CMYK and grayscale file samples unchanged', samples.get(4) == [cmyk.tobytes()]
          and samples.get(1) == [gray.tobytes()])
    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    ns = '{http://www.w3.org/2000/svg}'
    root = ET.parse(reopened).getroot()
    images = list(root.iter(ns + 'image'))
    check('native reopen, seven embedded pictures and following art', len(images) == 7
          and b'opens from its PDF page' not in p.stdout + p.stderr
          and all((i.get('{http://www.w3.org/1999/xlink}href') or i.get('href', '')).startswith('data:') for i in images)
          and len(list(root.iter(ns + 'path'))) >= 4)
    preview = out / 'reopened.png'
    run('preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
    points = (m['RGBA_POINTS'] + m['TURNED_POINTS'] + m['CMYK_POINTS'] + m['CLIPPED_POINTS'] + m['AFTER_POINTS']
              + [(475, 60), (500, 100), (530, 170), (470, 280), (520, 320), (540, 400), (570, 390), (450, 390)])
    delta = differences(canvas, preview, points)
    check('native pixels: rotation, fitting, reflection, gray, CMYK, clipping', max(delta) <= 3, max(delta))
    pdf = out / 'reopened.pdf'
    run('pdf', exe, reopened, f'--export-filename={pdf}')
    seen, ink_images = set(), []

    def walk(resources):
        for ref in resources.get('/XObject', {}).get_object().values() if resources.get('/XObject') else []:
            key = (getattr(ref, 'idnum', None), getattr(ref, 'generation', None))
            if key in seen:
                continue
            seen.add(key)
            obj = ref.get_object()
            if obj.get('/Subtype') == '/Image' and obj.get('/ColorSpace') == '/DeviceCMYK':
                ink_images.append(obj)
            if obj.get('/Subtype') == '/Form' and obj.get('/Resources'):
                walk(obj['/Resources'])
    walk(PdfReader(pdf).pages[0]['/Resources'])
    check('CMYK samples retained into vector PDF after reopen', len(ink_images) == 1
          and ink_images[0].get('/BitsPerComponent') == 8 and ink_images[0].get_data() == cmyk.tobytes())
    page = out / 'page'
    run('page', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', pdf, page)
    rgb_points = m['RGBA_POINTS'] + m['TURNED_POINTS'] + m['CLIPPED_POINTS'] + m['AFTER_POINTS'] + [(540, 400), (450, 390)]
    delta = differences(canvas, page.with_suffix('.png'), rgb_points)
    check('PDF RGB pictures and following art', max(delta) <= 8, max(delta))
    cmyk_source = out / 'cmyk-control.svg'
    cmyk_source.write_text(source_text.replace('<svg ', '<svg xmlns:proof="urn:x-proof:document:1" proof:color-mode="CMYK" ', 1))
    cmyk_ai = out / 'cmyk-control.ai'
    run('cmyk-save', exe, cmyk_source, legacy, f'--export-filename={cmyk_ai}')
    check('CMYK document conversion hint', b'%AI9_ColorModel: 2\n' in cmyk_ai.read_bytes())
    head = source_text.split('<g inkscape:groupmode=')[0]
    for label, body, reason in [
        ('alpha', f'<image width="80" height="60" href="{uri(rgba, "PNG")}"/>', b'picture transparency (Illustrator 7)'),
        ('linked', '<image width="80" height="60" href="sample.png"/>', b'linked pictures (Illustrator 7)'),
        ('opacity', f'<image width="80" height="60" opacity=".5" href="{uri(opaque, "PNG")}"/>', b'transparency'),
    ]:
        path = out / (label + '.svg')
        path.write_text(head + body + '</svg>')
        kept = ai.read_bytes()
        p = run(label, exe, path, legacy, f'--export-filename={ai}', success=False)
        check(label + ' refused with destination preserved', p.returncode == 1 and reason in p.stderr and ai.read_bytes() == kept)
    check('working SVG and linked fixture unchanged', all(hashlib.sha256(p.read_bytes()).hexdigest() == h for p, h in hashes.items()))
    (out / 'report.json').write_text(json.dumps(checks, indent=2))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
