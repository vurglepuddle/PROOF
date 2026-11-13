"""Embedded picture profiles in PDF and sRGB canvas pixels; synthetic fixtures only."""
import argparse
import base64
import ctypes as C
import io
import json
import os
from pathlib import Path
import struct
import subprocess
from PIL import Image, ImageCms
from pypdf import PdfReader


def profiles(dll):
    lib = C.CDLL(str(dll))
    class xyY(C.Structure):
        _fields_ = [('x', C.c_double), ('y', C.c_double), ('Y', C.c_double)]
    class Primaries(C.Structure):
        _fields_ = [('r', xyY), ('g', xyY), ('b', xyY)]
    def api(name, result, *args):
        f = getattr(lib, name)
        f.restype, f.argtypes = result, args
        return f
    gamma = api('cmsBuildGamma', C.c_void_p, C.c_void_p, C.c_double)
    rgb = api('cmsCreateRGBProfile', C.c_void_p, C.POINTER(xyY), C.POINTER(Primaries), C.POINTER(C.c_void_p))
    gray = api('cmsCreateGrayProfile', C.c_void_p, C.POINTER(xyY), C.c_void_p)
    save = api('cmsSaveProfileToMem', C.c_int, C.c_void_p, C.c_void_p, C.POINTER(C.c_uint32))
    close = api('cmsCloseProfile', C.c_int, C.c_void_p)
    free = api('cmsFreeToneCurve', None, C.c_void_p)
    curve = gamma(None, 2.2)
    white, primaries = xyY(.3127, .3290, 1), Primaries(xyY(.64, .33, 1), xyY(.21, .71, 1), xyY(.15, .06, 1))
    handles = [rgb(C.byref(white), C.byref(primaries), (C.c_void_p * 3)(curve, curve, curve)),
               gray(C.byref(xyY(.3457, .3585, 1)), curve)]
    result = []
    for handle in handles:
        assert handle
        size = C.c_uint32()
        assert save(handle, None, C.byref(size))
        buffer = C.create_string_buffer(size.value)
        assert save(handle, buffer, C.byref(size))
        result.append(buffer.raw[:size.value])
        close(handle)
    free(curve)
    return result


def psd(image, profile):
    channels = len(image.getbands())
    resource = b'8BIM' + struct.pack('>H', 1039) + b'\0\0' + struct.pack('>I', len(profile)) + profile
    resource += b'\0' * (len(profile) % 2)
    header = b'8BPS' + struct.pack('>H6xHIIHH', 1, channels, image.height, image.width, 8, 3 if channels == 3 else 1)
    return header + struct.pack('>I', 0) + struct.pack('>I', len(resource)) + resource + struct.pack('>IH', 0, 0) + b''.join(
        image.getchannel(i).tobytes() for i in range(channels))


def images(page):
    result = []
    def visit(resources):
        resources = resources.get_object()
        for ref in resources.get('/XObject', {}).values():
            obj = ref.get_object()
            if obj.get('/Subtype') == '/Image':
                result.append(obj)
            elif obj.get('/Subtype') == '/Form':
                visit(obj.get('/Resources', resources))
    visit(page['/Resources'])
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for arg in ('inkscape', 'dll-dir', 'data-dir', 'output'):
        ap.add_argument('--' + arg, type=Path, required=True)
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
    rgb_profile, gray_profile = profiles(args.dll_dir.resolve() / 'liblcms2-2.dll')
    srgb = ImageCms.createProfile('sRGB')
    checks = []
    def check(name, value, detail=''):
        checks.append(dict(name=name, passed=bool(value), detail=detail))
        print(('PASS ' if value else 'FAIL ') + name + (' ' + str(detail) if detail else ''), flush=True)
    def run(label, source, target, *options):
        p = subprocess.run([str(args.inkscape.resolve()), str(source), f'--export-filename={target}', *options],
                           env=env, capture_output=True, timeout=240)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        return p
    cases = [('rgb-png', 'RGB', 'PNG'), ('gray-png', 'L', 'PNG'),
             ('palette-png', 'P', 'PNG'), ('palette-alpha-png', 'P', 'PNG'),
             ('rgb-jpeg', 'RGB', 'JPEG'), ('gray-jpeg', 'L', 'JPEG'),
             ('rgba-tiff', 'RGBA', 'TIFF'), ('gray-alpha-tiff', 'LA', 'TIFF'),
             ('rgb-tiff', 'RGB', 'TIFF'), ('gray-tiff', 'L', 'TIFF'),
             ('rgb-psd', 'RGB', 'PSD'), ('gray-psd', 'L', 'PSD'),
             ('untagged-rgb', 'RGB', 'PNG'), ('untagged-gray', 'L', 'PNG')]
    for name, mode, format in cases:
        channels = 1 if mode in ('L', 'LA') else 3
        profile = (gray_profile if channels == 1 else rgb_profile) if not name.startswith('untagged') else b''
        value = {'RGB': (80, 150, 40), 'RGBA': (80, 150, 40, 128), 'L': 32, 'LA': (32, 128), 'P': 0}[mode]
        picture = Image.new(mode, (8, 8), value)
        if mode == 'P':
            picture.putpalette([80, 150, 40, 255, 0, 0])
            if name == 'palette-alpha-png':
                picture.info['transparency'] = bytes((128, 255))
        suffix = {'PNG': 'png', 'JPEG': 'jpg', 'TIFF': 'tif', 'PSD': 'psd'}[format]
        path = out / (name + '.' + suffix)
        if format == 'PSD':
            path.write_bytes(psd(picture, profile))
        else:
            picture.save(path, format=format, **({'icc_profile': profile} if profile else {}))
        decoded = Image.open(path)
        logical = decoded.convert('L' if channels == 1 else 'RGB')
        expected = ImageCms.profileToProfile(logical, ImageCms.ImageCmsProfile(io.BytesIO(profile)), srgb,
                    outputMode='RGB', renderingIntent=1).getpixel((4, 4)) if profile else logical.convert('RGB').getpixel((4, 4))
        alpha = picture.getpixel((4, 4))[-1] if mode in ('RGBA', 'LA') else (128 if name == 'palette-alpha-png' else 255)
        expected = tuple((v * alpha + 255 * (255 - alpha) + 127) // 255 for v in expected)
        mime = {'PNG': 'image/png', 'JPEG': 'image/jpeg', 'TIFF': 'image/tiff', 'PSD': 'image/vnd.adobe.photoshop'}[format]
        embedded = 'data:' + mime + ';base64,' + base64.b64encode(path.read_bytes()).decode()
        source = out / (name + '.svg')
        source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="32">'
                          f'<image id="linked" width="32" height="32" href="{path.as_uri()}"/>'
                          f'<image id="embedded" x="32" width="32" height="32" href="{embedded}"/></svg>', encoding='utf-8')
        pdf, png = out / (name + '.pdf'), out / (name + '-canvas.png')
        p = run(name + '-pdf', source, pdf)
        check(name + ': PDF written without findings', p.returncode == 0 and pdf.exists() and b'export findings' not in p.stderr)
        if pdf.exists():
            painted = images(PdfReader(pdf, strict=True).pages[0])
            check(name + ': two independently loaded images', len(painted) == 2)
            spaces = [obj['/ColorSpace'] for obj in painted]
            if profile:
                check(name + ': ICCBased with matching channels and original bytes', len(spaces) == 2 and all(
                    isinstance(cs, list) and cs[0] == '/ICCBased' and cs[1].get_object()['/N'] == channels and
                    cs[1].get_object().get_data() == profile for cs in spaces))
                refs = [tuple((cs[1].idnum, cs[1].generation)) for cs in spaces if isinstance(cs, list)]
                check(name + ': shared profile embedded once', len(refs) == 2 and refs[0] == refs[1])
            else:
                check(name + ': untagged device space', spaces == ['/DeviceGray' if channels == 1 else '/DeviceRGB'] * 2)
        p = run(name + '-canvas', source, png, '--export-background=white', '--export-background-opacity=1')
        check(name + ': canvas rasterised', p.returncode == 0 and png.exists())
        if png.exists():
            pixels = Image.open(png).convert('RGB')
            for route, point in (('linked', (16, 16)), ('embedded', (48, 16))):
                actual = pixels.getpixel(point)
                error = max(abs(a - b) for a, b in zip(actual, expected))
                check(name + ': ' + route + ' matches independent ImageCms within 2 levels', error <= 2,
                      dict(actual=actual, expected=expected, max_error=error))
    (out / 'summary.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)


if __name__ == '__main__':
    main()
