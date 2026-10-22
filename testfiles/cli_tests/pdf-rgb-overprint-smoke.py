"""Synthetic Illustrator-style RGB overprint pairs through PDF import and output.

No artwork or desktop input. General overprint and separation output are outside
this appearance control; an unmarked/shared state must retain its old behaviour.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (ArrayObject, BooleanObject, DecodedStreamObject,
                          DictionaryObject, FloatObject, NameObject, NumberObject)


def dictionary(**values):
    return DictionaryObject({NameObject('/' + k): v for k, v in values.items()})


def array(values):
    return ArrayObject([BooleanObject(v) if isinstance(v, bool) else FloatObject(v) for v in values])


def control(path, masked=False, marked=True, shared=False, picture=False, picture_shared=False, form=False,
            form_state=None):
    writer = PdfWriter()
    page = writer.add_blank_page(width=200, height=100)
    page[NameObject('/Group')] = dictionary(S=NameObject('/Transparency'), CS=NameObject('/DeviceRGB'),
                                           I=BooleanObject(False), K=BooleanObject(False))

    def function(start, end):
        return dictionary(FunctionType=NumberObject(2), Domain=array([0, 1]),
                          C0=array(start), C1=array(end), N=NumberObject(1))

    spot = ArrayObject([NameObject('/Separation'), NameObject('/SyntheticInk'), NameObject('/DeviceRGB'),
                        function([1, 1, 1], [0.2, 0.8, 0.4])])
    shaders = DictionaryObject()
    for name, radial in [('L', False), ('R', True)]:
        coords = [150, 50, 0, 150, 50, 40] if radial else [10, 20, 90, 20]
        for suffix, space, ramp in [('S', spot, function([0], [1])),
                                    ('P', NameObject('/DeviceRGB'), function([0.2, 0.3, 0.9], [1, 1, 1]))]:
            shaders[NameObject('/' + name + suffix)] = dictionary(
                ShadingType=NumberObject(3 if radial else 2), ColorSpace=space,
                Coords=array(coords), Extend=array([True, True]), Function=ramp)
    mask = NameObject('/None')
    if masked:
        mask_form = DecodedStreamObject()
        mask_form.set_data(b'0.5 g 0 0 200 100 re f')
        mask_form.update(dictionary(Type=NameObject('/XObject'), Subtype=NameObject('/Form'),
                               BBox=array([0, 0, 200, 100]), Resources=DictionaryObject(),
                               Group=dictionary(S=NameObject('/Transparency'), CS=NameObject('/DeviceGray'))))
        mask = dictionary(S=NameObject('/Luminosity'), G=writer._add_object(mask_form))

    def state(op):
        return dictionary(Type=NameObject('/ExtGState'), BM=NameObject('/Normal'),
                          CA=FloatObject(1), ca=FloatObject(1), OPM=NumberObject(1),
                          OP=BooleanObject(op), op=BooleanObject(op), SMask=mask)

    states = dictionary(Base=state(False), Ink=state(True))
    page[NameObject('/Resources')] = dictionary(Shading=shaders, ExtGState=states)
    content = ''
    if picture:
        object = DecodedStreamObject()
        if form:
            body = b'0.8 0.2 0.5 rg 0 0 1 1 re f'
            if form_state in ('own', 'base', 'ink'):
                body = (b'/Base gs ' if form_state == 'base' else b'/Ink gs ') + body
            object.set_data(body)
            object.update(dictionary(Type=NameObject('/XObject'), Subtype=NameObject('/Form'),
                                     BBox=array([0, 0, 1, 1])))
            if form_state == 'own':
                # Both names reference the same original state. The page's disposable
                # replacement must leave the Form's original Normal dictionary intact.
                original = writer._add_object(states['/Ink'])
                states[NameObject('/Ink')] = original
                object[NameObject('/Resources')] = dictionary(ExtGState=dictionary(Ink=original))
            elif form_state in ('nested', 'image'):
                child = DecodedStreamObject()
                if form_state == 'nested':
                    child.set_data(body)
                    child.update(dictionary(Type=NameObject('/XObject'), Subtype=NameObject('/Form'),
                                            BBox=array([0, 0, 1, 1]), Resources=DictionaryObject()))
                else:
                    child.set_data(bytes([204, 51, 128]))
                    child.update(dictionary(Type=NameObject('/XObject'), Subtype=NameObject('/Image'),
                                            Width=NumberObject(1), Height=NumberObject(1),
                                            BitsPerComponent=NumberObject(8), ColorSpace=NameObject('/DeviceRGB')))
                object.set_data(b'/Child Do')
                object[NameObject('/Resources')] = dictionary(XObject=dictionary(Child=writer._add_object(child)))
        else:
            object.set_data(bytes([204, 51, 128]))
            object.update(dictionary(Type=NameObject('/XObject'), Subtype=NameObject('/Image'),
                                     Width=NumberObject(1), Height=NumberObject(1),
                                     BitsPerComponent=NumberObject(8), ColorSpace=NameObject('/DeviceRGB')))
        page['/Resources'][NameObject('/XObject')] = dictionary(Picture=writer._add_object(object))
        picture_state = state(False)
        picture_state[NameObject('/SMask')] = NameObject('/None')
        states[NameObject('/Picture')] = picture_state
        content = 'q /Picture gs 0.6 0.7 0.8 rg 10 86 20 8 re f Q\n' if form_state == 'own' else ''
        content += f'q /{"Ink" if picture_shared else "Picture"} gs 20 0 0 8 10 86 cm /Picture Do Q\n'
    for name, x in [('L', 10), ('R', 110)]:
        content += '/MultiChannelPaint << >> BDC\n' if marked else ''
        content += f'q {x} 20 80 60 re W n q /Base gs /{name}S sh Q q /Ink gs /{name}P sh Q Q\n'
        content += 'EMC\n' if marked else ''
    if shared:
        content += 'q /Ink gs 0.8 0.2 0.5 rg 0 0 4 4 re f Q\n'
    stream = DecodedStreamObject()
    stream.set_data(content.encode('ascii'))
    page[NameObject('/Contents')] = writer._add_object(stream)
    writer.write(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inkscape', required=True, type=Path)
    parser.add_argument('--dll-dir', type=Path)
    parser.add_argument('--data-dir', type=Path)
    parser.add_argument('--pdftoppm', required=True, type=Path)
    cases = {'pair': {}, 'mask': dict(masked=True), 'unmarked': dict(marked=False),
             'shared': dict(shared=True), 'picture': dict(picture=True),
             'picture-mask': dict(picture=True, masked=True),
             'picture-shared': dict(picture=True, picture_shared=True),
             'form': dict(picture=True, form=True),
             'form-mask': dict(picture=True, form=True, masked=True),
             'form-own-state': dict(picture=True, form=True, form_state='own'),
             'form-inherited-state': dict(picture=True, form=True, form_state='base'),
             'form-inherited-ink': dict(picture=True, form=True, form_state='ink'),
             'form-nested': dict(picture=True, form=True, form_state='nested'),
             'form-image': dict(picture=True, form=True, form_state='image')}
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--case', action='append', choices=list(cases))
    parser.add_argument('--route', action='append', choices=['internal', 'cairo'])
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG='C', LC_ALL='C')
    for name in ('INKSCAPE_PROFILE_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'):
        env[name] = str(out / name.lower())
    if args.dll_dir:
        env['PATH'] = str(args.dll_dir.resolve()) + os.pathsep + env['PATH']
    if args.data_dir:
        env['INKSCAPE_DATADIR'] = str(args.data_dir.resolve())
    checks = []

    def check(name, ok, detail=''):
        checks.append(dict(name=name, passed=bool(ok), detail=detail))
        print(('PASS ' if ok else 'FAIL ') + name, detail if not ok else '', flush=True)

    def run(label, source, target, *options):
        result = subprocess.run([str(args.inkscape.resolve()), str(source),
                                 f'--export-filename={target}', *options],
                                env=env, capture_output=True, timeout=120)
        (out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        check(label, result.returncode == 0 and target.exists(), str(result.returncode))

    for case in args.case or list(cases):
        options = cases[case]
        masked = options.get('masked', False)
        marked = options.get('marked', True)
        picture = options.get('picture', False)
        form = options.get('form', False)
        supported = marked and not any(options.get(k, False) for k in ('shared', 'picture_shared'))
        supported = supported and options.get('form_state') not in ('ink', 'nested')
        source = out / (case + '.pdf')
        control(source, **options)
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        for route in args.route or ['internal', 'cairo']:
            label = case + '-' + route
            svg, png, pdf = [out / (label + suffix) for suffix in ('.svg', '.png', '-output.pdf')]
            run(label + '-import', source, svg, *(['--pdf-poppler'] if route == 'cairo' else []))
            run(label + '-canvas', svg, png, '--export-dpi=72', '--export-background=white')
            im = Image.open(png).convert('RGB')
            for x, y, t in [(30, 50, 0.25), (70, 50, 0.75), (160, 50, 0.25), (180, 50, 0.75)]:
                ink = [1 - t * (1 - c) for c in (0.2, 0.8, 0.4)]
                process = [c + t * (1 - c) for c in (0.2, 0.3, 0.9)]
                alpha = 0.5 if masked else 1
                if supported and route == 'internal':
                    expected = [(1 - alpha + alpha * a) * (1 - alpha + alpha * b) for a, b in zip(ink, process)]
                else:
                    expected = [(1 - alpha) * (1 - alpha + alpha * a) + alpha * b for a, b in zip(ink, process)]
                actual = im.getpixel((x, 100 - y))
                error = max(abs(a - round(b * 255)) for a, b in zip(actual, expected))
                check(f'{label}: expected ramp at {x},{y}', error <= 5, str((actual, expected, error)))
            root = ET.parse(svg).getroot()
            if route == 'internal':
                check(label + ': editable vector gradients', len(list(root.iter('{http://www.w3.org/2000/svg}linearGradient'))) >= 2)
                image_count = int(picture and (not form or options.get('form_state') == 'image'))
                check(label + ': only the original raster image',
                      len(list(root.iter('{http://www.w3.org/2000/svg}image'))) == image_count)
                if picture:
                    actual = im.getpixel((20, 10))
                    check(label + ': placed picture keeps its colour',
                          max(abs(a - b) for a, b in zip(actual, (204, 51, 128))) <= 2, str(actual))
                blends = [n for n in root.iter() if 'mix-blend-mode:multiply' in n.get('style', '')]
                check(label + ': multiply only on supported pairs', len(blends) == (2 if supported else 0))
                run(label + '-pdf', svg, pdf)
                def painted_images(obj, seen):
                    obj = obj.get_object() if hasattr(obj, 'get_object') else obj
                    if not isinstance(obj, (dict, list)) or id(obj) in seen:
                        return []
                    seen.add(id(obj))
                    if isinstance(obj, dict):
                        if obj.get('/Subtype') == '/Image':
                            # An image's own alpha plane is part of that picture, not another
                            # painted object. Gradient rasterization would have larger bounds.
                            return [(int(obj['/Width']), int(obj['/Height']))]
                        return sum((painted_images(v, seen) for v in obj.values()), [])
                    return sum((painted_images(v, seen) for v in obj), [])
                check(label + ': PDF keeps vector gradients and original picture',
                      painted_images(PdfReader(pdf).pages[0]['/Resources'], set()) == [(1, 1)] * image_count)
                prefix = out / (label + '-pdf-drawn')
                result = subprocess.run([str(args.pdftoppm.resolve()), '-r', '72', '-singlefile', '-png',
                                         str(pdf), str(prefix)], env=env, capture_output=True, timeout=30)
                (out / (label + '-pdf-drawn.log')).write_bytes(result.stdout + result.stderr)
                check(label + ': independent PDF render', result.returncode == 0)
                printed = Image.open(prefix.with_suffix('.png')).convert('RGB')
                for x, y in [(30, 50), (70, 50), (160, 50), (180, 50)] + ([(20, 90)] if picture else []):
                    a, b = im.getpixel((x, 100 - y)), printed.getpixel((x, 100 - y))
                    check(f'{label}: PDF agrees with canvas at {x},{y}',
                          max(abs(v - w) for v, w in zip(a, b)) <= 5, str((a, b)))
        check(case + ': source bytes unchanged', hashlib.sha256(source.read_bytes()).hexdigest() == digest)
    (out / 'report.json').write_text(json.dumps(dict(checks=checks), indent=2), encoding='utf-8')
    failures = sum(not c['passed'] for c in checks)
    print(f'{len(checks) - failures}/{len(checks)} checks passed')
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
