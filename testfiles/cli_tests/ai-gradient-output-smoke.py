"""Opaque linear gradient writing through both .ai formats; synthetic artwork only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image
from pypdf import PdfReader


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
        print(('PASS ' if ok else 'FAIL ') + name, flush=True)

    def run(label, *command, success=True):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        if success and p.returncode:
            raise RuntimeError(f'{label}: exit {p.returncode}; see log')
        return p

    exe = args.inkscape.resolve()
    svg = '''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
      xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="430" height="320" viewBox="0 0 430 320">
      <defs><linearGradient id="g"><stop stop-color="#ff0000"/>
      <stop offset="1" stop-color="#0000ff"/></linearGradient>
      <linearGradient id="d" x1="0" y1="0" x2="1" y2="1" xlink:href="#g"/>
      <linearGradient id="c"><stop style="stop-color:device-cmyk(0 0 1 0)"/>
      <stop offset="0.4" style="stop-color:device-cmyk(0 1 1 0)"/>
      <stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/></linearGradient>
      <clipPath id="clip"><rect x="50" y="20" width="70" height="60"/></clipPath></defs>
      <g inkscape:groupmode="layer" inkscape:label="Gradients">
      <g clip-path="url(#clip)"><rect x="20" y="20" width="100" height="60" fill="url(#g)"/></g>
      <rect x="180" y="100" width="200" height="60" fill="url(#g)"/>
      <rect x="20" y="190" width="100" height="60" transform="matrix(1 0 .4 1 0 0)" fill="url(#d)"/>
      <path d="M200 190 H380 V290 H200 Z M240 220 V260 H340 V220 Z"
        style="fill:url(#c);fill-rule:evenodd"/>
      </g></svg>'''
    source = out / 'control.svg'
    source.write_text(svg)
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    canvas = out / 'canvas.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    svg_ns = '{http://www.w3.org/2000/svg}'
    for name, extension in [('modern', 'native'), ('legacy', 'legacy7')]:
        ai = out / (name + '.ai')
        option = '--export-extension=org.inkscape.output.ai.' + extension
        run(name + '-save', exe, source, option, f'--export-filename={ai}')
        check(name + ': file saved', ai.is_file() and ai.stat().st_size > 100)
        reopened = out / (name + '.svg')
        run(name + '-reopen', exe, ai, f'--export-filename={reopened}')
        tree = ET.parse(reopened).getroot()
        check(name + ': native paths and clipping retained', len(list(tree.iter(svg_ns + 'path'))) == 5
              and not list(tree.iter(svg_ns + 'image')) and list(tree.iter(svg_ns + 'clipPath')))
        stops = list(tree.iter(svg_ns + 'stop'))
        check(name + ': CMYK stops retained', sum('device-cmyk' in s.get('style', '') for s in stops) == 3)
        preview = out / (name + '-reopened.png')
        run(name + '-preview', exe, reopened, '--export-background=white', f'--export-filename={preview}')
        points = [(70, 50), (110, 50), (190, 130), (280, 130), (370, 130), (130, 210), (180, 235)]
        with Image.open(canvas) as a, Image.open(preview) as b:
            a, b = a.convert('RGB'), b.convert('RGB')
            deltas = [max(abs(x - y) for x, y in zip(a.getpixel(p), b.getpixel(p))) for p in points]
            check(name + ': gradient placement after reopen', max(deltas) <= 4, deltas)
            check(name + ': compound hole stays empty', min(b.getpixel((280, 240))) >= 250)
        if name == 'modern':
            check('modern: PDF compatible', PdfReader(ai).pdf_header == '%PDF-1.7')
            prefix = out / 'modern-pdf'
            run('modern-pdf', args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', ai, prefix)
            with Image.open(canvas) as a, Image.open(prefix.with_suffix('.png')) as b:
                a, b = a.convert('RGB'), b.convert('RGB')
                deltas = [max(abs(x - y) for x, y in zip(a.getpixel(p), b.getpixel(p))) for p in points]
                check('modern: PDF gradient placement matches canvas', max(deltas) <= 4, deltas)
        else:
            check('legacy: editable gradient definitions', b'4 Bn\n' in ai.read_bytes()
                  and ai.read_bytes().count(b' Bg\n') == 4)
        for label, replacement, reason in [
            ('alpha', 'stop-color="#ff0000" stop-opacity="0.5"', b'gradient transparency'),
            ('repeat', 'stop-color="#ff0000"', b'repeating or reflected gradients'),
            ('stroke', 'stop-color="#ff0000"', b'gradients on both the fill and the stroke'),
        ]:
            unsupported = svg.replace('stop-color="#ff0000"', replacement)
            if label == 'repeat':
                unsupported = unsupported.replace('id="g"', 'id="g" spreadMethod="repeat"')
            if label == 'stroke':
                # A plain stroke is written now (b with 2 BB); a gradient on both is not yet.
                unsupported = unsupported.replace('fill="url(#g)"', 'fill="url(#g)" stroke="url(#g)"')
            path = out / (name + '-' + label + '.svg')
            path.write_text(unsupported)
            kept = ai.read_bytes()
            p = run(name + '-' + label, exe, path, option, f'--export-filename={ai}', success=False)
            check(name + ': ' + label + ' refused safely', p.returncode == 1 and reason in p.stderr
                  and ai.read_bytes() == kept)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == before)
    (out / 'report.json').write_text(json.dumps(checks, indent=2))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
