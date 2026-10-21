"""Registration paint through modern and Illustrator 7 native AI exports.

Synthetic art only; no desktop input. Modern native export checks PDF's reserved
/All Separation, native Registration records and editable paint/tint/overprint
reopening. Illustrator 7 must refuse Registration safely while continuing to
write ordinary spot inks. Actual Illustrator acceptance is checked separately.
Requires pypdf and zstd (normally supplied by --dll-dir).
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from pypdf import PdfReader
from pypdf.generic import ContentStream


SVG = "{http://www.w3.org/2000/svg}"
PROOF = "{urn:x-proof:document:1}"
HEAD = '''<svg xmlns="http://www.w3.org/2000/svg"
  xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
  xmlns:proof="urn:x-proof:document:1" width="320" height="180"
  viewBox="0 0 320 180" proof:color-mode="CMYK">'''


def swatch(identifier, name, tint=1, alternate="device-cmyk(1 1 1 1)", label=None):
    label = label if label is not None else name
    components = re.fullmatch(r"device-cmyk\(([^)]+)\)", alternate)
    shown = ("device-cmyk(" + " ".join(f"{float(v) * tint:g}" for v in components[1].split()) + ")"
             if components else "#000000")
    return f'''<linearGradient id="{identifier}" inkscape:swatch="solid"
      inkscape:label="{label}" proof:ink="{name}"
      proof:ink-alternate="{alternate}" proof:ink-tint="{tint}">
      <stop offset="0" style="stop-color:{shown};stop-opacity:1"/>
      </linearGradient>'''


def control(tint=.35, registration=True):
    # A display label is not an ink's identity. Case and trailing spaces are also
    # significant: only the exact reserved name All denotes Registration.
    neutral = "All" if registration else "PROOF Neutral"
    definitions = (swatch("full", neutral) + swatch("tint", neutral, tint) +
                   swatch("line", neutral, .6) +
                   swatch("orange", "PROOF Orange", .3, "device-cmyk(0 .6 1 0)", "[Registration]") +
                   swatch("lower", "all", .4, "device-cmyk(0 .6 1 0)") +
                   swatch("spaced", "All ", .5, "device-cmyk(0 .6 1 0)"))
    return HEAD + "<defs>" + definitions + '''</defs>
      <rect x="10" y="10" width="60" height="50" fill="url(#full)"/>
      <rect x="110" y="10" width="60" height="50" fill="url(#tint)"
        stroke="url(#line)" stroke-width="6" proof:overprint="fill stroke"/>
      <path d="M210 10H280V60H210Z" fill="none" stroke="url(#full)"
        stroke-width="6" proof:overprint="stroke"/>
      <rect x="10" y="100" width="60" height="50" fill="url(#orange)"/>
      <rect x="110" y="100" width="60" height="50" fill="url(#lower)"/>
      <rect x="210" y="100" width="60" height="50" fill="url(#spaced)"/>
      <rect x="290" y="100" width="15" height="50" fill="device-cmyk(.1 .2 .3 .4)"/>
      </svg>'''


def gradient(name):
    return HEAD + f'''<defs><linearGradient id="ramp">
      <stop offset="0" proof:ink="{name}" proof:ink-alternate="device-cmyk(1 1 1 1)"
        proof:ink-tint=".3" style="stop-color:device-cmyk(.3 .3 .3 .3)"/>
      <stop offset="1" style="stop-color:device-cmyk(0 0 0 0)"/>
      </linearGradient></defs>
      <rect x="10" y="10" width="100" height="80" fill="url(#ramp)"/></svg>'''


def paint(element, key):
    style = dict(kv.split(":", 1) for kv in element.get("style", "").split(";") if ":" in kv)
    return style.get(key, element.get(key, ""))


def used_inks(root):
    definitions = {e.get("id"): e for e in root.iter() if e.get(PROOF + "ink")}
    used = Counter()
    for path in root.iter(SVG + "path"):
        overprint = tuple(sorted(path.get(PROOF + "overprint", "").split()))
        for role in ("fill", "stroke"):
            match = re.fullmatch(r"url\(#([^)]+)\)", paint(path, role))
            if match and match[1] in definitions:
                ink = definitions[match[1]]
                used[(ink.get(PROOF + "ink"), round(float(ink.get(PROOF + "ink-tint", "1")), 6),
                      role, overprint)] += 1
    return used


def expected_inks(tint, registration):
    neutral = "All" if registration else "PROOF Neutral"
    return Counter({(neutral, 1, "fill", ()): 1,
                    (neutral, tint, "fill", ("fill", "stroke")): 1,
                    (neutral, .6, "stroke", ("fill", "stroke")): 1,
                    (neutral, 1, "stroke", ("stroke",)): 1,
                    ("PROOF Orange", .3, "fill", ()): 1,
                    ("all", .4, "fill", ()): 1,
                    ("All ", .5, "fill", ()): 1})


def pdf_registration(reader):
    """Inspect used page/Form resources and colour operators, without rendering.

    Page art can live in Form XObjects. Follow only invoked forms, inheriting
    resources when a form omits them. Resource names alone do not establish a
    Registration separation; resolve the underlying colour-space definition.
    """
    spaces, tints = [], set()

    def inspect(stream, resources, active):
        resources = resources.get_object()
        colors = resources.get("/ColorSpace", {}).get_object() if "/ColorSpace" in resources else {}
        selected = {"fill": None, "stroke": None}
        stack = []
        for operands, operator in ContentStream(stream, reader).operations:
            if operator == b"q":
                stack.append(dict(selected))
            elif operator == b"Q" and stack:
                selected = stack.pop()
            elif operator in (b"cs", b"CS"):
                role = "fill" if operator == b"cs" else "stroke"
                space = colors.get(operands[0])
                if space is not None:
                    space = space.get_object()
                if isinstance(space, list) and len(space) == 4 and space[0] == "/Separation":
                    selected[role] = str(space[1])
                    if space[1] == "/All":
                        spaces.append(space)
                else:
                    selected[role] = None
            elif operator in (b"scn", b"SCN"):
                role = "fill" if operator == b"scn" else "stroke"
                if selected[role] == "/All" and len(operands) == 1:
                    tints.add((role, round(float(operands[0]), 6)))
            elif operator == b"Do":
                objects = resources["/XObject"] if "/XObject" in resources else {}
                form = objects.get(operands[0])
                if form is None:
                    continue
                key = (form.idnum, form.generation) if hasattr(form, "idnum") else id(form)
                form = form.get_object()
                if form.get("/Subtype") == "/Form" and key not in active:
                    inspect(form, form.get("/Resources", resources), active | {key})

    for page in reader.pages:
        inspect(page.get_contents(), page["/Resources"], set())
    return spaces, tints


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inkscape", required=True, type=Path)
    ap.add_argument("--dll-dir", required=True, type=Path)
    ap.add_argument("--pdftoppm", type=Path, help=argparse.SUPPRESS)
    ap.add_argument("--data-dir", type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LANG="C", LC_ALL="C")
    env["PATH"] = str(args.dll_dir.resolve()) + os.pathsep + env["PATH"]
    if args.data_dir:
        env["INKSCAPE_DATADIR"] = str(args.data_dir.resolve())
    for key in ("INKSCAPE_PROFILE_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        folder = out / key.lower()
        folder.mkdir()
        env[key] = str(folder)
    exe = args.inkscape.resolve()
    checks, sources = [], {}

    def check(name, passed, detail=""):
        checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(("PASS " if passed else "FAIL ") + name, flush=True)

    def source(name, text):
        path = out / (name + ".svg")
        path.write_text(text, encoding="utf-8")
        sources[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        return path

    def run(label, source, target, extension=None, success=True):
        command = [str(exe), str(source), f"--export-filename={target}"]
        if extension:
            command.append("--export-extension=org.inkscape.output.ai." + extension)
        result = subprocess.run(command, env=env, capture_output=True, timeout=180)
        (out / (label + ".log")).write_bytes(result.stdout + result.stderr)
        if success and (result.returncode or not target.is_file()):
            raise RuntimeError(f"{label}: exit {result.returncode}; see log")
        return result

    def records(label, path, legacy):
        data = path.read_bytes()
        if legacy:
            check(label + ": standalone Illustrator 7", data.startswith(b"%!PS-Adobe-3.0\n") and
                  b"%AI5_FileFormat 3\n" in data and b"AIPrivateData" not in data)
            return data
        reader = PdfReader(path, strict=True)
        check(label + ": modern PDF 1.7 container", reader.pdf_header == "%PDF-1.7" and len(reader.pages) == 1)
        private = reader.pages[0]["/PieceInfo"]["/Illustrator"]["/Private"]
        packed = b"".join(private[f"/AIPrivateData{i}"].get_data() for i in range(1, private["/NumBlock"] + 1))
        marker = b"%AI24_ZStandard_Data"
        if not packed.startswith(marker):
            raise RuntimeError(label + ": missing Zstandard records")
        decoded = subprocess.run([str(args.dll_dir.resolve() / "zstd.exe"), "-d", "-c"],
                                 input=packed[len(marker):], capture_output=True, timeout=60, check=True)
        (out / (label + "-records.ps")).write_bytes(decoded.stdout)
        return decoded.stdout

    def inspect(label, path, legacy, tint):
        native = records(label, path, legacy)
        if legacy:
            paints = re.findall(rb"^1 1 1 1 \(PROOF Neutral\) ([\d.]+) (x|X)$", native, re.M)
            fill_op, stroke_op = "x", "X"
        else:
            paints = re.findall(rb"^1 1 1 1 \(\[Registration\]\) ([\d.]+) (Xs|XS)$", native, re.M)
            fill_op, stroke_op = "Xs", "XS"
        check(label + ": native fill and stroke tints",
              {(round(1 - float(t), 6), op.decode()) for t, op in paints} ==
              {(1, fill_op), (tint, fill_op), (.6, stroke_op), (1, stroke_op)}, str(paints))
        if legacy:
            check(label + ": no unqualified Registration records", b"([Registration])" not in native and
                  not re.search(rb" (Xs|XS)\n", native))
        declarations, active = [], False
        for line in native.splitlines():
            if line.startswith((b"%%DocumentCustomColors:", b"%%CMYKCustomColor:")):
                active = True
            elif not line.startswith(b"%%+"):
                active = False
            if active:
                declarations.append(line)
        check(label + ": Registration excluded from custom-ink declarations",
              declarations and not any(b"(All)" in line or b"([Registration])" in line for line in declarations))
        check(label + ": unrelated ink identities remain ordinary",
              all(name in b"\n".join(declarations) for name in (b"(PROOF Orange)", b"(all)", b"(All )")))
        check(label + ": native fill and stroke overprint", b"1 O\n" in native and b"1 R\n" in native)
        if not legacy:
            spaces, tints = pdf_registration(PdfReader(path, strict=True))
            check(label + ": PDF uses reserved All separation", bool(spaces) and all(s[2] == "/DeviceCMYK" for s in spaces))
            functions = [s[3].get_object() for s in spaces]
            # CapyPDF currently represents separation tint transforms as Type 4
            # calculator functions. Duplicating one tint three times gives four
            # identical process channels, including paper at 0 and full ink at 1.
            check(label + ": PDF Registration tint transform",
                  bool(functions) and all(f.get("/FunctionType") == 4 and list(f.get("/Domain", [])) == [0, 1] and
                  list(f.get("/Range", [])) == [0, 1] * 4 and f.get_data().split() == [b"{", b"dup", b"dup", b"dup", b"}"]
                  for f in functions))
            check(label + ": PDF Registration fill and stroke tints",
                  tints == {("fill", 1), ("fill", tint), ("stroke", .6), ("stroke", 1)}, str(sorted(tints)))
        reopened = out / (label + "-reopened.svg")
        result = run(label + "-reopen", path, reopened)
        root = ET.parse(reopened).getroot()
        check(label + ": editable native reopen", b"opens from its PDF page" not in result.stdout + result.stderr and
              len(list(root.iter(SVG + "path"))) == 7 and not list(root.iter(SVG + "image")))
        actual = used_inks(root)
        check(label + ": paint identity, tint and overprint retained", actual == expected_inks(tint, not legacy), str(actual))

    original = source("control", control())
    edited = source("edited", control(.25))
    ordinary = source("legacy-control", control(registration=False))
    ordinary_edited = source("legacy-edited", control(.25, registration=False))
    refusal_sources = []
    for label, name, alternate in (
            ("alias", "[Registration]", "device-cmyk(1 1 1 1)"),
            ("noncanonical-cmyk", "All", "device-cmyk(0 0 0 1)"),
            ("rgb", "All", "#000000"),
            ("lab", "All", "lab(50 0 0)")):
        text = HEAD + "<defs>" + swatch("ink", name, .5, alternate) + \
               '</defs><rect x="10" y="10" width="100" height="80" fill="url(#ink)"/></svg>'
        refusal_sources.append((label, source(label, text), b"Registration"))
    refusal_sources += [("gradient-all", source("gradient-all", gradient("All")), b"Registration in gradients"),
                        ("gradient-alias", source("gradient-alias", gradient("[Registration]")), b"Registration in gradients")]
    for extension in ("native", "legacy7"):
        label = "modern" if extension == "native" else "legacy7"
        legacy = extension == "legacy7"
        target = out / (label + ".ai")
        run(label + "-save", ordinary if legacy else original, target, extension)
        inspect(label, target, legacy, .35)
        if not legacy:
            direct_pdf = out / "registration.pdf"
            run("standalone-pdf", original, direct_pdf)
            spaces, tints = pdf_registration(PdfReader(direct_pdf, strict=True))
            check("standalone PDF retains Registration fill and stroke tints",
                  bool(spaces) and all(s[2] == "/DeviceCMYK" for s in spaces) and
                  tints == {("fill", 1), ("fill", .35), ("stroke", .6), ("stroke", 1)})
        before = target.read_bytes()
        run(label + "-overwrite", ordinary_edited if legacy else edited, target, extension)
        check(label + ": successful save replaces destination", target.read_bytes() != before)
        inspect(label + "-edited", target, legacy, .25)
        refusals = refusal_sources + ([("registration", original, b"Registration ink (Illustrator 7)")]
                                       if legacy else [])
        for problem, path, reason in refusals:
            name = label + "-refuse-" + problem
            kept = target.read_bytes()
            result = run(name, path, target, extension, success=False)
            check(name + ": clean named refusal", result.returncode == 1 and reason in result.stderr and
                  b"Nothing was saved" in result.stderr)
            check(name + ": previous file preserved", target.read_bytes() == kept)
            absent = out / (name + ".ai")
            result = run(name + "-new", path, absent, extension, success=False)
            check(name + ": no new destination", result.returncode == 1 and not absent.exists())
    check("all synthetic SVG sources unchanged", all(hashlib.sha256((out / n).read_bytes()).hexdigest() == sha
                                                     for n, sha in sources.items()))
    check("no temporary output files left", not list(out.glob("*.proof-*")))
    report = dict(checks=checks, sources=sources, passed=sum(c["passed"] for c in checks), total=len(checks))
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f'{report["passed"]}/{report["total"]} checks passed. {out / "report.json"}')
    return int(report["passed"] != report["total"])


if __name__ == "__main__":
    raise SystemExit(main())
