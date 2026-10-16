"""Exercise the built .ai exporter (requires pypdf and Pillow, no Illustrator).

This is an integration test, not a certification of Illustrator compatibility.
Use check-illustrator.ps1 separately for actual Illustrator opening.
"""
import argparse
import base64
import hashlib
import html
import io
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops
from pypdf import PdfReader


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inkscape", required=True, type=Path)
    ap.add_argument("--dll-dir", type=Path)
    ap.add_argument("--data-dir", type=Path)
    ap.add_argument("--pdftoppm", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--corpus", type=Path)
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
    rows = []

    def check(name, condition, detail=""):
        checks.append(dict(name=name, passed=bool(condition), detail=detail))
        print(("PASS " if condition else "FAIL ") + name, flush=True)

    def run(label, command):
        r = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=240)
        (out / (label + ".log")).write_bytes(r.stdout + r.stderr)
        if r.returncode:
            raise RuntimeError(f"{label}: exit {r.returncode}; see log")

    def export(source, target, *options):
        run(target.stem, [exe, source, f"--export-filename={target}", *options])
        check(target.name + " exists", target.is_file() and target.stat().st_size > 100)
        return PdfReader(target)

    def render(pdf, name):
        prefix = out / name
        run(name, [args.pdftoppm.resolve(), "-f", "1", "-singlefile", "-scale-to", "800", "-png", pdf, prefix])
        return prefix.with_suffix(".png")

    source = Path(__file__).parent / "testcases/ai-interchange.svg"
    original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    ai = out / "fixture.ai"
    reader = export(source, ai)
    check("PDF version restricted", reader.pdf_header in ("%PDF-1.4", "%PDF-1.5"), reader.pdf_header)
    check("both artboards exported", len(reader.pages) == 2)
    check("text remains text", "PROOF editable 123" in reader.pages[0].extract_text())
    check("page dimensions", list(reader.pages[0].mediabox) == [0, 0, 300, 225])
    check("no stale Illustrator private data", all("/PieceInfo" not in p for p in reader.pages))
    check("vectors remain vectors", b" re" in reader.pages[0].get_contents().get_data())
    outlined = export(source, out / "outlined.ai", "--export-text-to-path")
    check("outline option applied", not outlined.pages[0].extract_text().strip())
    legacy = export(source, out / "pdf14.ai", "--export-pdf-version=1.4")
    check("PDF 1.4 option applied", legacy.pdf_header == "%PDF-1.4")
    selected = export(source, out / "second-page.ai", "--export-page=2")
    check("page selection applied", len(selected.pages) == 1 and "Second artboard" in selected.pages[0].extract_text())
    # Exercise AI -> SVG -> edit -> AI, not merely re-exporting the input.
    imported = out / "reopened.svg"
    run("reopen", [exe, ai, "--pages=all", f"--export-filename={imported}"])
    text = imported.read_text(encoding="utf-8")
    check("reopened paths editable", "<path" in text)
    edited = out / "edited.svg"
    edited.write_text(text.replace("PROOF editable 123", "PROOF CHANGED 456").replace("PROOFeditable 123", "PROOF CHANGED 456"), encoding="utf-8")
    changed = export(edited, out / "edited.ai")
    check("edited artwork written", "PROOF CHANGED 456" in " ".join(changed.pages[0].extract_text().split()))
    check("fixture untouched", hashlib.sha256(source.read_bytes()).hexdigest() == original_hash)

    # An .ai with Illustrator's own records opens from them when they draw like its page, and
    # from its page when they don't. The second crashed PROOF until 2026-10-16, on the page's
    # first font: drawing the page for the comparison left Poppler without its settings for
    # the page import.
    def native_fixture(path, page):
        records = ("%!PS-Adobe-3.0\n%%BoundingBox: 0 0 200 100\n%AI3_Cropmarks: 0 0 200 100\n%AI5_RulerUnits: 1\n"
                   "%AI9_ColorModel: 1\n%%EndComments\n%%BeginSetup\n%%EndSetup\n"
                   "%AI5_BeginLayer\n1 1 1 1 0 0 0 0 79 128 255 0 50 0 Lb\n(Artwork) Ln\n0 A 0 Xw\n"
                   "0 g\n10 20 m 10 60 L 90 60 L 90 20 L 10 20 L\nf\nLB\n%AI5_EndLayer--\n"
                   "%%PageTrailer\nshowpage\n%%EOF\n")
        stream = lambda s: f"<< /Length {len(s)} >>\nstream\n{s}\nendstream"
        objects = {
            1: "<< /Type /Catalog /Pages 2 0 R >>",
            2: "<< /Type /Pages /Kids [5 0 R] /Count 1 >>",
            3: "<< /AIPrivateData1 4 0 R /NumBlock 1 /ContainerVersion 11 /CreatorVersion 16 /RoundtripVersion 16 >>",
            4: stream(records),
            5: "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /TrimBox [0 0 200 100] "
               "/Resources << /Font << /F1 7 0 R >> >> "
               "/Contents 6 0 R /PieceInfo << /Illustrator << /Private 3 0 R >> >> >>",
            6: stream(page),
            7: "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        }
        pdf = "%PDF-1.5\n"
        offsets = {}
        for n, body in objects.items():
            offsets[n] = len(pdf)
            pdf += f"{n} 0 obj\n{body}\nendobj\n"
        xref = len(pdf)
        pdf += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n"
        pdf += "".join(f"{offsets[n]:010d} 00000 n \n" for n in sorted(objects))
        pdf += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n"
        path.write_bytes(pdf.encode("ascii"))

    for label, page, native in (("native-matching", "0 g 10 20 80 40 re f", True),
                                ("native-differing", "0 g 110 20 80 40 re f BT /F1 12 Tf 120 70 Td (PROOF) Tj ET", False)):
        source_ai = out / (label + ".ai")
        native_fixture(source_ai, page)
        opened = out / (label + ".svg")
        r = subprocess.run([exe, source_ai, f"--export-filename={opened}"], env=env, capture_output=True, timeout=240)
        log = r.stdout + r.stderr
        (out / (label + ".log")).write_bytes(log)
        check(label + " opens from the command line", r.returncode == 0 and opened.is_file(), f"exit {r.returncode:#x}")
        check(label + (" opens from its records" if native else " opens from its page"),
              (b"opens from its PDF page" in log) != native, log.decode("utf-8", "replace")[-300:])

    # PDF import represents large bitmaps with unit-square SVG images. Both
    # quadrants must survive the tiny pixel-to-unit transform in Cairo export.
    pixels = Image.new("RGB", (1600, 1200), "red")
    pixels.paste((0, 0, 255), (800, 600, 1600, 1200))
    payload = io.BytesIO()
    pixels.save(payload, format="PNG")
    image_svg = out / "unit-image.svg"
    image_svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="200" height="200" viewBox="0 0 200 200"><image width="1" height="1" preserveAspectRatio="none" transform="scale(200)" xlink:href="data:image/png;base64,' + base64.b64encode(payload.getvalue()).decode() + '"/></svg>', encoding="utf-8")
    export(image_svg, out / "unit-image.ai")
    preview = Image.open(render(out / "unit-image.ai", "unit-image-preview")).convert("RGB")
    check("unit-square bitmap scale", preview.getpixel((100, 100)) == (255, 0, 0) and preview.getpixel((600, 600)) == (0, 0, 255))

    if args.corpus:
        for index, item in enumerate(sorted(args.corpus.glob("*.ai"))):
            stem = f"corpus-{index+1}"
            before = hashlib.sha256(item.read_bytes()).hexdigest()
            imported = out / (stem + ".svg")
            run(stem + "-import", [exe, item.resolve(), "--pages=all", f"--export-filename={imported}"])
            target = out / (stem + ".ai")
            exported = export(imported, target)
            src_pdf = PdfReader(item)
            check(item.name + " page count", len(src_pdf.pages) == len(exported.pages), str(len(exported.pages)))
            check(item.name + " unchanged", before == hashlib.sha256(item.read_bytes()).hexdigest(), before)
            source_png = render(item.resolve(), stem + "-source")
            output_png = render(target, stem + "-output")
            a, b = Image.open(source_png).convert("RGB"), Image.open(output_png).convert("RGB")
            # Diagnostic only. ICC differences/rounding and alignment affect it.
            diff_fraction = None
            if a.size == b.size:
                delta = ImageChops.difference(a, b)
                diff_fraction = sum(max(pixel) > 32 for pixel in delta.getdata()) / (a.width * a.height)
            rows.append(dict(file=item.name, pages=len(exported.pages), output=target.name,
                             source=source_png.name, preview=output_png.name,
                             pixel_difference_fraction=diff_fraction))
    result = dict(checks=checks, corpus=rows, passed=all(c["passed"] for c in checks),
                  scope="Automated structure only. RGB output; spots/CMYK semantics not preserved. Illustrator and visual review separate.")
    (out / "report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    cards = []
    for row in rows:
        cards.append(f'<h2>{html.escape(row["file"])}</h2><p>{row["pages"]} pages; source / exported page 1</p><div><img src="{row["source"]}"><img src="{row["preview"]}"></div>')
    (out / "gallery.html").write_text('<!doctype html><meta charset="utf-8"><title>PROOF AI interchange</title><style>body{font:16px sans-serif;background:#eee;margin:24px}div{display:flex;gap:12px}img{width:48%;object-fit:contain;background:white}</style><h1>AI interchange: source / exported</h1><p>Visual comparison, not a fidelity certificate. RGB output; spot inks and CMYK separations are lost.</p>' + ''.join(cards), encoding="utf-8")
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
