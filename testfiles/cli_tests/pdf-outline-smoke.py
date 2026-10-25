"""PDF outlines and cutting exemptions; synthetic fixtures, no desktop input."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from xml.etree import ElementTree as ET

from PIL import Image, ImageChops, ImageFilter, ImageStat
from pypdf import PdfReader
from pypdf.generic import ContentStream


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C')
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    checks = []

    def check(name, condition, detail=''):
        checks.append(dict(name=name, passed=bool(condition), detail=detail))
        print(('PASS ' if condition else 'FAIL ') + name + (' ' + detail if detail else ''), flush=True)

    def run(label, command, environment):
        result = subprocess.run([str(c) for c in command], env=environment, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        return result

    def export(label, source, outlined, text=False):
        profile = out / (label + '-profile')
        profile.mkdir()
        environment = env.copy()
        for name in ('INKSCAPE_PROFILE_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
            folder = profile / name.lower()
            folder.mkdir()
            environment[name] = str(folder)
        preferences = ET.Element('inkscape')
        extensions = ET.SubElement(preferences, 'group', id='extensions')
        extensions.set('org.inkscape.output.pdf.capypdf.strokesToPath', str(outlined).lower())
        ET.ElementTree(preferences).write(Path(environment['INKSCAPE_PROFILE_DIR']) / 'preferences.xml', encoding='utf-8')
        target = out / (label + '.pdf')
        command = [args.inkscape.resolve(), source, f'--export-filename={target}', '--export-pdf-version=1.7']
        if text:
            command.append('--export-text-to-path')
        result = run(label, command, environment)
        check(label + ': exported', result.returncode == 0 and target.is_file())
        return target

    def operators(reader):
        found = []

        def walk(content, resources):
            for values, op in ContentStream(content, reader).operations:
                found.append(op)
                if op == b'Do':
                    obj = resources['/XObject'][values[0]].get_object()
                    if obj.get('/Subtype') == '/Form':
                        walk(obj, obj.get('/Resources', resources))

        page = reader.pages[0]
        walk(page.get_contents(), page['/Resources'])
        return found

    def render(label, pdf):
        prefix = out / label
        result = run(label, [args.pdftoppm.resolve(), '-r', '144', '-singlefile', '-png', pdf, prefix], env)
        check(label + ': independent render', result.returncode == 0)
        with Image.open(prefix.with_suffix('.png')) as image:
            return image.convert('RGB')

    fixtures = {
        'ordinary': '''<path d="M20 25 C60 5 110 55 180 25 L180 65 L20 65Z"
          style="fill:#ffb040;stroke:#2060c0;stroke-width:5;stroke-opacity:0.6;opacity:0.8;
          stroke-linejoin:round;stroke-dasharray:13 5"/>''',
        'cut-layer': '''<g inkscape:groupmode="layer" inkscape:label="CutContour"><g
          inkscape:groupmode="layer" inkscape:label="Nested"><path d="M20 30H180"
          style="fill:none;stroke:#d02040;stroke-width:3"/></g></g>''',
        'cut-ink': '''<defs><linearGradient id="cut" inkscape:swatch="solid" proof:ink="CutContour"
          proof:ink-alternate="device-cmyk(0 1 0 0)"><stop stop-color="device-cmyk(0 1 0 0)"/></linearGradient></defs>
          <path d="M20 30H180" style="fill:none;stroke:url(#cut);stroke-width:3"/>''',
        'text': '''<text x="10" y="30" style="font-family:Arial;font-size:20px">OUTLINEME</text>
          <g inkscape:groupmode="layer" inkscape:label="Die line"><g><text x="10" y="65"
          style="font-family:Arial;font-size:20px">KEEPCUT</text></g></g>''',
        'clones-masks-markers': '''<defs>
          <marker id="arrow" orient="auto" markerWidth="4" markerHeight="4" refX="3" refY="2">
            <path d="M0 0L4 2L0 4Z" fill="context-stroke"/></marker>
          <clipPath id="clip"><rect x="10" y="5" width="180" height="80"/></clipPath>
          <mask id="mask"><rect width="220" height="100" fill="white"/></mask></defs>
          <g clip-path="url(#clip)" mask="url(#mask)"><path id="source" d="M20 15H150"
          style="fill:none;stroke:#2060c0;stroke-width:3;marker-end:url(#arrow)"/>
          <use xlink:href="#source" y="25"/><g inkscape:groupmode="layer" inkscape:label="Thru-cut">
          <use xlink:href="#source" y="50"/></g></g>''',
    }
    stroke_ops = {b'S', b's', b'B', b'B*', b'b', b'b*'}
    for name, body in fixtures.items():
        source = out / (name + '.svg')
        source.write_text('''<svg xmlns="http://www.w3.org/2000/svg"
          xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:proof="urn:x-proof:document:1"
          xmlns:xlink="http://www.w3.org/1999/xlink" width="220" height="100" viewBox="0 0 220 100">'''
          + body + '</svg>', encoding='utf-8')
        digest = hashlib.sha256(source.read_bytes()).digest()
        before = export(name + '-before', source, False)
        after = export(name + '-after', source, True, text=name == 'text')
        a, b = PdfReader(before, strict=True), PdfReader(after, strict=True)
        before_ops, after_ops = operators(a), operators(b)
        check(name + ': strict PDF 1.7', a.pdf_header == b.pdf_header == '%PDF-1.7')
        count = sum(op in stroke_ops for op in after_ops)
        if name == 'ordinary':
            check(name + ': reference strokes', any(op in stroke_ops for op in before_ops))
            check(name + ': outlined output has fills and no stroke operators', count == 0 and any(op in {b'f', b'f*'} for op in after_ops))
        elif name.startswith('cut-'):
            check(name + ': cutting stroke retained', count == 1)
        elif name == 'text':
            check(name + ': ordinary text outlined', 'OUTLINEME' not in b.pages[0].extract_text())
            check(name + ': cutting text retained', 'KEEPCUT' in b.pages[0].extract_text())
        else:
            check(name + ': only cutting clone stays stroked', count == 1)
        left, right = render(name + '-before-render', before), render(name + '-after-render', after)
        diff = ImageChops.difference(left, right)
        mean = sum(ImageStat.Stat(diff).mean) / 3
        changed = sum(max(pixel) > 16 for pixel in diff.getdata()) / (diff.width * diff.height)
        if name == 'text':
            # Live fonts are hinted; glyph outlines may move an edge by one pixel at 144 dpi.
            a_ink, b_ink = [im.convert('L').point(lambda v: 255 if v < 224 else 0) for im in (left, right)]
            outside = (ImageChops.subtract(a_ink, b_ink.filter(ImageFilter.MaxFilter(3))).getbbox() or
                       ImageChops.subtract(b_ink, a_ink.filter(ImageFilter.MaxFilter(3))).getbbox())
            check(name + ': appearance retained within one pixel', mean <= 0.5 and outside is None, f'mean={mean:.4f}')
        else:
            check(name + ': appearance retained', mean <= 0.5 and changed <= 0.005, f'mean={mean:.4f}, changed={changed:.5f}')
        check(name + ': source unchanged', hashlib.sha256(source.read_bytes()).digest() == digest)

    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)


if __name__ == '__main__':
    main()
