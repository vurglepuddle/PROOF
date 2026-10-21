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
- **Objects**: groups `u`/`U`; paths with corner (`L`, `C`) and smooth (`l`, `c`)
  destinations, including an explicit closed seam; compound paths `*u`/`*U`
  with `XR`; clipping groups `q`,
  one path, `h W n`, `Q`; hidden `1 Xw`, locked `1 A` (an object's own, not its
  layer's); clones written as copies of what they show.
- **Paint**: CMYK `k`/`K`, RGB `Xa`/`XA`, spot inks with CMYK alternates `x`/`X`
  at full strength and `Xx`/`XX` at a tint (the operand is 1 − tint), with
  `%%DocumentCustomColors` and `%%CMYKCustomColor`; overprint `O`/`R` from
  `proof:overprint`; stroke width, cap, join, miter limit and dashes scaled to
  points. `%AI9_ColorModel` follows the document's colour mode.
- **Linear fills**: opaque pad gradients with RGB or CMYK process stops,
  including affine transforms and per-object bounding-box coordinates. Their
  bounded subset and qualification are described below.
- **Refused by name**, writing nothing: type, pictures, unsupported gradients, mesh
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

At this first records-writer checkpoint, type, gradients, pictures, the
version 7 form and smooth points remained open: every point was written as a
corner. Version 7 and the subsequent smooth-point/linear-gradient round are
documented below. Type and pictures remain unsupported.

### Native Save As and its container (2026-10-17)

`AiNativeOutput` registers **Adobe Illustrator (native) (*.ai)** for Save As
and Save. It calls the records writer first; an unsupported object produces a
message naming the feature and saves nothing. The supported subset is the
records writer's list above, including the later smooth-point and linear-fill
support described below.

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
Several passages of one file now apply in turn, each still required to match
the pinned source exactly once.

**File and folder names in any script (2026-10-18).** On Windows, PDF export
and native Save As wrote nothing into a folder named in Cyrillic, and the
export still reported success. The collaborator's workspace sits under such a
folder. GLib hands file names over as UTF-8; CapyPDF opened them with the
narrow C runtime, `CreateFileA` and FreeType's own file access, which read
them in the ANSI code page. Overlays of `utils`, `imagefileops`, `mmapper`,
`pdfwriter`, `capi` and `document` now open by UTF-8 name: through
`std::filesystem::path` and the wide calls (`_wfopen`, `CreateFileW`,
`TIFFOpenW`) on Windows. A font is mapped that way and handed to FreeType from
memory. This covers the output and its temporary file, linked PNG, JPEG and
TIFF pictures, fonts, ICC profiles and embedded files. A temporary file is
removed when the final rename fails, and `PdfOutput::save` now reports a write
failure (`save_failed`) instead of only warning, so the command line exits
non-zero and a Save As says the file wasn't saved. PROOF's own reads and
writes of these files already used GLib's UTF-8 calls.

`testfiles/cli_tests/unicode-path-export-smoke.py` (the focused runner's
twelfth export check, synthetic art only) writes into folders and files named
in Cyrillic, the collaborator's `0х000`, Chinese, Japanese, Korean, Hebrew,
German with umlauts, emoji with joiners, and punctuation (`& # % [ ] { } ~ ! @
+ = , '`). It checks a PDF with a linked PNG, a linked JPEG and a placed PDF
page, a native `.ai` with Illustrator's records, a PNG and a plain SVG in each,
no temporary files left over, and that an unwritable PDF fails. Against the
build before this, PDF and `.ai` failed in all seven folders while PNG and SVG
passed, and a PDF that couldn't be written exited 0 with no file (a missing
folder was already refused by the command line). This build passes 66/66.

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


### Illustrator 7 copy export (2026-10-17)

**Save a Copy → Adobe Illustrator 7** writes one artboard as editable
standalone records, without a PDF preview or a modern private-data container.
The output ID is org.inkscape.output.ai.legacy7. It needs no CapyPDF backend
and uses the same artwork traversal as modern native output, through
write_records with RecordsVersion::Illustrator7.

The artboard origin is its bottom-left. Layers use the ten-operand layout;
CMYK spot tints use x/X at every tint. The reader distinguishes those layer
fields from the modern fourteen-field form. The reference is Adobe Developer
Support's [Illustrator File Format Specification, 23 February 1998](https://13thmonkey.org/documentation/fonts/AI7FileFormat.pdf),
sections 2, 5 and 9, together with generated Illustrator controls.

Illustrator's legacy conversion rounds half the artboard size when placing
its ruler origin. The degenerate TemplateBox uses ceil(width/2) and
height-ceil(height/2) so that the default artboard matches the crop rectangle.
A naive centre creates an extra displaced artboard, including with fractional
print dimensions. Integer, odd and fractional dimensions, including A4, have
CLI checks; actual Illustrator conversion is checked separately.

The shared writer subset and refusals apply. This variant additionally
refuses multiple artboards, names outside Windows-1252, individually hidden
objects, nested layers and image dimming other than 50%. Hidden layers work.
Artboard names and document ICC profiles are not retained. Corner/smooth flags
use the shared writer described below.
The original legacy control opened in CMYK and converted RGB process colours;
its CMYK numbers and CMYK spot tints survived. Since 2026-10-21, a later
colour-model advisory comment preserves the source document mode in newer
Illustrator; see "Opaque pictures in Illustrator 7" below.
Keep SVG as the working document and use modern native output for named
artboards and the broader metadata it supports.

CLI page selection can provide one artboard:

~~~text
inkscape working.svg --export-extension=org.inkscape.output.ai.legacy7 --export-page=2 --export-filename=page-2.ai
~~~

Validation happens before the destination is touched. A sibling temporary
file replaces it only after the standalone records are completely written.

Save As and Save a Copy now retain the selected output module. Filename
suffixes alone cannot distinguish native AI, legacy AI and PDF interchange,
or PROOF SVG and plain SVG. Each format has its own filter. Selecting a
format updates the filename suffix before confirmation; All Files retains
filename-based dispatch. Save As continues to exclude copy-only outputs.
The GTK chooser is used for these save dialogs: FileDialog returns only the
path, and the Windows native FileChooserNative getter loses the chosen filter.

Both writers now refuse unsupported opacity, masks, filters and blend modes
on layers and the SVG root, as well as ordinary objects. Root clipping is
refused. Page pruning also preserves objects inside both a removed artboard
and an overlapping retained artboard; unit and CLI checks cover this case.

The initial Illustrator 7 checkpoint on 2026-10-17 passed reader 70 tests with one opt-in
skip, importer 132 with two, PDF/output 5, and document 86. The legacy output
smoke passes 44 checks, modern native output 32 and PDF interchange 23.
The old layer-layout correction was checked on a copied synthetic legacy
control: layer colours are fixed, with unchanged geometry and paint (5/5).
All 871 cached owner records use fourteen-operand layers, so no ten-operand
owner inputs were affected. No new broad corpus acceptance is claimed.

Illustrator 29.2.1 opens the generated legacy control with one correctly
positioned artboard, editable compound paths, clipping, layer flags/colours,
CMYK numbers, spot tint and overprint. A process fill edited to 40/30/20/10
and saved as a new PDF-compatible Illustrator file reopens natively in PROOF
with those properties (10/10 round-trip checks). Illustrator also opens the
generated A4 control as one precisely sized artboard. GUI Save a Copy (legacy),
Save As (modern) and plain SVG selection pass, with the input SVG unchanged
(4/4 checks). Installed-build evidence is recorded in the workspace handoff.

### Smooth points and opaque linear fills (2026-10-17)

Both record variants now write corner/smooth destination flags, and the
reader/importer retain them. Explicitly marked corners stay corners even when
their handles align. Smooth interior anchors, closed seams and ellipse anchors
survive the Illustrator control. SVG auto and symmetric points become ordinary
smooth points: those additional editing constraints have no separate encoding
here. An open path's initial `m` has no point-type flag, so Illustrator chooses
that first point's type; the control's first point became smooth. This is an
explicit limit, not a claim that every SVG node constraint survives.

`ai-native-writer-gradient.*` adds **opaque pad linear fill gradients** with
homogeneous standard RGB or DeviceCMYK stops. Stop-only references work;
`userSpaceOnUse`, `objectBoundingBox`, local numeric geometry and nonsingular
affine transforms are supported. The placement uses each shape's local
geometric bounds and the inverse gradient scalar field, so a skewed or
non-square bounding-box gradient retains its colour bands. Definitions use
`Bd`/`Bs`/`BD`; each final painted contour has its own `Bb`/`Bg`/paint/`BB`
instance. The shared encoder uses absolute art-space `Bg` geometry with an
identity matrix for both forms, without relying on optional imaging matrices.
Compound paths keep their final gradient paint and clips remain editable.
Each painted shape currently receives its own gradient definition; shared SVG
stop-vector links become independently editable gradients in the output.

The writer refuses **any stroke on a gradient-filled shape**, including a
zero-width stroke, and refuses radial gradients, stop/fill/object transparency,
spot or Registration stops, mixed stop models, ICC-tagged stops (including
those assigned a document profile on import), repeat/reflect, hard stops or
offsets which coincide after rounding, nontrivial inherited geometry/transforms,
relative user-space lengths, invalid references and degenerate placement.
SVG offsets are normalized in their original order; they are never sorted to
repair a decreasing SVG ramp. There are at most 256 effective stops, including
terminal padding. Unsupported features still produce no records and preserve
the previous destination.

The PDF side also needed a placement correction: a shared bounding-box
gradient previously used the combined visual bounds of all its users. Pad
linear/radial fills on shapes now use each shape's geometric box, including
when the paint is inherited, with that box included in the pattern cache key.
This change does not extend repeat/reflect coverage or text gradients. Radial
PDF placement support does not enable radial native writing.
Marker context paint retains its previous placement path; the marker's bounds
must not replace those of the element providing its paint.

**Focused automated checks:** reader 70 passed with one opt-in skip, importer
138 with two, gradient writer 9, and PDF/output 5. The new CLI controls pass
20 checks; PDF placement controls pass 11. The marker context-fill and
context-stroke controls use an analytic ramp and also pass on the previous
build; forwarding marker bounds failed both before the correction. The separate
gradient tests exercise both record variants, an affine scalar-field control, shared bounding-box
instances, a compound hole, clipping, solid-paint state and named refusals.

**Illustrator 29.2.1:** the modern RGB control opens with editable linear fills,
and nine sampled painted, clipped and hole-interior points agree with the
reference to within one 8-bit channel value. The modern CMYK control opens
with exact stops `0/0/100/0`, `0/100/100/0` at 40%, and `100/0/0/0`.
Smooth interior flags, explicit aligned corners, closed seams and ellipse
anchors were also inspected. Illustrator converts CMYK stops in an RGB
document to RGB, and RGB stops in a CMYK document to CMYK; the document colour
mode therefore matters when checking channel retention.

The modern CMYK and Illustrator 7 controls were then edited in Illustrator:
their middle CMYK gradient stop became `40/30/20/10`, and each was saved to a
new PDF-compatible Illustrator file. Both reopen through PROOF's ordinary
native path, retaining that stop, the other CMYK endpoints, layers, point
flags, clipping and the compound hole (23 checks, including unchanged original
exports). This qualifies those generated controls. Imported ICC-tagged gradient
stops still fall outside this first writer subset, so reading an Illustrator
save does not yet guarantee that every gradient in it can be written again.
The records currently carry the process model and channel values; explicit
outgoing document-profile assignment remains open. PDF gradients use device
colour spaces. Assigned-profile gradient writing needs separate container,
PDF and Illustrator qualification before this refusal can be removed.

The build also generates CapyPDF's header as an output with a dependency,
rather than an always-running target. Two successive header builds retained
identical bytes and the second retained its timestamp and did no work, avoiding
needless backend recompilation on later iterations. The pinned submodule is
unchanged.

The reader milestone rechecked 871 staged copies below 50 MB: 862 native
(512 partial, 350 still awaiting complete-editability review), nine PDF
fallbacks, zero errors/timeouts/changed inputs and no native losses. The
earlier mesh work accounts for two gains; earlier native comparisons stayed
the same or improved, with unchanged notes. This is reader qualification;
real-document writer round trips remain separate from the controls above.

### Radial gradients (2026-10-20)

Both writers now write opaque pad **radial** fills with standard RGB or
DeviceCMYK stops, on the same terms as linear ones (`ai-native-writer-gradient.cpp`,
`radial_instance`): circles with or without a focal point in both forms;
ellipses, turned or not, in the modern form. Illustrator 7 records refuse an
ellipse by name ("elliptical radial gradients (Illustrator 7)"), as does SVG 2's
focal circle `fr` in either form ("radial gradients with a focal radius"). A
focal point outside the circle is written on it, where the canvas draws it.

**What Illustrator writes**, counted over the owner's 871 staged copies and,
separately, the collaborator's failure pack (140 copies under 100 MB):

- Every radial instance is `Bb`, `x y angle share Bh`, `Bg`, then `Bm` before
  the paint: 13,194 radial instances in 716 files and 53,128 in 76, all with
  `Bh` right after `Bb` (`0 0 0 0 Bh` at the centre); no linear one has it.
  VectorCraft drew the same rule from the 1998 specification (`c0e6d8eb`).
- **A radial definition lists its stops first first, a linear one last first**:
  4,482 radial definitions ascending and 6,906 linear ones descending, without
  exception. Illustrator takes them in that order whatever their ramp points
  say; a radial written last first opened inside out in Illustrator 29 while
  PROOF's reader, which pairs each colour with its own ramp point, read it right.
- `Bg` has an eleventh number in Illustrator's own files, the ellipse's aspect
  ratio (y radius over x), 1 in most, from 0.09 to 11.2 in others.

**How Illustrator reads them** (Illustrator 29.2.1, synthetic controls whose
records were varied with `tools\ai-records-rewrite.py`; evidence local in
`spike\files\radial-2026-10-20\` and `artifacts\ai-radial-output\`):

- **It reads `Bg`, never `Bm`.** A file without `Bm` draws the same; `Bm` is the
  PostScript placement, and Illustrator writes it again on saving.
- **It reads `Bg` two ways, by the file's `%%Creator`.** In a file whose creator
  is Illustrator, `Bg`'s origin and length are relative to the object's
  bounds: origin `0 0` is the box's centre and length 1 is
  √((half-width² + half-height²)/2). In a file from any other application, as
  PROOF's are, they are art-space values and **`Bg`'s angle turns nothing**: an
  ellipse lies along x, its x radius the length, its y radius the aspect ratio
  times that. Illustrator's own lines put into a PROOF file drew nothing (a
  circle of one point at the origin); the same lines with only the creator line
  changed to Illustrator's drew in place, and PROOF's own lines then didn't.
- **`Bg`'s matrix is applied on Illustrator's canvas, y down.** For a file read
  the second way, the art origin there is (4320 − template box x, 4320 +
  template box y): the centre of the 8640-point canvas of Illustrator 3 to 8.
  Measured on one document from a pure translation and two turns (each placed
  to a hundredth of a point as the rule predicts) and from Illustrator's own
  `/RulerOrigin`, then confirmed on a second document of another size, with
  two artboards and six turned ellipses.

PROOF therefore writes an ellipse lying along x with angle 0 (y's radius may be
the larger, as Illustrator's own aspect ratios above 1 are), and a turned one
with `Bg`'s matrix turning it about its centre on that canvas, to ten decimal
places: the turn is about a point some 4,000 points out, where four places put
the ellipse a fifth of a point off. `Bm` is the whole placement, its x axis
along the ellipse's first axis and mirrored as most of Illustrator's are; `Bh`'s
angle and share are taken in it, which is the same as taking them in the
unturned ellipse, as Illustrator does. PROOF's reader takes `Bm`, so it reads
both its own files and Illustrator's.

Claiming Illustrator as creator would let PROOF use the relative form, but it
would misstate the file's producer; that is the owner's decision and has not
been taken.

**Checks.** Unit tests (`ai-native-writer-gradient-test.cpp`, 13): a circle with
a focal point in both forms, a turned and sheared ellipse whose `Bg` is checked
against Illustrator's reading as measured above, an ellipse along y, a focal
point outside the circle, stop order in both kinds, and the refusals. The CLI
smoke `ai-radial-output-smoke.py` (19 checks) writes both forms from six
controls, reopens them natively, compares them with the canvas (within 1),
requires the modern file's PDF page to be PROOF's own PDF export of the same
drawing, compares that page with the canvas on the RGB shapes (Poppler's radial
shading and the canvas's dithering differ by up to 11 there), and checks the
named refusals keep the previous file.

**Illustrator 29.2.1** opened the smoke's modern file with six editable radial
fills; inside each RGB shape its drawing is within 3 to 7 of the canvas (median
0 to 3), and the six turned ellipses of the second document within 4 (median
0). Its own saved copy reopens in PROOF natively, within 1 of the canvas on the
RGB shapes and within 8 of Illustrator's drawing on the CMYK ones. CMYK stops in
an RGB document are converted to RGB by Illustrator on opening, so their colours
differ from PROOF's (median 23); placement does not. The Illustrator 7 file
opens converted, as any version 7 file does, into a CMYK document: centres and
focal points within 0.4 px of the canvas, and its saved copy reopens natively
with them in place.

**CMYK in an RGB document, 2026-10-20.** The canvas now shows DeviceCMYK paint in
an RGB document through the working CMYK profile, as Illustrator does
([document colours](PROOF-document-colors.md), "CMYK paint without a CMYK profile
of its own"). The smoke's last control, an RGB document mostly cyan, reopens from
its own records; before, it opened from its page.

### Gradient strokes, spot and ICC-tagged gradients, and the document's profile (2026-10-20)

**Both writers** now write three more kinds of gradient paint
(`ai-native-writer-gradient.cpp`, `ai-native-writer.cpp`):

- **A gradient fill with a plain stroke.** The fill's instance brackets `b` or
  `B`, and its end says what the operator does to the stroke: `0 BB` after `f`
  or `F`, `1 BB` after `S` or `B`, `2 BB` after `s` or `b` (Adobe's 1998
  specification, table 10: don't stroke, stroke, close and stroke). Illustrator
  writes it so 1,009 times in the owner's 871 copies and 745 in the
  collaborator's pack. Before, any stroke on a gradient fill was refused.
- **Tints of spot inks in gradient stops.** A stop carrying `proof:ink`, as the
  `.ai` import writes it, goes out as its ink at full strength, its name, the
  tint taken from 1 and colour style 3: `0 1 0.8 0.1 (Brand Red) 0.8 3 50 50
  %_Bs` is a 20% tint at the ramp's middle. The ink is declared with the solid
  spots (`%%DocumentCustomColors`, `%%CMYKCustomColor`), one name one
  definition. Spot and CMYK stops mix; spot and RGB stops don't ("mixed
  gradient colour spaces"). Refused by name: an ink defined in RGB or Lab,
  Registration, a stop that names an ink without a definition, and in
  Illustrator 7 an ink name outside Windows-1252.
- **CMYK stops in a profile**, `icc-color()` in the document's profile, as the
  import leaves every process paint of a CMYK file that has one. They are
  written as their numbers, as solid paint already was. Before, every gradient
  read from such a file was refused.

**The modern form** also writes **gradients on strokes**: the same instance,
opened with `1 Bb`, about `S` (`1 BB`) or `s` (`2 BB`), linear or radial,
turned ellipses included. That is "within stroke", Illustrator's default and
what an SVG stroke gradient means; its "along" and "across" have no SVG
counterpart. Illustrator 7 had no gradient strokes (they came with CS6), so its
export refuses them by name. After the Illustrator gate below, radial strokes
with an offset focal point are refused too: Illustrator 29 centres them on open.

At this stage, gradients on both paints and gradient strokes on filled paths
were refused. The later owner control establishes both without appearance
records; they are delivered below, "Filled and dual-gradient strokes"
(2026-10-21). The census below had only unfilled gradient-stroke paths.

**What Illustrator writes**, counted on the owner's 871 copies and, separately,
the collaborator's pack (140 files under 100 MB):

- `1 Bb` 663 times in 2 files and 207 in 5. 804 bracket a stroke operator
  (`S` and `1 BB` 666, `s` and `2 BB` 138); 66 are notes kept after a drawn
  object (`1 0 Xd`, `1 Bb`, `Bg` with flag 2, "disable rendering", `1 BB`,
  `6 () XW`). PROOF doesn't write those notes; Illustrator opening the file
  decides whether it needs them.
- Stops: 24,722 of the owner's are Illustrator's newer form, colour style 6:
  the colour, its own style, an opacity, `6`, midpoint, ramp point. Named stops
  are written twice: a stand-in `… %_BS` of style 3 (`c m y k (name) tint 3`)
  and the exact `%_… Bs` of style 5 (`(name) tint type 5`) or, for Lab inks, 4.
  PROOF writes the 1998 form, which Illustrator 29 already reads for process
  stops.

**The document's profile goes with the file** (`ai-native-output.cpp`). Native
Save As found the profile nowhere it is looked for: Illustrator keeps a
document's profile as a colour space based on it among each page's own
resources (53 of 60 of the owner's files checked; none anywhere else, not in
the private data, the metadata or an output intent), and PROOF's reader takes
it from there. PROOF's PDF export names it only inside the page's form. So
every PROOF `.ai` from a document with a profile of its own reopened in the
working profile, its numbers kept and its colours not: 11 to 16 apart for the
test profile against GRACoL 2006. Now each page also names `/CSDocument
[/ICCBased …]`, one Flate stream of the profile, the page's other resources
kept. LittleCMS writes the profile's description anew when it saves one; its
colour tables are the same bytes. The rendering intent isn't carried; the
import assigns with the working intent, as Illustrator's is a colour setting
too.

**Checks.** Gradient writer unit tests 20/20 (stroke instances and their ends in
both forms, compound stroke gradients, spot stops read back as tints of their
ink in both forms, ICC stops, and the refusals). New CLI smoke
`ai-gradient-paint-output-smoke.py`, 29/29 in the focused runner: a modern
control of six gradient strokes (linear, radial with a focal point, CMYK on a
circle, a compound path, a turned elliptical radial, spot tints), two gradient
fills with plain strokes and a spot fill, reopened from its own records within
1 of the canvas, its page PROOF's own PDF export and within 8 of the canvas on
the RGB shapes; the Illustrator 7 control (fills with plain strokes, the spot
fill) likewise; a CMYK document in the test data's profile, which reopens in
that profile with its stop numbers, drawn as before (0 apart; at the working
profile it was 11 to 16). The linear smoke's stroke refusal is now the
both-gradients one.

**Illustrator qualification** is recorded in the gate below. The earlier
headless checks alone did not establish Illustrator's rendering or profile assignment.

### Embedded pictures (2026-10-20)

Native Save As now writes a picture the document holds itself (a `data:`
address) as Illustrator writes an embedded raster
(`ai-native-writer-picture.cpp`). Counted on the owner's 871 copies (2,752
rasters in 282 files) and the collaborator's pack (2,007 in 83), every one has
this form:

```text
%AI5_File:
%AI5_BeginRaster
() 0 XG
/DeviceRGB XN
[matrix] w h 0 Xh
[matrix] 0 0 w h w h 8 type alpha kind 1 0 4 4 0 0
%%BeginData: n
XI
<colour samples><alpha samples>
%%EndData
XH
%AI5_EndRaster
N
```

- `type` is the 1998 specification's colour type (1 grey, 3 RGB, 4 CMYK) and
  `alpha` the alpha channel count. The field the specification calls
  "reserved" is the colour space (0 grey, 1 RGB, 2 CMYK) plus 16 with an alpha
  channel, in all 4,759 of Illustrator's.
- `n` counts `XI`, its line end and the colour samples, not the alpha plane
  after them (1772 × 1772 × 4 + 3 in one of the owner's).
- Samples are 8 bits, colour interleaved and straight, CMYK as inks; the alpha
  plane is one byte a pixel, after them.
- The matrix carries the pixels, x across and y up from the picture's top-left
  corner, into art space: the importer's with its third and fourth numbers
  negated, written to ten places.
- `N` after `%AI5_EndRaster`: VectorCraft's user found that without it
  Illustrator drops a clipped picture and what follows (2,732 `N` here; `F`
  only after the collaborator's 1-bit image masks).
- `XG` names the file a picture came from and whether it was edited since; an
  embedded picture has none.

**CMYK numbers are kept.** An Illustrator picture keeps its CMYK samples in
`proof:samples`, a TIFF beside the RGB it shows; a TIFF or Photoshop file
embedded in the document is read by PROOF itself (`read_tiff`, `read_psd`).
Either goes out as its own CMYK, 16-bit samples rounded to 8. Any other
picture goes out as the RGB it shows, with its transparency as an alpha channel
when it has any. Where the pixels lie is where PROOF draws and prints them
(`get_paintbox`), so a picture fitted into its box by `preserveAspectRatio` is
written as drawn, inside the box.

**Refused at this initial gate:** links, embedded SVG, slice cropping and all
Illustrator 7 pictures. Links in modern output were added in the next section;
opaque embedded Illustrator 7 pictures were added on 2026-10-21 below. Embedded
SVG and slice cropping remain refused.

**Checks.** Unit tests in the import suite: an Illustrator CMYK raster read,
written and read again keeps its samples and place; an RGB PNG with
transparency, stretched and turned, and an opaque one fitted into a square box,
read back with their colour, alpha and place; the refusals. New CLI smoke
`ai-picture-output-smoke.py`, 11/11 in the focused runner: an RGB PNG with
transparency over a grey box, a turned JPEG, an embedded CMYK TIFF and a
clipped picture with a shape after it reopen from their records drawn exactly
as the canvas (0 apart over the whole page) with the TIFF's CMYK numbers equal;
the page is PROOF's own PDF export, within 4 of the canvas at the RGB pictures.
Illustrator's actual open/save/reopen results are recorded below.

### Illustrator gate and writer controls (2026-10-20)

Illustrator 29.2.1 opened the modern gradient-paint control, its Illustrator 7
counterpart, the profiled CMYK control and the embedded-picture control. The
owner approved these checks. Calls were serialized with ten-second pauses
after document I/O.

- Gradient strokes, ordinary strokes on gradient fills and spot tints are
  editable in Illustrator. An offset radial stroke focus was lost: the
  re-saved records changed `15 15 45 0.3143 Bh` to `0 0 0 0 Bh`, with a
  visible difference of up to 67 channel values at the probes. The writer
  now refuses this case by name and preserves an existing destination.
  Centred radial strokes and focal points on fills remain supported.
- Removing that focus alone was insufficient: Illustrator also expands a
  radial stroke gradient from the path bounds to the stroked bounds. The
  writer now compensates `Bg` in the gradient's unturned frame: the radius
  uses the ratio of the two bounds' diagonals, and the centre reverses their
  box mapping. `Bm` retains the actual imaging geometry for PROOF's reader.
  The original rectangle's radius of 67.5 became 75.4096 in Illustrator;
  writing `Bg` radius 60.4200 restores it. A centred rotated ellipse and an
  off-centre unrotated circle likewise match within 3 channel values.
  **Off-centre rotated radial strokes remain refused**: the extra control
  still differed by up to 18. Do not relax this refusal without an actual
  Illustrator comparison. Fills keep their prior placement support.
- The modern Illustrator copy `fee7740403` falls back to its PDF page in
  PROOF (3.7% lightness, 3.5% colour). The comparison images isolate the
  disagreement to the two spot gradients: the PDF preview draws their spot
  portions white while Illustrator and the native drawing retain the ink.
  This remains the existing spot-gradient preview issue; the visual gate
  and its thresholds are unchanged. It is not a qualified native reopen.
- Illustrator 7 opens and re-saves the control (`a65c903f3f`). Illustrator
  converts its RGB to CMYK on open; placement agrees, with the expected
  colour differences. This is distinct from preserving RGB values.
- PROOF retains the embedded page-resource profile on its own reopen.
  Illustrator instead shows **Don't Color Manage This Document** in Assign
  Profile. Renaming the space `/CS0` and selecting it in page content did
  not fix this; that experiment was removed. At the owner's request this
  remains a follow-up: they normally embed profiles in print PDFs as needed.
  No Illustrator profile-assignment acceptance is claimed.
- Embedded pictures render within 2-3 channel values at RGB probes (alpha,
  rotation and clipping included); the CMYK TIFF differs by at most 11,
  median 2, after Illustrator converts it to the RGB document's space.
  Its saved copy `e8a098efe2` reopens natively in PROOF (0.2% page comparison).
  RGB pictures match the original canvas exactly. CMYK sample preservation
  is verified on PROOF's output, not on Illustrator's RGB conversion.

The owner's saved CMYK control `af7ea26f97` settles two further forms:

- A solid fill and gradient stroke use the solid fill paint, path, `1 Bb`,
  the gradient instance, `b`, `2 BB`. They do not require appearance records.
- Gradients on both use the path, `Bb` and the fill instance, `2 BB`, then
  `1 Bb` and the stroke instance, `b`, `2 BB`. These are consecutive blocks
  for the same path; the first does not contain a separate paint operator.
- Three links use `%AI5_BeginPlace`, `%%FileType: 1`, `[matrix] llx lly urx
  ury 4 4 0`, the path and backquote, `%%IncludeFile:`, optional relative-path
  and preview records, `~`, `%AI5_EndPlace`. A 120 by 80 pixel TIFF at 300 ppi
  has a 28.8 by 19.2 point box and scale 1. A 100 by 50 pixel PNG at 150 ppi
  has a pixel-sized box and scale 0.48; at 250% its matrix scale is 1.2 and
  its box is unchanged. Use the transformed stored box; do not infer its
  units from the file type or multiply by resolution a second time.

These controls established the next writer forms. Linked raster pictures
and filled/dual-gradient strokes are delivered in the 2026-10-21 sections
below, with independent Illustrator qualification. Artwork and comparison
images remain local.

**CLI qualification must select the exporter explicitly**:
`--export-extension=org.inkscape.output.ai.native`. A diagnostic generated
using only an `.ai` suffix selected PDF interchange, contained no `/PieceInfo`
records and prompted Illustrator to reinterpret an unknown PDF shading.
The owner confirmed the actual native control opened without that warning.
Verify the container as well as the tool's success response; a modal warning
can coexist with a reported successful open.

**Gate follow-up checks:** gradient writer 23/23; importer 143 cases with
zero failures and two opt-in skips; gradient-paint CLI smoke 31/31 (including
both new safe refusals); radial-fill CLI regression 22/22. The final native
control `63c0cc01dd` opened in Illustrator and all RGB probes differ by at most
3, including both corrected radial strokes. The earlier full focused run's
other twelve unit suites and all sixteen export smokes passed; its first
version of the new unit fixture failed because it used unsupported geometry
inheritance, then was corrected and superseded by these final unit results.
The square-tile layout and paint behavior live checks pass 7/7 and 27/27.

### Linked raster pictures (2026-10-21)

Modern native Save As keeps readable local PNG, JPEG, classic TIFF and
Photoshop PSD pictures **linked** (`ai-native-writer-picture.cpp`), including
relative SVG addresses saved into another folder and Unicode file names.
It writes an absolute file address, preserving the external file itself;
it does not convert its CMYK, alpha, precision or Photoshop layers to a raster
inside the Illustrator records. The PDF half still draws the current image
and retains the TIFF/PSD CMYK print samples through the existing PDF exporter.

The placed-file form is:

```
%AI5_BeginPlace
%%FileType: 1
[a b c d tx ty] 0 0 pixel-width pixel-height 4 4 0
(absolute file address)`
%%IncludeFile:absolute file address
~
%AI5_EndPlace
```

The matrix maps the picture's pixel bounds directly onto the art. Unlike an
embedded raster, it does **not** reverse the rows. Use the actual paint box
inside an SVG image's frame, so `preserveAspectRatio` fitting retains its
position. Parent transforms, reflection and clipping are handled as for
embedded pictures. A pixel-sized box works for TIFF independently of its
resolution: Illustrator 29 opened and re-saved a 120 by 80 pixel, 300 ppi
control at 28.8 by 19.2 points with that convention. No resolution is applied
twice. Native file-address strings escape parentheses, backslashes and UTF-8
bytes using the ordinary PostScript string rules.

**Refused by name:** missing or unreadable links, non-file addresses, unsafe
or overlong address lines, file signatures outside the supported raster
subset, slice cropping and linked pictures in Illustrator 7. PSB and BigTIFF
remain outside this first writer slice. Adobe's
[supported-format list](https://helpx.adobe.com/illustrator/kb/supported-file-formats-illustrator.html)
names PSD/PDD for placing; PSB has not been qualified here. Reading a PSB in
PROOF does not qualify writing a link that Illustrator must read.

A changed file modification time is also refused until the picture is
reloaded. This protects the loaded image/PDF from being paired with a newer
external link. The check uses the loader's timestamp; it is not a content
hash or a lock against external edits during saving.

**No cached link preview is written yet.** A missing link at save time is
refused. If the asset is moved after saving, relink it; the PDF half retains
the appearance at save time, but no native missing-link preview is promised.
Relative folder/package metadata is a later portability step; absolute links
make the current Save As behavior deterministic.

**Qualification:** synthetic control `865102886d` contains seven external
images: alpha PNG, rotated JPEG, CMYK TIFF at 300 ppi, CMYK PSD, a fitted PNG
under a Cyrillic/CJK/accented/emoji path, a clipped JPEG and a reflected PNG,
plus art after the images. Illustrator 29.2.1 finds all seven files as links.
RGB probes differ by at most 3; TIFF and PSD probes by at most 8, median 2.
Its saved copy `20b50f558e` reopens natively in PROOF (0.224% lightness,
0.053% colour difference; editability remains `NATIVE_UNVERIFIED`).

The headless check verifies external links after Save As elsewhere, native
reopen, exact TIFF/PSD CMYK samples in the PDF, source hashes unchanged, and
safe refusals. Pixels after native reopen agree exactly away from a fitted
image's two-pixel antialias fringe: SVG's larger frame permits filtering
outside the actual pixel bounds; a placed object's frame is the pixel bounds.
The acceptance thresholds in the production reader are unchanged. Three
importer unit cases cover transformed/fitted geometry and escaped file names,
a file disappearing after load, and a changed file requiring reload.

Final checks: all thirteen focused unit suites and sixteen export smoke
scripts pass. After the last format guards, the importer was rerun:
146 cases, zero failures, two opt-in skips; linked-picture smoke **13/13**.
The installed writer produces the Illustrator-qualified control's same
records apart from its generated artboard UUID. The final format checks
explicitly preserve a prior destination for PSB and BigTIFF refusals.

### Filled and dual-gradient strokes (2026-10-21)

Modern native Save As writes a plain fill with a gradient stroke and
independent gradients on both paints. They remain **one editable path**,
including an open path or a compound path with holes. Each paint gets its
own definition and placement; the path is not duplicated or flattened.
Existing spot/ICC stop handling and safe refusals apply to each paint.

The owner's control `af7ea26f97` establishes the sequence. With a plain fill,
write its colour, the path, `1 Bb`, the stroke instance, `b`, `2 BB`. With two
gradients, write the path, `Bb` and the fill instance, `2 BB`, `1 Bb` and the
stroke instance, `b`, `2 BB`. The fill block has no separate paint operator.
For an open path use `B` and `1 BB` instead. A compound path puts both
instances on its final contour. Paint order remains fill then stroke;
reversed paint order is refused.

**Linear stroke placement:** Illustrator expands a simple path's ramp from
its geometric bounds to its stroked bounds, measured in the ramp's frame.
Compensate `Bg`'s origin and length there. Keep the actual whole scalar field
in `Xm` for the native/PDF reader; Illustrator ignores that imaging matrix
on open and uses `Bg`. A ramp with no path extent along its axis is refused.
Compound paths receive no such expansion, so neither linear nor radial
strokes compensate their bounds. This also corrects existing linear stroke
writing; horizontal butt-cap lines and earlier compound controls retain
their prior placement.

**Radial fill highlights:** Illustrator shares `Bh` across consecutive fill
and stroke instances. A zero highlight on the centred radial stroke would
also erase the fill's noncentral focus. Omit that reset after a gradient
fill; PROOF's reader starts each stroke instance centred, and Illustrator
retains the fill focus and centres the stroke. Stroke focal points remain
refused. Off-centre rotated radial strokes remain refused on simple and
compound paths. No reader thresholds change.

**Actual Illustrator 29.2.1:** synthetic native control `4604c1bc74` has two
plain fills with linear/radial strokes and seven dual gradients covering
all four linear/radial pairs, an open triangle, a compound hole, a
rotated/scaled shape and a clip. A black shape follows to check paint state.
All 49 interior probes agree within **one RGB channel value** at 96 dpi,
without image resizing. Focal radial fills retain their offset.
Illustrator's saved copy `a34ac433f9` opens natively in PROOF with **0.000%
lightness and colour differences** at the unchanged reader gate; editability
is still classified `NATIVE_UNVERIFIED`.

The headless check verifies independent instances on one path, open geometry
and holes, whole-page native-reopen pixels (maximum 1), PDF probes (maximum
4), unchanged source hashes and destination-preserving refusals: Illustrator
7, reversed paint order, fill/stroke transparency, radial stroke focus and
stretched strokes. Two initial PDF probes were exactly on antialias edges
and were moved inward; production thresholds and the established radial PDF
tolerance are unchanged. Unit cases also cover independent geometry, dash
style, following plain paint, compound placement and shared highlights.

An extra Illustrator render control gives the radial fill a different centre,
size and rotation from its radial stroke; all 49 probes still differ by at
most 1. After the final refusal-guard review, the installed writer's control
records match the qualified control apart from the generated artboard UUID.

Final checks: **13 focused unit suites** pass (gradient writer **27/27**;
importer 146 cases, zero failures, two opt-in skips). All **17 export smokes**
pass, with the linear smoke's corrected **20/20** rerun superseding two stale
refusal assertions; gradient paint **29/29**, dual gradients **13/13**,
radial fills **22/22**, embedded pictures **11/11**, linked pictures **13/13**.

### Opaque pictures in Illustrator 7 (2026-10-21)

The standalone copy format now writes opaque embedded grayscale, RGB and CMYK
pictures as 8-bit samples. CMYK and grayscale TIFF planes keep their numbers;
CMYK also comes from retained Illustrator samples or embedded Photoshop data.
Other supported containers use the RGB pixels PROOF draws. Existing sample
conversion rounds 16-bit values to 8-bit; this is not a 16-bit legacy output.
Rotation, reflection, fitting, affine placement and explicit path clips use
the shared picture geometry. `N` follows every `%AI5_EndRaster`, retaining
clipped pictures and the art after them.

The encoding follows section 8 of Adobe Developer Support's
[1998 Illustrator format specification](https://13thmonkey.org/documentation/fonts/AI7FileFormat.pdf):
the original twelve scalar operands after the matrix, depth 8, image type
1/3/4 for gray/RGB/CMYK, and zero alpha/reserved/encoding/mask fields. `XI`
introduces uppercase hex samples on `%`-prefixed lines, 32 sample bytes per
line. DSC `%%BeginData: n ASCII Bytes` / `%%EndData` brackets count `XI`, its
newline, the prefixes and the encoded lines. No modern raster operands,
`XN`, `Xh` or `XH` are written. Hex takes about twice the raw colour bytes,
plus line prefixes and endings.

**Document mode matters.** Without a mode hint, Illustrator 29's legacy
converter opened the RGB control as CMYK and changed RGB probes by up to 49
channel values. The header now carries `%AI9_ColorModel: 1` for RGB or `2`
for CMYK. This is a later advisory comment; the artwork remains version 7.
Section 1.5 describes older applications ignoring newer comments/operators.
The hint is checked in current Illustrator, not an actual Illustrator 7
executable. Document ICC assignment is still outside this format's scope.

**Refused by name:** non-opaque picture alpha, external links, embedded SVG,
slice cropping, object/group opacity and the shared unsupported appearances.
An all-opaque alpha plane is removed; no picture is silently flattened to
make transparency fit. Validation leaves an existing destination intact.

**Reader safety:** uncounted legacy `XI`/hex data is not implemented. It now
fails with "raster data without byte-count brackets" instead of silently
skipping the picture when a standalone file has no PDF comparison. Counted
hex rasters reopen from their native records. General uncounted legacy raster
import remains a separate reader task; comparison thresholds are unchanged.

**Qualification.** The generated seven-picture RGB control `2acfae7a45`
opened in Illustrator 29.2.1 as RGB. At 96 dpi, RGB probes (including fitted,
reflected, clipped and rotated placements) differ by at most 3, grayscale by
5, CMYK preview by 11, and the following shape by 0 channel values. PROOF's
own native reopen matches all sampled pixels exactly. Its PDF retains the
CMYK plane byte for byte, with RGB probes within 4.

The CMYK control `50470d94bc` opens as CMYK. Its Illustrator-saved modern
copy `19c984a343` retains the exact 24 by 16, 8-bit CMYK sample plane in its
private records. That copy opens in PROOF through its PDF because Illustrator
writes the still-unsupported `XC` operator. This qualifies the CMYK sample
preservation, not a native reopen of Illustrator's saved copy. No `XC`
semantics or PDF acceptance thresholds were changed. Actual version 7
application compatibility and wider real-artwork writer coverage remain
unqualified.

Final automated checks: **13 focused unit suites** pass, including reader
**72 cases, zero failures, one opt-in skip**, importer **148 cases, zero
failures, two opt-in skips**, and gradient writer **27/27**. All **18 export
smokes** pass: legacy picture **13/13**, legacy output **44/44**, modern
embedded pictures **11/11**, links **13/13**; mask, foreign PDF, print-sample,
placed-page, path-text and Unicode-path regressions also pass. Both final
legacy controls are byte-identical to the Illustrator-qualified controls.
No wider artwork corpus was run in this writer round.

### Windows-1252 names in Illustrator 7 (2026-10-21)

Layer and ink names, supplied gradient names and the document title now use
strict Windows-1252 in standalone output. Characters that fit are retained;
characters outside that encoding refuse the save by name. There is no
transliteration, replacement question mark or silent loss of an ink name.
The converter also refuses invalid UTF-8 input and the five undefined byte
values 81, 8D, 8F, 90 and 9D. The character table is Microsoft's
[Windows-1252 mapping, published by Unicode](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP1252.TXT).

The existing PostScript string escaping follows conversion: high bytes and
control bytes use three-digit octal escapes; parentheses and backslashes
are escaped. The file remains ASCII. The same encoding is used in layer
`Ln`, solid fill/stroke ink operands, custom-colour declarations, gradient
definitions/instances and spot-stop names. The title is now written in a
`%%Title` header. Modern output retains its existing Unicode bytes and
broader name repertoire.

The traversal still generates unique gradient identifiers per paint; this
change does not add retention of SVG gradient labels. The lower-level
writer's supplied gradient name uses the same strict conversion as its
references. Unsupported gradient geometry or paint still refuses the save.

The reader selects Windows-1252 for strings and literal names when a known
old `AI5_FileFormat` header declares version 1 to 3, including the colon
header form. It no longer guesses that an old name's C3 A9 bytes mean the
single UTF-8 character U+00E9: in Windows-1252 those bytes are the two
characters U+00C3 U+00A9. The FE FF pair likewise remains the two Latin
letters U+00FE U+00FF in that mode. Modern or undeclared records keep the
existing UTF-8/Windows-1252 heuristic and BOM-marked UTF-16 decoding.
PDF comparison thresholds and unsupported-feature refusals are unchanged.

Unit coverage includes all 123 defined high-byte mappings, strict refusals,
escaping, distinct names whose bytes resemble UTF-8, old-header selection,
the title header, ink names and tints, and gradient definitions/references.
The installed-editor legacy CLI smoke passes **59/59**. It checks exact
encoded name/title bytes, three native layer names, named full-strength inks
and their 30%/60% tints, unchanged UTF-8 source, modern Unicode names, and
atomic refusals for unsupported layer/ink/title characters. Its first
58/59 result omitted the valid full-strength definitions from a tint
assertion; the corrected rerun supersedes it, with no production change.

In Illustrator 29.2.1 on Windows, generated control `33ef4abc4d` keeps all
three layer names and both ink names exactly, including byte pairs that
look like UTF-8 or a BOM, euro/ligature characters and escaped punctuation.
Its two tints remain 30% and 60%. Illustrator's modern saved copy
`c96227c80c` reopens natively in PROOF with the same logical names. A
record-only variant `7d868b955a` changes the generated gradient identifier
to a Windows-1252 name using the same byte escaping as the tested low-level
writer; Illustrator reads that gradient name and its references exactly.
This is a synthetic names check, not wider artwork coverage or an actual
Illustrator 7 application test.

Final checks: all **13 focused unit suites** pass; reader **73 cases, zero
failures, one opt-in skip**, importer **150 cases, zero failures, two opt-in
skips**, gradient writer **28/28**. All **18 export smokes** pass with the
corrected legacy **59/59** rerun superseding its initial assertion failure.
The existing legacy-picture smoke remains **13/13**; its records differ from
the previous Illustrator-qualified picture control only by the added title.
No broader artwork regression or new reader-corpus acceptance is claimed.

### Registration and picture slice crops (2026-10-21)

Qualified in Illustrator 29.2.1 the same day, after one correction to the crop
clip (below, "Illustrator gate"). The first part of this section describes the
candidate `d004d655ef` as it was written.

Modern native output writes canonical Registration fills and strokes with
`Xs`/`XS`, the inverted tint, and a `CMYKProcessColor` declaration for
`[Registration]`. It does not declare an ordinary custom ink named `All`.
The document's reserved identity is `proof:ink="All"`, with an opaque CMYK
alternate of `1 1 1 1`; this is the representation produced by native and
PDF imports. Display labels alone do not identify Registration. Ordinary
case-sensitive ink names such as `all` and `All ` remain ordinary inks.

The PDF half and direct CapyPDF output use a cached
`[/Separation /All /DeviceCMYK ...]` colour space. The Type 4 tint transform
`{ dup dup dup }` maps the tint to all four alternate channels. Mask rendering
keeps its existing luminosity preview. This is Registration support only;
ordinary spot separations, DeviceN and PDF/X remain separate work.

Native output refuses malformed Registration metadata, invalid tints,
noncanonical alternates and the ambiguous literal ink name `[Registration]`.
Registration in gradients remains refused. Illustrator 7 now also refuses
solid Registration explicitly: its version-specific encoding has not been
qualified, so it can no longer silently write it as a custom spot.

Picture slice crops now retain the complete raster or original link inside
an editable clipping group. The four viewport corners pass through the same
affine as the picture, preserving rotation, reflection and skew. Existing
explicit clips remain outside that viewport clip. Both native versions use
this geometry; the legacy restrictions on alpha and links still apply.
PDF raster output clips to the image viewport before painting its fitted
pixels. Meet and stretched placements retain their prior geometry.

An Illustrator-created synthetic control, `4c180201da`, was saved before
live automation stopped. Its records confirm `Xs`/`XS`, the process-colour
declaration and full/40%/0% fill and 60% stroke tints. The subsequent legacy
reference-save attempt ended with a failed COM RPC and Windows
`AppHangTransient`; the owner reported Illustrator had crashed. The cause
is unproven. No further Illustrator automation ran, and this is not
Illustrator acceptance of PROOF-generated candidate exports.

Combined final verification passes **13 unit suites and 20 export groups
(692 checks)**: importer **154 cases, zero failures, two opt-in skips**;
Registration **80/80**, crop **27/27**. All sampled native crop pixels match
the canvas exactly; PDF probes differ by at most 5. CMYK samples are retained
exactly, including behind clips. The initial new tests incorrectly expected
a gradient-reference form to be supported and absolute link spelling in saved
SVG; corrected importer/crop reruns supersede those expectations. No production
thresholds were changed. Original reports and reruns remain separate.

#### Illustrator gate, and a closing side for the crop clip

Illustrator opened the candidate's five controls one at a time, each read
through its scripting interface with ten idle seconds after every open, export,
save and close; nothing was driven by a script that waits inside Illustrator.
(The earlier batch slept ten seconds inside one ExtendScript call, which holds
Illustrator's main thread: a likely cause of its `AppHangTransient`.)

**Registration passes as written.** The modern control opens with Illustrator's
own `[Registration]` swatch on three objects: fills at 25% and 100%, strokes at
60% and 100%, overprint on the fill and stroke of the tinted box and on the
stroke of the open frame, as in the source. Inks named `all`, `All ` and a
swatch labelled `[Registration]` whose ink is `PROOF Orange` stay ordinary
spots. Illustrator's saved copy reopens in PROOF natively with the same seven
paints, tints and overprint sides. The Illustrator 7 control, where Registration
is refused and an ordinary spot stands in, opens with every tint exact.

**The crops did not, at first.** Every slice-cropped picture showed its clip's
lower left as a quarter round. Illustrator read each clip with three anchors:
the fourth corner had become the first anchor's incoming handle. Its own copy
writes the same clip as `m`, `L`, `L`, then a closing segment that ends on the
first point (`… V`), then `h W n`. **Illustrator takes a closed path's last
point for its first**: it always writes the closing side back to the start,
and on reading folds the last point into the first. Counted in its own files:
1,235,531 closed paths in the owner's 871 copies and 647,616 in 139 of the
collaborator's pack (files over 100 MB left out), and not one ends away from
its first point. PROOF's paths already ended there (`path_data` writes the
closing segment); the crop clip was the one path written by hand, with four
corners and no closing side. PROOF read it back square, so the smoke passed:
writer and reader agreed with each other, not with Illustrator.

The viewport clip now writes the first corner again before `h`. The crop smoke
checks that every closed path in the written records, modern and version 7,
ends on its first point (30/30); a sweep of every `.ai` the export checks write
found no other path that closed short. Regenerated controls in Illustrator:
the modern, version 7 and linked controls all draw as PROOF's canvas, every
sampled point inside a crop within 3 (the CMYK picture within 4), every point
outside exactly white, the art after them identical. The linked control keeps
all 25 links to their files, none broken, each picture at full size behind its
clip. Illustrator's saved copy of the modern control reopens in PROOF natively
with 25 pictures and 24 clips; every probe matches PROOF's canvas exactly but
the CMYK picture's, within 3.

#### Numbers that aren't numbers

A path whose coordinates overflow (a 10 pt path under `scale(1e308)`) was
written `inf -inf L`, with `inf w` and `-nan d` for its stroke: a file no reader
takes. Native Save As and the Illustrator 7 copy now refuse it by name:
"nonfinite path coordinates", or "nonfinite stroke widths or dashes" when only
the stroke overflows (native output smoke, two new checks, 34/34). Suggested by
VectorCraft's `2b0d71db`, read as a delta only.

#### Linked pictures on another computer

The writer refuses a linked picture on another computer by name ("linked
pictures on another computer"), as it is not looked up there (owner's choice,
2026-10-21): `PROOF-links.md`, "Links on other computers".
