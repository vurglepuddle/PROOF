"""Opacity masks in the modern .ai writer: Clip on and off (the import's white backdrop), greys and
gradients, a stop's own opacity, a turned group with a mask in its mask's art, a clipping group, a
picture, a group with transparency of its own, a stroke for a mask, a translucent object and a hidden
one, in a CMYK document and an RGB one. Found on the way, in PDF output: a group with nothing of its
own is drawn in place, so that a Multiply in it blends with what is under it in Poppler's Cairo backend
too. Synthetic artwork only; sends no desktop input. Illustrator's own reading is qualified separately
with the generated control file.
"""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

SVG = '{http://www.w3.org/2000/svg}'
WIDTH, HEIGHT = 640, 272
SIZE = 108


def cell(i):
    """The top-left corner of the control's cell i, five to a row."""
    return 16 + (i % 5) * 124, 16 + (i // 5) * 124


# Each cell, by what its mask does; the box compared, a few pixels inside the cell.
NAMES = ['grey halves, Clip on', 'Clip off, as the import makes it', 'linear gradient',
         'radial gradient with a stop opacity', 'turned group with a mask in its mask',
         'masked clipping group', 'masked picture', 'group at 60% after its mask',
         'a stroke for a mask', 'translucent object, and a hidden one']
REGIONS = {name: (cell(i)[0] + 3, cell(i)[1] + 3, cell(i)[0] + SIZE - 3, cell(i)[1] + SIZE - 3)
           for i, name in enumerate(NAMES)}
MASKS = 12  # Masks written: one in each cell, a second in the turned group's, and the hidden object's.


def png_uri():
    # Drawn at its own size: renderers that smooth a scaled picture and those that don't agree on it.
    picture = Image.new('RGB', (SIZE, SIZE))
    picture.putdata([(255 - 2 * x, 2 * y, 2 * x + 30) for y in range(SIZE) for x in range(SIZE)])
    data = io.BytesIO()
    picture.save(data, 'PNG')
    return 'data:image/png;base64,' + base64.b64encode(data.getvalue()).decode()


def control(cmyk=True):
    """The control's SVG; also used for Illustrator qualification."""
    def ink(c, m, y, k, rgb):
        return f'device-cmyk({c} {m} {y} {k})' if cmyk else rgb

    blue, red, green = ink(1, 0.7, 0, 0, '#1f4fa8'), ink(0, 0.9, 0.8, 0, '#e0302a'), ink(0.8, 0, 1, 0, '#2aa13a')
    purple, orange, black = ink(0.6, 0.9, 0, 0, '#7a2a9a'), ink(0, 0.5, 1, 0, '#f08c00'), ink(0, 0, 0, 1, '#1a1a1a')
    yellow = ink(0, 0, 1, 0, '#ffe600')
    # A mask's half: K in the CMYK document, as a designer there picks a grey.
    half = ink(0, 0, 0, 0.5, '#808080')
    defs, art = [], []
    x, y = cell(0)
    defs.append(f'<mask id="m0" maskUnits="userSpaceOnUse"><rect x="{x}" y="{y}" width="54" height="88" '
                f'style="fill:{half}"/><rect x="{x + 54}" y="{y}" width="54" height="88" style="fill:#ffffff"/></mask>')
    art.append(f'<rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" style="fill:{blue}" mask="url(#m0)"/>')
    x, y = cell(1)
    # The import's form of a mask with Clip off: a white backdrop under the art, in a wrapper.
    defs.append(f'<mask id="m1" maskUnits="userSpaceOnUse" x="{x - 10}" y="{y - 10}" width="{SIZE + 20}" '
                f'height="{SIZE + 20}" proof:ai-mask-clipping="false"><rect x="{x - 10}" y="{y - 10}" width="{SIZE + 20}" '
                f'height="{SIZE + 20}" style="fill:#ffffff" proof:mask-backdrop="white"/>'
                f'<rect x="{x}" y="{y}" width="{SIZE}" height="40" style="fill:#000000"/>'
                f'<rect x="{x}" y="{y + 40}" width="54" height="30" style="fill:#4d4d4d"/></mask>')
    art.append(f'<g mask="url(#m1)"><rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" style="fill:{red}"/></g>')
    x, y = cell(2)
    defs.append(f'<linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="{x}" y1="0" x2="{x + SIZE}" y2="0">'
                '<stop offset="0" style="stop-color:#000000"/><stop offset="1" style="stop-color:#ffffff"/></linearGradient>'
                f'<mask id="m2" maskUnits="userSpaceOnUse"><rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" '
                'style="fill:url(#fade)"/></mask>')
    art.append(f'<rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" style="fill:{green}" mask="url(#m2)"/>')
    x, y = cell(3)
    cx, cy = x + SIZE / 2, y + SIZE / 2
    defs.append(f'<radialGradient id="glow" gradientUnits="userSpaceOnUse" cx="{cx}" cy="{cy}" r="54">'
                '<stop offset="0" style="stop-color:#ffffff"/><stop offset="0.5" style="stop-color:#ffffff;stop-opacity:0.7"/>'
                '<stop offset="1" style="stop-color:#000000"/></radialGradient>'
                f'<mask id="m3" maskUnits="userSpaceOnUse"><circle cx="{cx}" cy="{cy}" r="54" style="fill:url(#glow)"/></mask>')
    art.append(f'<rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" style="fill:{purple}" mask="url(#m3)"/>')
    x, y = cell(4)
    defs.append('<mask id="inner" maskUnits="userSpaceOnUse"><circle cx="0" cy="-8" r="34" style="fill:#4d4d4d"/></mask>'
                '<mask id="m4" maskUnits="userSpaceOnUse"><rect x="-45" y="-45" width="90" height="70" '
                'style="fill:#ffffff" mask="url(#inner)"/><rect x="-45" y="25" width="90" height="20" style="fill:#ffffff"/></mask>')
    art.append(f'<g transform="translate({x + SIZE / 2} {y + SIZE / 2}) rotate(15)" mask="url(#m4)">'
               f'<rect x="-46" y="-46" width="92" height="92" style="fill:{orange}"/>'
               f'<rect x="-15" y="-15" width="30" height="30" style="fill:{black}"/></g>')
    x, y = cell(5)
    defs.append(f'<clipPath id="c5"><circle cx="{x + 54}" cy="{y + 54}" r="52"/></clipPath>'
                f'<mask id="m5" maskUnits="userSpaceOnUse"><rect x="{x}" y="{y}" width="{SIZE}" height="54" style="fill:#ffffff"/>'
                f'<rect x="{x}" y="{y + 54}" width="{SIZE}" height="54" style="fill:#b3b3b3"/></mask>')
    art.append(f'<g clip-path="url(#c5)" mask="url(#m5)"><rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" '
               f'style="fill:{blue}"/><rect x="{x + 34}" y="{y + 34}" width="40" height="40" style="fill:{yellow}"/></g>')
    x, y = cell(6)
    defs.append(f'<mask id="m6" maskUnits="userSpaceOnUse"><circle cx="{x + 54}" cy="{y + 54}" r="48" style="fill:#ffffff"/>'
                f'<rect x="{x}" y="{y + 70}" width="{SIZE}" height="38" style="fill:#808080"/></mask>')
    art.append(f'<image x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" preserveAspectRatio="none" '
               f'href="{png_uri()}" mask="url(#m6)"/>')
    x, y = cell(7)
    defs.append(f'<mask id="m7" maskUnits="userSpaceOnUse"><rect x="{x}" y="{y}" width="54" height="{SIZE}" style="fill:#ffffff"/>'
                f'<rect x="{x + 54}" y="{y}" width="54" height="{SIZE}" style="fill:#808080"/></mask>')
    art.append(f'<g style="opacity:0.6" mask="url(#m7)"><rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" '
               f'style="fill:{black}"/><rect x="{x + 20}" y="{y + 20}" width="40" height="40" style="fill:{red}"/></g>')
    x, y = cell(8)
    defs.append(f'<mask id="m8" maskUnits="userSpaceOnUse"><circle cx="{x + 54}" cy="{y + 54}" r="36" '
                'style="fill:none;stroke:#ffffff;stroke-width:24"/></mask>')
    art.append(f'<rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" style="fill:{green}" mask="url(#m8)"/>')
    x, y = cell(9)
    defs.append(f'<mask id="m9" maskUnits="userSpaceOnUse"><ellipse cx="{x + 54}" cy="{y + 54}" rx="54" ry="34" '
                'style="fill:#e6e6e6"/></mask>'
                f'<mask id="m10" maskUnits="userSpaceOnUse"><rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" style="fill:#ffffff"/></mask>')
    art.append(f'<rect x="{x}" y="{y}" width="{SIZE}" height="{SIZE}" style="fill:{orange};opacity:0.7" mask="url(#m9)"/>'
               f'<rect x="{x + 20}" y="{y + 20}" width="60" height="60" style="fill:{black};display:none" mask="url(#m10)"/>')
    mode = ' proof:color-mode="CMYK"' if cmyk else ''
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            f'xmlns:proof="urn:x-proof:document:1" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}"{mode}>'
            f'<defs>{"".join(defs)}</defs><g inkscape:groupmode="layer" inkscape:label="Masks">{"".join(art)}</g></svg>')


# PDF output found on the way: Multiply in groups with nothing of their own, over a coloured band.
GROUP_REGIONS = {
    'multiply in a layer': (24, 24, 76, 76),
    'multiply in a plain group': (104, 24, 156, 76),
    'multiply in a clipped group': (184, 24, 236, 76),
    'multiply two plain groups deep': (264, 24, 316, 76),
    'a layer in a layer': (326, 85, 334, 93),
}


def group_control():
    shade = 'fill:#7fa7d9;mix-blend-mode:multiply'
    art = ''.join([
        '<rect x="0" y="40" width="340" height="20" style="fill:#f0c020"/>',
        f'<rect x="20" y="20" width="60" height="60" style="{shade}"/>',
        f'<g><rect x="100" y="20" width="60" height="60" style="{shade}"/></g>',
        f'<g clip-path="url(#c)"><rect x="170" y="10" width="80" height="80" style="{shade}"/></g>',
        f'<g><g><g style="mix-blend-mode:multiply"><rect x="260" y="20" width="60" height="60" style="fill:#7fa7d9"/></g></g></g>',
        # A layer in the layer: its marked content may not nest in the other's in one stream.
        '<g inkscape:groupmode="layer" inkscape:label="Inner"><rect x="325" y="84" width="10" height="10" style="fill:#333333"/></g>',
    ])
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            'width="340" height="100" viewBox="0 0 340 100"><defs><clipPath id="c"><rect x="180" y="20" width="60" '
            f'height="60"/></clipPath></defs><g inkscape:groupmode="layer" inkscape:label="Band">{art}</g></svg>')


def pixels(png):
    with Image.open(png) as im:
        return np.asarray(im.convert('RGB'), dtype=np.int16)


def compare(a_png, b_png, scale=1.0, reach=3, regions=None):
    """Per region: the largest channel difference, the mean, the share of pixels more than 24 apart,
    and the shift of b (in pixels, within `reach`) that matches a best."""
    a, b = pixels(a_png), pixels(b_png)
    out = {}
    for name, (x0, y0, x1, y1) in (regions or REGIONS).items():
        x0, y0, x1, y1 = (round(v * scale) for v in (x0, y0, x1, y1))
        here = a[y0:y1, x0:x1]
        # Unshifted unless a shift matches better: an even ramp matches as well shifted along it.
        best = (float(np.abs(here - b[y0:y1, x0:x1]).mean()), 0, 0)
        for dy in range(-reach, reach + 1):
            for dx in range(-reach, reach + 1):
                there = b[y0 + dy:y1 + dy, x0 + dx:x1 + dx]
                mean = float(np.abs(here - there).mean())
                if mean < best[0] - 1e-9:
                    best = (mean, dx, dy)
        diff = np.abs(here - b[y0:y1, x0:x1]).max(axis=2)
        out[name] = dict(max=int(diff.max()), over24=round(float((diff > 24).mean()), 4),
                         mean=round(float(diff.mean()), 3), shift=[best[1], best[2]])
    return out


def soft_masks(pdf):
    """How many luminosity soft masks the PDF's first page uses, through its forms' resources."""
    from pypdf import PdfReader
    seen = set()

    def walk(resources):
        resources = resources.get_object() if resources is not None else {}
        count = 0
        for state in (resources.get('/ExtGState') or {}).values():
            reference = state.get_object().get('/SMask')
            if reference is None or reference == '/None':
                continue
            key = getattr(reference, 'idnum', None)
            mask = reference.get_object()
            if mask.get('/S') == '/Luminosity' and (key is None or key not in seen):
                seen.add(key)
                count += 1 + walk(mask['/G'].get_object().get('/Resources'))
        for xobject in (resources.get('/XObject') or {}).values():
            xobject = xobject.get_object()
            if xobject.get('/Subtype') == '/Form':
                count += walk(xobject.get('/Resources'))
        return count

    return walk(PdfReader(pdf, strict=True).pages[0]['/Resources'])


def mask_art_lines(records):
    """Every line of every mask's art: between X= and X+ in the mask dictionaries."""
    lines, inside = [], 0
    for line in records.split(b'\n'):
        if line == b'%_X=':
            inside += 1
        elif line == b'%_X+':
            inside -= 1
        elif inside:
            lines.append(line)
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inkscape', required=True, type=Path)
    ap.add_argument('--dll-dir', required=True, type=Path)
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--pdftoppm', type=Path)
    ap.add_argument('--pdftocairo', type=Path, help="default: beside --pdftoppm")
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        (out / name.lower()).mkdir()
        env[name] = str(out / name.lower())
    env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    exe = args.inkscape.resolve()
    native = '--export-extension=org.inkscape.output.ai.native'
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
        """The records the .ai file keeps beside its PDF page, Zstandard as Illustrator's."""
        from pypdf import PdfReader
        private = PdfReader(ai, strict=True).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        marker = b'%AI24_ZStandard_Data'
        if not packed.startswith(marker):
            raise RuntimeError(f'{ai.name}: missing Zstandard records')
        return subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'], input=packed[len(marker):],
                              capture_output=True, timeout=60, check=True).stdout

    def masks_in(svg):
        root = ET.parse(svg).getroot()
        return sum(1 for _ in root.iter(SVG + 'mask'))

    def opens_natively(p):
        return b'opens from its PDF page' not in p.stdout + p.stderr

    def draw(label, svg):
        png = out / f'{label}.png'
        run(label + '-draw', exe, svg, '--export-background=white', f'--export-filename={png}')
        return png

    source = out / 'control.svg'
    source.write_text(control(), encoding='utf-8')
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    canvas = draw('canvas', source)
    with Image.open(canvas) as im:
        im = im.convert('RGB')
        x, y = cell(0)
        # The first cell: blue through white, lighter through the half, paper below its mask.
        full, part, none = im.getpixel((x + 80, y + 40)), im.getpixel((x + 25, y + 40)), im.getpixel((x + 54, y + 100))
        check('the control draws its first mask', full[2] > full[0] + 60 and part[0] > full[0] + 40 and none == (255, 255, 255),
              [full, part, none])

    ai = out / 'modern.ai'
    run('save', exe, source, native, f'--export-filename={ai}')
    records = records_of(ai)
    (out / 'records.txt').write_bytes(records)
    dictionaries = records.count(b'%_/Mask :\n')
    check(f'{MASKS} masks, each in its object\'s dictionary, linked and not inverted',
          dictionaries == MASKS and records.count(b'%_1 /Bool (Linked) ,\n') == MASKS
          and records.count(b'%_0 /Bool (Inverted) ,\n') == MASKS, dictionaries)
    check('Clip off for the import\'s white backdrop alone, on for the rest',
          records.count(b'%_0 /Bool (Clipping) ,\n') == 1 and records.count(b'%_1 /Bool (Clipping) ,\n') == MASKS - 1)
    check('a mask in a mask\'s art is on hidden lines once', b'%_%_' not in records)
    art = mask_art_lines(records)
    painted = [line for line in art if re.match(rb'^%_.* (k|K|Xa|XA|x|X|Xx|XX)$', line)]
    greys = [line for line in art if re.match(rb'^%_[\d.]+ (g|G)$', line)]
    check("every colour in the masks' art is Gray, in a CMYK document", not painted and len(greys) >= 8,
          (len(greys), [line.decode() for line in painted[:4]]))
    stops = re.findall(rb'^[\d.]+ 0 (?:[\d.]+ 6 )?50 [\d.]+ %_Bs$', records, re.M)
    check("the masks' gradients have Gray stops", len(stops) == 5, len(stops))
    check('the hidden object\'s mask art is shown', b'%_X=\n%_0 Xw\n%_u\n' in records)

    reopened = out / 'modern.svg'
    p = run('reopen', exe, ai, f'--export-filename={reopened}')
    check('reopens natively', opens_natively(p))
    check(f'all {MASKS} masks come back', masks_in(reopened) == MASKS, masks_in(reopened))
    differences = compare(canvas, draw('modern', reopened))
    (out / 'reopened-differences.json').write_text(json.dumps(differences, indent=2), encoding='utf-8')
    check('every mask reopens drawn as it was',
          all(d['shift'] == [0, 0] and d['max'] <= 3 for d in differences.values()), differences)

    found = soft_masks(ai)
    # Each mask drawn (not the hidden object's), and one more for the stop's opacity in a mask's gradient.
    check('the PDF page has a luminosity soft mask for each mask drawn', found == MASKS - 1 + 1, found)

    again, again_svg = out / 'again.ai', out / 'again.svg'
    run('again', exe, reopened, native, f'--export-filename={again}')
    second = records_of(again)
    check('a second write keeps the same masks', second.count(b'%_/Mask :\n') == MASKS
          and second.count(b'%_0 /Bool (Clipping) ,\n') == 1, second.count(b'%_/Mask :\n'))
    p = run('again-reopen', exe, again, f'--export-filename={again_svg}')
    differences = compare(canvas, draw('again', again_svg))
    check('the second write opens natively and draws the same', opens_natively(p)
          and all(d['shift'] == [0, 0] and d['max'] <= 3 for d in differences.values()), differences)

    rgb = out / 'control-rgb.svg'
    rgb.write_text(control(cmyk=False), encoding='utf-8')
    rgb_ai, rgb_svg = out / 'rgb.ai', out / 'rgb.svg'
    run('rgb-save', exe, rgb, native, f'--export-filename={rgb_ai}')
    rgb_records = records_of(rgb_ai)
    neutral = re.findall(rb'^[-\d. ]+ ([\d.]+) \1 \1 2 (?:[\d.]+ 6 )?50 [\d.]+ %_Bs$', rgb_records, re.M)
    check("in an RGB document the masks' gradients have neutral RGB stops (Illustrator draws Gray ones "
          "through its Gray working space there)", len(neutral) == 5, len(neutral))
    p = run('rgb-reopen', exe, rgb_ai, f'--export-filename={rgb_svg}')
    rgb_canvas = draw('rgb-canvas', rgb)
    differences = compare(rgb_canvas, draw('rgb', rgb_svg))
    check('in an RGB document too: reopens natively, drawn as it was', opens_natively(p)
          and all(d['shift'] == [0, 0] and d['max'] <= 3 for d in differences.values()), differences)
    cairo = args.pdftocairo or (args.pdftoppm.with_name('pdftocairo.exe') if args.pdftoppm else None)
    if cairo and cairo.exists():
        # In RGB, where Poppler and the canvas agree on colour (they convert CMYK each its own way).
        run('pdf-cairo', cairo, '-scale-to-x', WIDTH, '-scale-to-y', HEIGHT, '-png', '-singlefile', rgb_ai, out / 'pdf-cairo')
        drawn = compare(rgb_canvas, out / 'pdf-cairo.png')
        (out / 'pdf-differences.json').write_text(json.dumps(drawn, indent=2), encoding='utf-8')
        # Edges only: a turned mask's in a mask is antialiased a little differently.
        check('the PDF page draws every mask as the canvas does',
              all(d['shift'] == [0, 0] and d['mean'] <= 2 and d['over24'] <= 0.02 for d in drawn.values()), drawn)
        # Poppler's Cairo backend starts every group from nothing: a group with nothing of its own is
        # drawn in place, or a Multiply in it would come out flat over the band.
        groups = out / 'groups.svg'
        groups.write_text(group_control(), encoding='utf-8')
        run('groups-pdf', exe, groups, f'--export-filename={out / "groups.pdf"}')
        run('groups-page', cairo, '-scale-to-x', 340, '-scale-to-y', 100, '-png', '-singlefile', out / 'groups.pdf',
            out / 'groups-page')
        drawn = compare(draw('groups', groups), out / 'groups-page.png', regions=GROUP_REGIONS)
        check('PDF output: a Multiply in a plain, clipped or nested group blends with what is under it; '
              'a layer in a layer is written', all(d['shift'] == [0, 0] and d['max'] <= 3 for d in drawn.values()), drawn)
        p = run('groups-ai', exe, groups, native, f'--export-filename={out / "groups.ai"}', ok=False)
        check('nested layers save as native .ai', p.returncode == 0 and (out / 'groups.ai').exists(),
              (p.stdout + p.stderr).decode('utf-8', 'replace')[-300:])

    legacy = out / 'legacy7.ai'
    legacy.write_bytes(b'before')
    p = run('legacy7', exe, source, '--export-extension=org.inkscape.output.ai.legacy7',
            f'--export-filename={legacy}', ok=False)
    check('the Illustrator 7 copy refuses masks by name without replacing a file',
          b'opacity masks (Illustrator 7)' in p.stdout + p.stderr and legacy.read_bytes() == b'before')
    refusals = [
        ('colour', '<mask id="bad"><rect width="50" height="50" style="fill:#ff0000"/></mask>',
         '<rect width="100" height="100" mask="url(#bad)"/>', 'colours other than grey in opacity masks'),
        ('pattern', '<pattern id="p" patternUnits="userSpaceOnUse" width="4" height="4"><rect width="2" height="2"/></pattern>'
         '<mask id="bad"><rect width="50" height="50" style="fill:url(#p)"/></mask>',
         '<rect width="100" height="100" mask="url(#bad)"/>', 'patterns in opacity masks'),
        ('layer', '<mask id="bad"><rect width="50" height="50" style="fill:#ffffff"/></mask>',
         '<g inkscape:groupmode="layer" mask="url(#bad)"><rect width="100" height="100"/></g>', 'opacity masks on layers'),
    ]
    for name, defs, art, reason in refusals:
        bad = out / f'refused-{name}.svg'
        bad.write_text('<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
                       f'width="100" height="100"><defs>{defs}</defs>{art}</svg>', encoding='utf-8')
        before = ai.read_bytes()
        p = run('refused-' + name, exe, bad, native, f'--export-filename={ai}', ok=False)
        check(name + ': refused by name without replacing an existing file',
              reason.encode() in p.stdout + p.stderr and ai.read_bytes() == before)
    check('working SVG unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)
    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
