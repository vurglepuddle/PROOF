"""Blend modes in groups, on the canvas and in the exported PDF (requires Pillow, no Illustrator).

A grey set to Multiply over a grey page, written into a plain group, a layer, a clipped
group and so on. Each drawing is exported twice with the built editor, as a PNG (what the
canvas shows) and as a PDF (what prints), and the middle of each is read. Where the shade
should darken the page the middle is 64; where the group is on its own it stays 128.

See doc/PROOF-clip-blending.md.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

from PIL import Image

GREY, DARK = 128, 64
HEAD = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
        'width="100" height="100" viewBox="0 0 100 100"><defs><clipPath id="c"><rect x="20" y="20" width="60" '
        'height="60"/></clipPath></defs><rect width="100" height="100" fill="#808080"/>')
SHADE = '<rect width="100" height="100" fill="#808080" style="mix-blend-mode:multiply"/>'
# Name, what is drawn over the grey page, the level expected in the middle and in a corner.
CASES = [
    ("shape", SHADE, DARK, DARK),
    ("plain group", "<g>" + SHADE + "</g>", DARK, DARK),
    ("layer", '<g inkscape:groupmode="layer" inkscape:label="Shading">' + SHADE + "</g>", DARK, DARK),
    ("group in a layer", '<g inkscape:groupmode="layer" inkscape:label="Art"><g><g>' + SHADE + "</g></g></g>", DARK, DARK),
    ("clipped group", '<g clip-path="url(#c)">' + SHADE + "</g>", DARK, GREY),
    ("group with its own blend", '<g style="mix-blend-mode:multiply"><rect width="100" height="100" fill="#808080"/></g>',
     DARK, DARK),
    # Groups that are on their own: the shade meets nothing in them and stays its own grey.
    ("group at half opacity", '<g style="opacity:0.5">' + SHADE + "</g>", GREY, GREY),
    ("isolated group", '<g style="isolation:isolate">' + SHADE + "</g>", GREY, GREY),
]


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
        if r.returncode:
            raise RuntimeError(f"{label}: exit {r.returncode}; see log")

    def level(picture, x, y):
        im = Image.open(picture).convert("RGB")
        return im.getpixel((int(x * im.width), int(y * im.height)))

    def near(pixel, want):
        return all(abs(c - want) <= 3 for c in pixel)

    for name, body, middle, corner in CASES:
        stem = name.replace(" ", "-")
        source = out / (stem + ".svg")
        source.write_text(HEAD + body + "</svg>", encoding="utf-8")
        png = out / (stem + "-canvas.png")
        run(stem + "-canvas", [exe, source, f"--export-filename={png}"])
        pdf = out / (stem + ".pdf")
        run(stem + "-pdf", [exe, source, f"--export-filename={pdf}"])
        run(stem + "-page", [args.pdftoppm.resolve(), "-f", "1", "-singlefile", "-r", "144", "-png", pdf, out / (stem + "-page")])
        page = out / (stem + "-page.png")
        for what, picture in (("canvas", png), ("PDF", page)):
            got_middle, got_corner = level(picture, 0.5, 0.5), level(picture, 0.06, 0.06)
            check(f"{name}: {what} middle", near(got_middle, middle), f"{got_middle}, wanted {middle}")
            check(f"{name}: {what} corner", near(got_corner, corner), f"{got_corner}, wanted {corner}")

    result = dict(checks=checks, passed=all(c["passed"] for c in checks))
    (out / "report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
