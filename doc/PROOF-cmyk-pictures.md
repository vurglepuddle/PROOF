# CMYK and cut-out TIFFs

A placed TIFF is what a print shop sends when a picture will be looked at from
close up: CMYK, at full resolution, often cut out with transparency. PROOF
reads such a TIFF's pixels itself, for the screen and for print
(`src/display/tiff-pixels.h`), since 2026-10-16.

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
  unchanged and the fault is not reported upstream yet.
- **`.ai` import**: a file with such links was compared with its page through
  the screen's loader, so it fell back. One of the collaborator's files,
  seven Photoshop 26.5 CMYK TIFFs with premultiplied transparency and an
  embedded profile, went from 33.4% different to 0.00%.

## What is read

Gray, RGB and CMYK (`Photometric` 1, 2, 5 with the CMYK ink set), 8 or 16 bits
a sample, unsigned, in strips or tiles, the samples of a pixel together
(Photoshop's "Interleaved"), any compression libtiff reads. The first extra
sample marked as alpha is the transparency; other extra channels (spot
channels, saved selections) are counted and left out. Other TIFFs take the old
paths.

## Limits

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
