"""Spot inks on their own plates in PDF output, in synthetic controls; no desktop input.

A CMYK document in a press profile with inks defined in CMYK, Lab and RGB, each at full strength
and at a tint: fills, a stroke, type, a translucent group, a pattern tile and an opacity mask
whose art is in an ink. PDF export must give every ink one /Separation named as the ink and
paint each object with it at its tint. Its alternate is the ink as the document prints colour:
the document's CMYK in its own ICC profile (a CMYK ink's numbers times the tint, a Lab or RGB
ink converted), by a calculator function, so it draws as the document's process colour does.
Ghostscript must find each ink on its own plate and none of it on the process plates. A mask
still draws its luminosity, Registration stays /All, an ink PDF can't name stays its display
colour. In an RGB document a CMYK ink keeps its numbers (DeviceCMYK) and the others are RGB.
The page must look as the output before plates drew it, within a few levels, and the native
.ai page must carry the same plates and reopen natively. Gradients with ink stops still blend
as process colour (later work).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from PIL import Image

WIDTH, HEIGHT = 480, 320
PROFILE = Path(__file__).resolve().parents[1] / 'data' / 'colors' / 'default_cmyk.icc'
INKS = {
    'PROOF Plate Orange': ('cmyk', (0, 0.6, 1, 0)),
    'PROOF Plate Violet': ('lab', (40, 50, -60)),
    'PROOF Plate Green': ('rgb', (0.08, 0.63, 0.35)),
    'PROOF Encre été': ('cmyk', (0.2, 0, 0.9, 0)),
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
    kind, v = INKS[name]
    if kind == 'lab':
        return css(kind, (100 - tint * (100 - v[0]), tint * v[1], tint * v[2]))
    if kind == 'rgb':
        return css(kind, tuple(1 - tint * (1 - x) for x in v))
    return css(kind, tuple(tint * x for x in v))


def swatch(sid, name, tint, ink=None):
    kind, v = INKS[name]
    ink = ink or name
    tint_attr = f' proof:ink-tint="{fmt(tint)}"' if tint < 1 else ''
    return (f'<linearGradient id="{sid}" inkscape:swatch="solid" proof:ink="{ink}" '
            f'proof:ink-alternate="{css(kind, v)}"{tint_attr}>'
            f'<stop offset="0" style="stop-color:{shown(name, tint)}"/></linearGradient>')


SWATCHES = [('orange', 'PROOF Plate Orange', 1.0), ('orange30', 'PROOF Plate Orange', 0.3),
            ('violet', 'PROOF Plate Violet', 1.0), ('violet45', 'PROOF Plate Violet', 0.45),
            ('green', 'PROOF Plate Green', 1.0), ('green60', 'PROOF Plate Green', 0.6),
            ('summer', 'PROOF Encre été', 1.0)]
# Where to look, at 96 dpi: (x, y).
PROBES = {
    'CMYK ink': (40, 40), 'CMYK ink at 30%': (120, 40), 'Lab ink': (200, 40), 'Lab ink at 45%': (280, 40),
    'RGB ink': (360, 40), 'RGB ink at 60%': (440, 40), 'stroke in an ink': (20, 120), 'paper in the stroke': (60, 120),
    'translucent group': (200, 120), 'pattern tile': (300, 110), 'masked by an ink': (420, 120),
    'unnamed ink': (40, 200), 'Registration': (120, 200), 'process beside them': (200, 200),
}


def control(mode='CMYK'):
    defs = ''.join(swatch(*s) for s in SWATCHES)
    defs += ('<linearGradient id="all" inkscape:swatch="solid" proof:ink="All" proof:ink-alternate="device-cmyk(1 1 1 1)">'
             '<stop offset="0" style="stop-color:device-cmyk(1 1 1 1)"/></linearGradient>')
    defs += ('<pattern id="tile" patternUnits="userSpaceOnUse" width="20" height="20">'
             '<rect width="10" height="20" style="fill:url(#green)"/></pattern>')
    defs += ('<mask id="fade" maskUnits="userSpaceOnUse"><rect x="380" y="80" width="80" height="80" style="fill:url(#orange30)"/></mask>')
    art = ''.join([
        '<rect x="10" y="10" width="60" height="60" style="fill:url(#orange)"/>',
        '<rect x="90" y="10" width="60" height="60" style="fill:url(#orange30)"/>',
        '<rect x="170" y="10" width="60" height="60" style="fill:url(#violet)"/>',
        '<rect x="250" y="10" width="60" height="60" style="fill:url(#violet45)"/>',
        '<rect x="330" y="10" width="60" height="60" style="fill:url(#green)"/>',
        '<rect x="410" y="10" width="60" height="60" style="fill:url(#green60)"/>',
        '<rect x="20" y="86" width="80" height="68" style="fill:none;stroke:url(#violet);stroke-width:8"/>',
        '<g style="opacity:0.5"><rect x="170" y="80" width="60" height="80" style="fill:url(#violet)"/></g>',
        '<rect x="260" y="80" width="80" height="80" style="fill:url(#tile)"/>',
        '<rect x="380" y="80" width="80" height="80" mask="url(#fade)" style="fill:url(#violet)"/>',
        '<rect x="10" y="170" width="60" height="60" style="fill:url(#summer)"/>',
        '<rect x="90" y="170" width="60" height="60" style="fill:url(#all)"/>',
        '<rect x="170" y="170" width="60" height="60" style="fill:device-cmyk(0.1 0.2 0.3 0.4)"/>',
        '<text x="250" y="210" style="font-family:Arial;font-size:36px;fill:url(#orange)">Ink '
        '<tspan style="fill:url(#violet45);stroke:url(#green60);stroke-width:1px">two</tspan></text>',
    ])
    profile = (f'<color-profile id="press" name="Press" rendering-intent="relative-colorimetric" '
               f'xlink:href="{PROFILE.as_uri()}"/>') if mode == 'CMYK' else ''
    assigned = ' proof:color-profile="Press"' if mode == 'CMYK' else ''
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="{mode}"'
            f'{assigned}><defs>{profile}{defs}</defs>'
            f'<g inkscape:groupmode="layer" inkscape:label="Plates">{art}</g></svg>')


def without_mask(svg):
    """The control without its opacity mask: the .ai writer refuses inks in masks by name."""
    return svg.replace(' mask="url(#fade)"', '')


def plates(pdf):
    """What a PDF's pages paint with its Separations: {ink: {'alternate', 'function', 'tints', 'in_text'}}, the
    pages and every Form, pattern and soft mask they draw followed."""
    from pypdf import PdfReader
    from pypdf.generic import ContentStream
    reader = PdfReader(pdf, strict=True)
    found = {}

    def scan(stream, resources, active, depth=0):
        resources = resources.get_object() if resources is not None else {}
        spaces = resources.get('/ColorSpace', {})
        spaces = spaces.get_object() if hasattr(spaces, 'get_object') else spaces
        names = {}
        for key, value in spaces.items():
            value = value.get_object()
            if isinstance(value, list) and len(value) == 4 and value[0] == '/Separation':
                name = bytes(value[1][1:], 'latin-1').decode('utf-8', 'replace') if isinstance(value[1], str) else str(value[1])
                name = re.sub(r'#([0-9a-fA-F]{2})', lambda m: chr(int(m.group(1), 16)), name)
                alternate = value[2].get_object()
                if isinstance(alternate, list) and alternate and alternate[0] == '/ICCBased':
                    alternate = '/ICCBased' + str(alternate[1].get_object().get('/N'))
                entry = found.setdefault(name, dict(alternate=str(alternate), function=value[3].get_object(),
                                                    tints=set(), in_text=False))
                names[key] = entry
        selected = {'fill': None, 'stroke': None}
        stack, inside = [], False
        for operands, op in ContentStream(stream, reader).operations:
            if op == b'q':
                stack.append(dict(selected))
            elif op == b'Q' and stack:
                selected = stack.pop()
            elif op == b'BT':
                inside = True
            elif op == b'ET':
                inside = False
            elif op in (b'cs', b'CS'):
                selected['fill' if op == b'cs' else 'stroke'] = names.get(operands[0])
            elif op in (b'scn', b'SCN'):
                entry = selected['fill' if op == b'scn' else 'stroke']
                if entry is not None and len(operands) == 1:
                    entry['tints'].add(round(float(operands[0]), 4))
            elif inside and op in (b'Tj', b'TJ', b"'", b'"') and selected['fill'] is not None:
                # Glyphs shown in the fill that holds, wherever it was chosen.
                selected['fill']['in_text'] = True
            elif op == b'Do' and depth < 12:
                objects = resources.get('/XObject', {})
                form = objects.get(operands[0])
                if form is not None:
                    key = (form.idnum, form.generation) if hasattr(form, 'idnum') else id(form)
                    form = form.get_object()
                    if form.get('/Subtype') == '/Form' and key not in active:
                        scan(form, form.get('/Resources', resources), active | {key}, depth + 1)
            elif op == b'gs' and depth < 12:
                state = resources.get('/ExtGState', {}).get(operands[0])
                mask = state.get_object().get('/SMask') if state is not None else None
                if mask is not None and not isinstance(mask.get_object(), str):
                    group = mask.get_object()['/G'].get_object()
                    scan(group, group.get('/Resources', resources), active, depth + 1)
        for pattern in resources.get('/Pattern', {}).values():
            pattern = pattern.get_object()
            if pattern.get('/PatternType') == 1 and depth < 12:
                scan(pattern, pattern.get('/Resources', {}), active, depth + 1)

    for page in reader.pages:
        scan(page.get_contents(), page['/Resources'], set())
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', required=True, type=Path)
    ap.add_argument('--baseline', type=Path, help='a PROOF without plates, to compare the pages with')
    ap.add_argument('--gs', type=Path, help="Ghostscript, to separate the page into its plates (tiffsep)")
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

    def render(pdf, label):
        prefix = out / label
        run(label, args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', pdf, prefix)
        return prefix.with_suffix('.png')

    source = out / 'control.svg'
    source.write_text(control(), encoding='utf-8')
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    pdf = out / 'control.pdf'
    p = run('pdf', args.inkscape.resolve(), source, f'--export-filename={pdf}')
    found = plates(pdf)
    named = {k for k in found if k != 'All'}
    check('one plate for each ink PDF can name', named == {'PROOF Plate Orange', 'PROOF Plate Violet', 'PROOF Plate Green'},
          sorted(found))
    check('Registration stays on /All', 'All' in found and found['All']['alternate'] == '/DeviceCMYK')
    check('an ink PDF cannot name is drawn in its display colour, said so',
          'PROOF Encre été' not in found and b"can't be named in PDF" in p.stdout + p.stderr)

    def function_at(entry, t):
        """Evaluate the plate's calculator function at a tint: the few operators it uses."""
        code = entry['function'].get_data().decode('ascii').split()
        stack = [t]

        def run_code(tokens):
            i = 0
            while i < len(tokens):
                w = tokens[i]
                if w == '{':
                    depth, j = 1, i + 1
                    while depth:
                        depth += {'{': 1, '}': -1}.get(tokens[j], 0)
                        j += 1
                    stack.append(tokens[i + 1:j - 1])
                    i = j
                    continue
                if w == 'dup':
                    stack.append(stack[-1])
                elif w == 'exch':
                    stack[-1], stack[-2] = stack[-2], stack[-1]
                elif w in ('mul', 'add', 'sub'):
                    b, a = stack.pop(), stack.pop()
                    stack.append(a * b if w == 'mul' else a + b if w == 'add' else a - b)
                elif w == 'lt':
                    b, a = stack.pop(), stack.pop()
                    stack.append(a < b)
                elif w == 'ifelse':
                    no, yes, cond = stack.pop(), stack.pop(), stack.pop()
                    run_code(yes if cond else no)
                else:
                    stack.append(float(w))
                i += 1

        run_code(code[1:-1])
        return stack

    orange, violet, green = (found.get(n) for n in ('PROOF Plate Orange', 'PROOF Plate Violet', 'PROOF Plate Green'))
    near = lambda a, b, tol=1e-6: len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))
    check("a CMYK ink: the document's ICC CMYK, the tint times its CMYK", orange and orange['alternate'] == '/ICCBased4'
          and near(function_at(orange, 0.3), (0, 0.18, 0.3, 0)) and near(function_at(orange, 0), (0, 0, 0, 0)))
    for label, entry in (('a Lab ink', violet), ('an RGB ink', green)):
        check(f"{label}: the document's ICC CMYK, paper at 0 and ink at 1", entry and entry['alternate'] == '/ICCBased4'
              and near(function_at(entry, 0), (0, 0, 0, 0), 2e-3) and sum(function_at(entry, 1)) > 0.5,
              entry and [round(v, 3) for v in function_at(entry, 1)])
    check('a converted ink grows smoothly with its tint', violet and all(
        all(a <= b + 1e-6 for a, b in zip(function_at(violet, i / 20), function_at(violet, (i + 1) / 20)))
        for i in range(20)))
    check('every tint used, on its plate', orange and violet and green and {1.0, 0.3} <= orange['tints']
          and {1.0, 0.45} <= violet['tints'] and {1.0, 0.6} <= green['tints'],
          {k: sorted(v['tints']) for k, v in found.items()})
    check('type in an ink is on its plate', orange and orange['in_text'])
    check("a later run's ink and its stroke's, inside the same text block", violet and violet['in_text']
          and green and 0.6 in green['tints'])

    # The alternates are the old colours: the page looks as before.
    page = render(pdf, 'page')
    with Image.open(page) as im:
        im = im.convert('RGB')
        probes = {k: im.getpixel(v) for k, v in PROBES.items()}
    (out / 'probes.json').write_text(json.dumps(probes, indent=1), encoding='utf-8')
    if args.baseline:
        before = out / 'before.pdf'
        run('before', args.baseline.resolve(), source, f'--export-filename={before}')
        with Image.open(render(before, 'before-page')) as a, Image.open(page) as b:
            a, b = a.convert('RGB'), b.convert('RGB')
            worst = {k: max(abs(x - y) for x, y in zip(a.getpixel(v), b.getpixel(v))) for k, v in PROBES.items()}
        (out / 'against-before.json').write_text(json.dumps(worst, indent=1), encoding='utf-8')
        check('every probe as the output before plates drew it (within 6)', all(v <= 6 for v in worst.values()), worst)
    check('the mask still fades by its ink\'s lightness', probes['masked by an ink'] != probes['Lab ink']
          and probes['masked by an ink'] != (255, 255, 255), probes['masked by an ink'])

    if args.gs:
        # What a press gets: each ink on its own plate (255 no ink, 0 solid), and not in process.
        sep = out / 'sep' / 'plate.tif'
        sep.parent.mkdir()
        run('separate', args.gs.resolve(), '-q', '-dNOPAUSE', '-dBATCH', '-dSAFER', '-sDEVICE=tiffsep', '-r96',
            f'-sOutputFile={sep}', pdf)

        def coverage(plate, at):
            path = sep.parent / f'plate({plate}).tif'
            if not path.exists():
                return None
            with Image.open(path) as im:
                return round(1 - im.convert('L').getpixel(at) / 255, 2)

        expect = {('PROOF Plate Orange', 'CMYK ink'): 1.0, ('PROOF Plate Orange', 'CMYK ink at 30%'): 0.3,
                  ('PROOF Plate Violet', 'Lab ink'): 1.0, ('PROOF Plate Violet', 'Lab ink at 45%'): 0.45,
                  ('PROOF Plate Green', 'RGB ink'): 1.0, ('PROOF Plate Green', 'RGB ink at 60%'): 0.6,
                  ('PROOF Plate Violet', 'stroke in an ink'): 1.0, ('PROOF Plate Violet', 'paper in the stroke'): 0.0}
        got = {f'{plate} / {where}': coverage(plate, PROBES[where]) for plate, where in expect}
        check('each ink on its own plate at its tint', all(
            got[f'{plate} / {where}'] is not None and abs(got[f'{plate} / {where}'] - want) <= 0.02
            for (plate, where), want in expect.items()), got)
        process = {p: coverage(p, PROBES[w]) for p in ('Cyan', 'Magenta', 'Yellow', 'Black')
                   for w in ('CMYK ink', 'Lab ink', 'RGB ink')}
        check('and nothing of them on the process plates', all(v == 0 for v in process.values()), process)
        check('process colour still on the process plates', coverage('Black', PROBES['process beside them']) > 0.3,
              coverage('Black', PROBES['process beside them']))

    unmasked = out / 'unmasked.svg'
    unmasked.write_text(without_mask(control()), encoding='utf-8')
    ai = out / 'control.ai'
    run('native', args.inkscape.resolve(), unmasked, native, f'--export-filename={ai}')
    in_ai = {k for k in plates(ai) if k != 'All'}
    check('the native .ai page carries the same plates', in_ai == named, sorted(in_ai))
    reopened = out / 'reopened.svg'
    p = run('reopen', args.inkscape.resolve(), ai, f'--export-filename={reopened}')
    check('the .ai reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr)

    # An RGB document: a CMYK ink keeps its numbers, the others are RGB from paper white.
    rgb_source = out / 'rgb.svg'
    rgb_source.write_text(control('RGB'), encoding='utf-8')
    rgb_pdf = out / 'rgb.pdf'
    run('rgb-pdf', args.inkscape.resolve(), rgb_source, f'--export-filename={rgb_pdf}')
    rgb = plates(rgb_pdf)
    orange, violet, green = (rgb.get(n) for n in ('PROOF Plate Orange', 'PROOF Plate Violet', 'PROOF Plate Green'))
    check('RGB document, a CMYK ink: DeviceCMYK, the tint times its CMYK', orange and orange['alternate'] == '/DeviceCMYK'
          and near(function_at(orange, 0.3), (0, 0.18, 0.3, 0)))
    green_rgb = [round(v * 255) / 255 for v in INKS['PROOF Plate Green'][1]]
    check('RGB document, an RGB ink: DeviceRGB, from paper white to the ink', green and green['alternate'] == '/DeviceRGB'
          and near(function_at(green, 0.6), [1 - 0.6 * (1 - v) for v in green_rgb]) and near(function_at(green, 0), (1, 1, 1)),
          green and function_at(green, 0.6))
    check('RGB document, a Lab ink: DeviceRGB, paper white at 0', violet and violet['alternate'] == '/DeviceRGB'
          and near(function_at(violet, 0), (1, 1, 1), 2e-3))
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == source_hash)
    (out / 'report.json').write_text(json.dumps(checks, indent=2, default=repr))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
