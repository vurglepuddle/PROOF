"""Hard stops (two gradient stops at one place) through the modern .ai writer, PDF output and PDF
import: a hard edge inside linear and radial gradients, two stops at the start, two of one colour at
the end. Synthetic artwork only; sends no desktop input. Illustrator's reading was qualified with
the same cells (PROOF-ai-interchange.md, "Hard stops").
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

import numpy as np
from PIL import Image

WIDTH, HEIGHT = 480, 240

# Inside each painted rectangle, away from its edges.
REGIONS = {
    'linear hard edge': (14, 24, 146, 96),
    'linear, two stops at the start': (334, 24, 466, 96),
    'linear, two of one colour at the end': (174, 24, 306, 96),
    'radial hard edge': (14, 129, 146, 231),
    'radial, two stops at the centre': (334, 129, 466, 231),
    'radial, two of one colour at the edge': (174, 129, 306, 231),
}


def control(end='#ff0000'):
    """The cells; `end` is the colour of the second stop at the end of the two end cells."""
    def linear(name, x1, x2, stops):
        return (f'<linearGradient id="{name}" x1="{x1}" y1="0" x2="{x2}" y2="0" gradientUnits="userSpaceOnUse">'
                + ''.join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in stops) + '</linearGradient>')

    def radial(name, cx, cy, r, stops):
        return (f'<radialGradient id="{name}" cx="{cx}" cy="{cy}" r="{r}" gradientUnits="userSpaceOnUse">'
                + ''.join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in stops) + '</radialGradient>')

    edge = [(0, '#ffffff'), (0.5, '#ff0000'), (0.5, '#0000ff'), (1, '#000000')]
    start = [(0, '#00a000'), (0, '#ff0000'), (1, '#ffffff')]
    ends = [(0, '#ffffff'), (1, '#ff0000'), (1, end)]
    defs = (linear('mid', 20, 140, edge) + linear('end', 180, 260, ends) + linear('start', 360, 440, start) +
            radial('rmid', 80, 180, 50, edge) + radial('rend', 240, 180, 40, ends) + radial('rstart', 400, 180, 50, start))
    art = ('<rect x="10" y="20" width="140" height="80" fill="url(#mid)"/>'
           '<rect x="170" y="20" width="140" height="80" fill="url(#end)"/>'
           '<rect x="330" y="20" width="140" height="80" fill="url(#start)"/>'
           '<rect x="10" y="125" width="140" height="110" fill="url(#rmid)"/>'
           '<rect x="170" y="125" width="140" height="110" fill="url(#rend)"/>'
           '<rect x="330" y="125" width="140" height="110" fill="url(#rstart)"/>'
           # An ordinary RGB colour, so that Illustrator opens the control as an RGB document.
           '<rect x="0" y="0" width="6" height="6" fill="#ff8000"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="RGB">'
            f'<defs>{defs}</defs>{art}</svg>')


def pixels(png):
    with Image.open(png) as im:
        return np.asarray(im.convert('RGB'), dtype=np.int16)


def compare(a_png, b_png):
    a, b = pixels(a_png), pixels(b_png)
    out = {}
    for name, (x0, y0, x1, y1) in REGIONS.items():
        diff = np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).max(axis=2)
        out[name] = dict(max=int(diff.max()), over24=round(float((diff > 24).mean()), 4))
    return out


def stitching(pdf):
    """[(domain, bounds)] of every stitching function the page's shadings use."""
    from pypdf import PdfReader
    found = []

    def function(f):
        f = f.get_object()
        if f.get('/FunctionType') == 3:
            found.append(([float(v) for v in f['/Domain']], [float(v) for v in f.get('/Bounds', [])]))
            for g in f['/Functions']:
                function(g)

    def walk(resources, depth=0):
        resources = resources.get_object() if resources is not None else {}
        if depth > 8:
            return
        for pattern in (resources.get('/Pattern') or {}).values():
            pattern = pattern.get_object()
            if '/Shading' in pattern:
                function(pattern['/Shading'].get_object()['/Function'])
            walk(pattern.get('/Resources'), depth + 1)
        for shading in (resources.get('/Shading') or {}).values():
            function(shading.get_object()['/Function'])
        for xobject in (resources.get('/XObject') or {}).values():
            xobject = xobject.get_object()
            if xobject.get('/Subtype') == '/Form':
                walk(xobject.get('/Resources'), depth + 1)

    walk(PdfReader(pdf, strict=True).pages[0]['/Resources'])
    return found


def pattern_space_pdf(path, rotate=0, crop=False):
    """Independent PDF: page patterns, nested/reused forms and a pattern after a form.
    Pattern matrices map to each content stream's initial user space, regardless
    of later cm operators. Both pattern and form matrices are nonidentity."""
    from pypdf import PdfWriter
    from pypdf.generic import (ArrayObject, BooleanObject, DecodedStreamObject, DictionaryObject,
                              FloatObject, NameObject, NumberObject)

    writer = PdfWriter()
    page = writer.add_blank_page(width=360, height=240)
    def array(values):
        return ArrayObject([FloatObject(v) for v in values])
    def dictionary(**values):
        return DictionaryObject({NameObject('/'+key): value for key, value in values.items()})
    def stream(text, **values):
        obj = DecodedStreamObject()
        obj.set_data(text.encode('ascii'))
        obj.update(dictionary(**values))
        return writer._add_object(obj)
    function = dictionary(FunctionType=NumberObject(2), Domain=array([0, 1]), C0=array([1, .15, 0]),
                          C1=array([0, .25, 1]), N=NumberObject(1))
    shading = dictionary(ShadingType=NumberObject(2), ColorSpace=NameObject('/DeviceRGB'),
                         Coords=array([0, 0, 100, 0]), Function=function,
                         Extend=ArrayObject([BooleanObject(True), BooleanObject(True)]))
    def patterns(matrix):
        return dictionary(P=writer._add_object(dictionary(PatternType=NumberObject(2),
                                                          Matrix=array(matrix), Shading=shading)))
    leaf = stream('q .85 0 .15 1 4 0 cm /Pattern cs /P scn 0 0 100 60 re f Q',
                  Type=NameObject('/XObject'), Subtype=NameObject('/Form'), BBox=array([0, 0, 110, 60]),
                  Matrix=array([1.1, .1, -.1, .9, 0, 0]),
                  Resources=dictionary(Pattern=patterns([1, .1, .2, 1, 0, 0])))
    outer = stream('/Leaf Do q 1 0 0 1 120 0 cm /Leaf Do Q',
                   Type=NameObject('/XObject'), Subtype=NameObject('/Form'), BBox=array([0, 0, 240, 80]),
                   Matrix=array([1, 0, .15, 1, 0, 0]), Resources=dictionary(XObject=dictionary(Leaf=leaf)))
    page[NameObject('/Resources')] = dictionary(XObject=dictionary(Outer=outer, Leaf=leaf),
                                               Pattern=patterns([1.3, 0, 0, 1, 10, 0]))
    page[NameObject('/Contents')] = stream(
        '/Pattern cs /P scn 10 145 130 70 re f '
        'q 1 0 0 1 20 40 cm /Outer Do Q '
        'q .7 0 0 .7 230 140 cm /Leaf Do Q '
        '/Pattern CS /P SCN 8 w 15 130 m 140 130 l S '
        'q 1 0 0 1 160 0 cm /Pattern cs /P scn 5 145 35 70 re f Q')
    if rotate:
        page.rotate(rotate)
    if crop:
        page.cropbox.lower_left = (12, 6)
        page.cropbox.upper_right = (348, 234)
    with path.open('wb') as file:
        writer.write(file)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', required=True, type=Path)
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
    env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    exe = args.inkscape.resolve()
    pdftoppm = (args.pdftoppm or args.dll_dir / 'pdftoppm.exe').resolve()
    pdftocairo = pdftoppm.with_name('pdftocairo.exe')
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

    def png(label, source):
        target = out / (label + '.png')
        p = run(label + '-png', exe, source, '--export-type=png', f'--export-filename={target}', '--export-dpi=96',
                '--export-area-page')
        return target, (p.stdout + p.stderr).decode('utf-8', 'replace')

    def records_of(ai):
        from pypdf import PdfReader
        private = PdfReader(ai, strict=True).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        marker = b'%AI24_ZStandard_Data'
        return subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'], input=packed[len(marker):],
                              capture_output=True, timeout=60, check=True).stdout.decode('latin-1')

    def close(results, limit=0.02):
        return all(r['over24'] <= limit for r in results.values())

    source = out / 'control.svg'
    source.write_text(control(), encoding='utf-8')
    original, _ = png('control', source)

    # The modern .ai: stops listed as Illustrator lists them, the hard edge in its order.
    ai = out / 'control.ai'
    run('save', exe, source, f'--export-filename={ai}', '--export-extension=org.inkscape.output.ai.native')
    records = records_of(ai)
    (out / 'control-records.ps').write_text(records, encoding='latin-1')
    definitions = re.findall(r'^\((PROOF_G\d+)\) ([01]) (\d+) Bd\n\[\n((?:.*%_Bs\n)+)BD$', records, re.M)
    # "c m y k r g b 2 50 ramp %_Bs": the ramp point, then the blue.
    ramps = {name: (kind, [line.split()[-2] + ' ' + line.split()[-5] for line in body.splitlines()])
             for name, kind, count, body in definitions}
    linear_edge = [r for k, r in ramps.values() if k == '0' and len(r) == 4]
    radial_edge = [r for k, r in ramps.values() if k == '1' and len(r) == 4]
    # Blue is 0 0 1 in RGB: its third value; red's is 0. Linear: listed last first.
    check('linear hard edge listed last first, blue before red', linear_edge == [['100 0', '50 1', '50 0', '0 1']],
          str(linear_edge))
    check('radial hard edge listed first first, red before blue', radial_edge == [['0 1', '50 0', '50 1', '100 0']],
          str(radial_edge))

    # Its PDF page, and a plain PDF: no piece of nothing between equal bounds.
    pdf = out / 'control.pdf'
    run('pdf', exe, source, f'--export-filename={pdf}')
    for label, document in (('.ai page', ai), ('PDF', pdf)):
        functions = stitching(document)
        increasing = all(all(a < b for a, b in zip([d[0]] + bounds, bounds + [d[1]])) for d, bounds in functions)
        check(f'{label}: stitching bounds strictly increase', functions and increasing, str(functions))

    # Reopened natively, and the PDF as Poppler draws it, as the canvas draws the control.
    reopened, log = png('reopened', ai)
    check('.ai reopens natively', 'opens from its PDF page' not in log, log.strip()[-200:])
    results = compare(original, reopened)
    check('.ai reopened draws as the control', close(results), json.dumps(results))
    for tool in (pdftoppm, pdftocairo):
        stem = out / ('pdf-' + tool.stem)
        run(tool.stem, tool, '-r', '96', '-png', '-singlefile', pdf, stem)
        results = compare(original, stem.with_suffix('.png'))
        check(f'PDF drawn by {tool.stem} as the control', close(results), json.dumps(results))

    # PROOF's PDF import keeps the hard edges (it took the colour after each at both its bounds).
    imported, _ = png('imported', pdf)
    results = compare(original, imported)
    check('PDF import draws as the control', close(results), json.dumps(results))

    # Two different colours at the end: Illustrator extends with the first, SVG with the last.
    refused = out / 'refused.svg'
    refused.write_text(control(end='#00a000'), encoding='utf-8')
    target = out / 'refused.ai'
    p = run('refuse', exe, refused, f'--export-filename={target}', '--export-extension=org.inkscape.output.ai.native',
            ok=False)
    check('two colours at an end refused by name', p.returncode == 1 and b'gradient stops of different colours at its end'
          in p.stderr and not target.exists(), p.stderr.decode('utf-8', 'replace').strip()[-200:])
    sentinel = b'keep the existing document\n'
    target.write_bytes(sentinel)
    p = run('refuse-replace', exe, refused, f'--export-filename={target}',
            '--export-extension=org.inkscape.output.ai.native', ok=False)
    check('endpoint refusal preserves an existing document', p.returncode == 1 and target.read_bytes() == sentinel)
    legacy = out / 'legacy.ai'
    p = run('legacy-refuse', exe, source, f'--export-filename={legacy}',
            '--export-extension=org.inkscape.output.ai.legacy7', ok=False)
    check('Illustrator 7 hard stops remain refused', p.returncode == 1 and not legacy.exists()
          and b'gradient hard stops (Illustrator 7)' in p.stderr)
    again = out / 'again.ai'
    run('again', exe, ai, f'--export-filename={again}', '--export-extension=org.inkscape.output.ai.native')
    repeated, log = png('again', again)
    results = compare(original, repeated)
    check('second write retains hard edges and reopens natively', 'opens from its PDF page' not in log
          and close(results), json.dumps(results))

    for name, rotate, crop in [('form-space', 0, False), ('rotated-form-space', 90, False)]:
        fixture = out / (name + '.pdf')
        pattern_space_pdf(fixture, rotate, crop)
        reference = out / (name + '-reference')
        run(name + '-reference', pdftoppm, '-cropbox', '-r', '96', '-png', '-singlefile', fixture, reference)
        imported, _ = png(name + '-imported', fixture)
        a, b = pixels(reference.with_suffix('.png')), pixels(imported)
        same_size = a.shape == b.shape
        differences = np.abs(a-b).max(axis=2) if same_size else None
        # Independent reference, including every form placement and page restoration.
        over24 = float((differences > 24).mean()) if same_size else 1
        check(name + ': PDF import matches the independent page within one percent', same_size and over24 <= .01,
              dict(reference=list(a.shape), imported=list(b.shape), over24=round(over24, 5)))

    report = dict(checks=checks, passed=sum(c['passed'] for c in checks), total=len(checks))
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'{report["passed"]}/{report["total"]} checks passed. {out / "report.json"}')
    return int(report['passed'] != report['total'])


if __name__ == '__main__':
    raise SystemExit(main())
