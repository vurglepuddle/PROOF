# PROOF AI interchange

Writing `.ai` has two routes. **File → Save As → Adobe Illustrator (native)**
writes editable Illustrator records and PROOF's PDF pages together for the
supported subset below. **Illustrator interchange (PDF-based)** remains a
copy-only RGB export for broader artwork. Neither requires Illustrator to write.

Reading `.ai` now reconstructs supported artwork from Illustrator's own
records, compares every artboard with its PDF page, and uses the PDF import
when anything is unsupported, incomplete or visually different. See the
[current progress in the README](../README.md#illustrator-ai-progress) and
[native-record reference](PROOF-ai-native-records.md) for coverage and limits.

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
- When native reading falls back to PDF, layer structure, text layout, masks
  and blends can change. Some text is already outlined in the source PDF.
- Page dimensions round up to whole PDF points; default interchange options
  preserve object dimensions rather than stretching objects to fit the rounding.
- Reading non-PDF-compatible modern AI is not implemented. Native writing
  supports the subset below and refuses everything else by name.
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

## Original hybrid importer direction (before native reading was delivered)

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
3. The original proposal was to adapt native text/name recovery from OpenDesign
   and selected drawing/spot operators from Scribus. The delivered reader
   instead uses a bounded, credited port of VectorCraft's operator meanings,
   with corrections checked against real files. See the
   [native-record reference](PROOF-ai-native-records.md) and
   [donor review](PROOF-ai-donor-review.md).
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

### The records writer (2026-10-17)

The owner chose one writer, toward the modern form, the version 7 form kept as
an export option later. A second spike first: Illustrator 29.2.1 drew a mesh
control from records rewritten and recompressed with Zstandard outside
Illustrator (13 blocks where it wrote 14), not from the file's PDF page; it
opened version 7 records inside a modern container as "[Converted]"; and the
current header without the artboard list put the art 3871.5 pt off its
artboard, as the recipe above says. `tools\ai-records-rewrite.py` (workspace)
replaces one string in a copy's records, or all of them, to test such rules.

`src/extension/internal/ai/ai-native-writer.*` writes the records of a PROOF
document (`write_records`), by the recipe above:

- **Artboards**: the document's pages (or its size), named, in points, the
  first one's top-left at 0,0 and y down; the document data with
  `CropAreaActive`, closed `/Recorded`. Names are UTF-8: Illustrator showed a
  Cyrillic artboard name as written.
- **The opening view** (`%AI10_OpenToVie`, `%AI9_OpenToView`): fits the first
  artboard. Without it Illustrator opened on empty canvas (the owner saw
  nothing). No `%AI5_OpenViewLayers`: copied from a one-layer file as "7", it
  moved the hidden and locked flags to other layers.
- **Layers** in Illustrator 29's `Lb` form: visible, preview, locked, printing,
  dim. Illustrator shows a layer in its preset colour by index, ignoring the RGB
  beside it, so the nearest of the presets the owner's files pair consistently
  is written (census of 879 files; indexes 4 to 6 vary and are left out).
- **Objects**: groups `u`/`U`; paths with corners only (`L`, `C`), a closed one
  ending on its start; compound paths `*u`/`*U` with `XR`; clipping groups `q`,
  one path, `h W n`, `Q`; hidden `1 Xw`, locked `1 A` (an object's own, not its
  layer's); clones written as copies of what they show.
- **Paint**: CMYK `k`/`K`, RGB `Xa`/`XA`, spot inks with CMYK alternates `x`/`X`
  at full strength and `Xx`/`XX` at a tint (the operand is 1 − tint), with
  `%%DocumentCustomColors` and `%%CMYKCustomColor`; overprint `O`/`R` from
  `proof:overprint`; stroke width, cap, join, miter limit and dashes scaled to
  points. `%AI9_ColorModel` follows the document's colour mode.
- **Refused by name**, writing nothing: type, pictures, gradients, mesh
  gradients, patterns, transparency, blend modes, opacity masks, filters,
  markers, context paint, spot inks with RGB or Lab alternates, one ink name
  with two definitions, strokes on stretched or skewed objects, strokes under
  their fill, non-scaling strokes, clipping paths other than one path.

**Checked.** Read back through PROOF's importer (`WrittenRecordsReadBackAsTheDocument`,
`WrittenRecordsKeepSpotInksClipsAndArtboards`, `TheWriterRefusesWhatItCannotWriteByName`):
every shape where it was to a thousandth of a pixel, CMYK values, stroke width,
the even-odd hole, overprint, the spot at 30%, the clip, two named artboards, an
A4 document in millimetres. In Illustrator 29.2.1 (`WriterRecordsForIllustrator`
writes them when `PROOF_AI_WRITER_OUT` is set; put into a copy's container with
the rewrite tool): both artboards at their sizes and names, four layers with
their names (Cyrillic too), hidden and locked as written, the green layer
green, every object at its place, CMYK fills and strokes to the number, the
spot as a spot swatch used at 100% and 30%, the dashed round-capped curve, the
clip, the hole. RGB fills in the CMYK document were converted by Illustrator,
as it does.

Not yet: type, gradients and pictures; the version 7 form; smooth points (all
points are written as corners, so Illustrator's direct selection sees corners
where PROOF had smooth ones).

### Native Save As and its container (2026-10-17)

`AiNativeOutput` registers **Adobe Illustrator (native) (*.ai)** for Save As
and Save. It calls the records writer first; an unsupported object produces a
message naming the feature and saves nothing. The supported subset is the
records writer's list above. Smooth-point editing still loses smooth flags.

For an accepted document it writes fresh CapyPDF pages from a document copy,
then appends Illustrator's private data in a PDF incremental update. Each
page has `/PieceInfo /Illustrator` and names the same `/Private` dictionary.
That dictionary contains header comments in `AIMetaData`, Zstandard-compressed
records prefixed by `%AI24_ZStandard_Data` in 64 KiB `AIPrivateDataN` streams,
and the container/version fields Illustrator 29 uses. The PDF cross-reference
writer is shared with placed-page export and retains the original table or
stream form.

**The PDF envelope is 1.7.** Illustrator 29.2.1 refused the first container
with CapyPDF's ordinary PDF 2.0 default, even though PROOF read its records.
A disposable header-only diagnostic confirmed the cause; that diagnostic is
not the writer. The bundled backend now has a per-document version selector:
the engine emits PDF 1.7 for native output from the outset. The writer's
accepted features fit that version. Ordinary PDF export retains PDF 2.0;
PDF/X and PDF/A version choices cannot be overridden through this selector.

`src/3rdparty/capypdf-compat.cmake` generates a guarded overlay of the pinned
backend in the build folder. The upstream submodule is untouched. Its changes
must still match the reviewed source, or configuration stops for review.
`capypdf-version.*` selects the version on that document's properties only.
Native output is unavailable with a system backend lacking this capability.

**Checked with Illustrator 29.2.1.** A control saved through the installed
native writer opens with its two named artboards (one Cyrillic), three editable
layers including hidden/locked flags, a clipping group, a compound-path hole,
CMYK fill/stroke numbers and the named spot at 30%. Illustrator's saved copy
reopens through PROOF's ordinary native open path with the artboards, layers,
clipping, spot tint and CMYK values retained. This qualifies that synthetic
subset, not a round trip for every imported document. The actual GUI Save As
also writes both pages and native data while leaving its input SVG copy intact.

The installed native smoke passes 32 checks, including PDF 1.7, unchanged
ordinary-PDF defaults, fresh artwork in both halves after an overwrite,
read-only destinations and failed replacements. The workspace's eleven unit
suites and eight export suites pass after the version change; earlier reader
corpus checkpoints remain separate.

The two halves come from the same document at the same save. A unique temporary
file beside the destination is replaced only after both halves are complete;
refusals and failed writes leave the previous file intact. Native page options
are independent of previous PDF/LaTeX choices, and PDF export does not mutate
the working document. The GUI reports the refusal through its existing save
error dialog; CLI exports report it on stderr and return failure, without a crash.

CLI native output is explicit; the existing filename-only `.ai` command keeps
its PDF-based behavior:

```text
inkscape working.svg --export-extension=org.inkscape.output.ai.native --export-filename=deliverable.ai
```

`--export-page=2` applies to both the PDF and the native artboard. The writer
does not reuse private data from an imported file.

## Automated checks

`testfiles/src/extensions-pdfoutput-test.cpp` covers the save guard, small valid
image transforms, rejected singular/non-finite transforms and existing style
behaviour. `testfiles/cli_tests/ai-interchange-smoke.py` drives the built executable,
checks page/text/edit round-tripping, an actual raster-scale fixture and optional
real AI files. It requires test-only pypdf/Pillow and a pdftoppm executable.
Actual Illustrator automation remains a separate check.

`ai-native-output-smoke.py` checks the installed native writer end to end with
synthetic artwork: strict PDF structure, two named pages, private data, native
reopening, editable layers/paths/clips, CMYK, a spot tint, matching PDF page
pixels, page selection, replacing both halves on a second save, and refusals
that preserve the previous file. Unit cases also cover multi-block records
and Save As eligibility. The workspace focused runner includes this smoke test.
