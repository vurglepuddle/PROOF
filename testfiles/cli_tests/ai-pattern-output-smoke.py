"""Patterns in the modern .ai writer: tiles painted directly and through wrappers, offset, turned,
scaled and moved; on fills, strokes, compound paths and transformed groups; tile art with compound
paths, transparency, clips and a spot ink. Synthetic artwork only; sends no desktop input.
Illustrator's own reading is qualified separately with the generated control file.
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
WIDTH, HEIGHT = 640, 340

# Each painted object, and the box inside it where its pattern alone shows, in document pixels.
REGIONS = {
    'tile drawn directly': (24, 24, 156, 136),
    'turned and scaled through a wrapper': (184, 24, 316, 136),
    'offset tile on an ellipse': (370, 50, 450, 110),
    'moved tile on a compound path with a stroke': (505, 25, 525, 135),
    'pattern stroke': (22, 172, 34, 308),
    'pattern in a transformed group': (200, 190, 300, 270),
    'pattern fill with its own opacity': (364, 174, 476, 296),
    'moved tile again': (504, 174, 616, 296),
}


def control():
    """The control's SVG; also used for Illustrator qualification."""
    defs = (
        '<linearGradient id="ink" inkscape:swatch="solid" proof:ink="PROOF Pattern Orange" '
        'proof:ink-alternate="device-cmyk(0 0.6 1 0)" proof:ink-tint="0.5">'
        '<stop offset="0" style="stop-color:device-cmyk(0 0.3 0.5 0)"/></linearGradient>'
        '<clipPath id="corner"><rect width="9" height="9"/></clipPath>'
        # Asymmetric, so that a tile placed anywhere but where PROOF puts it shows.
        '<pattern id="checker" inkscape:label="PROOF Checker" patternUnits="userSpaceOnUse" width="24" height="24">'
        '<rect width="24" height="24" style="fill:#f2f2f2"/><rect width="12" height="12" style="fill:#cc3300"/>'
        '<circle cx="18" cy="18" r="3.5" style="fill:none;stroke:#003399;stroke-width:1.5"/></pattern>'
        # Offset by x and y, as some of PROOF's library patterns are.
        '<pattern id="dots" inkscape:label="PROOF Dots" patternUnits="userSpaceOnUse" x="3" y="4" width="18" height="14">'
        '<rect width="18" height="14" style="fill:#ffffcc"/>'
        '<path d="M1 1 H11 V11 H1 Z M4 4 V8 H8 V4 Z" style="fill:device-cmyk(1 0 0 0);fill-rule:evenodd"/>'
        '<g style="opacity:0.5"><rect x="9" y="6" width="8" height="7" style="fill:#00aa00"/></g>'
        '<g clip-path="url(#corner)" transform="translate(12 1)"><circle cx="5" cy="5" r="6" style="fill:#6600cc"/></g>'
        '<rect x="13" y="10" width="4" height="3" style="fill:url(#ink)"/></pattern>'
        '<pattern id="turned" href="#checker" patternTransform="rotate(30) scale(1.5)"/>'
        '<pattern id="moved" href="#dots" patternTransform="translate(7 5)"/>')
    art = ''.join([
        '<rect x="20" y="20" width="140" height="120" style="fill:url(#checker)"/>',
        '<rect x="180" y="20" width="140" height="120" style="fill:url(#turned)"/>',
        '<ellipse cx="410" cy="80" rx="70" ry="60" style="fill:url(#dots)"/>',
        '<path d="M500 20 H620 V140 H500 Z M530 50 V110 H590 V50 Z" '
        'style="fill:url(#moved);fill-rule:evenodd;stroke:#000000;stroke-width:2"/>',
        '<rect x="28" y="178" width="124" height="124" style="fill:none;stroke:url(#checker);stroke-width:16"/>',
        '<g transform="translate(180 170) rotate(-10 70 60) scale(0.9)">'
        '<rect width="150" height="130" style="fill:url(#checker)"/></g>',
        '<rect x="360" y="170" width="120" height="130" style="fill:url(#dots);fill-opacity:0.6"/>',
        '<rect x="500" y="170" width="120" height="130" style="fill:url(#moved)"/>',
    ])
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            f'xmlns:proof="urn:x-proof:document:1" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">'
            f'<defs>{defs}</defs><g inkscape:groupmode="layer" inkscape:label="Patterns">{art}</g></svg>')


# PDF output only: the forms the writer refuses, drawn in PDF as the canvas draws them.
PDF_ONLY_REGIONS = {
    'tile sized to the object': (24, 24, 136, 96),
    'art fitted by a view box': (164, 24, 276, 96),
    'art in the units of the object': (304, 24, 416, 96),
}


def pdf_only_control():
    defs = ('<pattern id="sized" width="0.25" height="0.5">'
            '<rect width="9" height="9" style="fill:#cc3300"/><circle cx="20" cy="25" r="5" style="fill:#003399"/></pattern>'
            '<pattern id="fitted" patternUnits="userSpaceOnUse" width="20" height="16" viewBox="0 0 10 10">'
            '<rect width="5" height="5" style="fill:#008000"/><circle cx="7.5" cy="7.5" r="2" style="fill:#cc3300"/></pattern>'
            '<pattern id="relative" patternUnits="userSpaceOnUse" width="30" height="20" patternContentUnits="objectBoundingBox">'
            '<rect width="0.08" height="0.1" style="fill:#6600cc"/></pattern>')
    art = ('<rect x="20" y="20" width="120" height="80" style="fill:url(#sized)"/>'
           '<rect x="160" y="20" width="120" height="80" style="fill:url(#fitted)"/>'
           '<rect x="300" y="20" width="120" height="80" style="fill:url(#relative)"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="440" height="120" viewBox="0 0 440 120">'
            f'<defs>{defs}</defs>{art}</svg>')


def tiling_patterns(pdf):
    """{(matrix, box)} of the PDF's tiling patterns, rounded to three places; each must step by its box."""
    from pypdf import PdfReader
    found = set()

    def walk(resources):
        resources = resources.get_object() if resources is not None else {}
        for pattern in (resources.get('/Pattern') or {}).values():
            pattern = pattern.get_object()
            if pattern.get('/PatternType') == 1:
                box = tuple(round(float(v), 3) for v in pattern['/BBox'])
                steps = (round(float(pattern['/XStep']), 3), round(float(pattern['/YStep']), 3))
                matrix = tuple(round(float(v), 3) for v in pattern.get('/Matrix', [1, 0, 0, 1, 0, 0]))
                found.add((matrix, box) if steps == (box[2] - box[0], box[3] - box[1]) else (matrix, box, steps))
                walk(pattern.get('/Resources'))
        for xobject in (resources.get('/XObject') or {}).values():
            xobject = xobject.get_object()
            if xobject.get('/Subtype') == '/Form':
                walk(xobject.get('/Resources'))

    walk(PdfReader(pdf, strict=True).pages[0]['/Resources'])
    return {tuple(tuple(0.0 if v == 0 else v for v in part) for part in entry) for entry in found}


def pixels(png):
    with Image.open(png) as im:
        return np.asarray(im.convert('RGB'), dtype=np.int16)


def compare(a_png, b_png, scale=1.0, reach=6, regions=None):
    """Per region: the largest channel difference, the share of pixels more than 24 apart, and
    the shift of b (in pixels, within `reach`) that matches a best."""
    a, b = pixels(a_png), pixels(b_png)
    out = {}
    for name, (x0, y0, x1, y1) in (regions or REGIONS).items():
        x0, y0, x1, y1 = (round(v * scale) for v in (x0, y0, x1, y1))
        here = a[y0:y1, x0:x1]
        best = None
        for dy in range(-reach, reach + 1):
            for dx in range(-reach, reach + 1):
                there = b[y0 + dy:y1 + dy, x0 + dx:x1 + dx]
                mean = float(np.abs(here - there).mean())
                if best is None or mean < best[0] - 1e-9:
                    best = (mean, dx, dy)
        diff = np.abs(here - b[y0:y1, x0:x1]).max(axis=2)
        out[name] = dict(max=int(diff.max()), over24=round(float((diff > 24).mean()), 4),
                         mean=round(float(diff.mean()), 3), shift=[best[1], best[2]])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', required=True, type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', type=Path)
    ap.add_argument('--pdftocairo', type=Path, help="default: beside --pdftoppm")
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        (out / name.lower()).mkdir()
        env[name] = str(out / name.lower())
    env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    exe = args.inkscape.resolve()
    native = '--export-extension=org.inkscape.output.ai.native'
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

    def records_of(ai):
        """The records the .ai file keeps beside its PDF page, Zstandard as Illustrator's."""
        from pypdf import PdfReader
        private = PdfReader(ai, strict=True).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        marker = b'%AI24_ZStandard_Data'
        if not packed.startswith(marker):
            raise RuntimeError(f'{ai.name}: missing Zstandard records')
        return subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'], input=packed[len(marker):],
                              capture_output=True, timeout=60, check=True).stdout

    def definitions(records):
        """{name: (box line, number of hidden lines)} of the pattern definitions."""
        found = {}
        for m in re.finditer(rb'^%AI3_BeginPattern: \((.*)\)\n(\(.*\) [-\d. ]+)\n((?:%_.*\n)*)E\n%AI3_EndPattern$',
                             records, re.M):
            found[m.group(1).decode()] = (m.group(2).decode(), m.group(3).count(b'\n'))
        return found

    def painted(svg):
        """How many objects in the layers are painted with a pattern, and the tiles they use."""
        root = ET.parse(svg).getroot()
        ids = {e.get('id'): e for e in root.iter() if e.get('id')}
        uses, tiles = 0, set()
        for layer in root.iter(SVG + 'g'):
            if layer.get('{http://www.inkscape.org/namespaces/inkscape}groupmode') != 'layer':
                continue
            for e in layer.iter():
                for m in re.finditer(r'(?:fill|stroke):url\(#([^)]*)\)', e.get('style') or ''):
                    target = ids.get(m.group(1))
                    while target is not None and target.tag == SVG + 'pattern' and not len(target):
                        href = target.get('{http://www.w3.org/1999/xlink}href') or target.get('href')
                        target = ids.get(href[1:]) if href else None
                    if target is not None and target.tag == SVG + 'pattern':
                        uses += 1
                        tiles.add((target.get('width'), target.get('height'), len(list(target.iter())) - 1))
        return uses, sorted(tiles)

    def opens_natively(p):
        return b'opens from its PDF page' not in p.stdout + p.stderr

    source = out / 'control.svg'
    source.write_text(control(), encoding='utf-8')
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    run('canvas', exe, source, '--export-background=white', f'--export-filename={out / "canvas.png"}')
    with Image.open(out / 'canvas.png') as im:
        im = im.convert('RGB')
        # The first tile's corner at the document's 24, 24: red, then the light ground beside it.
        check('the control draws its asymmetric tile where SVG places it',
              im.getpixel((30, 30)) == (204, 51, 0) and im.getpixel((42, 30)) == (242, 242, 242),
              [im.getpixel((30, 30)), im.getpixel((42, 30))])

    ai = out / 'modern.ai'
    run('save', exe, source, native, f'--export-filename={ai}')
    records = records_of(ai)
    (out / 'records.txt').write_bytes(records)
    found = definitions(records)
    check('one definition for each tile, its box from 0,0, every line of its art hidden',
          sorted(found) == ['PROOF Checker', 'PROOF Dots'] and found['PROOF Checker'][0] == '(PROOF Checker) 0 0 24 24'
          and found['PROOF Dots'][0] == '(PROOF Dots) 0 0 18 14', found)
    setup_end = records.find(b'%%EndSetup')
    check('the definitions are in the setup', 0 < records.find(b'%AI3_BeginPattern') < setup_end)
    fills = re.findall(rb'^\((PROOF [A-Za-z]+)\) 0 0 1 1 0 0 0 0 0 \[([-\d. ]+)\] p$', records, re.M)
    strokes = re.findall(rb'^\((PROOF [A-Za-z]+)\) 0 0 1 1 0 0 0 0 0 \[([-\d. ]+)\] P$', records, re.M)
    check('seven pattern fills and one pattern stroke', len(fills) == 7 and len(strokes) == 1, (len(fills), len(strokes)))
    # The tile drawn directly: its top-left at the document's origin, y up in points.
    check("the direct tile's matrix carries its space onto the page in points",
          (b'PROOF Checker', b'0.75 0 0 0.75 0 -18') in fills, [(n.decode(), m.decode()) for n, m in fills[:2]])
    check('the tile keeps its spot ink, and the file declares it',
          re.search(rb'^%_0 0\.6 1 0 \(PROOF Pattern Orange\) 0\.5 0 Xx$', records, re.M) is not None
          and b'%%CMYKCustomColor: 0 0.6 1 0 (PROOF Pattern Orange)' in records)
    check('the transparent group in the tile keeps its opacity', b'%_0 0.5 0 0 0 Xy' in records)

    reopened = out / 'modern.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    check('reopens natively', opens_natively(p))
    uses, tiles = painted(reopened)
    check('all eight objects stay painted with the two tiles', uses == 8 and len(tiles) == 2, (uses, tiles))
    run('reopened-draw', exe, reopened, '--export-background=white', f'--export-filename={out / "modern.png"}')
    differences = compare(out / 'canvas.png', out / 'modern.png')
    (out / 'reopened-differences.json').write_text(json.dumps(differences, indent=2), encoding='utf-8')
    check('every tile reopens where it was, drawn the same',
          all(d['shift'] == [0, 0] and d['max'] <= 3 for d in differences.values()), differences)

    # The page's tiling patterns, each in the space of the object it paints: SVG's own placement,
    # translate(x, y) and the pattern transform, with the tile from 0,0 and steps of its size.
    expected = {((1, 0, 0, 1, 0, 0), (0, 0, 24, 24)), ((1.299, 0.75, -0.75, 1.299, 0, 0), (0, 0, 24, 24)),
                ((1, 0, 0, 1, 3, 4), (0, 0, 18, 14)), ((1, 0, 0, 1, 10, 9), (0, 0, 18, 14))}
    found_tiles = tiling_patterns(ai)
    check("the PDF page's tiling patterns place each tile as SVG does", found_tiles == expected, sorted(found_tiles))
    cairo = args.pdftocairo or (args.pdftoppm.with_name('pdftocairo.exe') if args.pdftoppm else None)
    if cairo and cairo.exists() and args.pdftoppm:
        # Each Poppler backend draws one part right: splash rounds a turned cell to whole pixels and
        # drifts, Cairo darkens transparency inside a tile. Each is asked only what it draws right.
        run('pdf-cairo', cairo, '-scale-to-x', WIDTH, '-scale-to-y', HEIGHT, '-png', '-singlefile', ai, out / 'pdf-cairo')
        run('pdf-splash', args.pdftoppm, '-scale-to-x', WIDTH, '-scale-to-y', HEIGHT, '-png', '-singlefile', ai,
            out / 'pdf-splash')
        drawn = compare(out / 'canvas.png', out / 'pdf-cairo.png')
        transparent = 'pattern fill with its own opacity'
        drawn[transparent] = compare(out / 'canvas.png', out / 'pdf-splash.png')[transparent]
        (out / 'pdf-differences.json').write_text(json.dumps(drawn, indent=2), encoding='utf-8')
        # CMYK cyan converts differently in Poppler and the canvas, by up to 14.
        check('the PDF page draws every tile where the canvas does',
              all(d['shift'] == [0, 0] and d['max'] <= 16 for k, d in drawn.items() if k != transparent)
              and drawn[transparent]['shift'] == [0, 0] and drawn[transparent]['over24'] <= 0.05, drawn)
        other = out / 'pdf-only.svg'
        other.write_text(pdf_only_control(), encoding='utf-8')
        run('pdf-only-canvas', exe, other, '--export-background=white', f'--export-filename={out / "pdf-only.png"}')
        run('pdf-only-save', exe, other, f'--export-filename={out / "pdf-only.pdf"}')
        run('pdf-only-page', cairo, '-scale-to-x', 440, '-scale-to-y', 120, '-png', '-singlefile',
            out / 'pdf-only.pdf', out / 'pdf-only-page')
        drawn = compare(out / 'pdf-only.png', out / 'pdf-only-page.png', regions=PDF_ONLY_REGIONS)
        check('PDF export: tiles sized to the object, fitted by a view box or in its units, as the canvas',
              all(d['shift'] == [0, 0] and d['max'] <= 3 for d in drawn.values()), drawn)

    again, again_svg = out / 'again.ai', out / 'again.svg'
    run('again', exe, reopened, native, f'--export-filename={again}')
    second = definitions(records_of(again))
    check('a second write keeps the same two definitions, no larger', second == found, second)
    p = run('again-reopen', exe, again, f'--export-filename={again_svg}')
    run('again-draw', exe, again_svg, '--export-background=white', f'--export-filename={out / "again.png"}')
    differences = compare(out / 'canvas.png', out / 'again.png')
    check('the second write opens natively and draws the same', opens_natively(p)
          and all(d['shift'] == [0, 0] and d['max'] <= 3 for d in differences.values()), differences)

    legacy = out / 'legacy7.ai'
    legacy.write_bytes(b'before')
    p = run('legacy7', exe, source, '--export-extension=org.inkscape.output.ai.legacy7',
            f'--export-filename={legacy}', ok=False)
    check('the Illustrator 7 copy refuses patterns by name without replacing a file',
          b'patterns (Illustrator 7)' in p.stdout + p.stderr and legacy.read_bytes() == b'before')
    refusals = [
        ('bounding-box', '<pattern id="bad" width="0.25" height="0.25"><rect width="5" height="5"/></pattern>',
         'patterns sized to their object'),
        ('nested', '<pattern id="inner" patternUnits="userSpaceOnUse" width="4" height="4"><rect width="2" height="2"/>'
         '</pattern><pattern id="bad" patternUnits="userSpaceOnUse" width="10" height="10">'
         '<rect width="5" height="5" style="fill:url(#inner)"/></pattern>', 'patterns inside patterns'),
        ('gradient', '<linearGradient id="g"><stop stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/>'
         '</linearGradient><pattern id="bad" patternUnits="userSpaceOnUse" width="10" height="10">'
         '<rect width="5" height="5" style="fill:url(#g)"/></pattern>', 'gradients in patterns'),
    ]
    for name, pattern, reason in refusals:
        bad = out / f'refused-{name}.svg'
        bad.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">'
                       f'<defs>{pattern}</defs><rect width="100" height="100" style="fill:url(#bad)"/></svg>',
                       encoding='utf-8')
        before = ai.read_bytes()
        p = run('refused-' + name, exe, bad, native, f'--export-filename={ai}', ok=False)
        check(name + ': refused by name without replacing an existing file',
              reason.encode() in p.stdout + p.stderr and ai.read_bytes() == before)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)
    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
