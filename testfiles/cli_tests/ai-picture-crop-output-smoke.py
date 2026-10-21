"""Synthetic slice crops through modern AI, Illustrator 7, native reopen and PDF.

Keep original raster samples and modern links behind editable viewport clips.
Check every alignment, both crop directions, rotation, reflection/skew, an outer
clip, alpha and CMYK. This script sends no input; Illustrator qualification is
separate. Each output is generated in the requested evidence directory.
"""
import argparse
import base64
import hashlib
import html
import io
import json
import math
import os
from pathlib import Path
import runpy
import subprocess
import xml.etree.ElementTree as ET
from urllib.parse import urljoin

from PIL import Image
from pypdf import PdfReader


SVG = '{http://www.w3.org/2000/svg}'
XLINK = '{http://www.w3.org/1999/xlink}'


def controls(addresses, alpha):
    cases = []
    alignments = ['x' + x + 'Y' + y for x in ('Min', 'Mid', 'Max') for y in ('Min', 'Mid', 'Max')]
    for align in alignments:
        cases.append(dict(name=align, image='rgb', w=60, h=80, aspect=align + ' slice'))
    for align in alignments:
        cases.append(dict(name=align + '-vertical', image='portrait', w=80, h=50, aspect=align + ' slice'))
    cases.extend([
        dict(name='turned', image='rgb', w=60, h=80, aspect='xMidYMid slice', turn=17),
        dict(name='reflected-skewed', image='rgb', w=60, h=80, aspect='xMaxYMin slice', reflect=True),
        dict(name='outer-clip', image='rgb', w=60, h=80, aspect='xMidYMid slice', outer=True),
        dict(name='CMYK', image='cmyk', w=60, h=80, aspect='xMidYMid slice'),
        dict(name='meet', image='rgb', w=60, h=80, aspect='xMidYMid meet'),
        dict(name='none', image='rgb', w=60, h=80, aspect='none'),
    ])
    if alpha:
        cases.append(dict(name='alpha', image='alpha', w=60, h=80, aspect='xMidYMid slice'))
    defs, body = [], []
    for index, case in enumerate(cases):
        x, y = 80 + index % 4 * 200, 70 + index // 4 * 190
        w, h = case['w'], case['h']
        if case.get('turn'):
            angle = math.radians(case['turn'])
            a, b, c, d = math.cos(angle), math.sin(angle), -math.sin(angle), math.cos(angle)
            e, f = x + (1 - a) * w / 2 - c * h / 2, y - b * w / 2 + (1 - d) * h / 2
        elif case.get('reflect'):
            a, b, c, d, e, f = -1, .15, .25, 1, x + w, y
        else:
            a, b, c, d, e, f = 1, 0, 0, 1, x, y
        case['matrix'] = a, b, c, d, e, f
        clip = ''
        if case.get('outer'):
            defs.append(f'<clipPath id="clip{index}"><path d="M8,8 H{w - 8} V{h - 8} H8 Z"/></clipPath>')
            clip = f' clip-path="url(#clip{index})"'
        matrix = ' '.join(str(v) for v in case['matrix'])
        address = html.escape(addresses[case['image']], quote=True)
        body.append(f'<g transform="matrix({matrix})"><image id="image{index}" width="{w}" height="{h}" '
                    f'preserveAspectRatio="{case["aspect"]}" href="{address}"{clip}/></g>')
    footer = 70 + math.ceil(len(cases) / 4) * 190
    body.append(f'<rect x="30" y="{footer}" width="120" height="20" fill="#2060c0"/>')
    text = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            f'width="900" height="{footer + 60}" viewBox="0 0 900 {footer + 60}"><defs>'
            + ''.join(defs) + '</defs>' + ''.join(body) + '</svg>')
    return text, cases, (70, footer + 10)


def probes(case):
    """Inside and outside the viewport, clear of antialiasing and outer-clip edges."""
    w, h = case['w'], case['h']
    points = [(w * u, h * v) for u in (.2, .5, .8) for v in (.2, .5, .8)]
    outside = [(-6, h / 2), (w + 6, h / 2), (w / 2, -6), (w / 2, h + 6)]
    if case.get('outer'):
        outside += [(3, h / 2), (w - 3, h / 2), (w / 2, 3), (w / 2, h - 3)]
    a, b, c, d, e, f = case['matrix']

    def mapped(p):
        return int(a * p[0] + c * p[1] + e), int(b * p[0] + d * p[1] + f)

    return list(map(mapped, points)), list(map(mapped, outside))


def cmyk_samples(pdf):
    seen, samples = set(), []

    def walk(resources):
        xobjects = resources.get('/XObject')
        if not xobjects:
            return
        for ref in xobjects.get_object().values():
            key = getattr(ref, 'idnum', None), getattr(ref, 'generation', None)
            if key in seen:
                continue
            seen.add(key)
            obj = ref.get_object()
            if obj.get('/Subtype') == '/Image' and obj.get('/ColorSpace') == '/DeviceCMYK':
                samples.append((obj.get('/Width'), obj.get('/Height'), obj.get('/BitsPerComponent'), obj.get_data()))
            elif obj.get('/Subtype') == '/Form' and obj.get('/Resources'):
                walk(obj['/Resources'])

    walk(PdfReader(pdf).pages[0]['/Resources'])
    return samples


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
    helper = runpy.run_path(str(Path(__file__).with_name('ai-picture-output-smoke.py')))
    images = dict(rgb=Image.new('RGB', (40, 20)), portrait=Image.new('RGB', (20, 40)),
                  cmyk=Image.new('CMYK', (40, 20)), alpha=Image.new('RGBA', (40, 20)))
    images['rgb'].putdata([(20 + x * 5, 30 + y * 8, 150) for y in range(20) for x in range(40)])
    images['portrait'].putdata([(20 + x * 8, 30 + y * 5, 150) for y in range(40) for x in range(20)])
    images['cmyk'].putdata([(x * 6, y * 12, 40, 0) for y in range(20) for x in range(40)])
    images['alpha'].putdata([(20 + x * 5, 30 + y * 8, 150, 80 + x * 4) for y in range(20) for x in range(40)])
    embedded, links, originals = {}, {}, {}
    assets = out / 'assets'
    assets.mkdir()
    for name, image in images.items():
        kind = 'TIFF' if name == 'cmyk' else 'PNG'
        embedded[name] = helper['data_uri'](image, kind)
        path = assets / (name + ('.tif' if name == 'cmyk' else '.png'))
        image.save(path, kind)
        links[name] = path.as_uri()
        originals[path] = hashlib.sha256(path.read_bytes()).hexdigest()

    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    for key in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        folder = out / key.lower()
        folder.mkdir()
        env[key] = str(folder)
    checks = []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name + (' ' + str(detail) if detail != '' else ''), flush=True)

    def run(name, *command):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (name + '.log')).write_bytes(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(f'{name}: exit {p.returncode}; see log')
        return p

    exe = args.inkscape.resolve()
    for label, legacy, linked in (('modern', False, False), ('legacy', True, False), ('linked', False, True)):
        folder = out / label
        folder.mkdir()
        text, cases, after = controls(links if linked else embedded, alpha=not legacy)
        source = folder / 'control.svg'
        source.write_text(text, encoding='utf-8')
        originals[source] = hashlib.sha256(source.read_bytes()).hexdigest()
        canvas, ai, reopened = folder / 'canvas.png', folder / 'control.ai', folder / 'reopened.svg'
        option = '--export-extension=org.inkscape.output.ai.' + ('legacy7' if legacy else 'native')
        run(label + '-canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
        run(label + '-save', exe, source, option, f'--export-filename={ai}')
        p = run(label + '-reopen', exe, ai, f'--export-filename={reopened}')
        root = ET.parse(reopened).getroot()
        pictures = list(root.iter(SVG + 'image'))
        check(label + ': native reopen and all pictures', b'opens from its PDF page' not in p.stdout + p.stderr
              and len(pictures) == len(cases), len(pictures))
        cropped = sum('slice' in case['aspect'] for case in cases)
        check(label + ': editable viewport and outer clips', len(list(root.iter(SVG + 'clipPath'))) >= cropped + 1)
        # Saving SVG makes local image addresses relative to the new SVG when
        # possible. Compare resolved links, rather than requiring absolute spelling.
        addresses = [urljoin(reopened.as_uri(), i.get(XLINK + 'href') or i.get('href', '')) for i in pictures]
        check(label + ': original links or embedded rasters retained', len(addresses) == len(cases)
              and all(address.startswith('file:') if linked else address.startswith('data:') for address in addresses))
        if linked:
            check(label + ': links still name original files', set(addresses) == {links[case['image']] for case in cases})
        else:
            samples = [i.get('{urn:x-proof:document:1}samples', '') for i in pictures]
            samples = [value for value in samples if value.startswith('data:image/tiff;base64,')]
            same = False
            if len(samples) == 1:
                with Image.open(io.BytesIO(base64.b64decode(samples[0].split(',', 1)[1]))) as image:
                    same = image.mode == 'CMYK' and image.size == (40, 20) and image.tobytes() == images['cmyk'].tobytes()
            check(label + ': full native CMYK samples retained behind clip', same)
        preview = folder / 'reopened.png'
        run(label + '-preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
        direct = folder / 'direct.pdf'
        run(label + '-pdf', exe, source, f'--export-filename={direct}')
        run(label + '-page', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', direct, folder / 'page')
        # The modern file's PDF half and standalone PDF must share this geometry.
        if not legacy:
            run(label + '-ai-page', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', ai, folder / 'ai-page')
        with Image.open(canvas) as original, Image.open(preview) as native, Image.open(folder / 'page.png') as pdf:
            original, native, pdf = original.convert('RGB'), native.convert('RGB'), pdf.convert('RGB')
            native_delta, pdf_delta, white_delta = [], [], []
            for case in cases:
                inside, outside = probes(case)
                for point in inside + outside:
                    native_delta.append(max(abs(a - b) for a, b in zip(original.getpixel(point), native.getpixel(point))))
                    if case['image'] != 'cmyk':
                        pdf_delta.append(max(abs(a - b) for a, b in zip(original.getpixel(point), pdf.getpixel(point))))
                for point in outside:
                    white_delta.extend(255 - c for c in pdf.getpixel(point))
            native_delta.extend(abs(a - b) for a, b in zip(original.getpixel(after), native.getpixel(after)))
            pdf_delta.extend(abs(a - b) for a, b in zip(original.getpixel(after), pdf.getpixel(after)))
            check(label + ': native crop pixels and following art', max(native_delta) <= 3, max(native_delta))
            check(label + ': PDF crop pixels and following art', max(pdf_delta) <= 8, max(pdf_delta))
            check(label + ': PDF stays inside both clipping paths', max(white_delta) <= 1, max(white_delta))
            if not legacy:
                with Image.open(folder / 'ai-page.png') as ai_page:
                    points = [p for case in cases for values in probes(case) for p in values] + [after]
                    ai_page = ai_page.convert('RGB')
                    delta = max(max(abs(a - b) for a, b in zip(pdf.getpixel(p), ai_page.getpixel(p))) for p in points)
                    check(label + ': AI PDF half agrees with direct PDF', delta <= 1, delta)
        samples = cmyk_samples(direct)
        check(label + ': full CMYK samples retained behind clip', samples == [(40, 20, 8, images['cmyk'].tobytes())])
    check('all working SVG and raster fixtures unchanged', all(hashlib.sha256(path.read_bytes()).hexdigest() == digest
                                                              for path, digest in originals.items()))
    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    passed = sum(check['passed'] for check in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
