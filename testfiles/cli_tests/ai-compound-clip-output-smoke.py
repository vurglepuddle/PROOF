"""Compound clips in both .ai writers: holes, independent rules, open contours, transforms,
nested clips, gradients and pictures. Synthetic artwork only; sends no desktop input.
Illustrator's own reading is qualified separately with the generated control files.
"""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image

SVG = '{http://www.w3.org/2000/svg}'
WIDTH, HEIGHT = 600, 300


def control(transparent=False):
    """SVG and probes in document pixels at 96 dpi; also used for Illustrator qualification."""
    outer = 'M0 0 H100 V100 H0 Z '
    opposite = outer + 'M30 30 V70 H70 V30 Z'
    same = outer + 'M30 30 H70 V70 H30 Z'
    picture = Image.new('RGB', (8, 8), '#008080')
    data = io.BytesIO()
    picture.save(data, 'PNG')
    uri = 'data:image/png;base64,' + base64.b64encode(data.getvalue()).decode()
    defs = (f'<clipPath id="winding"><path d="{opposite}"/></clipPath>'
            f'<clipPath id="parity" clip-rule="evenodd"><path d="{same}" fill-rule="nonzero"/></clipPath>'
            f'<clipPath id="solid"><path d="{same}" clip-rule="nonzero" fill-rule="evenodd"/></clipPath>'
            '<clipPath id="open"><path d="M0 0 H100 V100 M10 110 H40 V125 H10"/></clipPath>'
            f'<clipPath id="reflected"><path d="{same}" clip-rule="evenodd" '
            'transform="translate(105 5) scale(-1 .8)"/></clipPath>'
            '<clipPath id="half"><rect width="60" height="100"/></clipPath>'
            '<linearGradient id="ramp" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="100" y2="0">'
            '<stop style="stop-color:device-cmyk(0 1 1 0)"/>'
            '<stop offset="1" style="stop-color:device-cmyk(1 1 0 0)"/></linearGradient>')
    plain = '<rect x="-5" y="-5" width="130" height="140" fill="#cc3300" fill-rule="evenodd"/>'
    cells = [
        f'<g clip-path="url(#winding)">{plain}</g>',
        f'<g clip-path="url(#parity)">{plain}</g>',
        f'<g clip-path="url(#solid)">{plain}</g>',
        f'<g clip-path="url(#open)">{plain}</g>',
        f'<g clip-path="url(#reflected)">{plain}</g>',
        f'<g clip-path="url(#parity)"><g clip-path="url(#half)">{plain}</g></g>',
        '<g clip-path="url(#parity)"' + (' opacity="0.6"' if transparent else '') + '>'
        '<rect width="100" height="100" fill="url(#ramp)"/>'
        '<path d="M5 15 H95" fill="none" stroke="#008000" stroke-width="6"/></g>',
        f'<image x="0" y="0" width="100" height="100" href="{uri}" clip-path="url(#winding)"/>',
    ]
    art = ''.join(f'<g transform="translate({20 + 150 * (i % 4)} {10 + 150 * (i // 4)})">{c}</g>'
                  for i, c in enumerate(cells))
    # A painted object after all clips catches a leaked clip or fill rule.
    art += '<path d="M560 265 H590 V295 H560 Z M568 273 H582 V287 H568 Z" fill-rule="evenodd" fill="#336699"/>'
    text = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">'
            f'<defs>{defs}</defs><g inkscape:groupmode="layer" inkscape:label="Compound clips">{art}</g></svg>')
    local = [
        [(10, 10), (50, 50), (90, 90), (110, 50)],
        [(10, 10), (50, 50), (90, 90), (110, 50)],
        [(10, 10), (50, 50), (90, 90), (110, 50)],
        [(80, 20), (20, 80), (80, 90), (20, 117), (70, 117)],
        [(15, 15), (55, 45), (100, 75), (110, 50)],
        [(10, 10), (50, 50), (50, 90), (80, 10)],
        [(10, 10), (10, 15), (90, 90), (50, 50), (110, 50)],
        [(10, 10), (50, 50), (90, 90), (110, 50)],
    ]
    names = ['nonzero hole', 'even-odd hole', 'nonzero solid', 'open contours', 'reflected and scaled',
             'nested intersection', 'gradient and stroke', 'embedded picture']
    probes = {name: [(20 + 150 * (i % 4) + x, 10 + 150 * (i // 4) + y) for x, y in points]
              for i, (name, points) in enumerate(zip(names, local))}
    probes['after the clips'] = [(563, 270), (575, 280)]
    return text, probes


def probe(a_png, b_png, regions, scale=1):
    with Image.open(a_png) as a, Image.open(b_png) as b:
        a, b = a.convert('RGB'), b.convert('RGB')
        return {name: max(max(abs(x - y) for x, y in zip(a.getpixel((round(q[0] * scale), round(q[1] * scale))),
                                                         b.getpixel((round(q[0] * scale), round(q[1] * scale)))))
                         for q in points) for name, points in regions.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        (out / name.lower()).mkdir()
        env[name] = str(out / name.lower())
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    exe = args.inkscape.resolve()
    checks = []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name + (f' {detail}' if detail != '' else ''), flush=True)

    def run(label, *command, ok=True):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        if ok and p.returncode:
            raise RuntimeError(f'{label}: exit {p.returncode}; see log')
        return p

    def clips(svg):
        return sorted((len(re.findall(r'[Mm]', p.get('d') or '')), 'clip-rule:evenodd' in (p.get('style') or ''))
                      for c in ET.parse(svg).getroot().iter(SVG + 'clipPath') for p in c if p.tag == SVG + 'path')

    source = out / 'control.svg'
    text, regions = control()
    source.write_text(text, encoding='utf-8')
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    run('canvas', exe, source, '--export-background=white', f'--export-filename={out / "canvas.png"}')
    # Independent known pixels prove that the source controls contain real holes and open clips.
    with Image.open(out / 'canvas.png') as im:
        check('opposite winding and even-odd both leave holes; same winding stays solid',
              [im.convert('RGB').getpixel(p) for p in ((70, 60), (220, 60), (370, 60))]
              == [(255, 255, 255), (255, 255, 255), (204, 51, 0)])
    expected_clips = sorted([(2, False), (2, True), (2, False), (2, False), (2, True),
                             (2, True), (1, False), (2, True), (2, False)])
    for route, extension in [('modern', 'native'), ('legacy7', 'legacy7')]:
        option = '--export-extension=org.inkscape.output.ai.' + extension
        ai, reopened = out / f'{route}.ai', out / f'{route}.svg'
        run(route + '-save', exe, source, option, f'--export-filename={ai}')
        p = run(route + '-reopen', exe, ai, f'--export-filename={reopened}')
        check(route + ': reopens natively', b'opens from its PDF page' not in p.stdout + p.stderr)
        check(route + ': all compound clips and their rules remain editable', clips(reopened) == expected_clips, clips(reopened))
        drawn = out / f'{route}.png'
        run(route + '-draw', exe, reopened, '--export-background=white', f'--export-filename={drawn}')
        differences = probe(out / 'canvas.png', drawn, regions)
        check(route + ': holes, islands, transforms and nested intersections draw correctly', max(differences.values()) <= 3, differences)
        if args.pdftoppm and route == 'modern':
            run('pdf-page', args.pdftoppm, '-scale-to-x', WIDTH, '-scale-to-y', HEIGHT, '-png', '-singlefile', ai, out / 'pdf')
            differences = probe(out / 'canvas.png', out / 'pdf.png', regions)
            # The canvas and Poppler convert/interpolate CMYK gradients differently.
            # Verify their clip coverage independently of those colour differences;
            # the flat RGB controls still compare within three channel levels.
            with Image.open(out / 'canvas.png') as a, Image.open(out / 'pdf.png') as b:
                a, b = a.convert('RGB'), b.convert('RGB')
                coverage = all((min(a.getpixel(q)) >= 250) == (min(b.getpixel(q)) >= 250)
                               for points in regions.values() for q in points)
            flat = {k: v for k, v in differences.items() if k != 'gradient and stroke'}
            check('modern: PDF page keeps every clip and hole, and the flat paint colours',
                  coverage and max(flat.values()) <= 3, differences)
        again, again_svg = out / f'{route}-again.ai', out / f'{route}-again.svg'
        run(route + '-again', exe, reopened, option, f'--export-filename={again}')
        p = run(route + '-again-reopen', exe, again, f'--export-filename={again_svg}')
        check(route + ': a second write keeps the clips and opens natively', clips(again_svg) == expected_clips
              and b'opens from its PDF page' not in p.stdout + p.stderr)
        refusals = [
            ('multiple-objects', '<rect width="10" height="10"/><rect x="20" width="10" height="10"/>', '',
             'clipping paths other than one path'),
            ('bounding-box', '<path d="M0 0 H1 V1 H0 Z M.2 .2 H.8 V.8 H.2 Z"/>', ' clipPathUnits="objectBoundingBox"',
             'clipping paths other than one path'),
            ('nested-geometry', '<path d="M0 0 H80 V80 H0 Z M20 20 H60 V60 H20 Z" clip-path="url(#inner)"/>', '',
             "clipping on a clipping path's geometry"),
        ]
        for name, geometry, attrs, reason in refusals:
            bad = out / f'{route}-{name}.svg'
            bad.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">'
                           '<defs><clipPath id="inner"><rect width="10" height="10"/></clipPath>'
                           f'<clipPath id="bad"{attrs}>{geometry}</clipPath></defs>'
                           '<rect width="100" height="100" clip-path="url(#bad)"/></svg>', encoding='utf-8')
            before = ai.read_bytes()
            p = run(route + '-' + name, exe, bad, option, f'--export-filename={ai}', ok=False)
            check(route + ': ' + name + ' refused by name without replacing an existing file',
                  reason.encode() in p.stdout + p.stderr and ai.read_bytes() == before)
    transparent = out / 'transparent.svg'
    transparent.write_text(control(True)[0], encoding='utf-8')
    ai = out / 'transparent.ai'
    run('transparent-save', exe, transparent, '--export-extension=org.inkscape.output.ai.native', f'--export-filename={ai}')
    p = run('transparent-reopen', exe, ai, f'--export-filename={out / "transparent-reopened.svg"}')
    check('modern: compound clip with group transparency opens natively', b'opens from its PDF page' not in p.stdout + p.stderr)
    for name, svg in [('transparent-canvas', transparent), ('transparent-reopened', out / 'transparent-reopened.svg')]:
        run(name, exe, svg, '--export-background=white', f'--export-filename={out / (name + ".png")}')
    differences = probe(out / 'transparent-canvas.png', out / 'transparent-reopened.png', regions)
    check('modern: group transparency survives compound clipping', max(differences.values()) <= 3, differences)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)
    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
