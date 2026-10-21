"""Profile-tagged standard RGB gradient stops in both .ai writers, including repeated saves.
Synthetic controls only; no desktop input. Illustrator qualification is a separate gate.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageCms

SVG = '{http://www.w3.org/2000/svg}'
XLINK = '{http://www.w3.org/1999/xlink}'
WIDTH, HEIGHT = 640, 380
COLOURS = ((.2, .4, .6), (.8, .1, .3), (.1, .75, .35))


def profile(wrong=False):
    """Same sRGB colour data, deliberately different description and creation date.
    The negative control instead changes the shared RGB transfer curve."""
    data = bytearray(ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes())
    old, new = 'sRGB built-in'.encode('utf-16-be'), 'RGB test data'.encode('utf-16-be')
    assert len(old) == len(new) and data.count(old) == 1
    data = data.replace(old, new)
    struct.pack_into('>6H', data, 24, 2001, 2, 3, 4, 5, 6)
    data[84:100] = bytes(16)
    if wrong:
        for i in range(struct.unpack_from('>I', data, 128)[0]):
            tag, offset, size = struct.unpack_from('>4sII', data, 132 + 12 * i)
            if tag == b'rTRC':
                assert data[offset:offset + 4] == b'para' and size >= 16
                struct.pack_into('>I', data, offset + 12, 2 * 65536)
                break
        else:
            raise RuntimeError('standard sRGB profile has no RGB transfer curve')
    return bytes(data)


def control(modern=False, wrong=False, icc=None):
    def stops(fade=False):
        return ''.join(f'<stop offset="{offset}" style="stop-color:rgb({100*r}% {100*g}% {100*b}%);'
                       f'stop-opacity:{.25 if fade and i == 2 else 1}"/>'
                       for i, (offset, (r, g, b)) in enumerate(zip((0, .45, 1), COLOURS)))

    defs = []
    for i, x in enumerate((20, 230, 440)):
        defs.append(f'<linearGradient id="l{i}" gradientUnits="userSpaceOnUse" x1="{x}" y1="0" '
                    f'x2="{x+180}" y2="0">{stops()}</linearGradient>')
    for i, x in enumerate((110, 320, 530)):
        defs.append(f'<radialGradient id="r{i}" gradientUnits="userSpaceOnUse" cx="{x}" cy="180" '
                    f'r="60" fx="{x-20 if i == 1 else x}" fy="{160 if i == 1 else 180}">'
                    f'{stops()}</radialGradient>')
    defs.extend([
        f'<linearGradient id="shear" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="140" y2="0">{stops()}</linearGradient>',
        f'<linearGradient id="clip" gradientUnits="userSpaceOnUse" x1="260" y1="0" x2="390" y2="0">{stops()}</linearGradient>',
        '<clipPath id="hole"><path d="M260 280 H390 V350 H260 Z M295 300 V330 H360 V300 Z"/></clipPath>',
    ])
    art = [
        '<rect x="20" y="20" width="180" height="70" fill="url(#l0)"/>',
        '<rect x="230" y="20" width="180" height="70" fill="url(#l1)" stroke="#222222" stroke-width="4"/>',
        '<path d="M440 20 H620 V90 H440 Z M500 35 H560 V75 H500 Z" fill-rule="evenodd" fill="url(#l2)"/>',
        *(f'<circle cx="{x}" cy="180" r="60" fill="url(#r{i})"/>' for i, x in enumerate((110, 320, 530))),
        '<g transform="matrix(1 .15 .3 1 20 275)"><rect width="140" height="50" fill="url(#shear)"/></g>',
        '<g clip-path="url(#hole)"><rect x="250" y="270" width="160" height="90" fill="url(#clip)"/></g>',
    ]
    if modern:
        defs.append(f'<linearGradient id="stroke" gradientUnits="userSpaceOnUse" x1="420" y1="0" x2="620" y2="0">'
                    f'{stops(True)}</linearGradient>')
        art.append('<path d="M430 285 H610 V340" style="fill:none;stroke:url(#stroke);stroke-width:10;opacity:.7"/>')
    else:
        art.append('<rect x="440" y="285" width="180" height="55" fill="#334477"/>')
    uri = 'data:application/vnd.iccprofile;base64,' + base64.b64encode(icc if icc is not None and not wrong else profile(wrong)).decode()
    text = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xmlns:proof="urn:x-proof:document:1" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
            f'width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" '
            'proof:color-mode="RGB" proof:color-profile="ManagedRGB"><defs>'
            f'<color-profile name="ManagedRGB" rendering-intent="relative-colorimetric" xlink:href="{uri}"/>'
            + ''.join(defs) + '</defs><g inkscape:groupmode="layer" inkscape:label="RGB gradients">'
            + ''.join(art) + '</g></svg>')
    regions = {
        'linear': [(30, 50), (100, 50), (190, 50)],
        'filled and stroked': [(231, 50), (240, 50), (310, 50), (400, 50)],
        'compound hole': [(450, 50), (530, 55), (610, 50)],
        # Cairo and Poppler sample opposite sides of the exact radial centre.
        # Use interior ramp points, where a half-pixel offset stays within three levels.
        'radial': [(120, 180), (140, 180), (165, 180)],
        'radial with a focal point': [(300, 165), (320, 180), (370, 180)],
        'another radial': [(540, 180), (550, 180), (580, 180)],
        'sheared linear': [(46, 298), (149, 323)],
        'compound clip': [(270, 290), (325, 315), (380, 340), (250, 315)],
        'gradient stroke with opacity' if modern else 'ordinary RGB paint':
            [(450, 285), (520, 285), (600, 285), (610, 320)] if modern else [(450, 300), (600, 320)],
    }
    return text, regions


def probe(a_png, b_png, regions, scale=1):
    with Image.open(a_png) as a, Image.open(b_png) as b:
        a, b = a.convert('RGB'), b.convert('RGB')
        return {name: max(max(abs(x-y) for x, y in zip(a.getpixel((round(q[0]*scale), round(q[1]*scale))),
                                                      b.getpixel((round(q[0]*scale), round(q[1]*scale)))))
                         for q in points) for name, points in regions.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('inkscape', 'output'):
        ap.add_argument('--' + name, required=True, type=Path)
    for name in ('dll-dir', 'data-dir', 'pdftoppm'):
        ap.add_argument('--' + name, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C', INKSCAPE_PROFILE_DIR=str(out / 'profile'))
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        (out / name.lower()).mkdir()
        env[name] = str(out / name.lower())
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    exe, checks = args.inkscape.resolve(), []

    def check(name, passed, detail=''):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name + (f' {detail}' if detail != '' else ''), flush=True)

    def run(label, *command, ok=True):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        if ok and p.returncode:
            raise RuntimeError(f'{label}: exit {p.returncode}; see log')
        return p

    def records(ai, legacy):
        if legacy:
            return ai.read_bytes()
        from pypdf import PdfReader
        private = PdfReader(ai, strict=True).pages[0]['/PieceInfo']['/Illustrator']['/Private']
        packed = b''.join(private[f'/AIPrivateData{i}'].get_data() for i in range(1, private['/NumBlock'] + 1))
        marker = b'%AI24_ZStandard_Data'
        assert packed.startswith(marker)
        return subprocess.run([str(args.dll_dir.resolve() / 'zstd.exe'), '-d', '-c'], input=packed[len(marker):],
                              capture_output=True, timeout=60, check=True).stdout

    cases = [('modern', 'native', False, None), ('legacy7', 'legacy7', False, None), ('stroke', 'native', True, None)]
    # Independent system-supplied IEC v2 control, read only. The writer itself
    # generates its reference colour definition and needs no system ICC file.
    system_root = os.environ.get('SystemRoot')
    classic = Path(system_root) / 'System32/spool/drivers/color/sRGB Color Space Profile.icm' if system_root else None
    if classic and classic.is_file():
        cases += [('classic', 'native', False, classic.read_bytes()), ('classic-legacy7', 'legacy7', False, classic.read_bytes())]
    for route, extension, variant, icc in cases:
        option = '--export-extension=org.inkscape.output.ai.' + extension
        source, ai = out / f'{route}-source.svg', out / f'{route}.ai'
        text, regions = control(variant, icc=icc)
        source.write_text(text, encoding='utf-8')
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        run(route + '-save', exe, source, option, f'--export-filename={ai}')
        raw = records(ai, extension == 'legacy7')
        lines = [list(map(float, line[:-5].split())) for line in raw.splitlines() if line.endswith(b' %_Bs')]
        check(route + ': every process stop is RGB with its original channels', len(lines) >= 24
              and all(len(v) in (10, 12) and v[7] == 2 and tuple(v[4:7]) in COLOURS for v in lines))
        reopened = out / f'{route}.svg'
        p = run(route + '-reopen', exe, ai, f'--export-filename={reopened}')
        check(route + ': reopens natively', b'opens from its PDF page' not in p.stdout + p.stderr)
        if extension == 'native':
            profiles = list(ET.parse(reopened).getroot().iter(SVG + 'color-profile'))
            check(route + ': profile kept byte for byte', len(profiles) == 1
                  and base64.b64decode(profiles[0].get(XLINK + 'href').split('base64,', 1)[1]) == (icc if icc is not None else profile()))
        for label, svg in [('canvas', source), ('reopened', reopened)]:
            run(route + '-' + label, exe, svg, '--export-background=white', f'--export-filename={out / (route + "-" + label + ".png")}')
        differences = probe(out / f'{route}-canvas.png', out / f'{route}-reopened.png', regions)
        check(route + ': every sampled colour and hole agrees within three levels', max(differences.values()) <= 3, differences)
        if args.pdftoppm and extension == 'native':
            run(route + '-pdf', args.pdftoppm, '-scale-to-x', WIDTH, '-scale-to-y', HEIGHT, '-png', '-singlefile', ai, out / (route + '-pdf'))
            differences = probe(out / f'{route}-canvas.png', out / f'{route}-pdf.png', regions)
            check(route + ': PDF page agrees within three levels', max(differences.values()) <= 3, differences)
        again, again_svg, third = out / f'{route}-again.ai', out / f'{route}-again.svg', out / f'{route}-third.ai'
        run(route + '-again', exe, reopened, option, f'--export-filename={again}')
        p = run(route + '-again-reopen', exe, again, f'--export-filename={again_svg}')
        check(route + ': second write keeps profile-tagged stops and opens natively', again.exists()
              and b'opens from its PDF page' not in p.stdout + p.stderr)
        run(route + '-third', exe, again_svg, option, f'--export-filename={third}')
        run(route + '-third-png', exe, third, '--export-background=white', f'--export-filename={out / (route + "-third.png")}')
        differences = probe(out / f'{route}-canvas.png', out / f'{route}-third.png', regions)
        check(route + ': third write still draws the same', max(differences.values()) <= 3, differences)
        bad = out / f'{route}-other-profile.svg'
        bad.write_text(control(variant, True)[0], encoding='utf-8')
        before = ai.read_bytes()
        p = run(route + '-refuse', exe, bad, option, f'--export-filename={ai}', ok=False)
        check(route + ': altered RGB profile refused without replacing the file', ai.read_bytes() == before
              and b'gradient colours other than standard RGB or CMYK' in p.stdout + p.stderr)
        check(route + ': source unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)
    (out / 'report.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    passed = sum(c['passed'] for c in checks)
    print(f'{passed}/{len(checks)} checks passed. {out / "report.json"}')
    return int(passed != len(checks))


if __name__ == '__main__':
    raise SystemExit(main())
