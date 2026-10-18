#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export into folders and files named in any script Windows allows; synthetic art only.

Cyrillic, Chinese, Japanese, Korean, Hebrew, umlauts, emoji and punctuation, in folder and
file names alike: a PDF with linked PNG and JPEG pictures and a placed PDF page, a native
.ai, a PNG and a plain SVG. A save that can't be written must say so. Sends no input.
Requires pypdf and Pillow.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from xml.sax.saxutils import escape

from PIL import Image
from pypdf import PdfReader

NAMES = [
    "Кириллица тест",
    "0х000",
    "设计 デザイン 디자인",
    "עיצוב גרפי",
    "Größe Ünïcødé ß",
    "emoji 🎨✏️ 👩‍🎨",
    "spécial & #1 (copy) 100% [a] {b} ~x! @y+z=w, 'q'",
]

DOCUMENT = '''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="300" height="200" viewBox="0 0 300 200">
  <rect x="10" y="10" width="80" height="60" fill="#d4145a"/>
  <image x="100" y="10" width="60" height="60" xlink:href="{png}"/>
  <image x="170" y="10" width="60" height="60" xlink:href="{jpg}"/>
  <image x="10" y="90" width="120" height="90" xlink:href="{pdf}"/>
  <text x="150" y="140" font-family="sans-serif" font-size="14">PROOFTEXT</text>
</svg>
'''

SHAPES = '''<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120" viewBox="0 0 200 120">
  <rect x="20" y="20" width="80" height="60" fill="#29abe2"/>
  <path d="M 120 20 H 180 V 80 Z" fill="#1d1d1b"/>
</svg>
'''

PLACED = '''<svg xmlns="http://www.w3.org/2000/svg" width="160" height="120" viewBox="0 0 160 120">
  <rect x="0" y="0" width="160" height="120" fill="#fbb03b"/>
  <text x="10" y="60" font-family="sans-serif" font-size="16">PLACEDMARK</text>
</svg>
'''


def link(name):
    """A relative link as SVG allows it: Unicode stays as it is (an IRI); a link's own
    syntax characters are escaped, then the whole is escaped for the XML attribute."""
    for char in "%#?":
        name = name.replace(char, f"%{ord(char):02X}")
    return escape(name, {'"': "&quot;"})


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--inkscape", required=True, type=Path)
    ap.add_argument("--dll-dir", type=Path)
    ap.add_argument("--data-dir", type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG="C", LC_ALL="C")
    if args.dll_dir:
        env["PATH"] = str(args.dll_dir.resolve()) + os.pathsep + env["PATH"]
    if args.data_dir:
        env["INKSCAPE_DATADIR"] = str(args.data_dir.resolve())
    for name in ("INKSCAPE_PROFILE_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        folder = out / name.lower()
        folder.mkdir()
        env[name] = str(folder)
    exe = str(args.inkscape.resolve())
    checks = []

    def check(name, condition, detail=""):
        checks.append(dict(name=name, passed=bool(condition), detail=detail))
        print(("PASS " if condition else "FAIL ") + name, flush=True)

    def run(label, command):
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=240)
        (out / (label + ".log")).write_bytes(result.stdout + result.stderr)
        return result

    # The page to place is made once, in a plain folder.
    plain = out / "plain"
    plain.mkdir()
    (plain / "placed.svg").write_text(PLACED, encoding="utf-8")
    placed_pdf = plain / "placed.pdf"
    made = run("placed-page", [exe, plain / "placed.svg", f"--export-filename={placed_pdf}",
                               "--export-extension=org.inkscape.output.pdf.capypdf"])
    check("the page to place is made", made.returncode == 0 and placed_pdf.is_file())

    for index, name in enumerate(NAMES):
        label = f"case{index}"
        folder = out / name
        folder.mkdir()
        png, jpg, pdf = f"картинка {name}.png", f"写真 {name}.jpg", f"страница {name}.pdf"
        Image.new("RGB", (40, 40), (0, 146, 69)).save(folder / png)
        Image.new("RGB", (40, 40), (102, 45, 145)).save(folder / jpg, quality=90)
        (folder / pdf).write_bytes(placed_pdf.read_bytes())
        source = folder / f"{name}.svg"
        source.write_text(DOCUMENT.format(png=link(png), jpg=link(jpg), pdf=link(pdf)), encoding="utf-8")
        shapes = folder / f"{name} shapes.svg"
        shapes.write_text(SHAPES, encoding="utf-8")

        target = folder / f"{name} ✓.pdf"
        result = run(label + "-pdf", [exe, source, f"--export-filename={target}",
                                      "--export-extension=org.inkscape.output.pdf.capypdf"])
        written = result.returncode == 0 and target.is_file() and target.stat().st_size > 0
        check(f"{label}: PDF written into '{name}'", written, f"exit {result.returncode}")
        if written:
            page = PdfReader(target, strict=False).pages[0]
            pictures = 0
            resources = page.get("/Resources", {})
            for xobject in (resources.get("/XObject") or {}).values():
                xobject = xobject.get_object()
                if xobject.get("/Subtype") == "/Image":
                    pictures += 1
                elif xobject.get("/Subtype") == "/Form":
                    inner = (xobject.get("/Resources") or {}).get("/XObject") or {}
                    pictures += sum(1 for x in inner.values() if x.get_object().get("/Subtype") == "/Image")
            check(f"{label}: both linked pictures are in the PDF", pictures >= 2, f"{pictures} pictures")
            text = page.extract_text() or ""
            check(f"{label}: the placed page is in the PDF", "PLACEDMARK" in text)
            check(f"{label}: the text is in the PDF", "PROOFTEXT" in text)

        target = folder / f"{name} ✓.ai"
        result = run(label + "-ai", [exe, shapes, f"--export-filename={target}",
                                     "--export-extension=org.inkscape.output.ai.native"])
        written = result.returncode == 0 and target.is_file()
        check(f"{label}: native .ai written into '{name}'", written, f"exit {result.returncode}")
        if written:
            reader = PdfReader(target, strict=False)
            check(f"{label}: the .ai carries Illustrator's records",
                  reader.pdf_header == "%PDF-1.7" and "/PieceInfo" in reader.pages[0])
        leftovers = [p.name for p in folder.iterdir() if ".proof-" in p.name or p.name.endswith("~")]
        check(f"{label}: no temporary files left", not leftovers, ", ".join(leftovers))

        target = folder / f"{name} ✓.png"
        result = run(label + "-png", [exe, source, f"--export-filename={target}", "--export-area-page"])
        check(f"{label}: PNG written into '{name}'", result.returncode == 0 and target.is_file(),
              f"exit {result.returncode}")

        target = folder / f"{name} ✓ plain.svg"
        result = run(label + "-svg", [exe, source, f"--export-filename={target}", "--export-plain-svg"])
        check(f"{label}: plain SVG written into '{name}'", result.returncode == 0 and target.is_file(),
              f"exit {result.returncode}")

    # A save that can't be written must fail, not finish quietly with no file. The writer's
    # temporary file is blocked by a folder of that name, past the command line's own checks.
    blocked = plain / "blocked.pdf"
    (plain / "blocked.pdf~").mkdir()
    result = run("unwritable-pdf", [exe, plain / "placed.svg", f"--export-filename={blocked}",
                                    "--export-extension=org.inkscape.output.pdf.capypdf"])
    check("a PDF that can't be written is reported as failed", result.returncode != 0 and not blocked.exists(),
          f"exit {result.returncode}")
    missing = out / "no such folder ✗" / "never.pdf"
    result = run("missing-folder-pdf", [exe, plain / "placed.svg", f"--export-filename={missing}",
                                        "--export-extension=org.inkscape.output.pdf.capypdf"])
    check("a PDF into a missing folder is reported as failed", result.returncode != 0 and not missing.exists(),
          f"exit {result.returncode}")

    (out / "report.json").write_text(json.dumps(checks, ensure_ascii=False, indent=2), encoding="utf-8")
    passed = sum(c["passed"] for c in checks)
    print(f"{passed}/{len(checks)} checks passed. {out / 'report.json'}")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
