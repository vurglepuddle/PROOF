"""Illustrator 7 export through the editor; synthetic artwork, no desktop input.

Exercises the standalone container, native reopening, page selection and safe failure.
Actual Illustrator opening/editing is separate acceptance, not established by this script.
"""
import argparse
import base64
import hashlib
from html import escape
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import xml.etree.ElementTree as ET


def main():
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
    legacy = "--export-extension=org.inkscape.output.ai.legacy7"
    checks = []

    def check(name, condition):
        checks.append(dict(name=name, passed=bool(condition)))
        print(("PASS " if condition else "FAIL ") + name, flush=True)

    def run(label, source, target, *options):
        result = subprocess.run([exe, str(source), f"--export-filename={target}", *options],
                                env=env, capture_output=True, timeout=120)
        (out / (label + ".log")).write_bytes(result.stdout + result.stderr)
        return result

    def export(label, source, target, *options):
        result = run(label, source, target, *options)
        if result.returncode or not target.is_file():
            raise RuntimeError(f"{label}: exit {result.returncode}; see log")
        return target.read_bytes()

    def document(body, width=200, height=100):
        return '''<svg xmlns="http://www.w3.org/2000/svg"
          xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
          xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
          xmlns:proof="urn:x-proof:document:1" ''' + f'width="{width}" height="{height}" viewBox="0 0 {width} {height}" ' + \
          'proof:color-mode="CMYK">' + body + "</svg>"

    body = '''<defs>
      <clipPath id="clip"><rect x="10" y="50" width="40" height="30"/></clipPath>
      <linearGradient id="ink" inkscape:swatch="solid" inkscape:label="PROOF Orange"
        proof:ink="PROOF Orange" proof:ink-alternate="device-cmyk(0 0.6 1 0)" proof:ink-tint="0.3">
        <stop offset="0" style="stop-color:device-cmyk(0 0.18 0.3 0);stop-opacity:1"/>
      </linearGradient>
      </defs>
      <g inkscape:groupmode="layer" inkscape:label="Art" inkscape:highlight-color="#4fff4f">
        <rect x="10" y="10" width="40" height="30" style="fill:device-cmyk(0.1 0.2 0.3 0.4)"/>
        <path d="M 60 10 H 100 V 40 H 60 Z M 70 20 V 30 H 90 V 20 Z" fill="#2060c0" fill-rule="evenodd"/>
        <g><path d="M 110 10 C 120 0 130 40 140 10" style="fill:none;stroke:device-cmyk(1 0 0 0);stroke-width:2;stroke-dasharray:8 4;stroke-linecap:round"/></g>
        <rect x="150" y="10" width="20" height="30" style="fill:url(#ink);stroke:url(#ink);stroke-width:2" proof:overprint="fill stroke"/>
        <g clip-path="url(#clip)"><rect x="0" y="50" width="80" height="50" fill="#ffff00"/></g>
      </g>
      <g inkscape:groupmode="layer" inkscape:label="Hidden" style="display:none" sodipodi:insensitive="true">
        <rect x="170" y="60" width="20" height="20" fill="#ff0000"/>
      </g>'''
    source = out / "control.svg"
    source.write_text(document(body), encoding="utf-8")
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    ai = out / "control.ai"
    records = export("save-legacy", source, ai, legacy)
    check("standalone PostScript records", records.startswith(b"%!PS-Adobe-3.0\n"))
    check("Illustrator 7 version", b"%AI5_FileFormat 3\n" in records)
    check("single artboard dimensions in points", b"%AI3_Cropmarks: 0 0 150 75\n" in records)
    check("legacy conversion matches the crop artboard", b"%AI3_TemplateBox: 75 37 75 37\n" in records)
    check("legacy layer field count", all(len(line.split()) == 11 for line in records.splitlines() if line.endswith(b" Lb")))
    check("CMYK spot tint uses x and X", b"(PROOF Orange) 0.7 x\n" in records and b"(PROOF Orange) 0.7 X\n" in records)
    # A later advisory colour-model comment keeps newer Illustrator from converting an
    # RGB legacy document to CMYK. Older readers ignore it; no modern artwork is written.
    check("colour-model advisory only, no modern artwork", records.count(b"%AI9_ColorModel: 2\n") == 1
          and not any(x in records.replace(b"%AI9_ColorModel: 2\n", b"")
                      for x in (b"AI9_", b"AI24_", b" Xw", b"AIPrivateData")))

    reopened = out / "reopened.svg"
    export("reopen", ai, reopened)
    root = ET.parse(reopened).getroot()
    svg_ns = "{http://www.w3.org/2000/svg}"
    ink_ns = "{http://www.inkscape.org/namespaces/inkscape}"
    proof_ns = "{urn:x-proof:document:1}"
    layers = [g for g in root.iter(svg_ns + "g") if g.get(ink_ns + "groupmode") == "layer"]
    check("editable named layers", [g.get(ink_ns + "label") for g in layers] == ["Art", "Hidden"])
    check("legacy layer colour", layers[0].get(ink_ns + "highlight-color") == "#4fff4f")
    check("hidden and locked layer", "display:none" in layers[1].get("style", "") and
          layers[1].get("{http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd}insensitive") == "true")
    paths = list(root.iter(svg_ns + "path"))
    check("editable paths and compound hole", len(paths) == 7 and any("evenodd" in p.get("style", "") for p in paths))
    check("clipping retained", any(g.get("clip-path") for g in root.iter()))
    # SVG saving expresses these colours through an embedded CMYK ICC profile.
    # Check the profile's actual colour space and numeric components, not CSS spelling.
    cmyk_profiles = set()
    for profile in root.iter(svg_ns + "color-profile"):
        href = profile.get("{http://www.w3.org/1999/xlink}href", "")
        if href.startswith("data:application/vnd.iccprofile;base64,"):
            data = base64.b64decode(href.split(",", 1)[1], validate=True)
            if len(data) >= 128 and data[16:20] == b"CMYK":
                cmyk_profiles.add(profile.get("name"))

    def cmyk_fill_matches(path, expected):
        fill = next((s for s in path.get("style", "").split(";") if s.strip().startswith("fill:")), "")
        match = re.search(r"icc-color\(([^,]+),([^)]+)\)", fill)
        if not match or match[1].strip() not in cmyk_profiles:
            return False
        values = tuple(float(v.strip()) for v in match[2].split(","))
        return len(values) == 4 and all(abs(a - b) < 1e-6 for a, b in zip(values, expected))

    check("CMYK values retained", any(cmyk_fill_matches(p, (0.1, 0.2, 0.3, 0.4)) for p in paths))
    check("spot identity and tint retained", any(g.get(proof_ns + "ink") == "PROOF Orange" and
          abs(float(g.get(proof_ns + "ink-tint", "1")) - 0.3) < 1e-6 for g in root.iter()))

    # Representable names stay Windows-1252, including pairs that resemble UTF-8.
    named_layer = "Caf\u00e9 \u20ac \u0152 \u0178 \u2013 (A) \\ B"
    ambiguous = "\u00c3\u00a9"
    bom_letters = "\u00fe\u00ff"
    named_ink = "Ink Caf\u00e9 \u20ac (P) \\ Q"
    named_body = f'''<defs>
      <linearGradient id="one" inkscape:swatch="solid" proof:ink="{escape(named_ink, quote=True)}"
        proof:ink-alternate="device-cmyk(0 0.6 1 0)" proof:ink-tint="0.3">
        <stop offset="0" style="stop-color:device-cmyk(0 0.18 0.3 0)"/></linearGradient>
      <linearGradient id="two" inkscape:swatch="solid" proof:ink="{ambiguous}"
        proof:ink-alternate="device-cmyk(1 0.2 0 0)" proof:ink-tint="0.6">
        <stop offset="0" style="stop-color:device-cmyk(0.6 0.12 0 0)"/></linearGradient>
      <linearGradient id="ramp" x1="0" y1="0" x2="1" y2="0">
        <stop offset="0" style="stop-color:device-cmyk(0 1 0 0)"/>
        <stop offset="1" style="stop-color:device-cmyk(1 0 0 0)"/></linearGradient>
      </defs><g inkscape:groupmode="layer" inkscape:label="{escape(named_layer, quote=True)}">
        <rect x="10" y="10" width="60" height="30" fill="url(#one)" stroke="url(#two)" stroke-width="2"/>
        <rect x="80" y="10" width="70" height="30" fill="url(#ramp)"/></g>
      <g inkscape:groupmode="layer" inkscape:label="{ambiguous}">
        <rect x="10" y="50" width="60" height="30" fill="url(#two)"/></g>
      <g inkscape:groupmode="layer" inkscape:label="{bom_letters}">
        <rect x="80" y="50" width="30" height="30" fill="url(#one)"/></g>'''
    named_source = out / "Title Caf\u00e9 \u20ac.svg"
    named_source.write_text(document(named_body), encoding="utf-8")
    named_hash = hashlib.sha256(named_source.read_bytes()).hexdigest()
    named_ai = out / "names.ai"
    named_records = export("names-save", named_source, named_ai, legacy)

    def ps_name(name):
        return b'(' + b''.join(b'\\' + bytes([c]) if c in (40, 41, 92) else
                              ('\\%03o' % c).encode() if c < 32 or c >= 127 else bytes([c])
                              for c in name.encode('cp1252')) + b')'

    check("legacy title uses Windows-1252 octal bytes", b'%%Title: ' + ps_name(named_source.name) + b'\n' in named_records)
    check("legacy layer and ink names use Windows-1252 bytes", all(ps_name(n) in named_records
          for n in (named_layer, named_ink, ambiguous, bom_letters)) and all(c < 128 for c in named_records))
    named_svg = out / "names-reopened.svg"
    export("names-reopen", named_ai, named_svg)
    named_root = ET.parse(named_svg).getroot()
    check("legacy native layers keep distinct logical names", [g.get(ink_ns + "label") for g in named_root.iter(svg_ns + "g")
          if g.get(ink_ns + "groupmode") == "layer"] == [named_layer, ambiguous, bom_letters])
    check("legacy native spot names and tints retained",
          {(g.get(proof_ns + "ink"), round(float(g.get(proof_ns + "ink-tint", "1")), 2))
           for g in named_root.iter() if g.get(proof_ns + "ink")} ==
          {(named_ink, 1), (named_ink, 0.3), (ambiguous, 1), (ambiguous, 0.6)})
    check("named control reopens from records", b"opens from its PDF page" not in (out / "names-reopen.log").read_bytes())
    check("named UTF-8 working document unchanged", hashlib.sha256(named_source.read_bytes()).hexdigest() == named_hash)
    (out / "expected-names.json").write_text(json.dumps(dict(layers=[named_layer, ambiguous, bom_letters],
        inks=[named_ink, ambiguous]), indent=2) + '\n', encoding='utf-8')

    # Illustrator's legacy conversion rounds half the page size for its ruler origin.
    # Exercise odd and fractional dimensions, including a common A4 print page.
    for label, width, height, template in (
            ("odd-size", 151, 76, "76 38 76 38"),
            ("fractional-size", 150.3, 75.2, "76 37.2 76 37.2"),
            ("a4-size", 595.2756, 841.8898, "298 420.8898 298 420.8898")):
        sized_source = out / (label + ".svg")
        sized_source.write_text(document('<rect x="10" y="10" width="20" height="20"/>',
                                         width=width / 0.75, height=height / 0.75), encoding="utf-8")
        sized_ai = out / (label + ".ai")
        sized_records = export(label, sized_source, sized_ai, legacy)
        check(label + ": conversion origin", ("%AI3_TemplateBox: " + template + "\n").encode() in sized_records)
        sized_svg = out / (label + "-reopened.svg")
        export(label + "-reopen", sized_ai, sized_svg)
        sized_root = ET.parse(sized_svg).getroot()
        view = [float(v) for v in sized_root.get("viewBox").split()]
        check(label + ": page dimensions retained", len(view) == 4 and
              abs(view[2] - width) < 1e-4 and abs(view[3] - height) < 1e-4)

    def refused(label, svg, reason, title=None):
        path = out / (title or label + ".svg")
        path.write_text(svg, encoding="utf-8")
        before = ai.read_bytes()
        result = run(label, path, ai, legacy)
        check(label + ": clean failure", result.returncode == 1)
        check(label + ": feature named", reason.encode() in result.stderr)
        check(label + ": previous file preserved", ai.read_bytes() == before)
        target = out / (label + ".ai")
        result = run(label + "-new", path, target, legacy)
        check(label + ": no new file", result.returncode == 1 and not target.exists())

    multipage = document('''<defs><view id="one" viewBox="0 0 100 100"/><view id="two" viewBox="120 0 80 100"/></defs>
      <rect x="10" y="10" width="20" height="20" fill="#ff0000"/>
      <rect x="130" y="10" width="20" height="20" fill="#00ff00"/>''', width=100)
    refused("multiple-artboards", multipage, "more than one artboard")
    selected = out / "selected.ai"
    selected_records = export("selected-page", out / "multiple-artboards.svg", selected, legacy, "--export-page=2")
    check("explicit artboard selection", b"%AI3_Cropmarks: 0 0 60 75\n" in selected_records)
    selected_svg = out / "selected.svg"
    export("selected-reopen", selected, selected_svg)
    selected_paths = list(ET.parse(selected_svg).getroot().iter(svg_ns + "path"))
    check("only the selected artboard's artwork", len(selected_paths) == 1)
    # The first artboard also contains the second: keeping page 2 must preserve
    # its artwork even though it was fully inside the page being removed.
    overlapping = out / "overlapping.svg"
    overlapping.write_text(document('''<defs><view id="one" viewBox="0 0 200 100"/>
      <view id="two" viewBox="120 0 80 100"/></defs>
      <rect x="10" y="10" width="20" height="20" fill="#ff0000"/>
      <rect x="130" y="10" width="20" height="20" fill="#00ff00"/>'''), encoding="utf-8")
    shared_ai = out / "shared.ai"
    export("overlapping-page", overlapping, shared_ai, legacy, "--export-page=2")
    shared_svg = out / "shared.svg"
    export("shared-reopen", shared_ai, shared_svg)
    shared_paths = list(ET.parse(shared_svg).getroot().iter(svg_ns + "path"))
    check("shared artwork survives overlapping artboard selection", len(shared_paths) == 1 and
          "#00ff00" in shared_paths[0].get("style", ""))
    refused("live-type", document('<text x="10" y="20">Keep live type</text>'), "type")
    refused("hidden-object", document('<rect width="20" height="20" style="display:none"/>'), "hidden objects")
    refused("unicode-layer", document('<g inkscape:groupmode="layer" inkscape:label="&#20013;"><rect width="20" height="20"/></g>'),
            "layer names outside Windows-1252")
    refused("unicode-ink", document(named_body.replace(escape(named_ink, quote=True), "&#20013;")),
            "ink names outside Windows-1252")
    refused("unicode-title", document('<rect width="20" height="20"/>'),
            "document titles outside Windows-1252", title="\u4e2d.svg")
    broad = out / "unicode-layer.svg"
    modern_ai, modern_svg = out / "modern-names.ai", out / "modern-names.svg"
    export("modern-broad-names", broad, modern_ai, "--export-extension=org.inkscape.output.ai.native")
    export("modern-broad-reopen", modern_ai, modern_svg)
    check("modern Unicode layer names remain supported", any(g.get(ink_ns + "label") == "\u4e2d"
          for g in ET.parse(modern_svg).getroot().iter(svg_ns + "g")))

    edited = out / "edited.svg"
    edited.write_text(document(body.replace("0.1 0.2 0.3 0.4", "0.4 0.3 0.2 0.1")), encoding="utf-8")
    updated = export("overwrite", edited, ai, legacy)
    check("overwrite contains the edit", b"0.4 0.3 0.2 0.1 k\n" in updated and b"0.1 0.2 0.3 0.4 k\n" not in updated)
    folder = out / "destination.ai"
    folder.mkdir()
    result = run("directory-destination", source, folder, legacy)
    check("directory replacement fails safely", result.returncode == 1 and folder.is_dir())
    if os.name == "nt":
        readonly = out / "readonly.ai"
        readonly.write_bytes(b"previous read-only file")
        readonly.chmod(stat.S_IREAD)
        try:
            result = run("readonly", source, readonly, legacy)
            check("read-only file preserved", result.returncode == 1 and readonly.read_bytes() == b"previous read-only file")
        finally:
            readonly.chmod(stat.S_IWRITE | stat.S_IREAD)
    check("temporary files cleaned", not list(out.glob("*.proof-*")))
    check("working SVG unchanged", hashlib.sha256(source.read_bytes()).hexdigest() == source_hash)
    report = dict(checks=checks, passed=sum(c["passed"] for c in checks), total=len(checks))
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f'{report["passed"]}/{report["total"]} checks passed. {out / "report.json"}')
    return int(report["passed"] != report["total"])


if __name__ == "__main__":
    raise SystemExit(main())
