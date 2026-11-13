"""Lab and Separation PDF bytes and findings; synthetic fixtures, no desktop input."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from pypdf import PdfReader
from pypdf.generic import IndirectObject, StreamObject


def serialized(pdf):
    reader = PdfReader(pdf, strict=True)
    data = pdf.read_bytes()
    # Inspect original decoded bytes, including compressed object dictionaries and text commands.
    for generation, objects in reader.xref.items():
        for number in objects:
            if number:
                obj = reader.get_object(IndirectObject(number, generation, reader))
                if isinstance(obj, StreamObject):
                    data += obj.get_data()
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for option in ('inkscape', 'dll-dir', 'data-dir', 'output'):
        ap.add_argument('--' + option, type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C')
    env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    for name in ('INKSCAPE_PROFILE_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        folder = out / name.lower()
        folder.mkdir()
        env[name] = str(folder)
    checks = []

    def check(name, value):
        checks.append(dict(name=name, passed=bool(value)))
        print(('PASS ' if value else 'FAIL ') + name, flush=True)

    head = ('<svg xmlns="http://www.w3.org/2000/svg" '
            'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            'xmlns:proof="urn:x-proof:document:1" width="200" height="100">')
    flat = ('<rect id="flat" x="10" y="10" width="80" height="70" fill="lab(55 20 -30)" '
            'stroke="lab(65 -12 31)" stroke-width="3"/>'
            '<text id="type" x="100" y="40" style="font-family:Arial;font-size:20px;fill:lab(50 0 0)">A'
            '<tspan style="fill:lab(70 10 20);stroke:lab(60 -5 10);stroke-width:0.5">B</tspan></text>')
    stops = '<stop stop-color="lab(55 20 -30)"/><stop offset="1" stop-color="red"/>'
    mesh = ('<meshgradient id="g" x="10" y="10" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>' +
            ''.join(f'<stop path="{path}" stop-color="lab(55 20 -30)"/>' for path in
                    ('l80,0', 'l0,70', 'l-80,0', 'l0,-70')) + '</meshpatch></meshrow></meshgradient>')
    cases = {
        'flat': flat,
        'filtered': '<defs><filter id="f"><feGaussianBlur stdDeviation="1"/></filter></defs>'
                    '<rect id="target" width="80" height="80" fill="lab(55 20 -30)" filter="url(#f)"/>',
        'swatch': '<defs><linearGradient id="g" inkscape:swatch="solid"><stop stop-color="lab(55 20 -30)"/>'
                  '</linearGradient></defs><rect width="80" height="80" fill="url(#g)"/>',
        'gradient': f'<defs><linearGradient id="g">{stops}</linearGradient></defs>'
                    '<rect id="target" width="80" height="80" fill="url(#g)"/>',
        'radial': f'<defs><radialGradient id="g">{stops}</radialGradient></defs>'
                  '<rect id="target" width="80" height="80" fill="url(#g)"/>',
        'mesh': f'<defs>{mesh}</defs><rect id="target" width="100" height="90" fill="url(#g)"/>',
        'pattern': '<defs><pattern id="g" patternUnits="userSpaceOnUse" width="20" height="20">'
                   '<rect width="10" height="20" fill="lab(55 20 -30)"/></pattern></defs>'
                   '<rect width="100" height="90" fill="url(#g)"/>',
        'empty': '<defs><linearGradient id="g" inkscape:swatch="solid" proof:ink="" '
                 'proof:ink-alternate="device-cmyk(0 1 0 0)"><stop stop-color="red"/></linearGradient></defs>'
                 '<rect id="target" width="80" height="80" fill="url(#g)"/>',
        'name': '<defs><linearGradient id="g" inkscape:swatch="solid" proof:ink="Encre \u00e9 [%/#]()" '
                'proof:ink-alternate="device-cmyk(0 1 0 0)"><stop stop-color="red"/></linearGradient></defs>'
                '<rect id="target" width="80" height="80" fill="url(#g)"/>',
    }
    for label, body in cases.items():
        source, pdf = out / (label + '.svg'), out / (label + '.pdf')
        source.write_text(head + body + '</svg>', encoding='utf-8')
        result = subprocess.run([str(args.inkscape.resolve()), str(source), f'--export-filename={pdf}'],
                                env=env, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        text = result.stderr.decode('utf-8', 'replace')
        check(label + ': PDF written', result.returncode == 0 and pdf.is_file())
        if not pdf.is_file():
            continue
        data = serialized(pdf)
        if label == 'mesh':
            raw = pdf.read_bytes()
            dictionaries = re.findall(rb'<<(.*?)>>\s*stream\r?\n', raw, re.S)
            check('mesh: exactly one Length per original stream dictionary', dictionaries and all(
                len(re.findall(rb'/Length\b', dictionary)) == 1 for dictionary in dictionaries))
            correct = True
            for match in re.finditer(rb'<<(.*?)>>\s*stream\r?\n', raw, re.S):
                length = re.search(rb'/Length\s+(\d+)\s*(?:/|>>|$)', match[1])
                if length:
                    end = match.end() + int(length[1])
                    correct = correct and raw[end:].startswith((b'\nendstream', b'\r\nendstream'))
                else:
                    correct = False
            check('mesh: declared lengths match original stream bytes', correct)
        if label in ('flat', 'empty'):
            ai = out / (label + '.ai')
            previous = b'previous destination'
            if label == 'empty':
                ai.write_bytes(previous)
            saved = subprocess.run([str(args.inkscape.resolve()), str(source), f'--export-filename={ai}',
                                    '--export-extension=org.inkscape.output.ai.native'], env=env,
                                   capture_output=True, timeout=240)
            (out / (label + '-ai.log')).write_bytes(saved.stdout + saved.stderr)
            if label == 'flat':
                check('native page: Lab space retained', saved.returncode == 0 and ai.is_file() and b'/Lab' in serialized(ai))
            else:
                check('empty name: native refusal preserves destination', saved.returncode != 0 and ai.read_bytes() == previous and
                      b'Error [target]: Spot ink name is empty.' in saved.stderr)
        if label in ('flat', 'swatch', 'pattern'):
            check(label + ': one Lab space, no conversion finding', data.count(b'/Lab') == 1 and
                  'export findings' not in text)
            match = re.search(rb'/WhitePoint\s*\[([^]]+)\]', data)
            check(label + ': D50 white point', match and [float(v) for v in match[1].split()] == [0.9642, 1, 0.8249])
            match = re.search(rb'/Range\s*\[([^]]+)\]', data)
            check(label + ': stated a/b range', match and [float(v) for v in match[1].split()] == [-128, 127, -128, 127])
        if label == 'flat':
            for values, op in (((55, 20, -30), 'scn'), ((65, -12, 31), 'SCN'),
                               ((70, 10, 20), 'scn'), ((60, -5, 10), 'SCN')):
                operands = ' '.join(f'{v:.6f}' for v in values)
                check(f'flat: {operands} {op}', (operands + ' ' + op).encode() in data)
        if label in ('gradient', 'radial', 'mesh', 'filtered'):
            check(label + ': RGB fallback named on object', 'Warning [target]: Lab colour was converted to RGB.' in text)
        if label == 'empty':
            check('empty name: error finding names object', 'Error [target]: Spot ink name is empty.' in text)
        if label == 'name':
            check('name: UTF-8 and PDF delimiter bytes escaped',
                  b'/Separation /Encre#20#c3#a9#20#5b#25#2f#23#5d#28#29' in data)
    (out / 'summary.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)


if __name__ == '__main__':
    main()
