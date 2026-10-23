#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic CFF fonts with optional metadata absent: live PDF and native AI saves.

Requires fontTools, pypdf, Pillow and pdftoppm. Installs no fonts, sends no input.
The C++ cff-subsetter-test covers all metadata combinations, including CID fonts.
"""
import argparse
from io import BytesIO
import json
import os
from pathlib import Path
import subprocess
from xml.sax.saxutils import escape

from fontTools.cffLib import CFFFontSet
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont
from PIL import Image, ImageChops
from pypdf import PdfReader


METADATA = ("Notice", "FullName", "FamilyName", "Weight", "FontBBox")


def cff_fonts(resources, seen=None):
    """Text may live in a transparency Form's resources rather than the page's."""
    if seen is None:
        seen = set()
    for ref in resources.get("/Font", {}).values():
        font = ref.get_object()
        for child in font.get("/DescendantFonts", [font]):
            descriptor = child.get_object().get("/FontDescriptor")
            if descriptor and "/FontFile3" in descriptor.get_object():
                yield descriptor.get_object()["/FontFile3"].get_data()
    for ref in resources.get("/XObject", {}).values():
        identity = (getattr(ref, "idnum", id(ref)), getattr(ref, "generation", 0))
        if identity in seen:
            continue
        seen.add(identity)
        obj = ref.get_object()
        if "/Resources" in obj:
            yield from cff_fonts(obj["/Resources"], seen)


def make_font(path, family, missing):
    builder = FontBuilder(1000, isTTF=False)
    order = [".notdef", "space", "A", "B", "C", "D"]
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({32: "space", 65: "A", 66: "B", 67: "C", 68: "D"})
    builder.setupHorizontalMetrics({name: (600, 50) for name in order})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({"familyName": family, "styleName": "Regular",
                            "uniqueFontIdentifier": family, "fullName": family,
                            "psName": family + "-Regular"})
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupPost()
    chars = {}
    for i, name in enumerate(order):
        pen = T2CharStringPen(600, None)
        if name not in (".notdef", "space"):
            # Distinct outlines; a font fallback or wrong glyph mapping changes the image.
            pen.moveTo((50, 0))
            pen.lineTo((150 + 70 * i, 700))
            pen.curveTo((400, 700), (550, 400 - 40 * i), (550, 0))
            pen.closePath()
        chars[name] = pen.getCharString()
    builder.setupCFF(family + "-Regular", dict(Notice="PROOF synthetic regression font",
                     FullName=family, FamilyName=family, Weight="Regular",
                     FontBBox=[50, 0, 550, 700]), chars, {})
    # Keep the sfnt's own metrics; only remove the CFF metadata under test.
    builder.font.recalcBBoxes = False
    builder.font["head"].xMin, builder.font["head"].yMin = 50, 0
    builder.font["head"].xMax, builder.font["head"].yMax = 550, 700
    top = builder.font["CFF "].cff.topDictIndex[0]
    for key in missing:
        delattr(top, key)
    builder.save(path)
    with TTFont(path) as font:
        actual = font["CFF "].cff.topDictIndex[0].rawDict
        assert all((key in actual) == (key not in missing) for key in METADATA)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inkscape", required=True, type=Path)
    ap.add_argument("--dll-dir", type=Path)
    ap.add_argument("--data-dir", type=Path)
    ap.add_argument("--pdftoppm", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fonts = out / "fonts"
    fonts.mkdir()
    cases = [("complete", ())] + [("no-" + key.lower(), (key,)) for key in METADATA]
    cases.append(("no-metadata", METADATA))
    for i, (label, missing) in enumerate(cases):
        make_font(fonts / (label + ".otf"), "ProofCffCase" + str(i), missing)
    config = out / "fonts.conf"
    config.write_text('<fontconfig><dir>' + escape(fonts.as_posix()) + '</dir><cachedir>' +
                      escape((out / "font-cache").as_posix()) + '</cachedir></fontconfig>', encoding="utf-8")
    env = dict(os.environ, LANG="C", LC_ALL="C", INKSCAPE_FONTCONFIG=str(config))
    if args.dll_dir:
        env["PATH"] = str(args.dll_dir.resolve()) + os.pathsep + env["PATH"]
    if args.data_dir:
        env["INKSCAPE_DATADIR"] = str(args.data_dir.resolve())
    for key in ("INKSCAPE_PROFILE_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        env[key] = str(out / key.lower())
    checks, images = [], {}

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        print(("PASS " if ok else "FAIL ") + name, flush=True)

    def run(label, command):
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=180)
        (out / (label + ".log")).write_bytes(result.stdout + result.stderr)
        return result

    for i, (label, missing) in enumerate(cases):
        family = "ProofCffCase" + str(i)
        svg = out / (label + ".svg")
        svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="240" height="90">'
                       '<text x="10" y="65" font-size="40" font-family="' + family + '">'
                       'ABC BAC</text></svg>', encoding="utf-8")
        for kind, extension in (("pdf", "org.inkscape.output.pdf.capypdf"),
                                ("ai", "org.inkscape.output.ai.native")):
            tag = label + "-" + kind
            target = out / (tag + "." + kind)
            result = run(tag, [args.inkscape.resolve(), svg, "--export-filename=" + str(target),
                               "--export-extension=" + extension])
            made = result.returncode == 0 and target.is_file() and target.stat().st_size > 0
            check(tag + ": complete save", made)
            if not made:
                continue
            reader = PdfReader(target)
            page = reader.pages[0]
            check(tag + ": searchable text", "".join(page.extract_text().split()) == "ABCBAC")
            embedded = list(cff_fonts(page["/Resources"]))
            check(tag + ": embedded CFF font", len(embedded) == 1)
            if embedded:
                parsed = CFFFontSet()
                parsed.decompile(BytesIO(embedded[0]), None)
                top = parsed.topDictIndex[0]
                check(tag + ": expected font, real subset", parsed.fontNames == [family + "-Regular"]
                      and len(top.CharStrings.charStrings) == 5)
            image_prefix = out / (tag + "-render")
            rendered = run(tag + "-render", [args.pdftoppm.resolve(), "-singlefile", "-r", "144",
                                               "-png", target, image_prefix])
            check(tag + ": renders", rendered.returncode == 0)
            if rendered.returncode == 0:
                with Image.open(image_prefix.with_suffix(".png")) as image:
                    images[tag] = image.convert("RGB")
                check(tag + ": visible ink", images[tag].convert("L").getextrema()[0] < 80)
                baseline = images.get("complete-" + kind)
                if baseline is not None:
                    check(tag + ": same glyph appearance", ImageChops.difference(images[tag], baseline).getbbox() is None)
            if kind == "ai":
                reopened = out / (tag + "-reopened.svg")
                reopened_result = run(tag + "-reopen", [args.inkscape.resolve(), target,
                          "--export-type=svg", "--export-plain-svg", "--export-filename=" + str(reopened)])
                text = reopened.read_text(encoding="utf-8") if reopened.exists() else ""
                check(tag + ": editable native reopen", reopened_result.returncode == 0
                      and "<text" in text and "ABC BAC" in text and family in text)

    (out / "report.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
    print(f"{sum(c['passed'] for c in checks)}/{len(checks)} checks passed. {out / 'report.json'}")
    return 0 if all(c["passed"] for c in checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
