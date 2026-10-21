"""Transparency in the modern .ai records, in one synthetic control; no desktop input.

One CMYK document: the sixteen blend modes over a band and paper; an object at half opacity;
a group with its own opacity, blend mode and isolated blending; a knockout group; a compound
path; a clipped object; a fill's and a stroke's own opacity when each paints alone; a clone, nested
groups, a picture, a spot ink and a gradient, each with transparency of its own; knockout
neither on nor off, with the knockout shape; a plain group whose last member is transparent; a
gradient whose stops have opacities of their own. The copy must write each as Illustrator does
(`mode opacity isolate knockout shape Xy` before an object, after a group's end with `0 0 Xd`
and `6 () XW`, and `9 () XW` after a group with none), reopen from its own records with each
object's transparency, draw as the source at every probe, keep the blend modes in its PDF page,
and write the same records again. What it can't write yet is refused by name, and Illustrator 7
refuses all of it. Illustrator's own reading is separate qualification.
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
WIDTH, HEIGHT = 640, 400
# Illustrator's order, which `Xy` numbers.
MODES = ['normal', 'multiply', 'screen', 'overlay', 'soft-light', 'hard-light', 'color-dodge', 'color-burn',
         'darken', 'lighten', 'difference', 'exclusion', 'hue', 'saturation', 'color', 'luminosity']
SPOT = 'PROOF Rouge'


def cmyk(c, m, y, k):
    return f'device-cmyk({c} {m} {y} {k})'


def png_uri():
    import base64
    import io
    picture = Image.new('RGB', (4, 4))
    picture.putdata([(255 - 60 * x, 40 * y, 60 * x + 30) for y in range(4) for x in range(4)])
    data = io.BytesIO()
    picture.save(data, 'PNG')
    return 'data:image/png;base64,' + base64.b64encode(data.getvalue()).decode()


def control(layer_opacity=False, separate=False):
    """The control's SVG; its probes at 96 dpi; and what each object's transparency must read
    back as: (blend mode, opacity, isolate, knockout, knockout shape), in document order."""
    band = cmyk(0.6, 0, 0.4, 0)
    sampler = ''.join(f'<rect x="{20 + 38 * i}" y="40" width="30" height="40" '
                      f'style="fill:{cmyk(0, 0.9, 0.2, 0)};mix-blend-mode:{mode}"/>' for i, mode in enumerate(MODES))
    defs = (f'<clipPath id="clip"><circle cx="520" cy="170" r="30"/></clipPath>'
            f'<linearGradient id="spot" inkscape:swatch="solid" proof:ink="{SPOT}" '
            f'proof:ink-alternate="device-cmyk(0 1 0.8 0.05)"><stop offset="0" style="stop-color:device-cmyk(0 1 0.8 0.05)"/>'
            f'</linearGradient>'
            f'<linearGradient id="ramp" gradientUnits="userSpaceOnUse" x1="460" y1="0" x2="600" y2="0">'
            f'<stop style="stop-color:{cmyk(1, 0, 0, 0)}"/><stop offset="1" style="stop-color:{cmyk(0, 0, 1, 0)}"/>'
            f'</linearGradient>'
            f'<linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="190" y1="0" x2="330" y2="0">'
            f'<stop style="stop-color:{cmyk(1, 0, 0, 0)}"/>'
            f'<stop offset="1" style="stop-color:{cmyk(1, 0, 0, 0)};stop-opacity:0.2"/></linearGradient>')
    art = ''.join([
        # Backdrops: a band under the sampler's upper half, and one under each row.
        f'<rect x="10" y="30" width="620" height="25" style="fill:{band}"/>',
        f'<rect x="10" y="120" width="620" height="40" style="fill:{cmyk(0, 0.1, 0.9, 0)}"/>',
        f'<rect x="10" y="250" width="620" height="40" style="fill:{cmyk(0.3, 0, 0, 0.2)}"/>',
        sampler,
        f'<rect x="20" y="130" width="60" height="60" style="fill:{cmyk(0, 0, 0, 1)};opacity:0.5"/>',
        f'<g style="opacity:0.6;mix-blend-mode:screen;isolation:isolate">'
        f'<rect x="100" y="130" width="50" height="50" style="fill:{cmyk(1, 0, 0, 0)}"/>'
        f'<rect x="125" y="150" width="50" height="50" style="fill:{cmyk(0, 1, 0, 0)};mix-blend-mode:multiply"/></g>',
        f'<g proof:knockout="on"><rect x="200" y="130" width="50" height="50" style="fill:{cmyk(0, 0, 0, 1)};opacity:0.5"/>'
        f'<rect x="225" y="150" width="50" height="50" style="fill:{cmyk(0, 0, 0, 1)};opacity:0.5"/></g>',
        f'<path d="M 300 130 H 380 V 200 H 300 Z M 320 145 V 185 H 360 V 145 Z" '
        f'style="fill:{cmyk(0.9, 0.7, 0, 0)};opacity:0.7;mix-blend-mode:overlay"/>',
        f'<rect x="400" y="130" width="70" height="70" style="fill:{cmyk(0, 0, 0, 0.4)};opacity:0.5;mix-blend-mode:darken"/>',
        f'<rect x="480" y="130" width="80" height="80" clip-path="url(#clip)" '
        f'style="fill:{cmyk(0, 0.5, 1, 0)};opacity:0.5;mix-blend-mode:multiply"/>',
        f'<rect x="20" y="260" width="60" height="60" style="fill:{cmyk(0.7, 0, 1, 0)};fill-opacity:0.25"/>',
        f'<path d="M 100 255 L 160 325" style="fill:none;stroke:{cmyk(0, 0, 0, 1)};stroke-width:12;stroke-opacity:0.4"/>',
        f'<rect id="source" x="180" y="260" width="40" height="40" style="fill:{cmyk(0, 0.4, 0.9, 0)}"/>',
        '<use href="#source" transform="translate(50 0)" style="opacity:0.5;mix-blend-mode:luminosity"/>',
        f'<g style="opacity:0.8"><g style="opacity:0.5"><rect x="290" y="260" width="50" height="50" '
        f'style="fill:{cmyk(0.5, 0, 0, 0.5)}"/></g></g>',
        f'<image x="360" y="260" width="40" height="40" preserveAspectRatio="none" style="opacity:0.5" href="{png_uri()}"/>',
        '<rect x="410" y="260" width="40" height="50" style="fill:url(#spot);opacity:0.5;mix-blend-mode:multiply"/>',
        '<rect x="460" y="260" width="140" height="50" style="fill:url(#ramp);opacity:0.6"/>',
        f'<rect x="20" y="340" width="40" height="40" style="fill:{cmyk(0, 0, 0, 0.7)}" '
        'proof:knockout="neutral" proof:knockout-shape="true"/>',
        # A group with none of its own whose last member is transparent, then an object straight after it.
        f'<g><rect x="80" y="340" width="40" height="40" style="fill:{cmyk(0.2, 0.8, 0, 0)};opacity:0.6"/></g>',
        f'<rect x="130" y="340" width="40" height="40" style="fill:{cmyk(0, 0.3, 0.8, 0)};opacity:0.3"/>',
        # A gradient whose stops have opacities of their own, over a backdrop.
        f'<rect x="250" y="330" width="20" height="60" style="fill:{cmyk(0, 0, 0, 1)}"/>',
        '<rect x="190" y="340" width="140" height="40" style="fill:url(#fade)"/>',
    ])
    if separate:
        art += f'<rect x="500" y="340" width="40" height="40" style="fill:{cmyk(1, 0, 0, 0)};fill-opacity:0.5;stroke:#000000"/>'
    layer_style = ' style="opacity:0.9"' if layer_opacity else ''
    text = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="CMYK">'
            f'<defs>{defs}</defs><g inkscape:groupmode="layer" inkscape:label="Transparence"{layer_style}>{art}</g></svg>')
    regions = {f'{mode} over the band': [(35 + 38 * i, 47)] for i, mode in enumerate(MODES)}
    regions.update({f'{mode} over paper': [(35 + 38 * i, 70)] for i, mode in enumerate(MODES)})
    regions.update({
        'half opacity over yellow': [(50, 140)], 'half opacity over paper': [(50, 180)],
        'isolated screen group': [(110, 140), (140, 170), (165, 190)],
        'knockout group, apart': [(210, 140), (265, 190)],
        'compound path at 70% overlay': [(305, 140), (370, 190)], 'its hole': [(340, 165)],
        'darken at half': [(430, 140), (430, 190)],
        'clipped multiply': [(520, 170), (505, 150)], 'outside the clip': [(485, 135)],
        'fill at a quarter': [(50, 270), (50, 310)],
        'stroke at 40%': [(130, 290)],
        'opaque source': [(200, 280)], 'clone at half, luminosity': [(250, 270), (250, 295)],
        'nested groups': [(315, 270), (315, 300)],
        'picture at half': [(365, 265), (395, 295)],
        'spot ink, multiply at half': [(430, 270), (430, 305)],
        'gradient at 60%': [(470, 270), (530, 270), (590, 305)],
        'knockout shape': [(40, 360)],
        'plain group, transparent member': [(100, 360)], 'after a plain group': [(150, 360)],
        'stops with their own opacity': [(200, 360), (240, 360), (260, 360), (320, 360)],
    })
    expected = ([(0, 1.0, 0, 0, 0)] * 3 + [(i, 1.0, 0, 0, 0) for i in range(16)] + [
        (0, 0.5, 0, 0, 0),
        (2, 0.6, 1, 0, 0), (0, 1.0, 0, 0, 0), (1, 1.0, 0, 0, 0),
        (0, 1.0, 0, 1, 0), (0, 0.5, 0, 0, 0), (0, 0.5, 0, 0, 0),
        (3, 0.7, 0, 0, 0),
        (8, 0.5, 0, 0, 0),
        (1, 0.5, 0, 0, 0), (0, 1.0, 0, 0, 0),     # the clipping group, the object in it
        (0, 0.25, 0, 0, 0), (0, 0.4, 0, 0, 0),
        (0, 1.0, 0, 0, 0), (15, 0.5, 0, 0, 0), (0, 1.0, 0, 0, 0),
        (0, 0.8, 0, 0, 0), (0, 0.5, 0, 0, 0), (0, 1.0, 0, 0, 0),
        (0, 0.5, 0, 0, 0),
        (1, 0.5, 0, 0, 0),
        (0, 0.6, 0, 0, 0),
        (0, 1.0, 0, 2, 1),
        (0, 1.0, 0, 0, 0), (0, 0.6, 0, 0, 0), (0, 0.3, 0, 0, 0),
        (0, 1.0, 0, 0, 0), (0, 1.0, 0, 0, 0),
    ])
    return text, regions, expected


def transparency_of(e):
    """(blend mode, opacity, isolate, knockout, knockout shape) of an element, as the import writes them."""
    style = dict(p.split(':', 1) for p in (e.get('style') or '').split(';') if ':' in p)
    mode = MODES.index(style.get('mix-blend-mode', 'normal').strip())
    opacity = round(float(style.get('opacity', '1')), 4)
    knockout = {'on': 1, 'neutral': 2}.get(e.get(PROOF + 'knockout'), 0)
    return (mode, opacity, int(style.get('isolation', '').strip() == 'isolate'), knockout,
            int(e.get(PROOF + 'knockout-shape') == 'true'))


def art_elements(layer):
    """The layer's objects and groups in document order, clip paths and definitions apart."""
    out = []

    def walk(e):
        for k in e:
            if k.tag in (SVG + 'defs', SVG + 'clipPath', SVG + 'mask', SVG + 'title', SVG + 'desc'):
                continue
            if k.tag in (SVG + 'g', SVG + 'path', SVG + 'rect', SVG + 'image', SVG + 'use', SVG + 'circle'):
                out.append(k)
            if k.tag == SVG + 'g':
                walk(k)

    walk(layer)
    return out


def probe(a_png, b_png, regions):
    with Image.open(a_png) as a, Image.open(b_png) as b:
        a, b = a.convert('RGB'), b.convert('RGB')
        return {name: max(max(abs(x - y) for x, y in zip(a.getpixel(q), b.getpixel(q))) for q in points)
                for name, points in regions.items()}


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

    text, regions, expected = control()
    source = out / 'control.svg'
    source.write_text(text, encoding='utf-8')
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    ai = out / 'control.ai'
    run('save', exe, source, native, f'--export-filename={ai}')
    records = records_of(ai)
    (out / 'records.txt').write_bytes(records)
    xy = [line.split()[:-1] for line in records.split(b'\n') if line.endswith(b' Xy')]
    modes = {int(f[0]) for f in xy}
    check('Xy as Illustrator writes it: five numbers, all sixteen modes', all(len(f) == 5 for f in xy)
          and modes == set(range(16)), sorted(modes))
    check('an isolated group: its own after its end, with Xd and a style marker',
          b'U\n2 0.6 1 0 0 Xy\n0 0 Xd\n6 () XW\n' in records)
    check('a knockout group', b'U\n0 1 0 1 0 Xy\n0 0 Xd\n6 () XW\n' in records)
    check('a clipped object: its clipping group has its transparency', b'Q\n1 0.5 0 0 0 Xy\n0 0 Xd\n6 () XW\n' in records)
    plain = records.count(b'U\n9 () XW\n')
    check('groups with none of their own end with 9 () XW', plain >= 1, plain)
    check('a paint at a quarter, a stroke at 40%, knockout shape', b'\n0 0.25 0 0 0 Xy\n' in records
          and b'\n0 0.4 0 0 0 Xy\n' in records and b'\n0 1 0 2 1 Xy\n' in records)
    check("stops with their own opacity, in Illustrator's newer form", b'\n1 0 0 0 1 0.2 6 50 100 %_Bs\n' in records
          and b'\n1 0 0 0 1 1 6 50 0 %_Bs\n' in records and b'\n1 0 0 0 1 50 0 %_Bs\n' in records)

    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    check('reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr)
    root = ET.parse(reopened).getroot()
    layers = [g for g in root.iter(SVG + 'g') if g.get(INK + 'groupmode') == 'layer']
    got = [transparency_of(e) for e in art_elements(layers[0])] if layers else []
    check('each object and group has its transparency, in order', got == expected,
          [(i, g, e) for i, (g, e) in enumerate(zip(got, expected)) if g != e][:6] or (len(got), len(expected)))

    canvas, drawn = out / 'canvas.png', out / 'reopened.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    run('drawn', exe, reopened, '--export-background=white', f'--export-filename={drawn}')
    worst = probe(canvas, drawn, regions)
    (out / 'probes.json').write_text(json.dumps(worst, indent=1), encoding='utf-8')
    over = {k: v for k, v in worst.items() if v > 3}
    check('draws as the source at every probe (within 3)', not over, over or max(worst.values()))

    from pypdf import PdfReader
    page = PdfReader(ai).pages[0]
    found = set()
    stack = [page['/Resources']]
    while stack:
        resources = stack.pop()
        for state in (resources.get('/ExtGState') or {}).values():
            state = state.get_object()
            if '/BM' in state:
                found.add(str(state['/BM']))
        for ref in (resources.get('/XObject') or {}).values():
            obj = ref.get_object()
            if obj.get('/Resources'):
                stack.append(obj['/Resources'])
    pdf_modes = {'/' + ''.join(w.capitalize() for w in m.split('-')) for m in MODES[1:]}
    check("the PDF page keeps the fifteen blend modes", pdf_modes <= found, sorted(pdf_modes - found))
    group = page.get('/Group')
    check("the PDF page composites in CMYK, as Illustrator's pages of a CMYK document say",
          group is not None and group.get('/S') == '/Transparency' and group.get('/CS') == '/DeviceCMYK'
          and group.get('/I') == False and group.get('/K') == False, group)  # pypdf's false is truthy

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

    for label, kwargs, reason in (('layer', dict(layer_opacity=True), 'transparency on layers'),
                                  ('separate', dict(separate=True), 'separate fill and stroke transparency')):
        refused_svg, refused_ai = out / f'refused-{label}.svg', out / f'refused-{label}.ai'
        refused_svg.write_text(control(**kwargs)[0], encoding='utf-8')
        p = run(f'refused-{label}', exe, refused_svg, native, f'--export-filename={refused_ai}', ok=False)
        check(f'refused by name: {reason}', not refused_ai.exists() and reason.encode() in p.stdout + p.stderr)
    legacy_ai = out / 'legacy.ai'
    p = run('legacy', exe, source, legacy, f'--export-filename={legacy_ai}', ok=False)
    check('Illustrator 7 refuses transparency and blend modes', not legacy_ai.exists()
          and b'transparency' in p.stdout + p.stderr and b'blend modes' in p.stdout + p.stderr)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == source_hash)
    (out / 'report.json').write_text(json.dumps(checks, indent=2, default=repr))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
