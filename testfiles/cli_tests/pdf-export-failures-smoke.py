"""Export findings and atomic native refusal; synthetic fixtures, no desktop input."""
import argparse
import json
import os
from pathlib import Path
import subprocess
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
              'PDF written with export findings.' in text and 'Error [target]:' in text)
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
    (out / 'summary.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)


if __name__ == '__main__':
    main()
