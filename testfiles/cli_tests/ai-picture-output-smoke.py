"""Pictures the document holds itself, through native .ai Save As; synthetic pictures only.

Illustrator writes an embedded picture as a raster: its colour space, a matrix, 8-bit samples
interleaved and an alpha plane after them, `%AI5_EndRaster` and then `N` (without it Illustrator
drops a clipped picture and what follows, VectorCraft's user found). The control has an RGB PNG
with transparency over a grey box, a turned JPEG, a CMYK TIFF and a clipped picture with a shape
after it. The file must reopen from its own records with its pictures where they were and the
TIFF's CMYK numbers as they were; its PDF page is PROOF's own PDF export, compared with the
canvas at the RGB pictures (the canvas shows CMYK through a profile, Poppler by a formula).
Illustrator 7 and linked pictures are refused by name, keeping the previous file.
"""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops
from pypdf import PdfReader


def data_uri(image, kind, **options):
    buffer = io.BytesIO()
    image.save(buffer, kind, **options)
    mime = {'PNG': 'image/png', 'JPEG': 'image/jpeg', 'TIFF': 'image/tiff'}[kind]
    return f'data:{mime};base64,' + base64.b64encode(buffer.getvalue()).decode()


def pictures():
    """Smooth pictures, so that sampling where a source pixel's centre falls agrees everywhere."""
    rgba = Image.new('RGBA', (40, 30))
    rgba.putdata([(x * 6, y * 8, 128, 255 - x * 3) for y in range(30) for x in range(40)])
    rgb = Image.new('RGB', (30, 20))
    rgb.putdata([(200 - x * 5, 60 + y * 6, x * 4 + y * 3) for y in range(20) for x in range(30)])
    cmyk = Image.new('CMYK', (24, 16))
    cmyk.putdata([(x * 10, y * 15, 40, 0) for y in range(16) for x in range(24)])
    return rgba, rgb, cmyk


def svg(rgba, rgb, cmyk, linked=False):
    first = 'picture.png' if linked else data_uri(rgba, 'PNG')
    return f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
  xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="430" height="320" viewBox="0 0 430 320">
  <defs><clipPath id="c"><circle cx="330" cy="240" r="45"/></clipPath></defs>
  <g inkscape:groupmode="layer" inkscape:label="Pictures">
  <rect x="10" y="10" width="180" height="140" fill="#bbbbbb"/>
  <image x="20" y="20" width="160" height="120" preserveAspectRatio="none" xlink:href="{first}"/>
  <image x="230" y="30" width="150" height="100" preserveAspectRatio="none" transform="rotate(12 305 80)"
    xlink:href="{data_uri(rgb, 'JPEG', quality=95)}"/>
  <image x="20" y="180" width="168" height="112" preserveAspectRatio="none" xlink:href="{data_uri(cmyk, 'TIFF')}"/>
  <g clip-path="url(#c)"><image x="270" y="180" width="120" height="120" preserveAspectRatio="none"
    xlink:href="{data_uri(rgb, 'PNG')}"/></g>
  <rect x="300" y="290" width="60" height="20" fill="#2060c0"/>
  </g></svg>'''


def centres(x, y, w, h, columns, rows, scale_x, scale_y, step=3):
    """Where source pixels' centres land, away from the picture's edge."""
    return [(int(x + (i + 0.5) * scale_x), int(y + (j + 0.5) * scale_y))
            for j in range(1, rows - 1, step) for i in range(1, columns - 1, step)]


RGBA_POINTS = centres(20, 20, 160, 120, 40, 30, 4, 4, 6)
CMYK_POINTS = centres(20, 180, 168, 112, 24, 16, 7, 7, 4)
CLIPPED_POINTS = [(330, 240), (310, 225), (350, 255), (330, 215)]
AFTER_POINTS = [(310, 300), (350, 300)]
# The turned JPEG: points near its middle, through the same turn.
TURNED_POINTS = [(305, 80), (285, 70), (325, 90), (300, 60), (310, 100)]


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

    def records(ai):
        """A modern file's records: Zstandard in its private data, decoded with the toolchain's zstd."""
        private = PdfReader(ai).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        zstd = (args.dll_dir.resolve() if args.dll_dir else Path()) / 'zstd.exe'
        p = subprocess.run([str(zstd), '-d', '-c'], input=packed[len(b'%AI24_ZStandard_Data'):],
                           capture_output=True, timeout=60)
        return p.stdout if not p.returncode else b''

    exe = args.inkscape.resolve()
    svg_ns = '{http://www.w3.org/2000/svg}'
    rgba, rgb, cmyk = pictures()
    source = out / 'control.svg'
    source.write_text(svg(rgba, rgb, cmyk))
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    canvas = out / 'canvas.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    ai = out / 'pictures.ai'
    native = '--export-extension=org.inkscape.output.ai.native'
    run('save', exe, source, native, f'--export-filename={ai}')
    check('file saved', ai.is_file() and ai.stat().st_size > 1000)
    data = records(ai)
    rasters = data.count(b'%AI5_BeginRaster')
    check('four rasters as Illustrator writes them', rasters == 4 and data.count(b'%AI5_EndRaster\nN\n') == 4
          and data.count(b'/DeviceCMYK XN\n') == 1 and data.count(b'/DeviceRGB XN\n') == 3
          and data.count(b' 8 3 1 17 1 0 4 4 0 0\n%%BeginData: 3603\nXI\n') == 1
          and data.count(b' 8 4 0 2 1 0 4 4 0 0\n%%BeginData: 1539\nXI\n') == 1, rasters)

    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    tree = ET.parse(reopened).getroot()
    images = list(tree.iter(svg_ns + 'image'))
    check('reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr
          and len(images) == 4 and len(list(tree.iter(svg_ns + 'rect')) + list(tree.iter(svg_ns + 'path'))) >= 2,
          len(images))
    kept = [i.get('{urn:x-proof:document:1}samples', '') for i in images]
    kept = [k for k in kept if k.startswith('data:image/tiff;base64,')]
    same = False
    if len(kept) == 1:
        with Image.open(io.BytesIO(base64.b64decode(kept[0].split(',', 1)[1]))) as back:
            same = back.mode == 'CMYK' and back.size == cmyk.size and back.tobytes() == cmyk.tobytes()
    check("the TIFF's CMYK numbers kept", same, len(kept))
    preview = out / 'reopened.png'
    run('preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
    everywhere = RGBA_POINTS + TURNED_POINTS + CMYK_POINTS + CLIPPED_POINTS + AFTER_POINTS
    d = deltas(canvas, preview, everywhere)
    check('pictures where they were after reopen', max(d) <= 3, max(d))

    check('PDF compatible', PdfReader(ai).pdf_header == '%PDF-1.7')
    prefix = out / 'page'
    run('page', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', ai, prefix)
    direct = out / 'direct.pdf'
    run('direct', exe, source, f'--export-filename={direct}')
    run('direct-png', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', direct, out / 'direct')
    with Image.open(prefix.with_suffix('.png')) as a, Image.open(out / 'direct.png') as b:
        same_page = max(max(px) for px in ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getdata())
    check("PDF page is PROOF's own PDF export", same_page <= 1, same_page)
    d = deltas(canvas, prefix.with_suffix('.png'), RGBA_POINTS + TURNED_POINTS + CLIPPED_POINTS + AFTER_POINTS)
    check('PDF page matches canvas (RGB pictures)', max(d) <= 8, max(d))

    for label, body, option, reason in [
        ('linked', svg(rgba, rgb, cmyk, linked=True), native, b'linked pictures'),
        ('legacy', svg(rgba, rgb, cmyk), '--export-extension=org.inkscape.output.ai.legacy7', b'pictures (Illustrator 7)'),
    ]:
        path = out / (label + '.svg')
        path.write_text(body)
        if label == 'linked':
            rgba.save(out / 'picture.png')
        kept_bytes = ai.read_bytes()
        p = run(label, exe, path, option, f'--export-filename={ai}', success=False)
        check(label + ' refused safely', p.returncode == 1 and reason in p.stderr and ai.read_bytes() == kept_bytes)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == before)
    (out / 'report.json').write_text(json.dumps(checks, indent=2))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
