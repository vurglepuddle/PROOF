"""CMYK and cut-out TIFFs and Photoshop files, on the canvas and in the exported PDF (requires
Pillow and pypdf).

Small TIFFs and Photoshop files are written here, placed in drawings by link, as data and as the
CMYK samples an Illustrator picture keeps (proof:samples), and exported with the built editor.
The PDF must carry the CMYK numbers as they are (DeviceCMYK, Illustrator's "Preserve Numbers",
not tagged with the picture's own profile), the transparency straight in an SMask, Photoshop's
premultiplied or matted colour divided back out, and 16-bit samples at 16 bits. The canvas and
the printed page must show the picture where it is, and nothing where it is transparent.

See src/display/raster-pixels.h, tiff-pixels.h and psd-pixels.h.
"""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import struct
import subprocess

from PIL import Image
from pypdf import PdfReader

W = H = 8
GRACOL = Path("C:/Windows/System32/spool/drivers/color/CoatedGRACoL2006.icc")


def tiff(pixels, spp, bits=8, photometric=5, extras=(), icc=b""):
    """An uncompressed little-endian TIFF of one strip. `pixels`: a list of sample tuples."""
    fmt = "<" + ("B" if bits == 8 else "H") * spp
    data = b"".join(struct.pack(fmt, *p) for p in pixels)
    entries = []

    def tag(t, typ, values):
        entries.append((t, typ, values))

    tag(256, 4, [W])
    tag(257, 4, [H])
    tag(258, 3, [bits] * spp)
    tag(259, 3, [1])
    tag(262, 3, [photometric])
    tag(273, 4, [0])  # filled in below
    tag(277, 3, [spp])
    tag(278, 4, [H])
    tag(279, 4, [len(data)])
    tag(284, 3, [1])
    if photometric == 5:
        tag(332, 3, [1])
    if extras:
        tag(338, 3, list(extras))
    if icc:
        tag(34675, 7, icc)
    entries.sort()
    head = 8
    ifd_size = 2 + 12 * len(entries) + 4
    extra = bytearray()
    blobs = {}
    base = head + ifd_size
    for t, typ, values in entries:
        size = {3: 2, 4: 4, 7: 1}[typ] * len(values)
        if size > 4:
            blobs[t] = base + len(extra)
            extra += bytes(values) if typ == 7 else struct.pack("<" + {3: "H", 4: "I"}[typ] * len(values), *values)
            if len(extra) % 2:
                extra += b"\0"
    data_at = base + len(extra)
    out = bytearray(b"II*\0" + struct.pack("<I", head) + struct.pack("<H", len(entries)))
    for t, typ, values in entries:
        if t == 273:
            values = [data_at]
        size = {3: 2, 4: 4, 7: 1}[typ] * len(values)
        if size > 4:
            field = struct.pack("<I", blobs[t])
        elif typ == 7:
            field = bytes(values).ljust(4, b"\0")
        else:
            field = struct.pack("<" + {3: "H", 4: "I"}[typ] * len(values), *values).ljust(4, b"\0")
        out += struct.pack("<HHI", t, typ, len(values)) + field
    out += struct.pack("<I", 0) + extra + data
    return bytes(out)


# Left half: an opaque red of C0 M255 Y150 K0. Top right: nothing. Bottom right: that red,
# a quarter there.
def region(x, y):
    if x < W // 2:
        return "ink"
    return "none" if y < H // 2 else "quarter"


INK = (0, 255, 150, 0)
QUARTER = 64


def packbits(row):
    out = bytearray()
    i = 0
    while i < len(row):
        run = 1
        while i + run < len(row) and run < 128 and row[i + run] == row[i]:
            run += 1
        if run >= 3:
            out += bytes([257 - run, row[i]])
            i += run
            continue
        start = i
        while i < len(row) and i - start < 128 and not (i + 2 < len(row) and row[i] == row[i + 1] == row[i + 2]):
            i += 1
        out += bytes([i - start - 1]) + row[start:i]
    return bytes(out)


def psd(planes, mode, bits=8, rle=True, tagged=False):
    """A Photoshop file of its merged image alone: `planes`, the samples of each channel as
    Photoshop keeps them (CMYK inverted, all matted against white where transparent), the last
    its transparency. `tagged`: the layers in an "Lr16" block, as at 16 bits."""
    size = bits // 8
    data = [b"".join(v.to_bytes(size, "big") for v in plane) for plane in planes]
    out = b"8BPS" + struct.pack(">H6xHIIHH", 1, len(planes), H, W, bits, mode) + struct.pack(">I", 0)
    resources = b"8BIM" + struct.pack(">H", 1057) + b"\0\0" + struct.pack(">I", 12) + struct.pack(">IB7x", 1, 1)
    out += struct.pack(">I", len(resources)) + resources
    count = struct.pack(">h", -1)  # negative: the first extra channel is the merged transparency
    if tagged:
        layers = struct.pack(">II", 0, 0) + b"8BIMLr16" + struct.pack(">I", len(count)) + count
    else:
        layers = struct.pack(">I", len(count)) + count
    out += struct.pack(">I", len(layers)) + layers
    row = W * size
    if rle:
        rows = [packbits(p[y * row:(y + 1) * row]) for p in data for y in range(H)]
        return out + struct.pack(">H", 1) + b"".join(struct.pack(">H", len(r)) for r in rows) + b"".join(rows)
    return out + struct.pack(">H", 0) + b"".join(data)


def psd_cmyk(bits=8):
    top = 255 if bits == 8 else 65535
    planes = [[], [], [], [], []]
    for pixel in cmyk_pixels(False, bits):
        a = pixel[4]
        for ch in range(4):
            planes[ch].append(top - round(pixel[ch] * a / top))
        planes[4].append(a)
    return planes


def psd_rgb():
    planes = [[], [], [], []]
    for i in range(W * H):
        a = {"ink": 255, "none": 0, "quarter": QUARTER}[region(i % W, i // W)]
        colour = (255, 0, 0) if a else (0, 0, 0)
        for ch in range(3):
            planes[ch].append(round(colour[ch] * a / 255 + 255 * (255 - a) / 255))
        planes[3].append(a)
    return planes


def cmyk_pixels(premultiplied, bits=8):
    top = 255 if bits == 8 else 65535
    scale = top / 255
    out = []
    for y in range(H):
        for x in range(W):
            r = region(x, y)
            a = {"ink": 255, "none": 0, "quarter": QUARTER}[r] * scale
            ink = [round(v * scale) for v in INK] if r != "none" else [0, 0, 0, 0]
            if premultiplied:
                ink = [round(v * a / top) for v in ink]
            out.append(tuple(ink) + (round(a),))
    return out


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

    def images(res, depth=0):
        xo = res.get("/XObject") if res else None
        if not xo:
            return
        for _, ref in xo.get_object().items():
            o = ref.get_object()
            if o.get("/Subtype") == "/Image":
                yield o
            elif o.get("/Subtype") == "/Form" and depth < 8:
                yield from images(o.get("/Resources"), depth + 1)

    icc = GRACOL.read_bytes() if GRACOL.exists() else b""
    files = {
        "cmyk8-premultiplied.tif": tiff(cmyk_pixels(True), 5, extras=[1]),
        "cmyk16-straight.tif": tiff(cmyk_pixels(False, 16), 5, bits=16, extras=[2]),
        "cmyk8-profiled-opaque.tif": tiff([p[:4] for p in cmyk_pixels(False)], 4, icc=icc),
        "rgb8-cutout.tif": tiff([((255, 0, 0) if region(i % W, i // W) != "none" else (0, 0, 0)) +
                                 ({"ink": 255, "none": 0, "quarter": QUARTER}[region(i % W, i // W)],)
                                 for i in range(W * H)], 4, photometric=2, extras=[2]),
        "cmyk8-matted.psd": psd(psd_cmyk(), 4),
        "cmyk16-tagged.psd": psd(psd_cmyk(16), 4, bits=16, rle=False, tagged=True),
        "rgb8-cutout.psd": psd(psd_rgb(), 3),
    }
    for name, data in files.items():
        (out / name).write_bytes(data)
    # What an Illustrator picture shows with: its RGB, and the same transparency as its samples.
    rgb_look = io.BytesIO()
    look = Image.new("RGBA", (W, H))
    for i in range(W * H):
        look.putpixel((i % W, i // W), (200, 30, 60, {"ink": 255, "none": 0, "quarter": QUARTER}[region(i % W, i // W)]))
    look.save(rgb_look, "PNG")
    data_uri = lambda mime, b: f"data:{mime};base64," + base64.b64encode(b).decode()

    def drawing(image):
        return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
                'width="80" height="80" viewBox="0 0 80 80">' + image + "</svg>")

    image = '<image x="0" y="0" width="80" height="80" preserveAspectRatio="none" style="image-rendering:pixelated" '
    cases = [
        ("linked CMYK, premultiplied", image + 'xlink:href="cmyk8-premultiplied.tif"/>', "cmyk", 8, True),
        ("linked CMYK, 16 bits", image + 'xlink:href="cmyk16-straight.tif"/>', "cmyk", 16, True),
        ("linked CMYK with its own profile", image + 'xlink:href="cmyk8-profiled-opaque.tif"/>', "cmyk", 8, False),
        ("CMYK as data", image + 'xlink:href="' + data_uri("image/tiff", files["cmyk8-premultiplied.tif"]) + '"/>',
         "cmyk", 8, True),
        ("Illustrator's kept samples", image + 'xlink:href="' + data_uri("image/png", rgb_look.getvalue()) +
         '" proof:samples="' + data_uri("image/tiff", files["cmyk8-premultiplied.tif"]) +
         '" xmlns:proof="urn:x-proof:document:1"/>', "cmyk", 8, True),
        ("linked RGB cut-out", image + 'xlink:href="rgb8-cutout.tif"/>', "rgb", 8, True),
        ("linked Photoshop CMYK", image + 'xlink:href="cmyk8-matted.psd"/>', "cmyk", 8, True),
        ("linked Photoshop CMYK, 16 bits", image + 'xlink:href="cmyk16-tagged.psd"/>', "cmyk", 16, True),
        ("linked Photoshop RGB cut-out", image + 'xlink:href="rgb8-cutout.psd"/>', "rgb", 8, True),
    ]
    for name, body, model, bits, cutout in cases:
        stem = name.replace(" ", "-").replace(",", "").replace("'", "")
        source = out / (stem + ".svg")
        source.write_text(drawing(body), encoding="utf-8")
        pdf = out / (stem + ".pdf")
        run(stem + "-pdf", [exe, source, f"--export-filename={pdf}"])
        found = [i for i in images(PdfReader(str(pdf)).pages[0]["/Resources"]) if i.get("/Width") == W]
        check(f"{name}: one picture in the PDF", len(found) == 1, f"{len(found)}")
        if not found:
            continue
        img = found[0]
        cs = img.get("/ColorSpace")
        want_cs = "/DeviceCMYK" if model == "cmyk" else "/DeviceRGB"
        check(f"{name}: {want_cs}, not the picture's own profile", cs == want_cs, str(cs))
        check(f"{name}: {bits} bits", img.get("/BitsPerComponent") == bits, str(img.get("/BitsPerComponent")))
        samples = img.get_data()
        n = 4 if model == "cmyk" else 3
        size = 2 if bits == 16 else 1

        def sample(i, k):
            at = (i * n + k) * size
            return int.from_bytes(samples[at:at + size], "big") // (257 if bits == 16 else 1)

        ink_px = 0
        quarter_px = (H - 1) * W + (W - 1)
        if model == "cmyk":
            check(f"{name}: the numbers as they are", [sample(ink_px, k) for k in range(4)] == list(INK),
                  str([sample(ink_px, k) for k in range(4)]))
            if cutout:
                got = [sample(quarter_px, k) for k in range(4)]
                check(f"{name}: an edge keeps its full ink", all(abs(a - b) <= 2 for a, b in zip(got, INK)), str(got))
        if cutout:
            mask = img.get("/SMask")
            check(f"{name}: its transparency in an SMask", mask is not None)
            if mask is not None:
                m = mask.get_object().get_data()
                step = 2 if mask.get_object().get("/BitsPerComponent") == 16 else 1
                level = lambda i: m[i * step]
                check(f"{name}: SMask where it is, isn't, and a quarter",
                      (level(ink_px), level(W - 1), level(quarter_px)) == (255, 0, QUARTER),
                      str((level(ink_px), level(W - 1), level(quarter_px))))
        else:
            check(f"{name}: no SMask", img.get("/SMask") is None)
        # What prints and what the canvas shows: the picture on the left, nothing top right.
        run(stem + "-page", [args.pdftoppm.resolve(), "-f", "1", "-singlefile", "-r", "72", "-png", pdf, out / (stem + "-page")])
        png = out / (stem + "-canvas.png")
        run(stem + "-canvas", [exe, source, f"--export-filename={png}", "--export-background=white",
                               "--export-background-opacity=1"])
        for what, picture in (("printed", out / (stem + "-page.png")), ("canvas", png)):
            im = Image.open(picture).convert("RGB")
            left = im.getpixel((int(im.width * 0.2), int(im.height * 0.5)))
            empty = im.getpixel((int(im.width * 0.8), int(im.height * 0.2)))
            check(f"{name}: {what}: the picture shows", left[0] > 150 and left[1] < 120, str(left))
            if cutout:
                check(f"{name}: {what}: nothing where it is transparent", min(empty) > 245, str(empty))

    result = dict(checks=checks, passed=all(c["passed"] for c in checks))
    (out / "report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f'{sum(c["passed"] for c in checks)}/{len(checks)} checks passed. {out / "report.json"}', flush=True)
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
