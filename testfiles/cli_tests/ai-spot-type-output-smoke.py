"""Type in spot inks in the modern .ai records, in one synthetic control; no desktop input.

One CMYK document in a press profile with inks defined in CMYK, Lab and RGB: point type in one
ink at full strength, point type in a Lab ink at a tint with a word stroked in an RGB ink, point
type in process colour with a word in the CMYK ink, and area type in the Lab ink. Illustrator
29.2.1 names an ink on type by an art style (2026-10-24): each character's paint is a
/CAITextPaint whose /5 is the name of a /KnownStyle among the records, and the style's line is
the ink's, as a path filled with it has (a stroke's style fills too). The copy must write one
style for each ink and tint, before the text documents; it must reopen from its own records with
every character in its ink and tint, draw as the source, and write the same styles again. Its PDF
page draws type in an ink as it draws a path in one, by what the ink looks like: separate plates
in PDF are later prepress work. Illustrator's own reading is separate qualification.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops

SVG = '{http://www.w3.org/2000/svg}'
PROOF = '{urn:x-proof:document:1}'
WIDTH, HEIGHT = 480, 300
PROFILE = Path(__file__).resolve().parents[1] / 'data' / 'colors' / 'default_cmyk.icc'
# name: (kind, definition); Lab as L a b, RGB and CMYK from 0 to 1.
INKS = {
    'PROOF Type Orange': ('cmyk', (0, 0.6, 1, 0)),
    'PROOF Type Violet': ('lab', (40, 50, -60)),
    'PROOF Type Green': ('rgb', (0.08, 0.63, 0.35)),
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
    """The colour a tint of the ink shows (spot-ink.h, display_color)."""
    kind, v = INKS[name]
    if kind == 'lab':
        return css(kind, (100 - tint * (100 - v[0]), tint * v[1], tint * v[2]))
    if kind == 'rgb':
        return css(kind, tuple(1 - tint * (1 - x) for x in v))
    return css(kind, tuple(tint * x for x in v))


def swatch(sid, name, tint):
    kind, v = INKS[name]
    tint_attr = f' proof:ink-tint="{fmt(tint)}"' if tint < 1 else ''
    return (f'<linearGradient id="{sid}" inkscape:swatch="solid" proof:ink="{name}" '
            f'proof:ink-alternate="{css(kind, v)}"{tint_attr}>'
            f'<stop offset="0" style="stop-color:{shown(name, tint)}"/></linearGradient>')


# (swatch id, ink, tint)
SWATCHES = [('orange', 'PROOF Type Orange', 1.0), ('violet40', 'PROOF Type Violet', 0.4),
            ('green60', 'PROOF Type Green', 0.6), ('violet', 'PROOF Type Violet', 1.0)]
# What each story's characters should come back in: (ink, tint) of the fill, and of the stroke.
EXPECTED = {
    'Orange ink': [(('PROOF Type Orange', 1.0), None)] * 10,
    'Violet ink': [(('PROOF Type Violet', 0.4), None)] * 7 + [(('PROOF Type Violet', 0.4), ('PROOF Type Green', 0.6))] * 3,
    'Process ink': [(None, None)] * 8 + [(('PROOF Type Orange', 1.0), None)] * 3,
}


def control():
    font = 'font-family:Arial;font-size:30px'
    defs = ''.join(swatch(*s) for s in SWATCHES)
    defs += '<rect id="frame" x="260" y="20" width="200" height="120"/>'
    art = ''.join([
        f'<text id="whole" xml:space="preserve" x="20" y="60" style="{font};fill:url(#orange)">Orange ink</text>',
        f'<text id="tinted" xml:space="preserve" x="20" y="130" style="{font};fill:url(#violet40)">Violet '
        '<tspan style="stroke:url(#green60);stroke-width:1.5px">ink</tspan></text>',
        f'<text id="mixed" xml:space="preserve" x="20" y="200" style="{font};fill:device-cmyk(0.1 0.2 0.3 0.4)">'
        'Process <tspan style="fill:url(#orange)">ink</tspan></text>',
        '<text id="area" xml:space="preserve" style="shape-inside:url(#frame);white-space:pre;font-family:Arial;'
        'font-size:24px;fill:url(#violet)">Area type in a violet ink</text>',
    ])
    profile = (f'<color-profile id="press" name="Press" rendering-intent="relative-colorimetric" '
               f'xlink:href="{PROFILE.as_uri()}"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:proof="urn:x-proof:document:1" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" proof:color-mode="CMYK" '
            f'proof:color-profile="Press"><defs>{profile}{defs}</defs>'
            f'<g inkscape:groupmode="layer" inkscape:label="Type">{art}</g></svg>')


def characters(svg):
    """Each text's characters as {first words: [(fill (ink, tint), stroke (ink, tint))]}, in order."""
    root = ET.parse(svg).getroot()
    swatches = {}
    for e in root.iter():
        if e.get(PROOF + 'ink') and e.get('id'):
            swatches[e.get('id')] = (e.get(PROOF + 'ink'), round(float(e.get(PROOF + 'ink-tint') or 1), 4))

    def declared(e, prop):
        for part in (e.get('style') or '').split(';'):
            k, _, v = part.partition(':')
            if k.strip() == prop:
                return v.strip()
        return e.get(prop)

    def ink(value):
        m = re.match(r'url\(#([^)]+)\)', value or '')
        return swatches.get(m.group(1)) if m else None

    out = {}

    def walk(e, fill, stroke, into):
        fill = declared(e, 'fill') or fill
        stroke = declared(e, 'stroke') or stroke
        if e.text:
            into.extend([(ink(fill), ink(stroke))] * len(e.text.replace('\n', '')))
        for child in e:
            walk(child, fill, stroke, into)
            if child.tail:
                into.extend([(ink(fill), ink(stroke))] * len(child.tail.replace('\n', '')))

    for t in root.iter(SVG + 'text'):
        chars = []
        walk(t, None, None, chars)
        words = ''.join(t.itertext())
        out[' '.join(words.split()[:2])] = chars
    return out


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
        from pypdf import PdfReader
        private = PdfReader(ai, strict=True).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        marker = b'%AI24_ZStandard_Data'
        if not packed.startswith(marker):
            raise RuntimeError(f'{ai.name}: missing Zstandard records')
        return subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'], input=packed[len(marker):],
                              capture_output=True, timeout=60, check=True).stdout

    def text_document(records):
        """The first text document, decoded from its ASCII85 lines."""
        start = records.index(b'/AI11TextDocument : /ASCII85Decode ,\n') + len(b'/AI11TextDocument : /ASCII85Decode ,\n')
        end = records.index(b'~>', start)
        lines = [l[1:] if l.startswith(b'%') else l for l in records[start:end].split(b'\n')]
        return base64.a85decode(b'<~' + b''.join(lines).strip() + b'~>', adobe=True)

    def styles_of(records):
        return dict(re.findall(rb'/KnownStyle :\n\((Anon PROOF \d+)\) /Name ,\n/SimpleStyle :\n0 O\n([^\n]+)\n0 1 0 0 0 Xy\n',
                               records))

    source = out / 'control.svg'
    source.write_text(control(), encoding='utf-8')
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    ai = out / 'control.ai'
    run('save', exe, source, native, f'--export-filename={ai}')
    records = records_of(ai)
    (out / 'records.txt').write_bytes(records)

    styles = styles_of(records)
    check('one art style for each ink and tint type is painted with', len(styles) == 4, sorted(styles.values()))
    check('the CMYK ink whole as a path has it', b'0 0.6 1 0 (PROOF Type Orange) 0 x' in styles.values())
    check('the Lab ink at its tint, Xx type 2', any(v.endswith(b' 40 50 -60 (PROOF Type Violet) 0.6 2 Xx') for v in styles.values()))
    check('a stroke\'s style fills with its ink (lower case), Xx type 1',
          any(v.endswith(b' (PROOF Type Green) 0.4 1 Xx') for v in styles.values())
          and b'(PROOF Type Green) 0.4 1 XX' not in records)
    begin, typed = records.find(b'%AI9_BeginArtStyles'), records.find(b'%AI11_BeginTextDocument')
    check('the styles in the setup, before the text documents', 0 <= records.find(b'%%BeginSetup') < begin < typed
          and records.find(b'%AI9_EndArtStyles\n%AI5_End_NonPrinting--\n') < typed)
    check('every ink declared', all(f'({n})'.encode() in records[:records.find(b'%%EndComments')] for n in INKS))
    document = text_document(records)
    named = set(re.findall(rb'/99 /CAITextPaint /5 \(\xfe\xff((?:[^\\)]|\\.)*)\)', document))
    named = {n.decode('utf-16-be') for n in named}
    check('the characters name those styles by /CAITextPaint', named == {k.decode() for k in styles}, sorted(named))

    reopened = out / 'reopened.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    check('reopens from its own records', b'opens from its PDF page' not in p.stdout + p.stderr)
    got = characters(reopened)
    for words, want in EXPECTED.items():
        check(f'"{words}": every character in its ink and tint', got.get(words) == want, got.get(words))
    area = got.get('Area type', [])
    check('area type: every character in its ink', area and all(c == (('PROOF Type Violet', 1.0), None) for c in area),
          area[:3])

    canvas, drawn = out / 'canvas.png', out / 'reopened.png'
    run('canvas', exe, source, '--export-background=white', f'--export-filename={canvas}')
    run('drawn', exe, reopened, '--export-background=white', f'--export-filename={drawn}')
    with Image.open(canvas) as a, Image.open(drawn) as b:
        diff = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
        over = sum(1 for px in diff.getdata() if max(px) > 24)
    check('draws as the source (under 0.2% of pixels over 24 levels)', over < WIDTH * HEIGHT * 0.002, over)

    again = out / 'again.ai'
    run('again', exe, reopened, native, f'--export-filename={again}')
    check('written again from the reopened copy: the same styles', styles_of(records_of(again)) == styles)

    legacy_ai = out / 'legacy.ai'
    p = run('legacy', exe, source, legacy, f'--export-filename={legacy_ai}', ok=False)
    check('Illustrator 7 refuses type by name', not legacy_ai.exists() and b'type (Illustrator 7)' in p.stdout + p.stderr)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == source_hash)
    (out / 'report.json').write_text(json.dumps(checks, indent=2, default=repr))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
