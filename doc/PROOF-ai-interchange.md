# PROOF AI interchange

This first implementation is **PDF-based RGB interchange**, not a native
Illustrator authoring engine. It needs no installed Illustrator to export.

Open a PDF-compatible `.ai`, edit it, and save the working document as SVG.
Use **File → Save a Copy → Illustrator interchange (*.ai, PDF-based)** for the
Illustrator deliverable. The export dialog offers embedded fonts or text paths,
300 dpi effect rasterization, and page rounding options. Installed matching fonts
are needed for dependable text editing in Illustrator. Filter effects rasterize;
ordinary paths remain vector objects.

Ordinary Save/Save As exclude this copy-only format, including automatic format
selection for an imported `.ai`. Save a Copy still permits deliberately choosing
an existing filename through the normal overwrite prompt; it is not an immutable
backup. Keep the original AI file separately.

CLI:

```text
inkscape working.svg --export-filename=deliverable.ai
inkscape working.svg --export-filename=outlined.ai --export-text-to-path
inkscape working.svg --export-filename=page-2.ai --export-page=2
```

The writer uses Cairo's supported PDF 1.5/1.4 restriction, not a header patch.
It does not attach old Illustrator private streams. Illustrator must see the
newly exported artwork instead of an obsolete native copy. PDF page counts and
successful opening do not prove visual fidelity, retained layers or ink identity.

## Known limits

- CMYK separations, spot identities/tints and original Illustrator live effects
  are not preserved by this RGB export. It is not a print-production preset.
- Native layer structure, text layout, masks and blends can change during PDF
  import. Some text is already outlined in the source PDF.
- Page dimensions round up to whole PDF points; default interchange options
  preserve object dimensions rather than stretching objects to fit the rounding.
- Non-PDF-compatible modern AI and native AI writing are not implemented. A
  native writer is proven viable for a small subset; see the spike evidence below.
- The supplied Typical file still has strong colour differences. Giant File has
  existing import/appearance problems. Neither is certified as a faithful job.

## Fixes delivered with this format

- Copy-only output filtering is now enforced in file dialogs and ordinary Save.
- CLI exports reset the text mode explicitly, preventing a previous outline
  export from silently outlining later exports.
- Cairo's image transform guard now checks actual invertibility instead of
  rejecting every determinant below `1e-6`. PDF imports often represent large
  raster images as unit squares: dropping their small scale can magnify them
  millions of times and leave pages apparently blank.
- Cairo save propagates final surface errors and reads rounding preferences from
  the active output module rather than the separate PDF module.

## Hybrid importer direction

The proposed architecture is sound with explicit authority and edit tracking:

```text
AI container classification
  PDF representation -> Poppler artwork import
                       + bounded native metadata extraction
  native PGF         -> supported native drawing/text records
  old PS/EPS AI      -> bounded AI operator parser (not arbitrary PS execution)
                           |
                   PROOF document + provenance
                           |
             SVG master / PDF-based AI deliverable
             future constrained native AI writer
```

PDF compatibility is a content property, not merely the `.ai` suffix or `%PDF`
header: a private-data-only document may have a placeholder PDF page.

1. Keep original bytes, hashes, format version and unsupported records separately
   from the current document. They provide recovery/provenance, not proof that
   opaque native data remains valid after an edit.
2. Prefer a single artwork representation per object. Use native data to enrich
   objects only where membership and correspondence are established; never stack
   a second rendering of all private artwork on the PDF artwork.
3. Adapt native text/name recovery from OpenDesign, and selected drawing/spot
   operators from Scribus, behind the existing bounded AI24 decoder. See the
   [donor review](PROOF-ai-donor-review.md). No donor has yet been integrated.
4. Add real paint identity to the model: ICC-aware process colours, named spot
   ink, alternate colour and tint. RGB-hex matching cannot distinguish inks.
   Preserve original image bytes/profiles with the image object.
5. Track edits and dependencies for retained native objects. Copy native blocks
   unchanged only when proven unaffected. Regenerate supported changed blocks;
   report unsupported edits or export the explicit interchange representation.
6. A future dual-representation AI writer must regenerate both the PDF and private
   artwork from the same edited document. Test both with independent readers.

### Native writer: spike evidence (2026-10-02)

A modern `.ai` is a PDF whose page `PieceInfo` carries Illustrator's own native
records. Those records are a PostScript-dialect operator language. Illustrator
reads them, while every other application reads the PDF page. A hand-written
file in that shape, with PROOF as the declared creator, opened in Illustrator
29.2.1 as native artwork:
- named layers
- a CMYK document
- spot swatches with correct alternates and a 30% tint
- overprint
- a clip group, a compound path and a group

Illustrator ignored the PDF page. Both plain and AI12-zlib private data worked.
See the [donor review addendum](PROOF-ai-donor-review.md#native-writer-spike-against-illustrator-2921).

Illustrator also used the private data when the page was marked newer. **It
does not detect stale private data**, so writing an edited PDF page beside the
original private records would silently discard the edits. That is why the
current interchange export writes no private data, and why any native writer
must generate both halves from one document or neither.

Proposed writer order:
1. Records for the proven subset: layers, groups, paths, compound paths, clip
   groups, process CMYK, named spots with tints, overprint and stroke style.
   Next come the donor's gradient operators, then images, and text last.
2. The PDF half through CapyPDF, for DeviceCMYK, Separation and overprint.
   Cairo cannot express inks.
3. Fail closed. When an object uses an unsupported feature, either expand it
   identically in both halves, or refuse the native write and offer the
   PDF-only interchange export.

Open items:
- Spot identity now exists: imports keep named inks with their alternate and
  tint (see [PROOF-spot-inks.md](PROOF-spot-inks.md)). The writer still needs a
  spot-aware swatch editor so the stored alternate stays in sync. It writes
  `x`/`Xx` with the inverted AI tint, operand = 1 − tint.
- Illustrator 29 rejected an earlier CapyPDF fixture, so the CapyPDF page
  needs testing with private data present

### Writer recipe: named and multiple artboards (verified 2026-10-02)

This recipe was verified in Illustrator 29.2.1. A file built this way opened
with two named artboards at the intended positions, with the art exactly on
them. Illustrator then saved it as native AI and reopened it unchanged, keeping
our artboard names, UUIDs and ruler origins.

**Coordinates.** (0, 0) is the top-left corner of the first artboard, and y is
negative downward, matching Illustrator's rulers. W and H below are the
document's extent.

**Header comments:**
- `%AI5_FileFormat 14.0`. With `4.0` (Illustrator 8) Illustrator falls back to
  legacy reading: it ignores the artboard list and offsets the art by
  (16383 − 8640) / 2 = 3871.5 pt, from the old 120-inch canvas.
- `%%Canvassize: 16383`, `%AI5_ArtSize: 14400 14400`, `%AI24_LargeCanvasScale: 1`.
- `%AI3_Cropmarks` and `%AI3_TileBox`: `0 -H W 0`. `%%PageOrigin: 0 -H`.
- `%AI3_TemplateBox: cx cy cx cy`, with cx = W/2 + 0.5 and cy = −H/2 − 0.5. This
  pins the document centre to the canvas centre.

**Document data**, inside `%%BeginSetup`. This is the whole minimal block:

```text
%AI9_BeginDocumentData
%_/Document :
%_/Dictionary :
%_0 /Int (CropAreaActive) ,
%_/Array :
%_/Dictionary :                       <- one per artboard
%_L T /RealPointRelToROrigin
%_ (PositionPoint1) ,
%_R B /RealPointRelToROrigin
%_ (PositionPoint2) ,
%_0 /Bool (IsArtboardDefaultName) ,
%_0 /Bool (IsArtboardSelected) ,
%_0 /Int (DisplayMark) ,
%_rx ry /RealPoint
%_ (RulerOrigin) ,
%_(uuid) /String (ArtboardUUID) ,
%_1 /Real (PAR) ,
%_(name) /UnicodeString (Name) ,
%_; ,
%_; (ArtboardArray) ,
%_; /Recorded ,
%_;
%AI9_EndDocumentData
```

`RulerOrigin` is the artboard's top-left in canvas units:
(⌊8191.5 − cx + L⌋, ⌊8191.5 + cy − T⌋).

**Failure modes found on the way:**

| Mistake | Effect |
|---|---|
| No section dictionary inside `/Document` | Open is cancelled |
| Artboards closed as `/NotRecorded` | Artboard list ignored |
| No `CropAreaActive` | No active artboard; scripting calls such as `visibleBounds` fail with `PARM` (found by `tools/ai-docdata-bisect.py` from Illustrator's own 121-entry dictionary) |

Illustrator writes **one PDF page per artboard**, so PROOF's PDF half must do
the same. Illustrator ignores the page, but every other reader sees it.

Illustrator keeps the document ICC profile in the NotRecorded section
(`AI12 Document Profile Size` / `AI12 Document Profile Data`, ASCII85). That is
the place for PROOF's CMYK working profile, and is not yet tested.

Evidence is in the workspace's `artifacts/native-ai-spike/artboards/`.
`tools/ai-artboard-probe.py` generates the probes, and `tools/ai_docdata.py`
splits and rebuilds document-data blocks byte for byte without evaluating
anything. `tools/illustrator-resave-check.ps1` runs the save-and-reopen check.

Initial native subset: paths, basic fills/strokes, affine transforms, simple
clipping, named layers, process/spot paint and embedded images. Add text frames
after verifying mapping to native text records. Unsupported meshes, live effects,
symbols, complex transparency and knockout must not silently pretend to round-trip.

Acceptance requires a real edit (move/recolour/text change), Illustrator reopening,
PDF rendering comparison, artboard/layer/paint checks, and a save back from
Illustrator followed by PROOF reopening. Testing a converter against its own
parser alone is insufficient.

## Automated checks

`testfiles/src/extensions-pdfoutput-test.cpp` covers the save guard, small valid
image transforms, rejected singular/non-finite transforms and existing style
behaviour. `testfiles/cli_tests/ai-interchange-smoke.py` drives the built executable,
checks page/text/edit round-tripping, an actual raster-scale fixture and optional
real AI files. It requires test-only pypdf/Pillow and a pdftoppm executable.
Actual Illustrator automation remains a separate check.
