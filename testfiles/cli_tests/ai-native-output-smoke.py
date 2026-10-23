"""Native Illustrator Save As through the installed editor; synthetic art only.

Requires pypdf, Pillow and pdftoppm. Actual Illustrator opening is checked separately.
"""
import argparse
import hashlib
import json
import os
import re
import stat
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
    native = "--export-extension=org.inkscape.output.ai.native"
    checks = []

    def check(name, condition, detail=""):
        checks.append(dict(name=name, passed=bool(condition), detail=detail))
        print(("PASS " if condition else "FAIL ") + name, flush=True)

    def run(label, command, success=True):
        result = subprocess.run([str(c) for c in command], env=env, capture_output=True, timeout=240)
        (out / (label + ".log")).write_bytes(result.stdout + result.stderr)
        if success and result.returncode:
            raise RuntimeError(f"{label}: exit {result.returncode}; see log")
        return result

    def export(label, source, target, *options):
        return run(label, [exe, source, f"--export-filename={target}", *options])

    def render(pdf, label, page):
        prefix = out / label
        run(label, [args.pdftoppm.resolve(), "-f", page, "-l", page, "-singlefile",
                    "-scale-to", "800", "-png", pdf, prefix])
        with Image.open(prefix.with_suffix(".png")) as im:
            return im.convert("RGB")

    svg = '''<svg xmlns="http://www.w3.org/2000/svg"
      xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
      xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
      xmlns:proof="urn:x-proof:document:1" width="360" height="220" viewBox="0 0 360 220"
      proof:color-mode="CMYK">
      <defs>
        <view id="front" viewBox="0 0 360 220" inkscape:label="Front"/>
        <view id="back" viewBox="400 0 200 220" inkscape:label="Обратная"/>
        <clipPath id="c"><circle cx="60" cy="160" r="35"/></clipPath>
        <linearGradient id="ink" inkscape:swatch="solid" inkscape:label="PROOF Orange"
          proof:ink="PROOF Orange" proof:ink-alternate="device-cmyk(0 0.6 1 0)" proof:ink-tint="0.3">
          <stop offset="0" style="stop-color:device-cmyk(0 0.18 0.3 0);stop-opacity:1"/>
        </linearGradient>
      </defs>
      <g inkscape:groupmode="layer" inkscape:label="Process" inkscape:highlight-color="#4fff4f">
        <rect x="20" y="20" width="80" height="60"
          style="fill:device-cmyk(0 0.9 0.2 0);stroke:device-cmyk(0 0 0 1);stroke-width:3"/>
        <path d="M 120 20 H 200 V 80 H 120 Z M 140 35 V 65 H 180 V 35 Z"
          style="fill:#2060c0;fill-rule:evenodd"/>
        <path d="M 220 20 C 260 0 300 60 340 30"
          style="fill:none;stroke:device-cmyk(1 0 0 0);stroke-width:4;stroke-dasharray:12 6;stroke-linecap:round"/>
        <g clip-path="url(#c)">
          <rect x="20" y="120" width="80" height="80" style="fill:device-cmyk(0 0 1 0)"/>
          <rect x="60" y="120" width="40" height="80" style="fill:device-cmyk(0 0 0 0.6)"/>
        </g>
      </g>
      <g inkscape:groupmode="layer" inkscape:label="Back">
        <rect x="440" y="40" width="120" height="140" style="fill:device-cmyk(0.5 0 0.5 0)"/>
        <rect x="440" y="185" width="120" height="20" style="fill:url(#ink)"/>
      </g>
      <g inkscape:groupmode="layer" inkscape:label="Hidden" style="display:none" sodipodi:insensitive="true">
        <rect x="300" y="150" width="20" height="20" fill="#ff0000"/>
      </g>
    </svg>'''
    source = out / "control.svg"
    source.write_text(svg, encoding="utf-8")
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    ai = out / "control.ai"
    export("save-native", source, ai, native)
    check("native file exists", ai.is_file() and ai.stat().st_size > 100)
    reader = PdfReader(ai, strict=True)
    check("Illustrator-compatible PDF version", reader.pdf_header == "%PDF-1.7")
    check("two PDF pages", len(reader.pages) == 2)
    check("PDF page sizes", [list(p.mediabox) for p in reader.pages] == [[0, 0, 270, 165], [0, 0, 150, 165]])
    holders = [p["/PieceInfo"].raw_get("/Illustrator") for p in reader.pages]
    check("pages share Illustrator data", holders[0] == holders[1])
    private = holders[0].get_object()["/Private"]
    packed = b"".join(private[f"/AIPrivateData{i}"].get_data() for i in range(1, private["/NumBlock"] + 1))
    check("Zstandard records", packed.startswith(b"%AI24_ZStandard_Data\x28\xb5\x2f\xfd"))
    check("PROOF metadata", b"%%Creator: PROOF" in private["/AIMetaData"].get_data())
    check("modern container version", private["/ContainerVersion"] == 12 and private["/RoundtripVersion"] == 24)

    reopened = out / "reopened.svg"
    export("reopen", ai, reopened, "--pages=all")
    root = ET.parse(reopened).getroot()
    ns = {"s": "http://www.w3.org/2000/svg", "i": "http://www.inkscape.org/namespaces/inkscape"}
    attr = lambda n: "{" + ns["i"] + "}" + n
    layers = [g for g in root.iter("{" + ns["s"] + "}g") if g.get(attr("groupmode")) == "layer"]
    check("editable layers from records", [g.get(attr("label")) for g in layers] == ["Process", "Back", "Hidden"])
    check("hidden and locked layer", "display:none" in layers[-1].get("style", "") and
          layers[-1].get("{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}insensitive") == "true")
    views = list(root.iter("{" + ns["s"] + "}view"))
    check("named artboards from records", [v.get(attr("label")) for v in views] == ["Front", "Обратная"])
    paths = list(root.iter("{" + ns["s"] + "}path"))
    check("editable paths and clip", len(paths) == 9 and any(g.get("clip-path") for g in root.iter()))
    inks = [g for g in root.iter() if g.get("{urn:x-proof:document:1}ink") == "PROOF Orange"]
    check("spot identity and tint retained", any(
        abs(float(ink.get("{urn:x-proof:document:1}ink-tint", "1")) - 0.3) < 1e-6 for ink in inks))
    def has_cmyk(path, channels):
        text = path.read_text(encoding="utf-8")
        values_list = re.findall(r"device-cmyk\(([^)]+)\)", text)
        # Native import assigns the document's CMYK profile and keeps its four
        # original channels in icc-color; the RGB prefix is a display fallback.
        values_list += re.findall(r"icc-color\([^,]+,([^)]*)\)", text)
        for values in values_list:
            numbers = [float(v) for v in re.split(r"[,\s]+", values.strip())]
            if len(numbers) == 4 and all(abs(a - b) < 1e-6 for a, b in zip(numbers, channels)):
                return True
        return False

    check("CMYK channels retained", has_cmyk(reopened, [0, 0.9, 0.2, 0]))

    pdf = out / "reference.pdf"
    export("reference-pdf", source, pdf, "--export-extension=org.inkscape.output.pdf.capypdf")
    check("ordinary PDF keeps its default version", PdfReader(pdf, strict=True).pdf_header == "%PDF-2.0")
    for page in (1, 2):
        expected = render(pdf, f"reference-{page}", page)
        actual = render(ai, f"native-{page}", page)
        check(f"PDF page {page} matches document", expected.size == actual.size and
              ImageChops.difference(expected, actual).getbbox() is None)

    # Selecting a page must select both its PDF page and its records.
    selected = out / "second.ai"
    export("selected-page", source, selected, native, "--export-page=2")
    check("selected PDF page", len(PdfReader(selected, strict=True).pages) == 1)
    selected_svg = out / "second.svg"
    export("reopen-selected", selected, selected_svg)
    selected_views = list(ET.parse(selected_svg).getroot().iter("{" + ns["s"] + "}view"))
    check("selected native artboard", len(selected_views) == 1 and selected_views[0].get(attr("label")) == "Обратная")

    # A second save replaces both halves, including an existing destination.
    edited = out / "edited.svg"
    edited.write_text(svg.replace("0 0.9 0.2 0", "0.1 0.2 0.3 0.4"), encoding="utf-8")
    before = ai.read_bytes()
    export("overwrite", edited, ai, native)
    check("existing file replaced", ai.read_bytes() != before)
    export("reopen-overwritten", ai, out / "overwritten.svg")
    check("new records replace previous artwork", has_cmyk(out / "overwritten.svg", [0.1, 0.2, 0.3, 0.4]) and
          not has_cmyk(out / "overwritten.svg", [0, 0.9, 0.2, 0]))
    updated_pdf = out / "edited.pdf"
    export("edited-pdf", edited, updated_pdf, "--export-extension=org.inkscape.output.pdf.capypdf")
    check("new PDF replaces previous artwork", ImageChops.difference(
        render(updated_pdf, "edited-reference", 1), render(ai, "overwritten-page", 1)).getbbox() is None)

    # Point type is written as Illustrator's stories, and reopens as editable type.
    typed = out / "typed.svg"
    typed.write_text(svg.replace("</svg>", '<text x="10" y="30" style="font-family:Arial;font-size:12px">'
                                           'Live type</text></svg>'), encoding="utf-8")
    typed_ai, typed_svg = out / "typed.ai", out / "typed-reopened.svg"
    export("typed", typed, typed_ai, native)
    export("typed-reopen", typed_ai, typed_svg)
    stories = [e for e in ET.parse(typed_svg).getroot().iter("{http://www.w3.org/2000/svg}text")
               if e.get("{urn:x-proof:document:1}ai-story") is not None]
    check("point type reopens as a native story", len(stories) == 1 and "".join(stories[0].itertext()) == "Live type")

    # Type in a shape is written as Illustrator's area type, and reopens in its frame.
    framed = out / "framed.svg"
    framed.write_text(svg.replace("</defs>", '<rect id="area" x="10" y="10" width="120" height="60"/></defs>').replace(
        "</svg>", '<text xml:space="preserve" style="shape-inside:url(#area);white-space:pre;font-family:Arial;'
                  'font-size:12px">Area type that wraps in its frame</text></svg>'), encoding="utf-8")
    framed_ai, framed_svg = out / "framed.ai", out / "framed-reopened.svg"
    export("framed", framed, framed_ai, native)
    export("framed-reopen", framed_ai, framed_svg)
    areas = [e for e in ET.parse(framed_svg).getroot().iter("{http://www.w3.org/2000/svg}text")
             if e.get("{urn:x-proof:document:1}text-frame") is not None]
    check("type in a shape reopens as area type", len(areas) == 1 and
          "".join(areas[0].itertext()) == "Area type that wraps in its frame" and
          len(list(areas[0].iter("{http://www.w3.org/2000/svg}tspan"))) > 1)

    # Type on a path is written as Illustrator's path type, and reopens on its path between brackets.
    pathed = out / "pathed.svg"
    pathed.write_text(svg.replace("</defs>", '<path id="along" d="M10,80 C40,40 90,40 120,80"/></defs>').replace(
        "</svg>", '<text style="font-family:Arial;font-size:12px;text-anchor:middle">'
                  '<textPath href="#along" startOffset="50%">Path type</textPath></text></svg>'), encoding="utf-8")
    pathed_ai, pathed_svg = out / "pathed.ai", out / "pathed-reopened.svg"
    export("pathed", pathed, pathed_ai, native)
    export("pathed-reopen", pathed_ai, pathed_svg)
    on_paths = list(ET.parse(pathed_svg).getroot().iter("{http://www.w3.org/2000/svg}textPath"))
    check("type on a path reopens as path type", len(on_paths) == 1 and
          "".join(on_paths[0].itertext()) == "Path type" and
          on_paths[0].get("{urn:x-proof:document:1}path-end") is not None and
          "text-anchor:middle" in (on_paths[0].get("style") or ""))

    # An import's tabs keep their ruler; a centred line ending in a space stays centred, as
    # Illustrator centres it, with the space.
    tabbed = out / "tabbed.svg"
    tabbed.write_text(svg.replace("</svg>", '<text xml:space="preserve" style="font-family:Arial;font-size:12px">'
                                           '<tspan x="10" y="30" proof:tab-origin="10" proof:tab-interval="40" '
                                           'proof:tab-stops="25">A\tB\tC</tspan></text>'
                                           '<text xml:space="preserve" x="70" y="60" style="font-family:Arial;font-size:12px;'
                                           'text-anchor:middle">Centred </text></svg>'), encoding="utf-8")
    tabbed_ai, tabbed_svg = out / "tabbed.ai", out / "tabbed-reopened.svg"
    export("tabbed", tabbed, tabbed_ai, native)
    export("tabbed-reopen", tabbed_ai, tabbed_svg)
    reopened = [e for e in ET.parse(tabbed_svg).getroot().iter("{http://www.w3.org/2000/svg}text")
                if e.get("{urn:x-proof:document:1}ai-story") is not None]
    # The native file opens in points: 40 and 25 px are 30 and 18.75 pt.
    ruled = [s for e in reopened for s in e.iter("{http://www.w3.org/2000/svg}tspan")
             if abs(float(s.get("{urn:x-proof:document:1}tab-interval") or "nan") - 30) < 1e-6 and
             abs(float(s.get("{urn:x-proof:document:1}tab-stops") or "nan") - 18.75) < 1e-6]
    centred = [e for e in reopened if "".join(e.itertext()) == "Centred "]
    check("tabs reopen with their ruler, a centred line with its space", len(reopened) == 2 and len(ruled) == 1 and
          "".join(ruled[0].itertext()) == "A\tB\tC" and len(centred) == 1 and
          centred[0].get("{urn:x-proof:document:1}line-alignment") == "illustrator" and
          any("text-anchor:middle" in (s.get("style") or "") for s in centred[0].iter("{http://www.w3.org/2000/svg}tspan")))

    unsupported = out / "unsupported.svg"
    unsupported.write_text(svg.replace("</svg>", '<text x="10" y="30" style="writing-mode:vertical-rl">Live type</text></svg>'),
                           encoding="utf-8")
    kept = ai.read_bytes()
    refused = run("refusal", [exe, unsupported, f"--export-filename={ai}", native], success=False)
    check("refusal returns failure without crash", refused.returncode == 1)
    check("refusal names unsupported feature", b"type" in refused.stderr and b"Nothing was saved" in refused.stderr)
    check("refusal preserves previous file", ai.read_bytes() == kept)
    absent = out / "refused.ai"
    run("refusal-new", [exe, unsupported, f"--export-filename={absent}", native], success=False)
    check("refusal creates no file", not absent.exists())
    per_page = out / "refused-page.ai"
    result = run("refusal-page", [exe, unsupported, f"--export-filename={per_page}", native,
                                  "--export-page=1"], success=False)
    check("selected-page refusal returns failure", result.returncode == 1 and not per_page.exists())
    # Numbers that overflow are refused by name. Before, they were written "inf" and "nan".
    for name, art, why in (
            ("coordinates", '<path d="M0 0 L10 10 L0 10 Z" fill="#000" transform="scale(1e308)"/>',
             b"nonfinite path coordinates"),
            ("stroke", '<path d="M0 0 L10 10" stroke="#000" stroke-width="1e308" transform="scale(10)"/>',
             b"nonfinite stroke widths or dashes")):
        overflow = out / f"overflow-{name}.svg"
        overflow.write_text(svg.replace("</svg>", art + "</svg>"), encoding="utf-8")
        absent = out / f"overflow-{name}.ai"
        result = run(f"refusal-overflow-{name}", [exe, overflow, f"--export-filename={absent}", native], success=False)
        check(f"overflowing {name} refused by name", result.returncode == 1 and why in result.stderr and
              not absent.exists())
    directory = out / "directory.ai"
    directory.mkdir()
    result = run("failed-replace", [exe, source, f"--export-filename={directory}", native], success=False)
    check("failed replacement returns failure", result.returncode == 1 and directory.is_dir())
    if os.name == "nt":
        readonly = out / "readonly.ai"
        readonly.write_bytes(kept)
        readonly.chmod(stat.S_IREAD)
        try:
            result = run("readonly", [exe, source, f"--export-filename={readonly}", native], success=False)
            check("read-only file returns failure without crash", result.returncode == 1 and
                  b"read only" in result.stderr)
            check("read-only file preserved", readonly.read_bytes() == kept)
        finally:
            readonly.chmod(stat.S_IREAD | stat.S_IWRITE)
    check("no temporary files left", not list(out.glob("*.proof-*")))
    check("source unchanged", hashlib.sha256(source.read_bytes()).hexdigest() == source_hash)
    (out / "report.json").write_text(json.dumps(checks, ensure_ascii=False, indent=2), encoding="utf-8")
    passed = sum(c["passed"] for c in checks)
    print(f"{passed}/{len(checks)} checks passed. {out / 'report.json'}")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
