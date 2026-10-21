"""The Illustrator 7 export's declared subset together, in one synthetic control; no desktop input.

Every form the version 7 copy keeps, in one CMYK document of one artboard: eleven layers with
Windows-1252 names and every layer option it writes (hidden, locked, non-printing, outline,
dimmed), objects outside any layer, CMYK, RGB and two spot inks with tints and overprint, strokes
with their caps, joins and dashes, compound paths under both fill rules, smooth points, nested
transformed groups, a clone, a clip group, a locked object, linear, circular radial and
spot-stop gradients, and opaque grey, RGB and CMYK pictures fitted, turned, reflected, cropped
and clipped. The copy must be one standalone ASCII file whose closed paths end on their first
point, hold every picture's samples, reopen from its own records with every layer option, ink,
tint and overprint, draw as the source at every probe, and write the same records again.
Illustrator's own reading of the control is separate qualification (compare_illustrator below).
"""
import argparse
import hashlib
from html import escape
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image

HERE = Path(__file__).resolve().parent
SVG = '{http://www.w3.org/2000/svg}'
INK = '{http://www.inkscape.org/namespaces/inkscape}'
SODI = '{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}'
PROOF = '{urn:x-proof:document:1}'
WIDTH, HEIGHT = 600, 520
# Names that need Windows-1252: é E9, Ü DC, É C9, € 80.
ORANGE, GREEN = 'Orange spécial', 'Vert Ü'
LAYERS = ['Fonds', 'Ébauche €', 'Tracés', 'Dégradés', 'Images', 'Groupes',
          'Estompé', 'Verrouillé', 'Non imprimable', 'Contour', 'Caché']


def pictures():
    p = runpy.run_path(str(HERE / 'ai-picture-output-smoke.py'))
    _, rgb, cmyk = p['pictures']()
    gray = Image.new('L', (20, 30))
    gray.putdata([x * 8 + y * 3 for y in range(30) for x in range(20)])
    return dict(rgb=rgb, cmyk=cmyk, gray=gray), p['data_uri'], p['centres']


def control():
    """The control's SVG and its probes: {region: [(x, y), ...]} at 96 dpi; regions whose
    name starts with 'empty' must show what lies under the art (the background)."""
    images, uri, centres = pictures()
    rgb, cmyk, gray = images['rgb'], images['cmyk'], images['gray']

    def layer(name, body, **options):
        attrs = ''.join(f' {k}="{v}"' for k, v in options.items())
        return f'<g inkscape:groupmode="layer" inkscape:label="{escape(name, quote=True)}"{attrs}>{body}</g>'

    def spot(id, name, alternate, tint):
        stop = ' '.join(f'{c * tint:g}' for c in alternate)
        return (f'<linearGradient id="{id}" inkscape:swatch="solid" proof:ink="{escape(name, quote=True)}" '
                f'proof:ink-alternate="device-cmyk({" ".join(map(str, alternate))})" proof:ink-tint="{tint}">'
                f'<stop offset="0" style="stop-color:device-cmyk({stop})"/></linearGradient>')

    orange, green = (0, 0.6, 1, 0), (0.8, 0, 0.9, 0.1)
    defs = ''.join([
        spot('orange40', ORANGE, orange, 0.4), spot('green', GREEN, green, 1),
        '<clipPath id="round"><circle cx="240" cy="450" r="40"/></clipPath>',
        '<clipPath id="disc"><circle cx="535" cy="340" r="30"/></clipPath>',
        '<linearGradient id="lin" gradientUnits="userSpaceOnUse" x1="140" y1="0" x2="290" y2="0" '
        'gradientTransform="rotate(15 215 255)"><stop style="stop-color:device-cmyk(0 0 1 0)"/>'
        '<stop offset="0.5" style="stop-color:device-cmyk(0 1 1 0)"/>'
        '<stop offset="1" style="stop-color:device-cmyk(1 0.2 0 0.3)"/></linearGradient>',
        '<radialGradient id="rad" gradientUnits="userSpaceOnUse" cx="345" cy="255" r="28">'
        '<stop style="stop-color:device-cmyk(0 0 0.2 0)"/><stop offset="1" style="stop-color:device-cmyk(0.9 0.5 0 0.1)"/>'
        '</radialGradient>',
        f'<linearGradient id="spotlin" gradientUnits="userSpaceOnUse" x1="400" y1="0" x2="570" y2="0">'
        f'<stop style="stop-color:device-cmyk(0 0.6 1 0)" proof:ink="{escape(ORANGE, quote=True)}" '
        f'proof:ink-alternate="device-cmyk(0 0.6 1 0)"/>'
        f'<stop offset="0.5" style="stop-color:device-cmyk(0 0.15 0.25 0)" proof:ink="{escape(ORANGE, quote=True)}" '
        f'proof:ink-alternate="device-cmyk(0 0.6 1 0)" proof:ink-tint="0.25"/>'
        f'<stop offset="1" style="stop-color:device-cmyk(0.6 0 0.2 0)"/></linearGradient>',
    ])
    cmykf = lambda c, m, y, k: f'device-cmyk({c} {m} {y} {k})'
    background = layer(LAYERS[0], f'<rect x="5" y="5" width="590" height="510" '
                       f'style="fill:{cmykf(0.04, 0, 0.08, 0)};stroke:{cmykf(0, 0, 0, 0.6)};stroke-width:2"/>')
    paints = layer(LAYERS[1], ''.join([
        f'<rect x="30" y="30" width="90" height="60" sodipodi:insensitive="true" style="fill:{cmykf(0.1, 0.2, 0.3, 0.4)};'
        f'stroke:{cmykf(1, 0, 0, 0)};stroke-width:4;stroke-linejoin:miter"/>',
        '<ellipse cx="190" cy="60" rx="45" ry="30" style="fill:url(#orange40)" proof:overprint="fill"/>',
        f'<path d="M 250 35 L 330 35 L 320 95 L 260 95 Z" style="fill:{cmykf(0.7, 0, 0.2, 0)};stroke:url(#green);'
        'stroke-width:6;stroke-linejoin:round" proof:overprint="stroke"/>',
        '<circle cx="380" cy="60" r="30" style="fill:#2060c0"/>',
    ]), **{'inkscape:highlight-color': '#ff9900'})
    paths = layer(LAYERS[2], ''.join([
        f'<path d="M 30 120 H 130 V 200 H 30 Z M 55 140 V 180 H 105 V 140 Z" style="fill:{cmykf(0, 0.8, 0.6, 0)};fill-rule:evenodd"/>',
        f'<path d="M 150 120 H 250 V 200 H 150 Z M 175 140 V 180 H 225 V 140 Z" style="fill:{cmykf(0.6, 0.2, 0, 0)};fill-rule:nonzero"/>',
        f'<path d="M 270 190 C 290 110 330 110 350 160 S 400 210 420 130" style="fill:none;stroke:{cmykf(0, 0.5, 1, 0.2)};'
        'stroke-width:6;stroke-linecap:round;stroke-dasharray:14 8"/>',
        f'<path d="M 440 160 C 440 120 480 110 500 110 C 530 110 560 130 560 160 L 540 200 L 460 200 Z" sodipodi:nodetypes="csccc" '
        f'style="fill:{cmykf(0.2, 0, 0.6, 0)}"/>',
        f'<path d="M 30 230 L 80 215 L 120 240" style="fill:none;stroke:{cmykf(0, 0, 0, 0.8)};stroke-width:8;'
        'stroke-linecap:square;stroke-linejoin:bevel"/>',
    ]), **{'inkscape:highlight-color': '#4fff4f'})
    gradients = layer(LAYERS[3], ''.join([
        '<rect x="140" y="230" width="150" height="50" style="fill:url(#lin)"/>',
        f'<circle cx="345" cy="255" r="28" style="fill:url(#rad);stroke:{cmykf(0, 0, 0, 1)};stroke-width:2"/>',
        '<rect x="400" y="230" width="170" height="50" style="fill:url(#spotlin)"/>',
    ]))
    pictured = layer(LAYERS[4], ''.join([
        f'<image x="30" y="300" width="96" height="64" preserveAspectRatio="none" href="{uri(cmyk, "TIFF")}"/>',
        f'<image x="140" y="300" width="40" height="60" preserveAspectRatio="none" href="{uri(gray, "TIFF")}"/>',
        f'<image x="200" y="305" width="90" height="60" preserveAspectRatio="none" transform="rotate(10 245 335)" '
        f'href="{uri(rgb, "PNG")}"/>',
        f'<image x="300" y="300" width="60" height="80" preserveAspectRatio="xMidYMid slice" href="{uri(rgb, "PNG")}"/>',
        f'<g transform="translate(470 300) scale(-1 1)"><image width="90" height="60" preserveAspectRatio="none" '
        f'href="{uri(rgb, "PNG")}"/></g>',
        f'<g clip-path="url(#disc)"><image x="495" y="300" width="80" height="80" preserveAspectRatio="none" '
        f'href="{uri(rgb, "PNG")}"/></g>',
    ]))
    stripes = ''.join(f'<rect x="{195 + 15 * i}" y="405" width="15" height="90" style="fill:'
                      f'{cmykf(0, 1, 0, 0) if i % 2 else cmykf(0, 0, 0, 0.3)}"/>' for i in range(6))
    groups = layer(LAYERS[5], ''.join([
        '<g transform="translate(40 410) rotate(-8) scale(1.2)"><g id="badge">'
        f'<rect width="60" height="40" style="fill:{cmykf(0.3, 0, 0, 0)};stroke:{cmykf(1, 1, 0, 0)};stroke-width:2"/>'
        f'<circle cx="30" cy="20" r="10" style="fill:{cmykf(0, 0, 1, 0)}"/></g></g>',
        '<use href="#badge" transform="translate(125 410)"/>',
        f'<g clip-path="url(#round)"><g>{stripes}</g></g>',
    ]))
    box = lambda x, c: f'<rect x="{x}" y="410" width="50" height="40" style="fill:{c}"/>'
    options = ''.join([
        layer(LAYERS[6], box(310, cmykf(0.5, 0.5, 0, 0)), **{'proof:layer-dim': '50'}),
        layer(LAYERS[7], box(370, cmykf(0, 0.5, 0.5, 0)), **{'sodipodi:insensitive': 'true'}),
        layer(LAYERS[8], box(430, cmykf(0.5, 0, 0.5, 0)), **{'proof:layer-print': 'false'}),
        layer(LAYERS[9], box(490, cmykf(0.2, 0.2, 0.2, 0)), **{'proof:layer-preview': 'false'}),
        layer(LAYERS[10], '<rect x="300" y="460" width="280" height="40" fill="#ff0000"/>', style='display:none'),
    ])
    loose = f'<rect x="450" y="30" width="120" height="60" style="fill:{cmykf(0, 0.3, 0, 0.1)}"/>'
    text = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            'xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="CMYK">'
            f'<defs>{defs}</defs>{background}{paints}{paths}{gradients}{pictured}{groups}{options}{loose}</svg>')
    regions = {
        'locked CMYK fill': [(50, 50), (75, 60), (100, 75)],
        'CMYK stroke, miter': [(30, 60), (75, 30), (120, 60)],
        'spot tint, overprint fill': [(190, 60), (170, 55), (210, 70)],
        'CMYK fill under spot stroke': [(290, 65), (280, 80)],
        'spot stroke, round join, overprint': [(290, 35), (275, 95), (326, 60)],
        'RGB in a CMYK document': [(380, 60), (370, 70)],
        'object outside layers': [(510, 60), (470, 45), (555, 80)],
        'even-odd compound': [(40, 160), (120, 160), (80, 128)],
        'empty even-odd hole': [(80, 160)],
        'nonzero compound': [(160, 160), (240, 160), (200, 128)],
        'empty nonzero hole': [(200, 160)],
        'smooth points': [(500, 160), (470, 170), (530, 150)],
        'square cap, bevel join': [(55, 222), (100, 228)],
        'linear CMYK gradient': [(150, 255), (180, 255), (215, 255), (250, 255), (280, 255)],
        'radial CMYK gradient': [(345, 255), (335, 250), (355, 262), (345, 240)],
        'stroke on a gradient fill': [(345, 227), (317, 255)],
        'spot-stop gradient': [(410, 255), (450, 255), (485, 255), (530, 255), (560, 255)],
        'CMYK picture': centres(30, 300, 96, 64, 24, 16, 4, 4, 4),
        'grey picture': centres(140, 300, 40, 60, 20, 30, 2, 2, 5),
        'turned RGB picture': [(245, 335), (230, 330), (260, 340)],
        'slice-cropped picture': [(310, 310), (330, 340), (350, 370)],
        'empty beside the crop': [(295, 340), (366, 340)],
        'reflected picture': [(425, 330), (400, 320), (450, 345)],
        'clipped picture': [(535, 340), (525, 330), (545, 350)],
        'empty outside the picture clip': [(500, 305), (570, 375)],
        'nested turned group': [(79, 429), (57, 444)],
        'clone': [(155, 430), (135, 445)],
        'clip group': [(217, 450), (232, 450), (247, 450), (262, 450)],
        'empty outside the group clip': [(205, 412), (277, 490)],
        'dimmed layer': [(335, 430)],
        'locked layer': [(395, 430)],
        'non-printing layer': [(455, 430)],
        'outline layer': [(515, 430)],
        'empty under the hidden layer': [(440, 480), (330, 490)],
    }
    return text, regions, images


def records_of(path):
    return path.read_bytes()


def probe(a_png, b_png, regions, scale=1.0):
    """Largest channel difference per region, at points scaled from 96 dpi."""
    with Image.open(a_png) as a, Image.open(b_png) as b:
        a, b = a.convert('RGB'), b.convert('RGB')
        worst = {}
        for name, points in regions.items():
            worst[name] = max(max(abs(x - y) for x, y in zip(a.getpixel(q), b.getpixel(q)))
                              for q in ((int(px * scale), int(py * scale)) for px, py in points))
        return worst


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
    legacy = '--export-extension=org.inkscape.output.ai.legacy7'
    checks = []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name + (f' {detail}' if detail != '' else ''), flush=True)

    def run(label, *command):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(f'{label}: exit {p.returncode}; see log')
        return p

    crop = runpy.run_path(str(HERE / 'ai-picture-crop-output-smoke.py'))
    text, regions, images = control()
    source = out / 'control.svg'
    source.write_text(text, encoding='utf-8')
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    ai = out / 'control.ai'
    run('save', exe, source, legacy, f'--export-filename={ai}')
    records = ai.read_bytes()
    (out / 'expected.json').write_text(json.dumps(dict(layers=LAYERS + ['Layer'], inks=[ORANGE, GREEN],
                                                       regions=regions), ensure_ascii=False, indent=1), encoding='utf-8')

    def ps_name(name):
        return b'(' + b''.join(b'\\' + bytes([c]) if c in (40, 41, 92) else
                              ('\\%03o' % c).encode() if c < 32 or c >= 127 else bytes([c])
                              for c in name.encode('cp1252')) + b')'

    check('standalone version 7 records, all ASCII', records.startswith(b'%!PS-Adobe-3.0\n')
          and b'%AI5_FileFormat 3\n' in records and b'%AI9_ColorModel: 2\n' in records
          and all(c < 128 for c in records) and b'%PDF-' not in records and b'AIPrivateData' not in records)
    check('one artboard', b'%AI3_Cropmarks: 0 0 450 390\n' in records)
    written_layers = re.findall(rb'^(\([^\n]*\)) Ln$', records, re.M)
    check('eleven named layers and one for objects outside them, in order',
          written_layers == [ps_name(n) for n in LAYERS] + [b'(Layer)'], len(written_layers))
    flags = [line.split() for line in records.split(b'\n') if line.endswith(b' Lb')]
    expected_flags = {6: (b'1', b'1', b'1', b'1', b'1'), 7: (b'1', b'1', b'0', b'1', b'0'),
                      8: (b'1', b'1', b'1', b'0', b'0'), 9: (b'1', b'0', b'1', b'1', b'0'),
                      10: (b'0', b'1', b'1', b'1', b'0')}
    check('layer options: dimmed, locked, non-printing, outline, hidden',
          len(flags) == 12 and all(tuple(flags[i][:5]) == f for i, f in expected_flags.items())
          and all(tuple(f[:5]) == (b'1', b'1', b'1', b'1', b'0') for i, f in enumerate(flags) if i not in expected_flags))
    check('spot inks by Windows-1252 name, tint and overprint',
          ps_name(ORANGE) + b' 0.6 x' in records and ps_name(GREEN) + b' 0 X' in records
          and b'1 R\n' in records and b'1 O\n' in records)
    check('linear, radial and spot-stop gradients defined', b'3 Bn\n' in records
          and len(re.findall(rb' 0 3 Bd$', records, re.M)) == 2 and len(re.findall(rb' 1 2 Bd$', records, re.M)) == 1)
    check('closed paths end on their first point', not crop['unclosed_paths'](records))
    blocks = re.findall(rb'\[[^\n]*\] ([^\n]+)\n%%BeginData: (\d+) ASCII Bytes\nXI\n((?:%[0-9A-F]+\n)+)%%EndData\n'
                        rb'%AI5_EndRaster\nN\n', records)
    samples = {}
    for fields, _, lines in blocks:
        f = list(map(int, fields.split()))
        samples.setdefault(f[7], []).append(bytes.fromhex(lines.replace(b'%', b'').decode()))
    check('six opaque pictures, CMYK and grey samples exact', len(blocks) == 6
          and samples.get(4) == [images['cmyk'].tobytes()] and samples.get(1) == [images['gray'].tobytes()]
          and len(samples.get(3, [])) == 4, len(blocks))

    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    root = ET.parse(reopened).getroot()
    layers = [g for g in root.iter(SVG + 'g') if g.get(INK + 'groupmode') == 'layer']
    check('reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr)
    check('layer names as written', [g.get(INK + 'label') for g in layers] == LAYERS + ['Layer'],
          [g.get(INK + 'label') for g in layers])
    named = {g.get(INK + 'label'): g for g in layers}
    check('layer options as written', named[LAYERS[6]].get(PROOF + 'layer-dim') == '50'
          and named[LAYERS[7]].get(SODI + 'insensitive') == 'true'
          and named[LAYERS[8]].get(PROOF + 'layer-print') == 'false'
          and named[LAYERS[9]].get(PROOF + 'layer-preview') == 'false'
          and 'display:none' in named[LAYERS[10]].get('style', ''))
    check('locked object stays locked', any(e.get(SODI + 'insensitive') == 'true' for e in named[LAYERS[1]].iter()
                                            if e is not named[LAYERS[1]]))
    inks = {(e.get(PROOF + 'ink'), round(float(e.get(PROOF + 'ink-tint', '1')), 2)) for e in root.iter() if e.get(PROOF + 'ink')}
    check('spot inks and tints', {(ORANGE, 0.4), (GREEN, 1.0)} <= inks and {n for n, _ in inks} == {ORANGE, GREEN}, sorted(inks))
    overprints = sorted(e.get(PROOF + 'overprint') for e in root.iter() if e.get(PROOF + 'overprint'))
    check('overprint fill and stroke', overprints == ['fill', 'stroke'], overprints)
    text_back = reopened.read_text(encoding='utf-8')
    # The import gives a CMYK document without a profile the working one: its numbers in icc-color().
    check('CMYK numbers', re.search(r'device-cmyk\(0\.1 0\.2 0\.3 0\.4\)|icc-color\([^,)]+, 0\.1, 0\.2, 0\.3, 0\.4\)', text_back))
    check('RGB paint kept as RGB', 'fill:#2060c0' in text_back)
    # In the machine's own locale, as the editor runs: a Windows-1252 one once made an ink's id
    # of a UTF-8 byte, and the SVG unreadable.
    own = out / 'reopened-own-locale.svg'
    p = subprocess.run([str(exe), str(ai), f'--export-filename={own}'], capture_output=True, timeout=180,
                       env={k: v for k, v in env.items() if k not in ('LANG', 'LC_ALL')})
    try:
        ET.parse(own)
        well_formed = p.returncode == 0
    except (ET.ParseError, OSError):
        well_formed = False
    check("reopens to well-formed SVG in the machine's own locale", well_formed)
    check('six embedded pictures, grey and CMYK samples kept for print',
          sum(1 for _ in root.iter(SVG + 'image')) == 6 and sum(1 for e in root.iter(SVG + 'image') if e.get(PROOF + 'samples')) == 2)

    canvas, drawn = out / 'canvas.png', out / 'reopened.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    run('drawn', exe, reopened, '--export-background=white', f'--export-filename={drawn}')
    worst = probe(canvas, drawn, regions)
    (out / 'probes.json').write_text(json.dumps(worst, ensure_ascii=False, indent=1), encoding='utf-8')
    # The reopened document has the working CMYK profile, and shows RGB as it would print, as
    # Illustrator does in a CMYK document; the source, without one, shows it as it is.
    over = {k: v for k, v in worst.items() if v > 3 and k != 'RGB in a CMYK document'}
    check('draws as the source at every probe (within 3), RGB apart', not over, over or worst)
    with Image.open(canvas) as a, Image.open(drawn) as b:
        diff = [max(abs(x - y) for x, y in zip(p, q)) for p, q in zip(a.convert('RGB').getdata(), b.convert('RGB').getdata())]
    check('few pixels differ, at edges and the RGB circle only', sum(d > 32 for d in diff) < 0.002 * len(diff),
          sum(d > 32 for d in diff))

    if args.pdftoppm:
        from pypdf import PdfReader
        pdf = out / 'reopened.pdf'
        run('pdf', exe, reopened, f'--export-filename={pdf}')
        found = {}
        for page in PdfReader(pdf).pages:
            stack = [page['/Resources']]
            while stack:
                resources = stack.pop()
                for ref in (resources.get('/XObject') or {}).values():
                    obj = ref.get_object()
                    if obj.get('/Subtype') == '/Image':
                        found.setdefault(str(obj.get('/ColorSpace')), []).append(obj.get_data())
                    elif obj.get('/Subtype') == '/Form' and obj.get('/Resources'):
                        stack.append(obj['/Resources'])
        check('PDF keeps the grey picture grey and the CMYK one CMYK, samples exact',
              found.get('/DeviceGray') == [images['gray'].tobytes()] and found.get('/DeviceCMYK') == [images['cmyk'].tobytes()],
              {k: len(v) for k, v in found.items()})

    # Written again from the reopened copy: the same records, but for the RGB paints (the hidden
    # layer's too), which the writer converts through the profile the document now has. Numbers
    # agree to 0.0005: PROOF's colour text keeps three decimals (colors/printer.h), and a turned
    # picture's matrix comes back from SVG with ten digits.
    again, again_svg, third = out / 'again.ai', out / 'again.svg', out / 'third.ai'
    run('again', exe, reopened, legacy, f'--export-filename={again}')
    body = lambda data: re.sub(rb'^%%(Title|CreationDate):[^\n]*\n', b'', data, flags=re.M).split(b'\n')

    def same(a, b):
        x, y = a.split(), b.split()
        if len(x) != len(y):
            return False
        for p, q in zip(x, y):
            try:
                if abs(float(p) - float(q)) > 5e-4:
                    return False
            except ValueError:
                if p != q:
                    return False
        return True

    def changes(a, b):
        return [(p, q) for p, q in zip(body(a), body(b)) if not same(p, q)] if len(body(a)) == len(body(b)) else None

    changed = changes(records, again.read_bytes())
    check('written again: the same records but the RGB paints, now CMYK', changed is not None
          and [p for p, _ in changed] == [b'0.1255 0.3765 0.7529 Xa', b'1 0 0 Xa'] and all(q.endswith(b' k') for _, q in changed),
          changed[:3] if changed else changed)
    run('again-reopen', exe, again, f'--export-filename={again_svg}')
    run('third', exe, again_svg, legacy, f'--export-filename={third}')
    check('and from then on the same records', changes(again.read_bytes(), third.read_bytes()) == [])
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == source_hash)
    (out / 'report.json').write_text(json.dumps(checks, indent=2, default=repr))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
