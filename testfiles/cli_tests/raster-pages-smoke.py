"""Synthetic named pages through the existing CLI PNG page renderer, without GTK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from PIL import Image


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('inkscape', 'dll-dir', 'data-dir', 'output'):
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
    source = out / 'pages.svg'
    source.write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="96" height="48" viewBox="0 0 96 48"
      xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
      xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">
      <sodipodi:namedview><inkscape:page x="0" y="0" width="96" height="48" inkscape:label="Front"/>
      <inkscape:page x="110" y="0" width="80" height="60" inkscape:label="Back"/>
      <inkscape:page x="210" y="0" width="40" height="60" inkscape:label="Detail"/></sodipodi:namedview>
      <rect width="96" height="48" fill="red"/><rect x="110" width="80" height="60" fill="lime"/>
      <rect x="210" width="40" height="60" fill="blue"/></svg>''', encoding='utf-8')
    original = hashlib.sha256(source.read_bytes()).digest()
    checks = []
    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
    for pages, stem in [('all', 'pages'), ('2', 'single')]:
        result = subprocess.run([str(args.inkscape.resolve()), str(source), '--export-page=' + pages,
            '--export-dpi=144', '--export-filename=' + str(out / (stem + '.png'))],
            env=env, capture_output=True, timeout=180)
        (out / (stem + '.log')).write_bytes(result.stdout + result.stderr)
        check(stem + ': export succeeds', result.returncode == 0)
    expected = [('pages_p1.png', (144, 72), (255, 0, 0)), ('pages_p2.png', (120, 90), (0, 255, 0)),
                ('pages_p3.png', (60, 90), (0, 0, 255)), ('single.png', (120, 90), (0, 255, 0))]
    check('CLI filenames retain numeric page convention and single exact name',
          {p.name for p in out.glob('*.png')} == {name for name, _, _ in expected})
    for name, size, color in expected:
        path = out / name
        check(name + ': exists', path.exists())
        if path.exists():
            with Image.open(path) as image:
                check(name + ': own page size at 144 DPI', image.size == size)
                check(name + ': correct page pixels', image.convert('RGB').getpixel((size[0] // 2, size[1] // 2)) == color)
    check('source unchanged', hashlib.sha256(source.read_bytes()).digest() == original)
    (out / 'summary.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)


if __name__ == '__main__':
    main()
