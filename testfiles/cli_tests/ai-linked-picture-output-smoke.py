"""Local raster links through native AI, SVG reopen and vector PDF; synthetic inputs only.

Links remain external, with the source bytes unchanged. Exercise resolution, alpha, CMYK,
rotation, reflection, fitting, clipping, a Unicode path and Save As into another folder.
Missing/unsupported links, slice cropping and Illustrator 7 must preserve a prior destination.
Actual Illustrator open/save qualification is separate from this headless check.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops
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
    out.mkdir(parents=True, exist_ok=True)
    assets = out / 'assets'
    assets.mkdir(exist_ok=True)
    unicode_dir = assets / '\u041f\u0440\u043e\u0432\u0435\u0440\u043a\u0430_\u6f22\u5b57_\u00e9_\U0001f7e9'
    unicode_dir.mkdir(exist_ok=True)
    old = runpy.run_path(str(Path(__file__).with_name('ai-picture-output-smoke.py')))
    rgba, rgb, cmyk = old['pictures']()
    rgba.save(assets / 'sample.png', dpi=(150, 150))
    rgb.save(assets / 'sample.jpg', quality=95, dpi=(200, 200))
    cmyk.save(assets / 'sample.tif', dpi=(300, 300))
    rgba.save(unicode_dir / 'sample.png', dpi=(150, 150))
    # PSD merged image, 8-bit planar CMYK with Photoshop's inverted ink bytes.
    psd = b'8BPS' + struct.pack('>H6sHIIHH', 1, b'\0' * 6, 4, cmyk.height, cmyk.width, 8, 4)
    psd += b'\0' * 14  # empty colour data, resources, layers; raw compression
    psd += b''.join(bytes(255 - v for v in channel.tobytes()) for channel in cmyk.split())
    (assets / 'sample.psd').write_bytes(psd)
    # Separate containers outside this Illustrator link-writing qualification.
    (assets / 'unsupported.psb').write_bytes(psd[:4] + b'\0\2' + psd[6:26] + b'\0' * 18 + psd[40:])
    entries = [
        (256, 4, 1, 4), (257, 4, 1, 2),
        (258, 3, 3, int.from_bytes(struct.pack('<HHH', 8, 8, 8), 'little')),
        (259, 3, 1, 1), (262, 3, 1, 2), (273, 16, 1, 232),
        (277, 3, 1, 3), (278, 4, 1, 2), (279, 16, 1, 24), (284, 3, 1, 1),
    ]
    big = b'II' + struct.pack('<HHHQ', 43, 8, 0, 16) + struct.pack('<Q', len(entries))
    big += b''.join(struct.pack('<HHQQ', *entry) for entry in entries) + b'\0' * 8
    (assets / 'unsupported-big.tif').write_bytes(big + b'\x33\x66\x99' * 8)
    Image.new('RGB', (4, 2), '#336699').save(assets / 'unsupported.gif')
    unicode_href = unicode_dir.as_uri() + '/sample.png'
    head = '''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
      width="640" height="420" viewBox="0 0 640 420">'''
    body = f'''<defs><clipPath id="clip"><circle cx="490" cy="230" r="50"/></clipPath></defs>
      <rect x="10" y="10" width="180" height="140" fill="#bbbbbb"/>
      <image id="alpha" x="20" y="20" width="160" height="120" preserveAspectRatio="none" xlink:href="assets/sample.png"/>
      <image x="230" y="30" width="150" height="100" preserveAspectRatio="none" transform="rotate(12 305 80)" xlink:href="assets/sample.jpg"/>
      <image x="20" y="180" width="168" height="112" preserveAspectRatio="none" xlink:href="assets/sample.tif"/>
      <image x="230" y="180" width="144" height="96" preserveAspectRatio="none" xlink:href="assets/sample.psd"/>
      <image x="440" y="20" width="120" height="120" xlink:href="{unicode_href}"/>
      <g clip-path="url(#clip)"><image x="410" y="170" width="160" height="120" preserveAspectRatio="none" xlink:href="assets/sample.jpg"/></g>
      <g transform="translate(610 280) scale(-1 1)"><image width="120" height="90" preserveAspectRatio="none" xlink:href="assets/sample.png"/></g>
      <rect x="420" y="390" width="160" height="20" fill="#2060c0"/>'''
    source = out / 'control.svg'
    source.write_text(head + body + '</svg>', encoding='utf-8')
    inputs = [source, *assets.rglob('*')]
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs if p.is_file()}
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for key in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[key] = str(out / key.lower())
        Path(env[key]).mkdir(exist_ok=True)
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    checks = []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name + (' ' + str(detail) if detail != '' else ''), flush=True)

    def run(name, *command, success=True):
        p = subprocess.run([str(v) for v in command], env=env, capture_output=True, timeout=180)
        (out / (name + '.log')).write_bytes(p.stdout + p.stderr)
        if success and p.returncode:
            raise RuntimeError(f'{name}: exit {p.returncode}; see {out / (name + ".log")}')
        return p

    exe = args.inkscape.resolve()
    native = '--export-extension=org.inkscape.output.ai.native'
    saved = out / 'saved-elsewhere'
    saved.mkdir(exist_ok=True)
    ai = saved / 'control.ai'
    canvas = out / 'canvas.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    run('save', exe, source, native, f'--export-filename={ai}')
    reader = PdfReader(ai)
    private = reader.pages[0]['/PieceInfo']['/Illustrator']['/Private']
    packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
    p = subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'],
                       input=packed[len(b'%AI24_ZStandard_Data'):], capture_output=True, check=True, timeout=60)
    records = p.stdout
    check('seven placed links, no embedded rasters', records.count(b'%AI5_BeginPlace\n') == 7
          and records.count(b'%AI5_EndPlace\n') == 7 and b'%AI5_BeginRaster' not in records)
    check('absolute source addresses despite Save As elsewhere', records.count(b'%%IncludeFile:') == 7
          and str(assets).encode() in records)
    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    tree = ET.parse(reopened).getroot()
    ns = '{http://www.w3.org/2000/svg}'
    images = list(tree.iter(ns + 'image'))
    check('native reopen with seven external images', len(images) == 7
          and b'opens from its PDF page' not in p.stdout + p.stderr
          and all(not (i.get('{http://www.w3.org/1999/xlink}href') or i.get('href', '')).startswith('data:') for i in images))
    preview = out / 'reopened.png'
    run('preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
    with Image.open(canvas) as a, Image.open(preview) as b:
        difference = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
        # Fitting into a larger SVG frame lets Cairo filter just outside the pixel bounds;
        # the native placed object has the pixel bounds themselves. Check the entire page
        # strictly, excluding only a two-pixel antialias fringe on that fitted image.
        maximum = 0
        fringe_pixels = 0
        for y in range(difference.height):
            for x in range(difference.width):
                d = max(difference.getpixel((x, y)))
                outer = 438 <= x < 562 and 33 <= y < 127
                inner = 442 <= x < 558 and 37 <= y < 123
                if outer and not inner:
                    fringe_pixels += int(d > 3)
                else:
                    maximum = max(maximum, d)
    check('placement, alpha and CMYK after native reopen (fitted edge fringe excluded)',
          maximum <= 3, {'max': maximum, 'fitted_fringe_pixels': fringe_pixels})

    # Walk the PDF forms, checking print samples without rasterizing the page first.
    seen, print_images = set(), []
    def walk(resources):
        for ref in resources.get('/XObject', {}).get_object().values() if resources.get('/XObject') else []:
            key = (getattr(ref, 'idnum', None), getattr(ref, 'generation', None))
            if key in seen:
                continue
            seen.add(key)
            obj = ref.get_object()
            if obj.get('/Subtype') == '/Image' and obj.get('/ColorSpace') == '/DeviceCMYK':
                print_images.append(obj)
            if obj.get('/Subtype') == '/Form' and obj.get('/Resources'):
                walk(obj['/Resources'])
    walk(reader.pages[0]['/Resources'])
    check('TIFF and Photoshop retain CMYK print samples', len(print_images) >= 2
          and all(i.get('/BitsPerComponent') == 8 and i.get_data() == cmyk.tobytes() for i in print_images), len(print_images))
    page = out / 'page'
    run('page', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', ai, page)
    points = old['RGBA_POINTS'] + old['TURNED_POINTS'] + [(470, 60), (520, 90), (490, 230), (550, 320), (500, 400)]
    with Image.open(canvas) as a, Image.open(page.with_suffix('.png')) as b:
        a, b = a.convert('RGB'), b.convert('RGB')
        delta = [max(abs(x - y) for x, y in zip(a.getpixel(p), b.getpixel(p))) for p in points]
    check('PDF matches the canvas on RGB pictures and following art', max(delta) <= 8, max(delta))
    for label, content, option, reason in [
        ('missing', '<image width="40" height="20" href="assets/missing.png"/>', native, b'missing or unreadable linked pictures'),
        ('unsupported', '<image width="40" height="20" href="assets/unsupported.gif"/>', native, b'linked pictures other than PNG, JPEG, TIFF or Photoshop'),
        ('psb', '<image width="40" height="20" href="assets/unsupported.psb"/>', native, b'linked Photoshop versions other than PSD'),
        ('bigtiff', '<image width="40" height="20" href="assets/unsupported-big.tif"/>', native, b'linked BigTIFF pictures'),
        ('slice', '<image width="40" height="80" preserveAspectRatio="xMidYMid slice" href="assets/sample.png"/>', native, b'pictures cropped to their box'),
        ('legacy', body, '--export-extension=org.inkscape.output.ai.legacy7', b'linked pictures (Illustrator 7)'),
    ]:
        path = out / (label + '.svg')
        path.write_text(head + content + '</svg>', encoding='utf-8')
        before = ai.read_bytes()
        p = run(label, exe, path, option, f'--export-filename={ai}', success=False)
        check(label + ' refused with destination preserved', p.returncode == 1 and reason in p.stderr
              and ai.read_bytes() == before)
    check('source SVG and every linked file unchanged', all(hashlib.sha256(p.read_bytes()).hexdigest() == h for p, h in hashes.items()))
    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
