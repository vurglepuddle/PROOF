"""Gradient stops of different colour models through the modern .ai writer and PDF output: CMYK,
spot inks (defined in CMYK, Lab and RGB) and RGB stops in one gradient, in an RGB and a CMYK
document. Each stop keeps a colour style of its own, as Illustrator writes them, and PDF output
blends them in the document's colour mode, as Illustrator does. Synthetic artwork only; sends no
desktop input. Illustrator's reading was qualified with the same cells (PROOF-ai-interchange.md,
"Gradient stops of different colour models").
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
    'CMYK to RGB': (14, 24, 146, 96),
    'spot ink to RGB': (174, 24, 306, 96),
    'RGB, CMYK, RGB': (334, 24, 466, 96),
    'RGB to Lab ink': (14, 129, 146, 231),
    'spot ink to RGB at 90%, midpoint off centre': (174, 129, 306, 231),
    'RGB ink to CMYK': (334, 129, 466, 231),
}

LEAF = ('PROOF Leaf', 'device-cmyk(0.306 0 0.6 0)')
LAB_BLUE = ('PROOF Lab Blue', 'lab(30 20 -60)')
RGB_GREEN = ('PROOF RGB Green', '#33cc66')

# The import's reading of the owner's one such gradient, scaled: a spot ink at 0, RGB at 90% at
# 0.68, the midpoint at 68.7% sampled in RGB between them.
OWNERS = [(0, None, 1), (0.085, '#bbd483', 0.997853), (0.17, '#b6cc81', 0.992275), (0.255, '#aec07f', 0.983663),
          (0.34, '#a4b07c', 0.972206), (0.425, '#979c79', 0.958028), (0.51, '#888474', 0.941222),
          (0.595, '#76686f', 0.921859), (0.68, '#62496a', 0.9)]

# The styles of each definition's stops: 1 CMYK, 2 RGB, 3 a spot ink, 4 its exact Lab or RGB.
STYLES = sorted([[1, 2], [2, 3], [1, 2], [2, 3, 4], [2, 3], [1, 3, 4]])


def control(mode):
    def ink(offset, which, opacity=1):
        name, alternate = which
        return (f'<stop offset="{offset}" style="stop-color:{alternate};stop-opacity:{opacity}" '
                f'proof:ink="{name}" proof:ink-alternate="{alternate}"/>')

    def stop(offset, colour, opacity=1):
        return f'<stop offset="{offset}" style="stop-color:{colour};stop-opacity:{opacity}"/>'

    def linear(name, x1, x2, stops):
        return (f'<linearGradient id="{name}" x1="{x1}" y1="0" x2="{x2}" y2="0" gradientUnits="userSpaceOnUse">'
                + ''.join(stops) + '</linearGradient>')

    def radial(name, cx, cy, r, stops):
        return (f'<radialGradient id="{name}" cx="{cx}" cy="{cy}" r="{r}" gradientUnits="userSpaceOnUse">'
                + ''.join(stops) + '</radialGradient>')

    # Saturated RGB in the RGB document, where it shows best how the stops blend; RGB inside a
    # press gamut in the CMYK one, since a CMYK .ai reopens with the working CMYK profile and RGB
    # paint is then shown as CMYK in it (DocumentColors::interpret).
    red, blue, green = ('#ff0000', '#0040c0', RGB_GREEN) if mode == 'RGB' else \
        ('#c8503c', '#3c64a0', ('PROOF RGB Green', '#5a9a64'))
    owners = [ink(0, LEAF) if colour is None else stop(o, colour, a) for o, colour, a in OWNERS]
    defs = (linear('cr', 20, 140, [stop(0, 'device-cmyk(1 0 0 0)'), stop(1, red)]) +
            linear('sr', 180, 300, [ink(0, LEAF), stop(1, '#62496a')]) +
            linear('rcr', 340, 460, [stop(0, '#ffffff'), stop(0.5, 'device-cmyk(0 0.8 1 0)'), stop(1, blue)]) +
            radial('rl', 80, 180, 50, [stop(0, '#ffe000'), ink(1, LAB_BLUE)]) +
            radial('own', 240, 180, 60, owners) +
            radial('gc', 400, 180, 50, [ink(0, green), stop(1, 'device-cmyk(0 0 0 0.6)')]))
    art = ('<rect x="10" y="20" width="140" height="80" fill="url(#cr)"/>'
           '<rect x="170" y="20" width="140" height="80" fill="url(#sr)"/>'
           '<rect x="330" y="20" width="140" height="80" fill="url(#rcr)"/>'
           '<rect x="10" y="125" width="140" height="110" fill="url(#rl)"/>'
           '<rect x="170" y="125" width="140" height="110" fill="url(#own)"/>'
           '<rect x="330" y="125" width="140" height="110" fill="url(#gc)"/>')
    if mode == 'RGB':
        # An ordinary RGB colour, so that Illustrator opens the control as an RGB document.
        art += '<rect x="0" y="0" width="6" height="6" fill="#ff8000"/>'
    # No profile: in a CMYK document with one, PROOF reads RGB paint as CMYK in it
    # (DocumentColors::interpret), and no stops are mixed.
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="{mode}">'
            f'<defs>{defs}</defs>{art}</svg>')


def pixels(png):
    """On white, as Illustrator's renders are: a stop's opacity shows as it does there."""
    with Image.open(png) as im:
        rgba = np.asarray(im.convert('RGBA'), dtype=np.float64)
    alpha = rgba[..., 3:] / 255
    return np.rint(rgba[..., :3] * alpha + 255 * (1 - alpha)).astype(np.int16)


def compare(a_png, b_png):
    a, b = pixels(a_png), pixels(b_png)
    out = {}
    for name, (x0, y0, x1, y1) in REGIONS.items():
        diff = np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).max(axis=2)
        out[name] = dict(max=int(diff.max()), over24=round(float((diff > 24).mean()), 4))
    return out


def shading_spaces(pdf):
    """The colour space of every shading the page paints with (soft masks' grey left out)."""
    from pypdf import PdfReader
    found = []

    def walk(resources, depth=0):
        resources = resources.get_object() if resources is not None else {}
        if depth > 8:
            return
        for pattern in (resources.get('/Pattern') or {}).values():
            pattern = pattern.get_object()
            if '/Shading' in pattern:
                found.append(str(pattern['/Shading'].get_object()['/ColorSpace']))
            walk(pattern.get('/Resources'), depth + 1)
        for shading in (resources.get('/Shading') or {}).values():
            found.append(str(shading.get_object()['/ColorSpace']))
        for xobject in (resources.get('/XObject') or {}).values():
            xobject = xobject.get_object()
            if xobject.get('/Subtype') == '/Form':
                walk(xobject.get('/Resources'), depth + 1)

    walk(PdfReader(pdf, strict=True).pages[0]['/Resources'])
    return found


def styles(records):
    """The colour styles of each gradient definition's stops, sorted."""
    out = []
    for body in re.findall(r'^\(PROOF_G\d+\) [01] \d+ Bd\n\[\n(.*?)^BD$', records, re.M | re.S):
        found = set()
        for line in body.splitlines():
            words = line.split()
            if not words or words[-1] not in ('%_Bs', '%_BS', 'Bs', 'BS'):
                continue
            # [opacity 6] midpoint ramp after the style in the newer form.
            found.add(int(words[-6] if words[-4] == '6' else words[-4]))
        out.append(sorted(found))
    return sorted(out)


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

    for mode in ('RGB', 'CMYK'):
        stem = mode.lower()
        source = out / f'{stem}-control.svg'
        source.write_text(control(mode), encoding='utf-8')
        original, _ = png(f'{stem}-control', source)

        # The modern .ai: every stop in its own colour style.
        ai = out / f'{stem}-control.ai'
        run(f'{stem}-save', exe, source, f'--export-filename={ai}', '--export-extension=org.inkscape.output.ai.native')
        records = records_of(ai)
        (out / f'{stem}-control-records.ps').write_text(records, encoding='latin-1')
        found = styles(records)
        check(f'{mode}: each stop keeps its colour style', found == STYLES, str(found))
        model = b'%AI9_ColorModel: ' + (b'1' if mode == 'RGB' else b'2')
        check(f'{mode}: the document says its mode', model in records.encode('latin-1'))
        # Three inks: a `%%+` line before the format left Illustrator misplacing every radial.
        check(f'{mode}: the inks follow the format, as Illustrator writes them',
              0 <= records.find('%AI3_ColorUsage: Color\n%%DocumentCustomColors: ') < records.find('%%+ '))

        # Its PDF page and a plain PDF: the stops blend in the document's mode.
        pdf = out / f'{stem}-control.pdf'
        run(f'{stem}-pdf', exe, source, f'--export-filename={pdf}')
        for label, document in (('.ai page', ai), ('PDF', pdf)):
            spaces = shading_spaces(document)
            check(f'{mode} {label}: every shading in the document\'s mode',
                  len(spaces) == len(REGIONS) and set(spaces) == {f'/Device{mode}'}, str(spaces))

        # Reopened natively, and drawn as the canvas draws the control.
        reopened, log = png(f'{stem}-reopened', ai)
        check(f'{mode}: .ai reopens natively', 'opens from its PDF page' not in log, log.strip()[-200:])
        results = compare(original, reopened)
        check(f'{mode}: .ai reopened draws as the control', close(results), json.dumps(results))
        if mode == 'RGB':
            # The canvas blends every gradient in RGB; in an RGB document so does the PDF.
            for tool in (pdftoppm, pdftocairo):
                target = out / f'{stem}-pdf-{tool.stem}'
                run(f'{stem}-{tool.stem}', tool, '-r', '96', '-png', '-singlefile', pdf, target)
                results = compare(original, target.with_suffix('.png'))
                check(f'{mode}: PDF drawn by {tool.stem} as the control', close(results), json.dumps(results))

        legacy = out / f'{stem}-legacy.ai'
        p = run(f'{stem}-legacy-refuse', exe, source, f'--export-filename={legacy}',
                '--export-extension=org.inkscape.output.ai.legacy7', ok=False)
        check(f'{mode}: Illustrator 7 refuses mixed stops by name', p.returncode == 1 and not legacy.exists()
              and b'mixed gradient colour spaces (Illustrator 7)' in p.stderr, p.stderr.decode('utf-8', 'replace')[-200:])

        # The CMYK .ai reopens with the working CMYK profile, its RGB stops then CMYK in it.
        expected = STYLES if mode == 'RGB' else sorted(sorted(set(s) - {2} | ({1} if 2 in s else set()))
                                                       for s in STYLES)
        again = out / f'{stem}-again.ai'
        run(f'{stem}-again', exe, ai, f'--export-filename={again}', '--export-extension=org.inkscape.output.ai.native')
        found = styles(records_of(again))
        repeated, log = png(f'{stem}-again', again)
        results = compare(original, repeated)
        check(f'{mode}: a second write keeps the styles and reopens natively', found == expected
              and 'opens from its PDF page' not in log and close(results), json.dumps(dict(styles=found, **results)))

    report = dict(checks=checks, passed=sum(c['passed'] for c in checks), total=len(checks))
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'{report["passed"]}/{report["total"]} checks passed. {out / "report.json"}')
    return int(report['passed'] != report['total'])


if __name__ == '__main__':
    raise SystemExit(main())
