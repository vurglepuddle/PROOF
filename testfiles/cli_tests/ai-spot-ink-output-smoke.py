"""Spot inks defined in Lab and RGB in the modern .ai records, in one synthetic control; no
desktop input.

One CMYK document in a press profile with two inks defined in Lab (as Pantone's books define
theirs), one in RGB and one in CMYK, each at full strength and at a tint, as fills, strokes and gradient stops. The
copy must write each as Illustrator does: `c m y k v1 v2 v3 (name) tint type Xx`, type 2 for Lab
and 1 for RGB, at full strength too, the CMYK that of the colour at the tint used; the header's
%%RGBCustomColor list for RGB and Lab inks after the %%CMYKCustomColor list; gradient stops
twice, a stand-in of colour style 3 and the exact stop of style 4. It must
reopen from its own records with each ink's definition and tints and the document's profile byte
for byte, draw as the source, and write the same records again. Illustrator 7 refuses inks defined in RGB or Lab by name. Illustrator's
own reading is separate qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image

SVG = '{http://www.w3.org/2000/svg}'
INK = '{http://www.inkscape.org/namespaces/inkscape}'
PROOF = '{urn:x-proof:document:1}'
WIDTH, HEIGHT = 640, 360
PROFILE = Path(__file__).resolve().parents[1] / 'data' / 'colors' / 'default_cmyk.icc'
# name: (kind, definition); Lab as L a b, RGB from 0 to 1, CMYK from 0 to 1.
INKS = {
    'PROOF Lab Red': ('lab', (48, 68, 36)),
    'PROOF Lab Blue': ('lab', (30, 20, -60)),
    'PROOF RGB Green': ('rgb', (0.2, 0.8, 0.4)),
    'PROOF CMYK Orange': ('cmyk', (0, 0.6, 1, 0)),
}


def fmt(v):
    s = f'{v:.4f}'.rstrip('0').rstrip('.')
    return '0' if s == '-0' else s


def css(kind, values):
    if kind == 'lab':
        return 'lab({} {} {})'.format(*map(fmt, values))
    if kind == 'rgb':
        return '#' + ''.join(f'{round(v * 255):02x}' for v in values)
    return 'device-cmyk({} {} {} {})'.format(*map(fmt, values))


def shown(name, tint):
    """The colour a tint of the ink shows (spot-ink.h, display_color): CMYK scales, Lab and RGB
    blend toward paper white."""
    kind, v = INKS[name]
    if kind == 'lab':
        return css(kind, (100 - tint * (100 - v[0]), tint * v[1], tint * v[2]))
    if kind == 'rgb':
        return css(kind, tuple(1 - tint * (1 - x) for x in v))
    return css(kind, tuple(tint * x for x in v))


def ink_attrs(name, tint):
    kind, v = INKS[name]
    tint_attr = f' proof:ink-tint="{fmt(tint)}"' if tint < 1 else ''
    return f'proof:ink="{name}" proof:ink-alternate="{css(kind, v)}"{tint_attr}'


def swatch(sid, name, tint):
    return (f'<linearGradient id="{sid}" inkscape:swatch="solid" {ink_attrs(name, tint)}>'
            f'<stop offset="0" style="stop-color:{shown(name, tint)}"/></linearGradient>')


def stop(offset, name=None, tint=1.0, process=None):
    if name is None:
        return f'<stop offset="{offset}" style="stop-color:{process}"/>'
    return f'<stop offset="{offset}" style="stop-color:{shown(name, tint)}" {ink_attrs(name, tint)}/>'


def control():
    """The control's SVG and its probes at 96 dpi."""
    swatches = [('red', 'PROOF Lab Red', 1.0), ('red35', 'PROOF Lab Red', 0.35), ('blue', 'PROOF Lab Blue', 1.0),
                ('blue60', 'PROOF Lab Blue', 0.6), ('green', 'PROOF RGB Green', 1.0), ('green50', 'PROOF RGB Green', 0.5),
                ('orange', 'PROOF CMYK Orange', 1.0), ('orange40', 'PROOF CMYK Orange', 0.4)]
    defs = ''.join(swatch(*s) for s in swatches)
    defs += ('<linearGradient id="labramp" gradientUnits="userSpaceOnUse" x1="20" y1="0" x2="300" y2="0">'
             + stop(0, 'PROOF Lab Red') + stop(0.5, 'PROOF Lab Red', 0.3) + stop(1, process='device-cmyk(1 0 0 0)')
             + '</linearGradient>')
    defs += ('<linearGradient id="rgbramp" gradientUnits="userSpaceOnUse" x1="340" y1="0" x2="620" y2="0">'
             + stop(0, 'PROOF RGB Green') + stop(1, 'PROOF RGB Green', 0.0) + '</linearGradient>')
    defs += ('<linearGradient id="tworamp" gradientUnits="userSpaceOnUse" x1="20" y1="0" x2="300" y2="0">'
             + stop(0, 'PROOF Lab Blue') + stop(1, 'PROOF CMYK Orange') + '</linearGradient>')
    art = ''.join([
        '<rect x="20" y="20" width="60" height="60" style="fill:url(#red)"/>',
        '<rect x="100" y="20" width="60" height="60" style="fill:url(#red35)"/>',
        '<rect x="184" y="24" width="52" height="52" style="fill:none;stroke:url(#blue60);stroke-width:8"/>',
        '<rect x="264" y="24" width="52" height="52" style="fill:url(#blue);stroke:url(#red);stroke-width:8"/>',
        '<rect x="340" y="20" width="60" height="60" style="fill:url(#green)"/>',
        '<rect x="420" y="20" width="60" height="60" style="fill:url(#green50)"/>',
        '<rect x="500" y="20" width="50" height="60" style="fill:url(#orange)"/>',
        '<rect x="560" y="20" width="50" height="60" style="fill:url(#orange40)"/>',
        '<rect x="20" y="120" width="280" height="80" style="fill:url(#labramp)"/>',
        '<rect x="340" y="120" width="280" height="80" style="fill:url(#rgbramp)"/>',
        '<rect x="20" y="240" width="280" height="80" style="fill:url(#tworamp)"/>',
    ])
    # The CMYK beside an RGB or Lab ink comes through the document's profile, as Illustrator's
    # through its colour settings.
    profile = (f'<color-profile id="press" name="Press" rendering-intent="relative-colorimetric" '
               f'xlink:href="{PROFILE.as_uri()}"/>')
    text = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="CMYK" '
            f'proof:color-profile="Press"><defs>{profile}{defs}</defs>'
            f'<g inkscape:groupmode="layer" inkscape:label="Encres">{art}</g></svg>')
    regions = {
        'Lab red': [(50, 50)], 'Lab red at 35%': [(130, 50)], 'Lab blue stroke at 60%': [(188, 50)],
        'paper inside it': [(210, 50)], 'Lab blue fill': [(290, 50)], 'Lab red stroke': [(268, 50)],
        'RGB green': [(370, 50)], 'RGB green at 50%': [(450, 50)],
        'CMYK orange': [(525, 50)], 'CMYK orange at 40%': [(585, 50)],
        'Lab ramp': [(30, 160), (160, 160), (290, 160)], 'RGB ramp': [(350, 160), (480, 160), (610, 160)],
        'Lab to CMYK ink': [(30, 280), (160, 280), (290, 280)],
    }
    return text, regions


def probe(a_png, b_png, regions):
    with Image.open(a_png) as a, Image.open(b_png) as b:
        a, b = a.convert('RGB'), b.convert('RGB')
        return {name: max(max(abs(x - y) for x, y in zip(a.getpixel(q), b.getpixel(q))) for q in points)
                for name, points in regions.items()}


def inks_of(svg):
    """{(name, tint): alternate} of the spot swatches and ink stops in an SVG the import wrote."""
    root = ET.parse(svg).getroot()
    out = {}
    for e in root.iter():
        name = e.get(PROOF + 'ink')
        if name:
            out[(name, round(float(e.get(PROOF + 'ink-tint') or 1), 4))] = e.get(PROOF + 'ink-alternate')
    return out


def numbers(text):
    return [float(v) for v in re.findall(r'-?\d+(?:\.\d+)?', text or '')]


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
    native = '--export-extension=org.inkscape.output.ai.native'
    legacy = '--export-extension=org.inkscape.output.ai.legacy7'
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

    text, regions = control()
    source = out / 'control.svg'
    source.write_text(text, encoding='utf-8')
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    ai = out / 'control.ai'
    run('save', exe, source, native, f'--export-filename={ai}')
    records = records_of(ai)
    (out / 'records.txt').write_bytes(records)
    head = records[:records.find(b'%%EndComments')] if b'%%EndComments' in records else records[:4000]
    check('every ink named once in %%DocumentCustomColors',
          b'%%DocumentCustomColors: (PROOF CMYK Orange)\n%%+ (PROOF Lab Blue)\n%%+ (PROOF Lab Red)\n%%+ (PROOF RGB Green)\n'
          in head)
    cmyk_list = head.find(b'%%CMYKCustomColor: 0 0.6 1 0 (PROOF CMYK Orange)\n')
    rgb_list = head.find(b'%%RGBCustomColor: ')
    rgb_lines = re.findall(rb'^%%(?:RGBCustomColor:|\+) ([-\d. ]+) \((PROOF [^)]+)\)$', head[rgb_list:], re.M)
    check('CMYK inks by their CMYK, then RGB and Lab inks by their RGB', 0 <= cmyk_list < rgb_list
          and [n for _, n in rgb_lines] == [b'PROOF Lab Blue', b'PROOF Lab Red', b'PROOF RGB Green']
          and rgb_lines[2][0] == b'0.2 0.8 0.4', rgb_lines)

    def xx(name, tint, kind, op):
        """The CMYK of an Xx/XX record for the ink at the tint, or None."""
        ink_kind, v = INKS[name]
        values = ' '.join(fmt(x) for x in v).encode()
        m = re.search(rb'^([-\d.]+ [-\d.]+ [-\d.]+ [-\d.]+) ' + re.escape(values) + rb' \(' + re.escape(name.encode())
                      + rb'\) ' + re.escape(fmt(1 - tint).encode()) + rb' ' + kind + rb' ' + op + rb'$', records, re.M)
        return m.group(1) if m else None

    red_full, red_35 = xx('PROOF Lab Red', 1, b'2', b'Xx'), xx('PROOF Lab Red', 0.35, b'2', b'Xx')
    check('Lab inks: Xx type 2 at full strength and at a tint', red_full and red_35
          and xx('PROOF Lab Blue', 0.6, b'2', b'XX') and xx('PROOF Lab Blue', 1, b'2', b'Xx')
          and xx('PROOF Lab Red', 1, b'2', b'XX'), (red_full, red_35))
    check('beside the Lab, the CMYK of the colour at that tint', red_full and red_35 and red_full != red_35
          and all(a >= b for a, b in zip(numbers(red_full.decode()), numbers(red_35.decode()))), (red_full, red_35))
    check('RGB inks: Xx type 1', xx('PROOF RGB Green', 1, b'1', b'Xx') and xx('PROOF RGB Green', 0.5, b'1', b'Xx'))
    check('an ink at no tint is paper beside its definition', b'\n%_0 0 0 0 0.2 0.8 0.4 (PROOF RGB Green) 1 1 4 '
          in records)
    check('CMYK inks as before: x at full strength, Xx type 0 at a tint',
          b'\n0 0.6 1 0 (PROOF CMYK Orange) 0 x\n' in records and b'\n0 0.6 1 0 (PROOF CMYK Orange) 0.6 0 Xx\n' in records)
    stand_ins = re.findall(rb'^[-\d. ]+ \((PROOF [^)]+)\) ([-\d.]+) 3 1 6 50 ([-\d.]+) %_BS\n%_([-\d. ]+) \((PROOF [^)]+)\) '
                           rb'([-\d.]+) (\d) (\d) 1 6 50 ([-\d.]+) Bs$', records, re.M)
    kinds = sorted({(s[0], s[6], s[7]) for s in stand_ins if s[0] == s[4] and s[1] == s[5] and s[2] == s[8]})
    check('named stops twice: a stand-in, then the exact stop of style 4', kinds == [
        (b'PROOF Lab Blue', b'2', b'4'), (b'PROOF Lab Red', b'2', b'4'), (b'PROOF RGB Green', b'1', b'4')]
          and len(stand_ins) == 5, (kinds, len(stand_ins)))
    check('the other stops of those gradients in the newer form too', b'\n1 0 0 0 1 1 6 50 100 %_Bs\n' in records
          and b'\n0 0.6 1 0 (PROOF CMYK Orange) 0 3 1 6 50 100 %_Bs\n' in records)

    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    check('reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr)
    got = inks_of(reopened)
    want = {}
    for name, (kind, v) in INKS.items():
        for tint in {'PROOF Lab Red': (1, 0.35, 0.3), 'PROOF Lab Blue': (1, 0.6), 'PROOF RGB Green': (1, 0.5, 0.0),
                     'PROOF CMYK Orange': (1, 0.4)}[name]:
            want[(name, tint)] = (kind, v)
    wrong = []
    for key, (kind, v) in want.items():
        alternate = got.get(key)
        if alternate is None:
            wrong.append((key, 'missing'))
            continue
        values = numbers(alternate) if kind != 'rgb' else [int(alternate[i:i + 2], 16) / 255 for i in (1, 3, 5)]
        if not alternate.startswith({'lab': 'lab(', 'rgb': '#', 'cmyk': 'device-cmyk('}[kind]) or \
                any(abs(a - b) > (0.01 if kind == 'lab' else 0.003) for a, b in zip(values, v)):
            wrong.append((key, alternate))
    check('each ink comes back with its definition and tints', not wrong, wrong[:6])
    import base64
    embedded = re.search(r'<color-profile[^>]*xlink:href="data:application/vnd\.iccprofile;base64,([^"]+)"',
                         reopened.read_text(encoding='utf-8'), re.S)
    check("the document's profile comes back byte for byte", embedded is not None
          and base64.b64decode(re.sub(r'\s', '', embedded.group(1))) == PROFILE.read_bytes())

    canvas, drawn = out / 'canvas.png', out / 'reopened.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    run('drawn', exe, reopened, '--export-background=white', f'--export-filename={drawn}')
    worst = probe(canvas, drawn, regions)
    (out / 'probes.json').write_text(json.dumps(worst, indent=1), encoding='utf-8')
    over = {k: v for k, v in worst.items() if v > 3}
    check('draws as the source at every probe (within 3)', not over, over or max(worst.values()))

    again, again_svg, third = out / 'again.ai', out / 'again.svg', out / 'third.ai'
    run('again', exe, reopened, native, f'--export-filename={again}')
    body = lambda data: re.sub(rb'^%%(Title|CreationDate):[^\n]*\n|^%_\(.{36}\) /String \(ArtboardUUID\) ,\n',
                               b'', data, flags=re.M).split(b'\n')

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

    changed = changes(records, records_of(again))
    check('written again from the reopened copy: the same records', changed == [], changed[:4] if changed else changed)
    run('again-reopen', exe, again, f'--export-filename={again_svg}')
    run('third', exe, again_svg, native, f'--export-filename={third}')
    check('and again', changes(records_of(again), records_of(third)) == [])

    legacy_ai = out / 'legacy.ai'
    p = run('legacy', exe, source, legacy, f'--export-filename={legacy_ai}', ok=False)
    check('Illustrator 7 refuses inks defined in RGB or Lab by name', not legacy_ai.exists()
          and b'spot inks defined in RGB or Lab (Illustrator 7)' in p.stdout + p.stderr)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == source_hash)
    (out / 'report.json').write_text(json.dumps(checks, indent=2, default=repr))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
