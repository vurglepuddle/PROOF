"""Export findings and atomic native refusal; synthetic fixtures, no desktop input."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
from PIL import Image
from pypdf import PdfReader


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('inkscape', 'dll-dir', 'data-dir', 'pdftoppm', 'output'):
        ap.add_argument('--' + name, type=Path, required=True)
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
    # Only the PDF format requests stroke outlines; the native format keeps fixed options.
    (out / 'inkscape_profile_dir' / 'preferences.xml').write_text('''<inkscape><group id="extensions"
        org.inkscape.output.pdf.capypdf.strokesToPath="true"/></inkscape>''', encoding='utf-8')
    checks = []

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        print(('PASS ' if ok else 'FAIL ') + name, flush=True)

    def run(label, command):
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        return result, (result.stdout + result.stderr).decode('utf-8', errors='replace')

    png = ('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ'
           'AAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==')
    bodies = {
        'font': '<text id="target" x="10" y="30" style="font-family:Arial;font-size:20px">Test</text>',
        'picture': f'<image id="target" x="10" y="10" width="20" height="20" href="{png}"/>',
        'outline': '<path id="target" d="M10 20H80" fill="none" stroke="black" stroke-width="4"/>',
        'object': '<rect id="target" x="10" y="10" width="20" height="20" opacity="0.5"/>',
        'missing-picture': '<image id="target" width="20" height="20" href="missing.png"/>',
    }
    for kind, body in bodies.items():
        source, pdf = out / (kind + '.svg'), out / (kind + '.pdf')
        source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">' + body +
                          '<rect x="10" y="60" width="20" height="20" fill="red"/></svg>', encoding='utf-8')
        env['PROOF_TEST_PDF_FAILURE'] = kind + ':target'
        result, text = run(kind, [args.inkscape.resolve(), source, f'--export-filename={pdf}',
                                  '--export-pdf-version=1.7'])
        check(kind + ': PDF written with listed finding', result.returncode == 0 and pdf.is_file() and
              'PDF written with export findings. Some content may be missing or not converted.' in text and
              ('Warning [target]: Linked picture is missing:' if kind == 'missing-picture' else 'Error [target]:') in text)
        if pdf.is_file():
            check(kind + ': readable PDF', len(PdfReader(pdf).pages) == 1)
            prefix = out / (kind + '-render')
            rendered, _ = run(kind + '-render', [args.pdftoppm.resolve(), '-r', '96', '-singlefile',
                                                '-png', pdf, prefix])
            check(kind + ': following object survives', rendered.returncode == 0 and
                  Image.open(prefix.with_suffix('.png')).convert('RGB').getpixel((20, 70)) == (255, 0, 0))
        if kind == 'outline':
            continue  # Covered by the unit test with PDF options passed to the native save guard.
        ai = out / (kind + '.ai')
        previous = b'existing destination\x00\xff'
        ai.write_bytes(previous)
        result, text = run(kind + '-ai', [args.inkscape.resolve(), source, f'--export-filename={ai}',
                                        '--export-extension=org.inkscape.output.ai.native'])
        check(kind + ': native save refuses and preserves destination', result.returncode != 0 and
              ai.read_bytes() == previous)
        check(kind + ': refusal explains failure', 'Failed to save' in text and
              ('Error [target]:' in text or kind == 'missing-picture'))
        check(kind + ': no incomplete native file remains', not list(out.glob(ai.name + '.proof-*')))
    env.pop('PROOF_TEST_PDF_FAILURE', None)
    source, ai = out / 'kept-preview.svg', out / 'kept-preview.ai'
    missing = out / 'absent.png'
    source.write_text('<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1" '
                      'width="100" height="100"><image id="target" width="20" height="20" '
                      f'preserveAspectRatio="none" href="{missing.as_uri()}" proof:link-preview="{png}"/>'
                      '<rect x="10" y="60" width="20" height="20" fill="red"/></svg>', encoding='utf-8')
    result, text = run('kept-preview-ai', [args.inkscape.resolve(), source, f'--export-filename={ai}',
                                        '--export-extension=org.inkscape.output.ai.native'])
    check('missing link with kept preview: native save succeeds with warning', result.returncode == 0 and
          ai.is_file() and 'Warning [target]: Linked picture is missing:' in text and str(missing) in text)
    if ai.is_file():
        page = PdfReader(ai).pages[0]
        def image_count(owner):
            count = 0
            for reference in owner.get('/Resources', {}).get('/XObject', {}).values():
                obj = reference.get_object()
                count += 1 if obj.get('/Subtype') == '/Image' else image_count(obj)
            return count
        check('missing link: PDF page omits picture as before', image_count(page) == 0)
        reopened = out / 'kept-preview-reopened.svg'
        result, text = run('kept-preview-reopen', [args.inkscape.resolve(), ai, f'--export-filename={reopened}'])
        pictures = list(ET.parse(reopened).getroot().iter('{http://www.w3.org/2000/svg}image')) if reopened.is_file() else []
        check('missing link and preview survive native reopen', result.returncode == 0 and len(pictures) == 1 and
              pictures[0].get('{http://www.w3.org/1999/xlink}href', pictures[0].get('href', '')).endswith('absent.png') and
              pictures[0].get('{urn:x-proof:document:1}link-preview', '').startswith('data:image/png;'))
        repeated = out / 'kept-preview-repeated.pdf'
        result, text = run('kept-preview-repeat', [args.inkscape.resolve(), reopened, f'--export-filename={repeated}'])
        check('reopened link is still missing and warns again', result.returncode == 0 and
              'Linked picture is missing:' in text and 'Warning [' in text)
    for label, attributes, reason in (
            ('unreadable-preview', f'preserveAspectRatio="none" proof:link-preview="data:image/png;base64,bm90IHBuZw=="',
             "preview can't be read"),
            ('unfitted-preview', f'proof:link-preview="{png}"', "don't fill their box")):
        source = out / (label + '.svg')
        source.write_text('<svg xmlns="http://www.w3.org/2000/svg" xmlns:proof="urn:x-proof:document:1">'
                          f'<image id="target" width="20" height="20" href="{missing.as_uri()}" {attributes}/></svg>',
                          encoding='utf-8')
        refused = out / (label + '.ai')
        refused.write_bytes(b'unchanged destination')
        result, text = run(label, [args.inkscape.resolve(), source, f'--export-filename={refused}',
                                   '--export-extension=org.inkscape.output.ai.native'])
        check(label + ': refuses with destination intact', result.returncode != 0 and reason in text and
              refused.read_bytes() == b'unchanged destination')
    (out / 'summary.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)


if __name__ == '__main__':
    main()
