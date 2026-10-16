# CMYK and cut-out TIFFs, and Photoshop files

A placed TIFF is what a print shop sends when a picture will be looked at from
close up: CMYK, at full resolution, often cut out with transparency. PROOF
reads such a TIFF's pixels itself, for the screen and for print
(`src/display/tiff-pixels.h`), since 2026-10-16. Since 2026-10-17 it reads a
placed Photoshop file's merged image the same way (`src/display/psd-pixels.h`;
below, "Photoshop files"). Both land in one form, shown and printed from it
(`src/display/raster-pixels.h`).

The owner, on why it matters: "my vendors do not take TIFF, and their vendors
take TIFFs, so therefore their files have this issue. This is huge, and
important to add." And on colour: "90% of the time we want preserve numbers
(ignore linked profiles). TIFF is used when you want really juicy raster images
in an enormous resolution to be printed somewhere people will stare at them
from 10-20 cm (so we cannot cheat and use JPG). So we want to preserve as much
as possible. Most of the TIFF created were created with the same exact profile
that's used int he .AI file, but sometimes they might differ (subcontractor
brought crusty RGB TIFF we converted to the best of our ability, for example)".

## Preserve Numbers

A CMYK picture's numbers are the document's own, as Illustrator's default CMYK
policy "Preserve Numbers (Ignore Linked Profiles)" has it:

| Where | What |
|---|---|
| Screen | Through the document's CMYK profile, into sRGB, with its transparency. The picture's own profile only when the document has no CMYK profile (an RGB document), else the working CMYK profile |
| PDF | Its numbers unchanged, as `DeviceCMYK` under the document's output profile, at their own depth (8 or 16 bits), Flate-compressed (lossless). Not tagged with the picture's own profile, which a RIP would convert from |
| Transparency | Straight in the image's SMask. Photoshop keeps CMYK transparency premultiplied (`ExtraSamples` 1): the inks are divided back out, so a soft edge keeps its full ink and fades by its mask |
| `.ai` import | A linked CMYK TIFF whose own profile differs from the document's is named in the notes: "1 linked CMYK picture carries a profile other than the document's (name: profile); its numbers are kept, and shown through the document's profile" |
| `.ai` embedded CMYK pictures | Their CMYK samples, kept in `proof:samples` beside the RGB they show with, go to the PDF (while the picture is still theirs: the same size). Before, they printed as their RGB |

## What it was before (measured 2026-10-16)

- **Screen**: gdk-pixbuf's TIFF loader turned CMYK into RGB by plain
  arithmetic, without the profile, and dropped the transparency: cut-outs
  stood on opaque white boxes, paler than the page.
- **PDF**: CapyPDF's own TIFF reader gave every pixel the first pixel's alpha
  (`separate_tif_alpha` in `src/3rdparty/capypdf/src/imagefileops.cpp`, line
  260: `image.pixels[num_color_channels]` where `image.pixels[i +
  num_color_channels]` is meant). A cut-out's corner is transparent, so its
  whole SMask was zero and **the picture vanished from the PDF**, RGB cut-outs
  too. The premultiplied inks went out as they were, and the TIFF's own
  profile was attached (`ICCBased`). PROOF now hands CapyPDF the pixels itself
  (`Document::add_tiff` in `pdfoutput/build-document.cpp`); the submodule is
  unchanged. The fault is recorded here and not reported upstream: CapyPDF
  doesn't accept contributions made with an AI's help (the owner checked,
  2026-10-16).
- **`.ai` import**: a file with such links was compared with its page through
  the screen's loader, so it fell back. One of the collaborator's files,
  seven Photoshop 26.5 CMYK TIFFs with premultiplied transparency and an
  embedded profile, went from 33.4% different to 0.00%.

## CapyPDF's alpha fault, for whoever updates the submodule

As of CapyPDF `master` at `5504541` (2026-10-12), `separate_tif_alpha` in
`src/imagefileops.cpp` (line 260) takes every pixel's alpha from the first
pixel:

```cpp
image.alpha.push_back(image.pixels[num_color_channels]);       // as it is
image.alpha.push_back(image.pixels[i + num_color_channels]);   // as meant
```

Gray, RGB and CMYK TIFFs with alpha all go through it. It also passes
associated alpha (`ExtraSamples` 1, which Photoshop writes for CMYK cut-outs)
on with its colour premultiplied, where a PDF wants straight colour beside its
SMask. `Document::add_tiff` avoids both by reading the TIFF itself
(`display/tiff-pixels.*`) and handing CapyPDF the pixels. After an update of
the submodule, keep that path until both are fixed there: the TIFF export
smoke (`tiff-export-smoke.py`) fails if a cut-out TIFF vanishes or its edges
lighten.

## Photoshop files (2026-10-17)

The owner, on linked Photoshop files: "yes, they use photoshop more than me,
unsurprising." Nothing in PROOF read them before: the picture loader has no
Photoshop reader, so an `.ai` that linked one fell back to its page ("a linked
file is of a kind PROOF can't show yet"), and an SVG showed a broken picture.

Illustrator shows and prints the merged image Photoshop saves beside the
layers ("Maximize Compatibility", on by default). PROOF reads that image:

- Gray, RGB and CMYK, 8 or 16 bits a channel, `.psd` and `.psb`, uncompressed or
  RLE (PackBits), as Photoshop writes the merged image.
- CMYK is kept inverted in the file (255 is no ink) and read as inks, which
  then follow Preserve Numbers like a TIFF's.
- The transparency is the first extra channel when the file says so: a
  negative layer count (in the layer information, or in its `Lr16`/`Lr32`
  block at 16 and 32 bits). Other extra channels are left out.
- Photoshop mattes the merged image against white where it isn't opaque. In
  CMYK that is the ink scaled by the transparency, as in a premultiplied TIFF;
  in RGB and gray, the distance from white. Divided back out, straight.
- Its own profile (image resource 1039) for the note at `.ai` import.

Measured on the collaborator's five linked Photoshop files (2026-10-17, copies):
all CMYK, 8 bits, a transparency channel ("Transparency", "Trasparenza"),
merged image saved, RLE; two with an embedded profile, three without. Where a
pixel is clear, every channel holds 255; on soft edges the ink divided by the
transparency matches the solid ink beside it. An `.ai` exported to PDF holds
each at full size as `DeviceCMYK` with its SMask; against a decoder written
apart from PROOF's, every opaque pixel's inks equal the file's (5.7 million
pixels in the three of one file), the SMask equals its transparency, and the
edges unmatted match.

Not read, and said so when an `.ai` links one: a file saved without its merged
image ("it was saved without its merged image (Photoshop's Maximize
Compatibility)"), bitmap, indexed, duotone, Lab and multichannel files, 32-bit
channels, a merged image compressed with ZIP.

## What is read

Gray, RGB and CMYK (`Photometric` 1, 2, 5 with the CMYK ink set), 8 or 16 bits
a sample, unsigned, in strips or tiles, the samples of a pixel together
(Photoshop's "Interleaved"), any compression libtiff reads. The first extra
sample marked as alpha is the transparency; other extra channels (spot
channels, saved selections) are counted and left out. Other TIFFs take the old
paths.

## Limits

- A Photoshop file shows its merged image only: not its layers, so not a layer
  comp or layer visibility chosen in Illustrator's placement options.
- A Photoshop file in RGB or gray shows its values as they are on screen (its
  profile isn't applied, as for every RGB picture in PROOF) and goes to the
  PDF as `DeviceRGB` or `DeviceGray`, its profile dropped.
- At `.ai` import a linked Photoshop file is read whole to check that its
  merged image can be read, then again to show it.
- An RGB TIFF with transparency goes out as `DeviceRGB`: CapyPDF's raster
  builder takes no profile, so its own is dropped. One without transparency
  keeps CapyPDF's path and its profile.
- Spot channels in a TIFF are left out, not printed as spot plates.
- Pictures with planes apart (`PlanarConfiguration` 2), 32-bit or floating
  samples aren't read here.
- The profile note is made at `.ai` import only; an SVG document's linked
  TIFFs aren't checked for it.
- Whether a TIFF already shown redraws when the document is given another
  profile has not been checked.

## Checks

- `testfiles/src/tiff-pixels-test.cpp` (`test_tiff-pixels`, 7): premultiplied
  CMYK read straight, 16-bit tiled CMYK exact, extra channels left out, RGB and
  gray with transparency, other kinds refused, the screen through the given
  profile and not the picture's own, a pixbuf with its transparency.
- `testfiles/cli_tests/tiff-export-smoke.py` (60 checks, in
  `tools\run-focused-suites.ps1`): linked, as data and as kept samples, 8 and
  16 bits, with and without a profile, an RGB cut-out; `DeviceCMYK`, the
  numbers, an edge's full ink, the SMask, and the printed page and the canvas
  where the picture is and where it isn't.
- `AiNativeImportTest.ALinkedCmykTiffWithAnotherProfileIsNamedAndKeepsItsNumbers`.
- `testfiles/src/psd-pixels-test.cpp` (`test_psd-pixels`, 6; files built by
  `testfiles/src/psd-fixture.h`): CMYK with transparency unmatted, raw and RLE;
  a 16-bit RGB `.psb` with its layers in `Lr16`; its profile kept and extra
  channels left out, and no transparency without the mark; the kinds not read
  say why; a pixbuf through the document's profile with its transparency; RGB
  and gray as their values.
- The TIFF export smoke's Photoshop cases (31 of its 91 checks): CMYK matted
  (RLE), 16-bit CMYK with its layers in `Lr16`, an RGB cut-out.
- `AiNativeImportTest.ALinkedPhotoshopFileShowsItsMergedImage`: shown, and one
  saved without its merged image refused with the reason.
