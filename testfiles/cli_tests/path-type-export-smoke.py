# SPDX-License-Identifier: GPL-2.0-or-later
"""Path text stays vector text in PDF and hides the same overflow as the canvas.

Uses independent synthetic SVG fixtures; no Illustrator or desktop input.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image, ImageChops, ImageFilter
from pypdf import PdfReader


def has_fonts(resources):
    return bool(resources.get('/Font')) or any(
        has_fonts(obj.get_object().get('/Resources', {}))
        for obj in resources.get('/XObject', {}).values()
        if obj.get_object().get('/Subtype') == '/Form')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inkscape', required=True, type=Path)
    parser.add_argument('--pdftoppm', required=True, type=Path)
    parser.add_argument('--dll-dir', type=Path)
    parser.add_argument('--data-dir', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    for key in ('INKSCAPE_PROFILE_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[key] = str((args.output / key.lower()).resolve())

    def run(command, log):
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=120)
        log.write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, (log, result.returncode)

    def mask(path):
        return Image.open(path).convert('L').point(lambda value: 255 if value < 200 else 0)

    # Name, geometry, text, active-end metadata, expected visible/PDF text.
    cases = [
        ('full', 'M20,40 h200', 'ABCD', '199.9404', 'ABCD'),
        ('short', 'M20,40 h10', 'WWWW', '9.997', 'W'),
        ('closed', 'M20,40 h20 v20 h-20 z', 'W' * 20, '10', 'W'),
        ('invalid-end', 'M20,40 h200', 'ABCD', 'bad', ''),
        ('edited-end', 'M20,40 h200', 'WWWW', '10', 'W'),
        ('svg-default', 'M20,40 h10', 'WWWW', None, 'W'),
    ]
    results = []
    for name, geometry, text, end, expected in cases:
        folder = args.output / name
        folder.mkdir(exist_ok=True)
        svg, png, pdf = (folder / ('input.svg'), folder / 'canvas.png', folder / 'output.pdf')
        active = '' if end is None else f' proof:path-start="0" proof:path-end="{end}"'
        svg.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xmlns:proof="urn:x-proof:document:1" width="260pt" height="100pt" viewBox="0 0 260 100">'
            f'<defs><path id="p" d="{geometry}"/></defs>'
            '<text style="font-family:sans-serif;font-size:16px" xml:space="preserve">'
            f'<textPath xlink:href="#p"{active}>{text}</textPath></text></svg>', encoding='utf-8')
        run([args.inkscape, svg, f'--export-filename={png}', '--export-dpi=144',
             '--export-background=white', '--export-background-opacity=1'], folder / 'canvas.log')
        run([args.inkscape, svg, f'--export-filename={pdf}'], folder / 'pdf.log')
        run([args.pdftoppm, '-f', '1', '-singlefile', '-r', '144', '-png', pdf, folder / 'page'],
            folder / 'render.log')
        reader = PdfReader(pdf, strict=True)
        assert len(reader.pages) == 1, name
        page = reader.pages[0]
        actual = ''.join(page.extract_text().split())
        assert actual == expected, (name, actual, expected)
        assert not page.images, name
        assert not expected or has_fonts(page['/Resources']), name
        canvas, exported = mask(png), mask(folder / 'page.png')
        assert canvas.size == exported.size, name
        ink = max(np.count_nonzero(np.asarray(canvas)), np.count_nonzero(np.asarray(exported)))
        unmatched = 0 if ink == 0 else max(
            np.count_nonzero(np.asarray(ImageChops.subtract(canvas, exported.filter(ImageFilter.MaxFilter(5))))),
            np.count_nonzero(np.asarray(ImageChops.subtract(exported, canvas.filter(ImageFilter.MaxFilter(5)))))) / ink
        assert bool(ink) == bool(expected) and unmatched <= 0.02, (name, ink, unmatched)
        results.append(dict(case=name, visible_text=expected, unmatched_ink=float(unmatched)))
    (args.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(f'{len(results)} path-text canvas/PDF cases passed: text, vector fonts and overflow')


if __name__ == '__main__':
    main()
