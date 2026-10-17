#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify a saved, UI-edited mesh object reaches the same new edge in SVG and PDF.

Use the disposable edited SVG produced by tools/mesh-object-check.py. Sends no input.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageChops, ImageFilter
from pypdf import PdfReader


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--svg', type=Path, required=True)
    parser.add_argument('--inkscape', type=Path, required=True)
    parser.add_argument('--pdftoppm', type=Path, required=True)
    parser.add_argument('--dll-dir', type=Path)
    parser.add_argument('--data-dir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.output / 'edited-copy.svg'
    shutil.copy2(args.svg, source)
    assert hashlib.sha256(source.read_bytes()).digest() == hashlib.sha256(args.svg.read_bytes()).digest()
    root = ET.parse(source).getroot()
    shape = next(node for node in root.iter() if node.get('id') == 'rectangle')
    assert shape.get('{urn:x-proof:document:1}mesh-object') == 'true'
    env = os.environ.copy()
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    for name in ('INKSCAPE_PROFILE_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[name] = str((args.output / name.lower()).resolve())

    def run(command, name):
        result = subprocess.run([str(x) for x in command], env=env, capture_output=True, timeout=120)
        (args.output / name).write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, (name, result.returncode)

    png, pdf = args.output / 'canvas.png', args.output / 'output.pdf'
    run([args.inkscape, source, f'--export-filename={png}', '--export-area-page',
         '--export-dpi=96', '--export-background=white', '--export-background-opacity=1'], 'canvas.log')
    run([args.inkscape, source, f'--export-filename={pdf}', '--export-area-page'], 'pdf.log')
    run([args.pdftoppm, '-singlefile', '-f', '1', '-r', '96', '-png', pdf, args.output / 'page'],
        'render.log')
    assert len(PdfReader(pdf, strict=True).pages) == 1
    canvas = Image.open(png).convert('RGB')
    exported = Image.open(args.output / 'page.png').convert('RGB')
    assert canvas.size == exported.size == (720, 500), (canvas.size, exported.size)
    pixels = {}
    for name, picture in [('canvas', canvas), ('pdf', exported)]:
        rgb = picture.getpixel((305, 100))
        assert min(rgb) < 220 and max(rgb) - min(rgb) > 20, (name, rgb)
        pixels[name] = rgb
    def ink_mask(picture):
        return picture.convert('L').point(lambda value: 255 if value < 245 else 0)
    a, b = ink_mask(canvas), ink_mask(exported)
    total = max(np.count_nonzero(np.asarray(a)), np.count_nonzero(np.asarray(b)))
    missing_a = np.count_nonzero(np.asarray(ImageChops.subtract(a, b.filter(ImageFilter.MaxFilter(3)))))
    missing_b = np.count_nonzero(np.asarray(ImageChops.subtract(b, a.filter(ImageFilter.MaxFilter(3)))))
    unmatched = max(missing_a, missing_b) / total
    assert unmatched <= .02, unmatched
    result = dict(checks=5, copied_input_hash_verified=True, marker_preserved=True,
                  canvas_expanded=True, pdf_expanded=True, outside_original_rectangle_rgb=pixels,
                  unmatched_ink=float(unmatched))
    (args.output / 'summary.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('5/5 mesh-object SVG/PDF checks passed')


if __name__ == '__main__':
    main()
