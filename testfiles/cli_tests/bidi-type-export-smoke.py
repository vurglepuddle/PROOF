#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded Arabic/Hebrew text: complete native AI/PDF saves and editable reopen.

Synthetic art only; installs no fonts and sends no input. Requires Arial (with
Arabic/Hebrew coverage), pypdf, Pillow, and pdftoppm/pdftotext. Illustrator qualification
is a separate live check, not claimed by this script.
"""
import argparse
from collections import Counter
import json
import os
import re
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

from PIL import Image, ImageChops, ImageFilter
from pypdf import PdfReader


CASES = [
    ("rtl", "start", "Latin text 123", False),
    ("rtl", "middle", "Latin text 123", False),
    ("rtl", "end", "Latin text 123", False),
    ("rtl", "start", "אבגד", False),
    ("rtl", "middle", "אבגד", False),
    ("rtl", "end", "אבגד", False),
    ("ltr", "start", "אבגד", False),
    ("ltr", "middle", "ABC אבגד 123 (45)", False),
    ("rtl", "middle", "ABC אבגד 123 (45)", False),
    ("ltr", "middle", "ABC ב 123", True),
    ("ltr", "middle", "אבגד", True),
    ("rtl", "start", "العربية", False),
    ("rtl", "end", "العربية", False),
    ("rtl", "middle", "العَرَبِيَّة", False),
    ("rtl", "middle", "ABC العربية 123 (45)", False),
    ("ltr", "middle", "ABC العربية 123 (45)", False),
    ("rtl", "middle", "العربية ١٢٣ (٤٥)", False),
    ("ltr", "middle", "العَرَبِيَّة", False),
    ("rtl", "middle", "لا", False),
    ("rtl", "middle", "السلام عليكم", False),
    ("ltr", "middle", "AB لا 123", False),
    ("rtl", "middle", "AB لا 123", False),
]
SVG = "http://www.w3.org/2000/svg"


def unicode_maps(resources, seen=None):
    """Text may live inside a transparency Form, rather than on the page."""
    if seen is None:
        seen = set()
    for ref in resources.get("/Font", {}).values():
        font = ref.get_object()
        if "/ToUnicode" in font:
            yield font["/ToUnicode"].get_data()
    for ref in resources.get("/XObject", {}).values():
        identity = (getattr(ref, "idnum", id(ref)), getattr(ref, "generation", 0))
        if identity in seen:
            continue
        seen.add(identity)
        obj = ref.get_object()
        if "/Resources" in obj:
            yield from unicode_maps(obj["/Resources"], seen)


def document(body, height):
    return f'<svg xmlns="{SVG}" xmlns:proof="urn:x-proof:document:1" width="640" height="{height}">{body}</svg>'


def text(direction, anchor, content, override=False, x=320, y=35, extra=""):
    style = f"font-family:Arial;font-size:24px;fill:#000000;direction:{direction};text-anchor:{anchor}"
    if override:
        style += ";unicode-bidi:bidi-override"
    return f'<text x="{x}" y="{y}" xml:space="preserve" style="{style}" {extra}>{escape(content)}</text>'


def ink_matches(first, second):
    a, b = (Image.open(p).convert("L") for p in (first, second))
    w, h = min(a.width, b.width), min(a.height, b.height)
    if abs(a.width-b.width) > 1 or abs(a.height-b.height) > 1:
        return False
    for im in (a, b):
        if im.width > w and im.crop((w, 0, im.width, im.height)).getextrema() != (255, 255):
            return False
        if im.height > h and im.crop((0, h, im.width, im.height)).getextrema() != (255, 255):
            return False
    masks = [im.crop((0, 0, w, h)).point(lambda v: 255 if v < 160 else 0) for im in (a, b)]
    for i in (0, 1):
        own = masks[i].histogram()[255]
        unmatched = ImageChops.subtract(masks[i], masks[1-i].filter(ImageFilter.MaxFilter(5)))
        if not own or unmatched.histogram()[255] / own > .01:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inkscape", required=True, type=Path)
    ap.add_argument("--dll-dir", type=Path)
    ap.add_argument("--data-dir", type=Path)
    ap.add_argument("--pdftoppm", required=True, type=Path)
    ap.add_argument("--pdftotext", type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    pdftotext = (args.pdftotext or args.pdftoppm.with_name("pdftotext" + args.pdftoppm.suffix)).resolve()
    if not pdftotext.is_file():
        ap.error("provide --pdftotext or keep it beside pdftoppm")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG="C", LC_ALL="C")
    if args.dll_dir:
        env["PATH"] = str(args.dll_dir.resolve()) + os.pathsep + env["PATH"]
    if args.data_dir:
        env["INKSCAPE_DATADIR"] = str(args.data_dir.resolve())
    for key in ("INKSCAPE_PROFILE_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        env[key] = str(out / key.lower())
    exe = args.inkscape.resolve()
    checks = []

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        print(("PASS " if ok else "FAIL ") + name, flush=True)

    def run(label, command):
        result = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + ".log")).write_bytes(result.stdout + result.stderr)
        return result

    point = document("".join(text(*row, y=35+i*45) for i, row in enumerate(CASES)), 40+len(CASES)*45)
    area_words = ["אבגד", "العَرَبِيَّة"]
    area = document("".join(text("rtl", "start", word, x=620, y=40+i*80,
        extra=f'proof:text-frame="M20 {10+i*80} H620 V{70+i*80} H20 Z" proof:first-baseline="0 22.5"')
        for i, word in enumerate(area_words)), 180)
    for label, source, expected in (("point", point, [row[2] for row in CASES]), ("area", area, area_words)):
        svg = out / (label + ".svg")
        svg.write_text(source, encoding="utf-8")
        png = out / (label + ".png")
        made = run(label + "-canvas", [exe, svg, "--export-dpi=144", "--export-background=white", f"--export-filename={png}"])
        check(label + ": canvas renders", made.returncode == 0 and png.exists())
        for kind, extension in (("pdf", "org.inkscape.output.pdf.capypdf"), ("ai", "org.inkscape.output.ai.native")):
            tag = label + "-" + kind
            target = out / (tag + "." + kind)
            made = run(tag, [exe, svg, f"--export-extension={extension}", f"--export-filename={target}"])
            saved = made.returncode == 0 and target.is_file() and target.stat().st_size > 0
            check(tag + ": complete save", saved)
            if not saved:
                continue
            page = PdfReader(target).pages[0]
            # pypdf's bidi heuristic can drop the Latin prefix of a mixed-direction run.
            # Use Poppler's raw extractor as an independent check of the emitted Unicode.
            raw_text = run(tag + "-text", [pdftotext, "-raw", "-enc", "UTF-8", target, "-"])
            extracted = raw_text.stdout.decode("utf-8", errors="replace")
            check(tag + ": live PDF text", raw_text.returncode == 0 and bool(page.extract_text()) and
                  all(c in extracted for word in expected for c in word if not c.isspace()))
            if label == "point":
                # Extractors may reverse RTL sequences for display. Check the actual
                # ligature's logical Unicode mapping, independent of that heuristic.
                check(tag + ": lam-alef maps to both Unicode characters",
                      any(re.search(rb"<[0-9a-f]+>\s*<06440627>", cmap, re.I)
                          for cmap in unicode_maps(page["/Resources"])))
            prefix = out / (tag + "-pdf")
            rendered = run(tag + "-render", [args.pdftoppm.resolve(), "-singlefile", "-r", "144", "-png", target, prefix])
            check(tag + ": vector PDF matches canvas", rendered.returncode == 0 and ink_matches(png, prefix.with_suffix(".png")))
            if kind == "ai":
                reopened = out / (tag + "-reopened.svg")
                read = run(tag + "-reopen", [exe, target, f"--export-filename={reopened}"])
                root = ET.parse(reopened).getroot() if reopened.exists() else None
                objects = list(root.iter(f"{{{SVG}}}text")) if root is not None else []
                check(tag + ": full editable stories", read.returncode == 0 and
                      Counter("".join(t.itertext()).strip() for t in objects) == Counter(expected))
                if label == "point":
                    actual = Counter()
                    for obj in objects:
                        style = obj.get("style", "")
                        composer = next((v for k,v in obj.attrib.items() if k.split("}")[-1] == "ai-text-composer"), "")
                        actual[("".join(obj.itertext()).strip(), "direction:rtl" in style,
                                "unicode-bidi:bidi-override" in style, composer)] += 1
                    wanted = Counter((content, direction == "rtl", override, "latin" if override else "middle-eastern")
                                     for direction, anchor, content, override in CASES)
                    check(tag + ": direction, composer and typed-order mode", actual == wanted)

    for i, body in enumerate((text("rtl", "start", "العربية", True),
        text("rtl", "start", "العربية", extra='letter-spacing="1"'),
        '<defs><path id="p" d="M10 50 H500"/></defs><text font-family="Arial"><textPath href="#p">العربية</textPath></text>',
        '<text font-family="Arial" direction="rtl">العربية<tspan direction="ltr">ABC</tspan></text>')):
        svg = out / f"refused-{i}.svg"
        svg.write_text(document(body, 100), encoding="utf-8")
        target = out / f"refused-{i}.ai"
        made = run(f"refused-{i}", [exe, svg, "--export-extension=org.inkscape.output.ai.native", f"--export-filename={target}"])
        check(f"unsupported case {i}: refused without a partial file", made.returncode != 0 and not target.exists())
    (out / "report.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
    print(f"{sum(c['passed'] for c in checks)}/{len(checks)} checks passed. {out / 'report.json'}")
    return 0 if all(c["passed"] for c in checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
