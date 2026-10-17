"""Per-shape PDF gradient placement; synthetic inputs and canvas comparisons only."""
import argparse
import json
import os
from pathlib import Path
import subprocess

from PIL import Image
from pypdf import PdfReader


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--only-context', action='store_true', help='Run only the marker context-paint controls')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        folder = out / name.lower()
        folder.mkdir()
        env[name] = str(folder)
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    checks = []

    def run(label, command):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(f'{label}: exit {p.returncode}; see log')

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)

    definitions = '''<linearGradient id="g"><stop stop-color="#f00010"/>
      <stop offset="1" stop-color="#1000f0"/></linearGradient>
      <radialGradient id="r"><stop stop-color="#f00010"/>
      <stop offset="1" stop-color="#1000f0"/></radialGradient>
      <linearGradient id="d" x2="1" y2="1"><stop stop-color="#f00010"/>
      <stop offset="1" stop-color="#1000f0"/></linearGradient>
      <linearGradient id="u" gradientUnits="userSpaceOnUse" x1="20" x2="380">
      <stop stop-color="#f00010"/><stop offset="1" stop-color="#1000f0"/></linearGradient>
      <clipPath id="clip"><rect x="50" y="30" width="300" height="110"/></clipPath>'''
    rectangles = '<rect x="20" y="20" width="100" height="60" {}/>' \
                 '<rect x="180" y="100" width="200" height="60" {}/>'
    centers = [(70, 50), (280, 130)]
    cases = [
        ('shared-bbox', rectangles.format('fill="url(#g)"', 'fill="url(#g)"'), centers),
        ('inherited', '<g fill="url(#g)">' + rectangles.format('', '') + '</g>', centers),
        ('geometric-not-stroke-bounds', rectangles.format(
            'fill="url(#g)" stroke="black" stroke-width="16"', 'fill="url(#g)"'),
            [(40, 50), (100, 50), (200, 130), (360, 130)]),
        ('shared-radial', rectangles.format('fill="url(#r)"', 'fill="url(#r)"'),
            [(70, 50), (95, 50), (280, 130), (330, 130)]),
        ('diagonal-bbox', rectangles.format('fill="url(#d)"', 'fill="url(#d)"'),
            [(40, 40), (100, 60), (220, 120), (340, 140)]),
        ('user-space', rectangles.format('fill="url(#u)"', 'fill="url(#u)"'), centers),
        ('clipped', '<g clip-path="url(#clip)">' + rectangles.format(
            'fill="url(#g)"', 'fill="url(#g)"') + '</g>', centers),
        ('transformed', '<g transform="translate(15 0) skewX(6)">' + rectangles.format(
            'fill="url(#g)"', 'fill="url(#g)"') + '</g>', [(90, 50), (309, 130)]),
    ]
    if args.only_context:
        cases = []
    for name, body, points in cases:
        svg = out / (name + '.svg')
        svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="430" height="180" '
                       'viewBox="0 0 430 180"><defs>' + definitions + '</defs>' + body + '</svg>')
        canvas, pdf, rendered = (out / (name + ext) for ext in ('.png', '.pdf', '-pdf'))
        run(name + '-canvas', [args.inkscape.resolve(), svg, '--export-background=white',
                               f'--export-filename={canvas}'])
        run(name + '-pdf', [args.inkscape.resolve(), svg,
                            '--export-extension=org.inkscape.output.pdf.capypdf', f'--export-filename={pdf}'])
        run(name + '-render', [args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', pdf, rendered])
        with Image.open(canvas) as a, Image.open(rendered.with_suffix('.png')) as b:
            a, b = a.convert('RGB'), b.convert('RGB')
            deltas = [max(abs(x - y) for x, y in zip(a.getpixel(p), b.getpixel(p))) for p in points]
            check(name + ': canvas matches PDF', max(deltas) <= 4, {'points': points, 'deltas': deltas})
        if name == 'shared-bbox':
            # The fix must preserve distinct per-object matrices in the vector PDF,
            # rather than achieving a visual match by flattening the shapes.
            matrices = []
            def visit(resources):
                for pattern in resources.get('/Pattern', {}).values():
                    pattern = pattern.get_object()
                    if pattern.get('/PatternType') == 2:
                        matrices.append(tuple(pattern.get('/Matrix', [])))
                for form in resources.get('/XObject', {}).values():
                    form = form.get_object()
                    if form.get('/Subtype') == '/Form':
                        visit(form.get('/Resources', {}))
            visit(PdfReader(pdf).pages[0]['/Resources'])
            check('shared gradient has distinct vector placements',
                  (100, 0, 0, 60, 20, 20) in matrices and (200, 0, 0, 60, 180, 100) in matrices,
                  matrices)
    for kind in ('fill', 'stroke'):
        name = 'marker-context-' + kind
        source = out / (name + '.svg')
        stroke = 'stroke="url(#g)" stroke-width="0"' if kind == 'stroke' else ''
        source.write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="260" height="210"
          viewBox="-20 -80 260 210"><defs>
          <linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>
          <marker id="m" markerUnits="userSpaceOnUse" markerWidth="80" markerHeight="80"
            refX="0" refY="0" orient="0" overflow="visible">
          <rect y="-60" width="80" height="40" fill="context-''' + kind + '''"/>
          </marker></defs><path d="M0 0H200V100Z" fill="url(#g)" marker-start="url(#m)" '''
          + stroke + '/></svg>')
        pdf, rendered = out / (name + '.pdf'), out / (name + '-pdf')
        run(name + '-pdf', [args.inkscape.resolve(), source,
                            '--export-extension=org.inkscape.output.pdf.capypdf', f'--export-filename={pdf}'])
        run(name + '-render', [args.pdftoppm.resolve(), '-r', '96', '-singlefile', '-png', pdf, rendered])
        # Context paint uses the source object's 200-unit gradient, not the marker's
        # 80-unit bounds. The canvas has an older context-placement limitation, so
        # use the independently calculated ramp and same-x source probes here.
        with Image.open(rendered.with_suffix('.png')) as im:
            im = im.convert('RGB')
            colors = [im.getpixel(p) for p in ((40, 40), (80, 40), (40, 85), (80, 85))]
            expected = [(229, 0, 26), (178, 0, 77)] * 2
            delta = max(abs(a - b) for c, e in zip(colors, expected) for a, b in zip(c, e))
            check(name + ': preserves source gradient placement', delta <= 4, colors)
    (out / 'report.json').write_text(json.dumps(checks, indent=2))
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return 0 if passed == len(checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
