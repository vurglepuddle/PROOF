"""Placed PDF pages (linked .pdf and .ai), on the canvas and in the exported PDF (requires Pillow
and pypdf).

Two small PDFs are written here: one with a spot ink, CMYK numbers and type on page 1 and a
CMYK picture on page 2, one with a separate CMYK picture. A drawing places them by link,
pages 1 and 2 each twice, and is exported with the built editor,
as PDF 2.0 and PDF 1.7 (both use a cross-reference stream). The page must go into
the PDF as it is: the spot ink by its name, the numbers and the picture's samples unchanged, the
font kept and shared across the pages, one form per repeated page, and no stand-in left (see
src/extension/internal/pdfoutput/placed-pages.h). The file must read strictly and draw without
complaint, and the printed page and the canvas must show each page where it is placed.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

from PIL import Image
from pypdf import PdfReader

SPOT_CONTENT = b"/CS0 cs 1 scn 0 0 100 100 re f 0 0 1 0 k 100 0 100 50 re f BT /F1 20 Tf 110 70 Td (PROOF) Tj ET"
PICTURE = bytes([0, 0, 0, 255, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0])  # black, cyan / magenta, yellow
PAGE_TWO_CONTENT = b"q 100 0 0 100 10 20 cm /Im2 Do Q"


def pdf(objects, root=1):
    """A PDF of `objects` ({number: body}), its cross-references a table."""
    out = b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
    offsets = {}
    for n in sorted(objects):
        offsets[n] = len(out)
        out += f"{n} 0 obj\n".encode() + objects[n] + b"\nendobj\n"
    size = max(objects) + 1
    xref = len(out)
    out += f"xref\n0 {size}\n0000000000 65535 f \n".encode()
    for n in range(1, size):
        out += (f"{offsets[n]:010d} 00000 n \n" if n in offsets else "0000000000 65535 f \n").encode()
    out += f"trailer\n<< /Size {size} /Root {root} 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    return out


def stream(data, extra=b""):
    return b"<< /Length " + str(len(data)).encode() + extra + b" >>\nstream\n" + data + b"\nendstream"


def spot_pdf():
    # Numbered with gaps and high, as real files are, to check the renumbering.
    return pdf({
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources 40 0 R /Contents 50 0 R >>",
        4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 120 140] /CropBox [10 20 110 120] "
           b"/Resources 41 0 R /Contents 51 0 R /Group << /S /Transparency /CS /DeviceCMYK >> >>",
        40: b"<< /ColorSpace << /CS0 [/Separation /SpotOrange /DeviceCMYK 61 0 R] >> /Font << /F1 70 0 R >> >>",
        41: b"<< /XObject << /Im2 80 0 R >> /Font << /F1 70 0 R >> >>",
        50: stream(SPOT_CONTENT),
        51: stream(PAGE_TWO_CONTENT),
        61: b"<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0.6 1 0] /N 1 >>",
        70: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        80: stream(PICTURE, b" /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceCMYK "
                           b"/BitsPerComponent 8"),
    })


def picture_pdf():
    return pdf({
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /XObject << /Im1 8 0 R >> >> "
           b"/Contents 4 0 R >>",
        4: stream(b"q 100 0 0 100 0 0 cm /Im1 Do Q"),
        8: stream(PICTURE, b" /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceCMYK "
                           b"/BitsPerComponent 8"),
    })


def forms(reader):
    """Every form in the file's pages, once."""
    seen, out = set(), []

    def walk(res, depth=0):
        if not res:
            return
        res = res.get_object()
        for _, ref in (res.get("/XObject") or {}).items():
            o = ref.get_object()
            key = getattr(ref, "idnum", id(o))
            if key in seen:
                continue
            seen.add(key)
            if o.get("/Subtype") == "/Form" and depth < 12:
                out.append(o)
                walk(o.get("/Resources"), depth + 1)

    for page in reader.pages:
        walk(page.get("/Resources"))
    return out


def images(reader):
    found = []
    for form in forms(reader) + [p for p in reader.pages]:
        res = form.get("/Resources")
        res = res.get_object() if res else {}
        for _, ref in (res.get("/XObject") or {}).items():
            o = ref.get_object()
            if o.get("/Subtype") == "/Image":
                found.append(o)
    return found


def native_control(out, exe, check, run):
    """Independent native records and PDF reference pixels for a selected page."""
    path = str((out / 'spot.pdf').resolve()).replace('\\', '\\\\').replace('(', '\\(').replace(')', '\\)')
    native = (
        '%!PS-Adobe-3.0\n%%BoundingBox: 0 0 200 100\n%AI3_Cropmarks: 0 0 200 100\n'
        '%AI5_RulerUnits: 1\n%AI9_ColorModel: 1\n%%EndComments\n%%BeginSetup\n%%EndSetup\n'
        '%AI5_BeginLayer\n1 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb\n(Artwork) Ln\n0 A 0 Xw\n'
        '%AI5_BeginPlace\n%%FileType: 1\n[1 0 0 -1 0 100] 50 0 150 100 4 4 0\n('
        + path + ')`\n%%IncludeFile:' + path + '\n~\n%AI5_EndPlace\n'
        '%_/ArtDictionary :\n%_2 /Int (AI11 PlacedPDFPageIndex) ,\n'
        '%_3 /Int (AI11 PlacedPDFCropTo) ,\n%_;\n9 () XW\nLB\n%AI5_EndLayer--\n'
        '%%PageTrailer\nshowpage\n%%EOF\n').encode('utf-8')
    source = out / 'selected-page.ai'
    source.write_bytes(pdf({
        1: b'<< /Type /Catalog /Pages 2 0 R >>',
        2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] '
           b'/Resources << /XObject << /Im1 80 0 R >> >> /Contents 4 0 R '
           b'/PieceInfo << /Illustrator << /Private 5 0 R >> >> >>',
        4: stream(b'q 100 0 0 100 50 0 cm /Im1 Do Q'),
        5: b'<< /AIPrivateData1 6 0 R /NumBlock 1 /ContainerVersion 11 /CreatorVersion 16 /RoundtripVersion 16 >>',
        6: stream(native),
        80: stream(PICTURE, b' /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceCMYK '
                           b'/BitsPerComponent 8'),
    }))
    svg = out / 'selected-native.svg'
    p = run('native-import', [exe, source, f'--export-filename={svg}'])
    check('native AI: page 2 passes the PDF comparison', p.returncode == 0 and svg.is_file() and
          b'opens from its PDF page' not in p.stderr)
    if not svg.is_file():
        return
    check('native AI: SVG retains selected page', 'proof:pdf-page="2"' in svg.read_text(encoding='utf-8'))
    target = out / 'selected-native.pdf'
    p = run('native-print', [exe, svg, f'--export-filename={target}'])
    check('native AI: saved SVG exports PDF', p.returncode == 0 and target.is_file())
    if target.is_file():
        found = [f for f in forms(PdfReader(target, strict=True)) if f.get_data() == PAGE_TWO_CONTENT]
        check('native AI: original page 2 vectors retained', len(found) == 1 and
              [float(v) for v in found[0]['/BBox']] == [10, 20, 110, 120])
    canvas = out / 'selected-native.png'
    run('native-canvas', [exe, svg, f'--export-filename={canvas}', '--export-width=200', '--export-height=100',
                          '--export-background=white', '--export-background-opacity=1'])
    check('native AI: saved SVG draws', canvas.is_file())
    if canvas.is_file():
        shown = Image.open(canvas).convert('RGB')
        check('native AI: page 2 black in position', max(shown.getpixel((75, 25))) < 80)
        r, g, b = shown.getpixel((125, 25))
        check('native AI: page 2 cyan in position', r < 90 and g > 120 and b > 180)


def report(out, checks):
    result = dict(checks=checks, passed=all(c['passed'] for c in checks))
    (out / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    raise SystemExit(0 if result['passed'] else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inkscape", required=True, type=Path)
    ap.add_argument("--dll-dir", type=Path)
    ap.add_argument("--data-dir", type=Path)
    ap.add_argument("--pdftoppm", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--native-only", action="store_true", help="Only the independent native-AI page-2 control")
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG="C", LC_ALL="C")
    if args.dll_dir:
        env["PATH"] = str(args.dll_dir.resolve()) + os.pathsep + env["PATH"]
    if args.data_dir:
        env["INKSCAPE_DATADIR"] = str(args.data_dir.resolve())
    for var in ("INKSCAPE_PROFILE_DIR", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME"):
        p = out / var.lower()
        p.mkdir()
        env[var] = str(p)
    exe = str(args.inkscape.resolve())
    checks = []

    def check(name, condition, detail=""):
        checks.append(dict(name=name, passed=bool(condition), detail=detail))
        print(("PASS " if condition else "FAIL ") + name + ((" (" + detail + ")") if detail and not condition else ""), flush=True)

    def run(label, command):
        r = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=240)
        (out / (label + ".log")).write_bytes(r.stdout + r.stderr)
        return r

    (out / "spot.pdf").write_bytes(spot_pdf())
    (out / "picture.pdf").write_bytes(picture_pdf())
    if args.native_only:
        native_control(out, exe, check, run)
        report(out, checks)
    svg = out / "placed.svg"
    image = '<image preserveAspectRatio="none" '
    svg.write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        'xmlns:proof="urn:x-proof:document:1" '
        'width="400" height="300" viewBox="0 0 400 300">'
        + image + 'x="0" y="0" width="200" height="100" xlink:href="spot.pdf"/>'
        + image + 'x="0" y="150" width="100" height="50" xlink:href="spot.pdf"/>'
        + image + 'x="250" y="0" width="100" height="100" xlink:href="picture.pdf"/>'
        + image + 'x="250" y="150" width="100" height="100" proof:pdf-page="2" xlink:href="spot.pdf"/>'
        + image + 'x="125" y="150" width="50" height="50" proof:pdf-page="2" xlink:href="spot.pdf"/>'
        + "</svg>", encoding="utf-8")

    # Where each page shows, in the drawing's units, and what it shows there.
    spots = [("the spot ink", (50, 50), "orange"), ("the CMYK yellow", (150, 75), "yellow"),
             ("the second placement", (25, 175), "orange"), ("the picture's black", (275, 25), "black"),
             ("the picture's cyan", (325, 25), "cyan"), ("nothing placed", (375, 250), "white"),
             ("page 2 black", (275, 175), "black"), ("page 2 cyan", (325, 175), "cyan"),
             ("page 2 repeated", (137, 162), "black")]

    def looks(rgb, what):
        r, g, b = rgb
        return {"orange": r > 200 and 70 < g < 210 and b < 90, "yellow": r > 200 and g > 200 and b < 90,
                "black": max(rgb) < 80, "cyan": r < 90 and g > 120 and b > 180, "white": min(rgb) > 240}[what]

    for version in ("2.0", "1.7"):
        name = f"PDF {version}"
        target = out / f"placed-{version}.pdf"
        r = run(f"export-{version}", [exe, svg, f"--export-filename={target}", f"--export-pdf-version={version}"])
        check(f"{name}: exported", r.returncode == 0 and target.is_file() and target.stat().st_size > 100, f"exit {r.returncode}")
        if not target.is_file():
            continue
        try:
            reader = PdfReader(str(target), strict=True)
            all_forms = forms(reader)
            check(f"{name}: reads strictly", True)
        except Exception as e:
            check(f"{name}: reads strictly", False, str(e))
            continue
        spot_forms = [f for f in all_forms if SPOT_CONTENT[:20] in f.get_data()]
        check(f"{name}: one form for the file placed twice", len(spot_forms) == 1, str(len(spot_forms)))
        if spot_forms:
            form = spot_forms[0]
            data = form.get_data()
            check(f"{name}: the page's content as it is", SPOT_CONTENT in data)
            res = form["/Resources"].get_object()
            cs = res["/ColorSpace"]["/CS0"].get_object()
            check(f"{name}: the spot ink by its name", str(cs[0]) == "/Separation" and str(cs[1]) == "/SpotOrange", str(cs))
            fn = cs[3].get_object()
            check(f"{name}: the spot ink's CMYK", [float(v) for v in fn["/C1"]] == [0, 0.6, 1, 0], str(fn.get("/C1")))
            font = res["/Font"]["/F1"].get_object()
            check(f"{name}: its font", font.get("/BaseFont") == "/Helvetica", str(font.get("/BaseFont")))
            check(f"{name}: the page's box", [float(v) for v in form["/BBox"]] == [0, 0, 200, 100], str(form["/BBox"]))
        pictures = [i for i in images(reader) if i.get("/Width") == 2 and i.get("/Height") == 2]
        check(f"{name}: the pictures' samples as they are", len(pictures) == 2 and all(i.get_data() == PICTURE
              and i.get("/ColorSpace") == "/DeviceCMYK" for i in pictures), str(len(pictures)))
        second_forms = [f for f in all_forms if PAGE_TWO_CONTENT in f.get_data()]
        check(f"{name}: one form for page 2 placed twice", len(second_forms) == 1, str(len(second_forms)))
        if second_forms:
            form = second_forms[0]
            check(f"{name}: page 2's own crop", [float(v) for v in form["/BBox"]] == [10, 20, 110, 120])
            check(f"{name}: page 2's transparency group", form["/Group"]["/CS"] == "/DeviceCMYK")
            check(f"{name}: shared font copied once", spot_forms and
                  form["/Resources"]["/Font"].raw_get("/F1") == spot_forms[0]["/Resources"]["/Font"].raw_get("/F1"))
        check(f"{name}: no stand-in left", not any(b"7700001.000000" in f.get_data() or b"7700002.000000" in f.get_data()
                                                or b"7700003.000000" in f.get_data()
                                                for f in all_forms))
        raw = target.read_bytes()
        check(f"{name}: an update appended", raw.count(b"startxref") == 2 and raw.rstrip().endswith(b"%%EOF"))
        stem = out / f"printed-{version.replace('.', '')}"
        r = run(f"printed-{version}", [args.pdftoppm.resolve(), "-r", "72", "-singlefile", "-png", target, stem])
        # Poppler says which fonts of its own it lacks on the machine, for every file: not this one's.
        complaints = " | ".join(line for line in (r.stdout + r.stderr).decode("utf-8", "replace").splitlines()
                                if line.strip() and "No display font for" not in line)
        check(f"{name}: drawn without complaint", r.returncode == 0 and not complaints, complaints[:200])
        printed = Image.open(stem.with_suffix(".png")).convert("RGB")
        for label, (x, y), what in spots:
            # 96 drawing units to the inch, 72 points.
            rgb = printed.getpixel((int(x * 0.75), int(y * 0.75)))
            check(f"{name}: printed: {label}", looks(rgb, what), str(rgb))

    canvas = out / "canvas.png"
    run("canvas", [exe, svg, f"--export-filename={canvas}", "--export-background=white", "--export-background-opacity=1"])
    check("canvas: exported", canvas.is_file())
    if canvas.is_file():
        shown = Image.open(canvas).convert("RGB")
        for label, (x, y), what in spots:
            rgb = shown.getpixel((x, y))
            check(f"canvas: {label}", looks(rgb, what), str(rgb))

    # Save/reopen must retain the selected page; a missing selection must fail export.
    saved = out / "saved.svg"
    run("save", [exe, svg, f"--export-filename={saved}"])
    reopened = out / "reopened.png"
    run("reopen", [exe, saved, f"--export-filename={reopened}", "--export-background=white",
                   "--export-background-opacity=1"])
    check("SVG reopen: same selected pages", reopened.is_file() and canvas.is_file() and
          Image.open(reopened).tobytes() == Image.open(canvas).tobytes())
    invalid = out / "invalid.svg"
    invalid.write_text(svg.read_text(encoding="utf-8").replace('proof:pdf-page="2"', 'proof:pdf-page="3"'), encoding="utf-8")
    failed = out / "invalid.pdf"
    r = run("invalid-page", [exe, invalid, f"--export-filename={failed}"])
    check("missing selected page: export fails", r.returncode != 0 and not failed.exists())

    native_control(out, exe, check, run)
    report(out, checks)


if __name__ == "__main__":
    main()
