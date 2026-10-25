"""PDF compatibility and placed-resource refusals; synthetic fixtures, no desktop input."""
import argparse
import base64
from io import BytesIO
import json
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageChops
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (ArrayObject, ByteStringObject, DecodedStreamObject,
                           DictionaryObject, IndirectObject, NameObject, TextStringObject)


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
    for name in ('INKSCAPE_PROFILE_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        folder = out / name.lower()
        folder.mkdir()
        env[name] = str(folder)
    checks = []

    def check(name, condition):
        checks.append(dict(name=name, passed=bool(condition)))
        print(('PASS ' if condition else 'FAIL ') + name, flush=True)

    def run(label, command):
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        return result

    def export(label, source, target, *options):
        return run(label, [args.inkscape.resolve(), source, f'--export-filename={target}', *options])

    def render(label, path, page):
        prefix = out / label
        result = run(label, [args.pdftoppm.resolve(), '-f', page, '-l', page, '-singlefile',
                             '-scale-to', '600', '-png', path, prefix])
        check(label + ': independent render', result.returncode == 0)
        with Image.open(prefix.with_suffix('.png')) as image:
            return image.convert('RGB')

    def decoded_objects(reader):
        seen = set()

        def walk(obj):
            if isinstance(obj, IndirectObject):
                ref = (obj.idnum, obj.generation)
                if ref in seen:
                    return
                seen.add(ref)
                yield from walk(obj.get_object())
                return
            yield obj
            if isinstance(obj, dict):
                for value in obj.values():
                    yield from walk(value)
            elif isinstance(obj, list):
                for value in obj:
                    yield from walk(value)

        yield from walk(reader.trailer)
        for generation, entries in reader.xref.items():
            for number in entries:
                if number:
                    yield from walk(IndirectObject(number, generation, reader))
        for number in reader.xref_objStm:
            yield from walk(IndirectObject(number, 0, reader))

    def string_bytes(obj):
        if isinstance(obj, TextStringObject):
            return obj.original_bytes
        return bytes(obj) if isinstance(obj, ByteStringObject) else b''

    def features(reader):
        found = set()
        for obj in decoded_objects(reader):
            if isinstance(obj, dict):
                found.update(str(k) for k in obj if k in ('/UseBlackPtComp', '/AF', '/AFRelationship'))
            elif string_bytes(obj).startswith(b'\xef\xbb\xbf'):
                found.add('UTF-8 text string')
        return found

    png = BytesIO()
    Image.new('RGB', (2, 2), (35, 90, 210)).save(png, 'PNG')
    encoded = base64.b64encode(png.getvalue()).decode('ascii')
    source = out / 'control.svg'
    source.write_text('''<svg xmlns="http://www.w3.org/2000/svg"
      xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
      xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
      xmlns:xlink="http://www.w3.org/1999/xlink" width="240" height="160" viewBox="0 0 240 160">
      <title>PROOF café — PDF compatibility</title>
      <sodipodi:namedview><inkscape:page x="0" y="0" width="240" height="160" inkscape:label="Front"/>
      <inkscape:page x="260" y="0" width="120" height="160" inkscape:label="Back café"/></sodipodi:namedview>
      <defs><linearGradient id="g"><stop stop-color="#ff8000"/><stop offset="1" stop-color="#2030e0"/></linearGradient>
      <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter></defs>
      <rect x="10" y="10" width="160" height="80" fill="url(#g)"/>
      <circle cx="130" cy="80" r="35" fill="#20a050" opacity="0.45"/>
      <rect x="185" y="20" width="30" height="30" fill="red" filter="url(#blur)"/>
      <text x="15" y="135" style="font-family:Arial;font-size:20px">PROOF PDF 123</text>
      <image x="280" y="30" width="60" height="60" xlink:href="data:image/png;base64,'''
      + encoded + '"/></svg>', encoding='utf-8')
    default, pdf17, pdf20 = [out / name for name in ('default.pdf', 'v17.pdf', 'v20.pdf')]
    for label, target, options, header in (
        ('default', default, (), '%PDF-2.0'),
        ('v17', pdf17, ('--export-pdf-version=1.7',), '%PDF-1.7'),
        ('v20', pdf20, ('--export-pdf-version=2.0',), '%PDF-2.0'),
    ):
        result = export(label, source, target, *options)
        check(label + ': exported', result.returncode == 0 and target.is_file())
        if target.is_file():
            reader = PdfReader(target, strict=True)
            check(label + ': header', reader.pdf_header == header)
            check(label + ': two pages', len(reader.pages) == 2)
            check(label + ': live text', 'PROOF PDF 123' in reader.pages[0].extract_text())
            if label == 'v17':
                check('1.7: audited constructs absent', not features(reader))
    for page in (1, 2):
        if pdf17.is_file() and pdf20.is_file():
            a = render(f'v17-page-{page}', pdf17, page)
            b = render(f'v20-page-{page}', pdf20, page)
            check(f'page {page}: versions draw identically', a.size == b.size and ImageChops.difference(a, b).getbbox() is None)
    again = out / 'default-after-17.pdf'
    result = export('default-after-17', source, again)
    check('CLI default ignores saved 1.7', result.returncode == 0 and PdfReader(again).pdf_header == '%PDF-2.0')

    for label, version, extension in (
        ('unsupported', '1.3', 'org.inkscape.output.pdf.capypdf'),
        ('cairo-17', '1.7', 'org.inkscape.output.pdf.cairorenderer'),
        ('cairo-20', '2.0', 'org.inkscape.output.pdf.cairorenderer'),
    ):
        target = out / (label + '.pdf')
        previous = b'previous destination, must remain unchanged'
        target.write_bytes(previous)
        result = export(label, source, target, f'--export-pdf-version={version}', f'--export-extension={extension}')
        check(label + ': nonzero refusal', result.returncode != 0)
        check(label + ': destination untouched', target.read_bytes() == previous)

    for feature in ('plain', 'UseBlackPtComp', 'AF', 'AFRelationship', 'utf8'):
        writer = PdfWriter()
        writer.pdf_header = '%PDF-1.7' if feature == 'plain' else '%PDF-2.0'
        page = writer.add_blank_page(width=100, height=100)
        state = DictionaryObject()
        if feature == 'UseBlackPtComp':
            state[NameObject('/UseBlackPtComp')] = NameObject('/ON')
        elif feature == 'AF':
            state[NameObject('/AF')] = ArrayObject()
        elif feature == 'AFRelationship':
            state[NameObject('/AFRelationship')] = NameObject('/Data')
        elif feature == 'utf8':
            state[NameObject('/TestDescription')] = ByteStringObject(b'\xef\xbb\xbfPROOF')
        page[NameObject('/Resources')] = DictionaryObject({NameObject('/ExtGState'): DictionaryObject({
            NameObject('/GS0'): writer._add_object(state)})})
        stream = DecodedStreamObject()
        stream.set_data(b'/GS0 gs 0 0.6 0 rg 10 10 80 80 re f')
        page[NameObject('/Contents')] = writer._add_object(stream)
        placed = out / (feature + '-source.pdf')
        writer.write(placed)
        if feature == 'utf8':
            check('utf8: source carries UTF-8 BOM string', 'UTF-8 text string' in features(PdfReader(placed, strict=True)))
        svg = out / (feature + '.svg')
        svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
                       'width="100" height="100"><image width="100" height="100" xlink:href="'
                       + placed.name + '"/></svg>', encoding='utf-8')
        for version in ('1.7', '2.0'):
            label = feature + '-' + version
            target = out / (label + '.pdf')
            previous = b'previous PDF destination' if feature == 'UseBlackPtComp' and version == '1.7' else None
            if previous:
                target.write_bytes(previous)
            result = export(label, svg, target, f'--export-pdf-version={version}')
            if feature in ('UseBlackPtComp', 'AF', 'AFRelationship') and version == '1.7':
                name = ('/' + feature).encode()
                check(label + ': refused by name', result.returncode != 0 and name in result.stdout + result.stderr)
                check(label + ': destination preserved', target.read_bytes() == previous if previous else not target.exists())
            else:
                check(label + ': accepted', result.returncode == 0 and target.is_file())
                if target.is_file():
                    reader = PdfReader(target, strict=True)
                    check(label + ': strict parse and version', reader.pdf_header == '%PDF-' + version and len(reader.pages) == 1)
                    if feature in ('UseBlackPtComp', 'AF', 'AFRelationship'):
                        check(label + ': placed resource preserved', '/' + feature in features(reader))
                    elif feature == 'utf8':
                        descriptions = [string_bytes(obj['/TestDescription']) for obj in decoded_objects(reader)
                                        if isinstance(obj, dict) and '/TestDescription' in obj]
                        check(label + ': copied string is UTF-16BE', descriptions == [b'\xfe\xff' + 'PROOF'.encode('utf-16-be')])
                        check(label + ': no UTF-8 BOM strings in decoded objects', 'UTF-8 text string' not in features(reader))

    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)


if __name__ == '__main__':
    main()
