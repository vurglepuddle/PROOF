#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic TrueType/CFF2 variable fonts: editable AI and live, static-instance PDF.

Requires fontTools, pypdf, Pillow and pdftoppm. Installs no fonts, sends no input.
Includes fractional/negative/custom axes, named/default instances and cache separation.
"""
import argparse
from collections import Counter
from io import BytesIO
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

from fontTools.cffLib import CFFFontSet
from fontTools.designspaceLib import AxisDescriptor, DesignSpaceDocument, SourceDescriptor
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.varLib import build
from fontTools.varLib.instancer import instantiateVariableFont
from PIL import Image, ImageChops, ImageFilter
from pypdf import PdfReader


INSTANCES = [(400, 0), (900, 0), (350.25, -.625), (350.75, -.625)]


def make_font(path, family, cff, custom_only=False):
    space = DesignSpaceDocument()
    for tag, lo, default, hi in ((("TEST", -1, 0, 1),) if custom_only else
                               (("wght", 100, 400, 900), ("TEST", -1, 0, 1))):
        axis = AxisDescriptor()
        axis.name = axis.tag = tag
        axis.minimum, axis.default, axis.maximum = lo, default, hi
        space.addAxis(axis)
    for weight, custom in ((400, 0), (100, 0), (900, 0), (400, -1), (400, 1)):
        if custom_only and weight != 400:
            continue
        builder = FontBuilder(1000, isTTF=not cff)
        order = [".notdef", "space", "A", "B", "C", "D"]
        builder.setupGlyphOrder(order)
        builder.setupCharacterMap({32: "space", 65: "A", 66: "B", 67: "C", 68: "D"})
        builder.setupHorizontalMetrics({name: (700, 0) for name in order})
        builder.setupHorizontalHeader(ascent=800, descent=-200)
        builder.setupNameTable(dict(familyName=family, styleName="Regular", fullName=family + " Regular",
            uniqueFontIdentifier=family, psName=family + "-Regular"))
        builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
        builder.setupPost()
        glyphs = {}
        for i, name in enumerate(order):
            pen = T2CharStringPen(700, None) if cff else TTGlyphPen(None)
            if name not in (".notdef", "space"):
                pen.moveTo((50, 0))
                pen.lineTo((150 + weight * .35, 700))
                pen.lineTo((550 + custom * 180, 350 + i * 15))
                pen.lineTo((550 + custom * 180, 0))
                pen.closePath()
            glyphs[name] = pen.getCharString() if cff else pen.glyph()
        if cff:
            builder.setupCFF(family + "-Regular", dict(FullName=family, FamilyName=family, Weight="Regular"), glyphs, {})
        else:
            builder.setupGlyf(glyphs)
        source = SourceDescriptor()
        source.name = f"master-{weight}-{custom}"
        source.font = builder.font
        source.location = dict(TEST=custom) if custom_only else dict(wght=weight, TEST=custom)
        if (weight, custom) == (400, 0):
            source.copyInfo = source.copyLib = source.copyFeatures = True
        space.addSource(source)
    font, _, _ = build(space)
    font["name"].setName(family, 25, 3, 1, 0x409)
    # A real named instance, distinct from arbitrary coordinates and the default.
    from fontTools.ttLib.tables._f_v_a_r import NamedInstance
    for subfamily, weight in (("Regular", 400), ("Heavy", 900)):
        if custom_only and weight != 400:
            continue
        inst = NamedInstance()
        inst.subfamilyNameID = font["name"].addName(subfamily)
        inst.postscriptNameID = font["name"].addName(family + "-" + subfamily)
        inst.coordinates = dict(TEST=0) if custom_only else dict(wght=weight, TEST=0)
        font["fvar"].instances.append(inst)
    font.save(path)


def embedded_fonts(resources, seen=None):
    if seen is None:
        seen = set()
    for ref in resources.get("/Font", {}).values():
        font = ref.get_object()
        identity = (getattr(ref, "idnum", id(ref)), getattr(ref, "generation", 0))
        if identity in seen:
            continue
        seen.add(identity)
        for child in font.get("/DescendantFonts", [font]):
            descriptor = child.get_object().get("/FontDescriptor")
            if descriptor:
                desc = descriptor.get_object()
                for kind in ("/FontFile2", "/FontFile3"):
                    if kind in desc:
                        yield kind, desc[kind].get_data()
    for ref in resources.get("/XObject", {}).values():
        obj = ref.get_object()
        if "/Resources" in obj:
            yield from embedded_fonts(obj["/Resources"], seen)


def bounds(glyph):
    pen = BoundsPen(None)
    glyph.draw(pen)
    return pen.bounds


def ink_matches(a, b):
    a, b = (Image.open(p).convert("L") for p in (a, b))
    if a.size != b.size:
        return False
    masks = [im.point(lambda v: 255 if v < 160 else 0) for im in (a, b)]
    for i in (0, 1):
        own = masks[i].histogram()[255]
        diff = ImageChops.subtract(masks[i], masks[1-i].filter(ImageFilter.MaxFilter(5)))
        if not own or diff.histogram()[255] / own > .01:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inkscape", required=True, type=Path)
    ap.add_argument("--dll-dir", type=Path)
    ap.add_argument("--data-dir", type=Path)
    ap.add_argument("--pdftoppm", required=True, type=Path)
    ap.add_argument("--family", choices=("all", "truetype", "cff2", "custom"), default="all")
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fonts = out / "fonts"
    fonts.mkdir()
    families = (("ProofVariableTT", False, False), ("ProofVariableCFF", True, False),
                ("ProofVariableCustom", False, True))
    if args.family != "all":
        families = tuple(row for row in families if row[0] == {
            "truetype": "ProofVariableTT", "cff2": "ProofVariableCFF", "custom": "ProofVariableCustom"}[args.family])
    for family, cff, custom_only in families:
        make_font(fonts / (family + ".otf"), family, cff, custom_only)
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
    checks = []

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(("PASS " if passed else "FAIL ") + name, flush=True)

    def run(label, command):
        p = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        (out / (label + ".log")).write_bytes(p.stdout + p.stderr)
        return p

    for family, cff, custom_only in families:
        instances = [(400, 0), (400, 1), (400, -.625), (400, .25)] if custom_only else INSTANCES
        svg = out / (family + ".svg")
        body = []
        for i, (weight, custom) in enumerate(instances):
            # First two use default/implicit named weight, with no variation declaration.
            settings = f"font-variation-settings:'TEST' {custom};" if custom_only else (
                f"font-weight:{weight};" if i < 2 else f"font-variation-settings:'wght' {weight}, 'TEST' {custom};")
            body.append(f'<text x="20" y="{55+i*65}" style="font-family:{family};font-size:48px;{settings}">ABCD</text>')
        svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="400" height="280">' + ''.join(body) + '</svg>', encoding="utf-8")
        canvas = out / (family + ".png")
        p = run(family + "-canvas", [args.inkscape.resolve(), svg, "--export-background=white", "--export-dpi=144", "--export-filename=" + str(canvas)])
        check(family + ": canvas", p.returncode == 0 and canvas.exists())
        expected_bounds = []
        for weight, custom in instances:
            with TTFont(fonts / (family + ".otf")) as font:
                limits = dict(TEST=custom) if custom_only else dict(wght=weight, TEST=custom)
                frozen = instantiateVariableFont(font, limits, inplace=False)
                expected_bounds.append(bounds(frozen.getGlyphSet()["A"]))
        for kind, ext in (("pdf", "org.inkscape.output.pdf.capypdf"), ("ai", "org.inkscape.output.ai.native")):
            label = family + "-" + kind
            target = out / (label + "." + kind)
            p = run(label, [args.inkscape.resolve(), svg, "--export-extension=" + ext, "--export-filename=" + str(target)])
            saved = p.returncode == 0 and target.exists()
            check(label + ": complete save", saved)
            if not saved:
                continue
            page = PdfReader(target).pages[0]
            check(label + ": searchable text", ''.join(page.extract_text().split()) == "ABCD" * len(instances))
            embedded = list(embedded_fonts(page["/Resources"]))
            check(label + ": four distinct embedded instances", len(embedded) == len(instances))
            got_bounds = []
            for fontkind, blob in embedded:
                # CapyPDF also uses FontFile3/OpenType for an sfnt with TrueType outlines.
                if blob[:4] in (b"\0\1\0\0", b"OTTO"):
                    with TTFont(BytesIO(blob)) as font:
                        check(label + ": embedded TrueType is static", "fvar" not in font and "gvar" not in font)
                        got_bounds.append(bounds(font.getGlyphSet()[font.getGlyphOrder()[1]]))
                else:
                    font = CFFFontSet()
                    font.decompile(BytesIO(blob), None)
                    check(label + ": embedded CFF1 is static", font.major == 1)
                    top = font.topDictIndex[0]
                    got_bounds.append(bounds(top.CharStrings[top.charset[1]]))
            remaining = list(got_bounds)
            for expected in expected_bounds:
                match = next((b for b in remaining if b is not None and max(abs(x-y) for x,y in zip(b, expected)) <= 1), None)
                check(label + ": instance glyph matches fontTools reference", match is not None)
                if match is not None:
                    remaining.remove(match)
            prefix = out / (label + "-render")
            p = run(label + "-render", [args.pdftoppm.resolve(), "-singlefile", "-r", "144", "-png", target, prefix])
            check(label + ": PDF matches canvas", p.returncode == 0 and ink_matches(canvas, prefix.with_suffix(".png")))
            if kind == "ai":
                reopened = out / (label + "-reopened.svg")
                p = run(label + "-reopen", [args.inkscape.resolve(), target, "--export-filename=" + str(reopened)])
                texts = list(ET.parse(reopened).getroot().iter("{http://www.w3.org/2000/svg}text")) if reopened.exists() else []
                check(label + ": four complete editable stories", p.returncode == 0 and len(texts) == len(instances) and
                      all(''.join(t.itertext()).strip() == "ABCD" for t in texts))
                actual = Counter()
                for t in texts:
                    axes = dict((tag, float(value)) for tag,value in re.findall(r"'([A-Za-z0-9 ]{4})'\s+(-?[\d.]+)", t.get("style", "")))
                    actual[(axes.get("wght"), axes.get("TEST"))] += 1
                expected = Counter((None if custom_only else weight, custom) for weight, custom in instances)
                check(label + ": exact editable axes, including implicit named weight", actual == expected)
    (out / "report.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
    print(f"{sum(c['passed'] for c in checks)}/{len(checks)} checks passed. {out / 'report.json'}")
    return int(not all(c["passed"] for c in checks))


if __name__ == "__main__":
    raise SystemExit(main())
