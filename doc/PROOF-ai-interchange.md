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
  definition. Spot and CMYK stops mix; spot and RGB stops didn't ("mixed
  gradient colour spaces") until 2026-10-22 ("Gradient stops of different
  colour models"). Refused by name: an ink defined in RGB or Lab,
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

### The Illustrator 7 copy: what it keeps and refuses (2026-10-21)

The bounded Illustrator 7 export is closed for the subset below: each form was
checked alone in its own round, and now all of them together, in one control
(`testfiles/cli_tests/ai-legacy-representative-smoke.py`), in PROOF and in
Illustrator 29.2.1. An actual Illustrator 7 executable remains untested.

**Kept:**

- One artboard: the document's, or the page Save a Copy or `--export-page`
  names; its size exact, its origin where Illustrator's converter puts it.
- Layers in order, with Windows-1252 names, their highlight colour (as
  Illustrator's nearest preset), hidden, locked, non-printing, outline, and
  image dimming at 50%. Objects outside any layer go to a layer named `Layer`,
  above the others.
- Groups, nested and transformed; clones, as copies of what they show; clip
  groups whose clip is one path (compound paths included); locked objects.
- Paths, open and closed, compound under either fill rule, with their smooth
  and corner points. Every closed path ends on its first point.
- Strokes: width, caps, joins, miter limit and dashes, on objects scaled the
  same both ways.
- Paint: CMYK; RGB (Illustrator converts it in a CMYK document); spot inks
  defined in CMYK, with Windows-1252 names, at any tint; overprint of fill and
  stroke.
- Gradient fills: linear and circular radial, with CMYK, RGB or spot-ink stops.
- Opaque embedded pictures at 8 bits, grey, RGB or CMYK: fitted, cropped by
  `slice`, turned, reflected and clipped.

**Refused by name** (nothing is saved and an earlier file stays as it was):

- Only in Illustrator 7: more than one artboard; individually hidden objects;
  nested layers; image dimming other than 50%; names outside Windows-1252;
  gradients on strokes; elliptical radial gradients; transparent pictures;
  linked pictures; Registration ink; and, since the modern form writes them
  (evening of 2026-10-21, below), transparency, blend modes and gradient
  stops' own opacity, and (night of 2026-10-21) spot inks defined in RGB or
  Lab.
- In both forms: type; transparency and blend modes on layers or the whole
  document; a fill and a stroke each with an opacity of its own; opacity
  masks; filters; patterns; meshes; markers; non-scaling strokes; strokes
  painted under their fill; strokes on stretched or skewed objects; clips of
  more than one path object or object-bounding-box units; clipping on a
  clip's geometry or the SVG root; repeating or reflected
  gradients; gradient hard stops; spot inks defined in other colour spaces;
  context paint; numbers that overflow.

Unit checks: `BothWritersNameEveryFormOutsideTheirSubset` (new) and
`LegacyWriterRefusesFeaturesThatNeedTheModernFormat`, with the export smokes.

#### The control in Illustrator

The control (`459dccc7a4`, 600 by 520 px, CMYK) has eleven layers with
accented names, every layer option, the forms above and an object outside the
layers. In PROOF its copy is one ASCII file whose closed paths all end on their
first point; it reopens from its own records with every layer option, both
inks and their tints, overprint and the pictures' samples, and draws as the
source at every probe (0 channel values apart). Written again from that
reopen, its records differ only where the RGB paints became CMYK (the reopened
document has the working profile, which the writer converts through), and from
then on not at all.

Illustrator 29.2.1 (one MCP operation at a time, ten idle seconds after each
open, export, save and close) opens it as CMYK with all twelve layers in
order, their names, colours, hidden and locked; six plates (process and the
two spot inks, named, their CMYK definitions exact); overprint on exactly the
two objects, at their tints. Its drawing at 96 dpi is within 5 channel values
of PROOF's at the flat paints, compound holes, clips, crops, the clone, the
groups, the layer probes and the RGB, turned, reflected and clipped pictures
(the non-printing layer is drawn by both), and within 8 at the CMYK picture and
the spot-ink gradient. Four places differ by how the two programs show colour,
not by what the file holds:

- **CMYK gradients** (up to 42 between stops; the ends and stops agree):
  Illustrator blends a CMYK gradient in CMYK, as it prints; PROOF's canvas
  blends the stops' screen colours.
- **The grey picture** (up to 31): Illustrator shows it as black ink on paper,
  never darker than K alone; PROOF's canvas shows the grey as it is.
- **A deep green spot** (20, in red only): the two clip a colour outside the
  screen's range differently.
- **The RGB circle** (24): Illustrator converts it into the CMYK document.

Edited in Illustrator (a compound path's fill to 40/30/20/10) and saved as a
modern copy, it reopens in PROOF natively: twelve layers with every option as
written, the edit, both inks and their tints, overprint, the locked object and
six pictures with their print samples (Illustrator converted the RGB ones to
CMYK). It draws as PROOF's own canvas everywhere but the edited shape and the
RGB circle.

#### Found on the way, and fixed

- **Grey pictures went to print in RGB.** The import shows every picture
  through an RGB PNG and kept print samples only for CMYK, so a grey picture
  came back RGB from any save, and Illustrator then put it on all four plates.
  Grey samples are now kept beside the PNG as well (`proof:samples`, a grey
  TIFF); both writers write them as a grey raster, and PDF export as
  `/DeviceGray` with the same bytes. Unit check
  `WrittenPicturesKeepTheirGreySamples`; the smoke checks the PDF.
- **`XC`, the operator that kept Illustrator's copies on their PDF page**, is
  how Illustrator 29 saves a grey picture it opened from a version 7 file:
  coloured with black, `0 /DeviceGray XC` in place of `/DeviceGray XN`. That
  prints as the grey picture itself, and is read as one now. A picture coloured
  with any other paint is refused by name, "pictures coloured with a paint":
  two of the owner's record sets colour a grey picture with a Pantone ink.
  Illustrator's copies of the opaque-picture controls of the same day open
  natively now, as does this control's. Reader check
  `GreyPictureColouredBlackIsTheGreyPicture`.
- **An accented spot ink name made the SVG unreadable** in the user's own
  locale. Its id was lowercased byte by byte with the C library, which in a
  Windows-1252 locale takes UTF-8 bytes for letters (`Orange spécial` gave an
  id holding byte 0xE3). Ids are ASCII now (`spot-ink.cpp`, check
  `StemsStayAsciiUnderAWindows1252Locale`); the smoke reopens in the
  machine's own locale. Illustrator's copy of the names control of the same
  day opened from its PDF page for this reason; it opens natively now.

#### Real documents: what the two writers would make of them

`tools/ai-writer-census.py` (workspace) reads copies in the importer's test
program and hands each document as read to both writers in memory
(`PROOF_AI_WRITE`): what each refuses, or whether the reader takes its records
back. The owner's 871 staged copies, 2026-10-21, before the fixes above:

| | Illustrator 7 | modern |
|---|---|---|
| Opened natively | 862 | 862 |
| Written (all read back) | **99** (11%) | **129** (15%) |
| Refused by transparency | 660 | 660 |
| Refused by blend modes | 674 | 674 |
| Refused by spot inks defined in RGB or Lab | 138 | 138 |
| Refused by more than one artboard | 64 | — |
| Refused by type | 60 | 60 |

Blend modes and transparency come together (multiply, overlay and soft light
at part opacity, in the owner's sampled files). Writing them would let the
modern form write **636** of the 862 (74%); with Lab or RGB spot inks too,
**736** (85%); with type, **780** (90%). Illustrator 7 has no transparency,
so its copy stays the bounded option it is: 99 files, and 27 more blocked only
by their extra artboards, which a one-page copy avoids.

#### Still open

- PROOF's colour text keeps three decimals (`colors/printer.h`). Of 160,188
  CMYK paint operators in the cached records, 118,819 (in 254 of 1,976 record
  sets) have four, so the import rounds them: by at most 0.05% ink, below a
  plate's step, but Illustrator's panel then reads 74.5% where it read 74.51%.
- The canvas's CMYK gradients and grey pictures, above.
- An actual Illustrator 7 executable; Lab spot inks in either form.

Checks: all **13 unit suites, 0 failures** (reader 74 cases with one opt-in
skip, importer 159 with two, spot inks 19); **21 export groups, 721 checks**,
the representative control **24/24**. The writer census was the round's
corpus run, before the fixes; the 8 of the owner's copies holding a grey
raster or `XC` were rerun after them, unchanged.

### Transparency and blend modes in the modern form (2026-10-21)

The modern records now keep what Illustrator's Transparency panel holds for an
object or a group: its opacity, its blend mode (all sixteen), isolated
blending, and knockout (on, off or neither) with the knockout shape. A
gradient's stops keep their own opacity. Illustrator 7 has none of this, and
its copy still refuses all of it.

**How it is written**, as Illustrator 29 writes its own (300 to 400 of the
owner's cached record sets surveyed, then the same content saved by
Illustrator itself, below):

- `mode opacity isolate knockout shape Xy`, five numbers, the modes in
  Illustrator's order (Normal, Multiply, Screen, Overlay, Soft Light, Hard
  Light, Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Hue,
  Saturation, Color, Luminosity; PROOF's CSS modes come in another order and
  are mapped by table). It holds until changed, so it is written only where it
  changes: after an object's paints and before its path; before a compound
  path, whose paths take it; before a picture.
- A group's own comes after its end: `U` (`Q` for a clipping group), its `Xy`
  (written even when it holds already), `0 0 Xd`, `6 () XW`. A group with none
  of its own ends `9 () XW`, as Illustrator ends one. Without either, the next
  object's `Xy` straight after a group's end would be the group's: the state
  after a group belongs to it, in Illustrator and in PROOF's reader. A
  picture's crop group ends `9 () XW` too.
- A clipped object's transparency is its clipping group's; the object in it is
  opaque, and so is the clipping path, as an ordinary path once released.
- A clone with transparency of its own is written in a group that has it.
- A fill's or a stroke's own opacity (`fill-opacity`, `stroke-opacity`, a
  colour's alpha), when it is the object's only paint, is the object's
  opacity: the same picture. A fill and a stroke each with an opacity of its
  own needs an appearance, and is refused by name ("separate fill and stroke
  transparency"). A gradient-filled object's opacity was refused as "gradient
  transparency"; it is the object's now.
- Stops with an opacity take Illustrator's newer stop, `colour style opacity
  6 midpoint ramp`, and every stop of that gradient is written so. Gradients
  whose stops are all opaque keep the older form, byte for byte.
- Where the transparency in force is not opaque when a layer begins, its first
  object's is written in full: whether a layer starts with what held before it
  is the reader's to say.
- Still refused, by name: transparency or blend modes on layers ("transparency
  on layers"; Illustrator keeps a layer's apart from its objects', in a form
  not yet studied) and on the whole document; separate fill and stroke
  transparency; opacity masks.

#### The PDF page composites in CMYK

PROOF's PDF export (the page an .ai file carries, and Save as PDF) already drew
opacity, blend modes and isolated blending, but its pages named no blending
colour space, so whatever displays or prints the page chose one, most often
RGB. Illustrator's pages of a CMYK document say `/Group << /S /Transparency
/CS /DeviceCMYK /I false /K false >>`, and a RIP composites them in CMYK, as
the inks print. A CMYK document's pages now say the same
(`PageContext::set_blending_space`); groups without a space of their own
inherit it. RGB documents are unchanged. Poppler composites in RGB whatever the
page says (Illustrator's own page draws as PROOF's under it), so this is
checked in the page dictionary, not in pixels. Under poppler, PROOF's page and
Illustrator's page of the same control agree within 3 at every probe but the
RGB picture, which Illustrator converted to CMYK (16), and a stroke's edge
(8).

#### The control in Illustrator

`ai-transparency-output-smoke.py` (new, in the focused suites): one CMYK
document with the sixteen modes over a band and over paper, an object at half
opacity, an isolated screen group with a multiplying member, a knockout group,
a compound path at 70% Overlay, Darken at half, a clipped object, a fill at a
quarter and a stroke at 40% each painting alone, a clone at half in
Luminosity, nested groups at 80% and 50%, a picture, a spot ink and a gradient
each at part opacity, knockout neither on nor off with the knockout shape, a
plain group whose last member is transparent followed by an object, and a
gradient whose stops fade. PROOF: written in Illustrator's form, reopened
from its own records with each of the 47 objects' and groups' transparency in
order, drawn as the source at every probe, the fifteen blend modes in the PDF
page and the page composited in CMYK, the same records written again from the
reopen and again from that; layer transparency and separate fill and stroke
opacity refused by name, and Illustrator 7 refusing it all. 18/18.

Illustrator 29.2.1 (one MCP operation at a time, ten idle seconds after each
open, export, save and close):

- **Read:** every object's and group's opacity and blend mode as written,
  through its scripting interface: the sixteen modes in order, the groups
  (screen at 60%, luminosity at 50% for the clone, 80% and 50% nested, the
  clipping group at 50% Multiply with its path and its member opaque), the
  compound path, the quarter fill, the 40% stroke, the picture, the spot ink,
  the gradient, and the plain group at 100% with its 60% member and the object
  after it at 30%.
- **Drawn:** the knockout group knocks out (its two half-black squares don't
  darken where they overlap), and the gradient fades to show the bar behind.
  Over paper every mode is within 5 of PROOF's canvas. Over coloured inks the
  two differ (up to 191 for Color Burn, 75 for half black over yellow):
  Illustrator composites a CMYK document in CMYK, PROOF's canvas in RGB. That
  is the display, not the file: Illustrator's own PDF page under poppler draws
  as PROOF's.
- **Saved** as a modern copy, whose records hold every object's transparency in
  the form PROOF wrote (`2 0.6 1 0 0 Xy`, `0 0 Xd`, `6 () XW` after the
  isolated group; `0 1 0 1 0` after the knockout group; `9 () XW` after the
  plain one; `0 1 0 2 1` before the knockout-shape object; stops `… 1 0.2 6 50
  100 Bs`), and which **reopens in PROOF natively** with all 47 the same, in
  order, isolation and knockout included.

Local evidence: `artifacts\transparency-20261021\` (`in\`, `out\`,
`records-copy\`, `reopen\`, `compare.py`, `smoke-*`).

#### Real documents

The writer census (`tools\ai-writer-census.py`, the round's corpus run) over
the owner's 871 staged copies, 862 read natively: the modern form writes
**642**, every one read back (129 before), with the object transparency
alone; the 14 still refused for gradient transparency were rerun after stops
kept their opacity, and 4 more write (the other 10 are held by type, spot inks
in Lab or RGB, clips and gradient colours). **646 of 862 (75%).** What holds
the rest, with those 14 as rerun:

| Modern form, 2026-10-21 evening | Files | Only reason |
|---|---|---|
| Spot inks defined in RGB or Lab | 138 | 102 |
| Type | 60 | 28 |
| Clipping paths other than one path | 19 | 12 |
| Gradient colours other than standard RGB or CMYK | 19 | 8 |
| Patterns | 15 | 11 |
| Opacity masks | 7 | 3 |
| Linked pictures missing or unreadable | 6 | 0 |
| Noncanonical Registration ink definitions | 4 | 4 |
| Mesh gradients | 3 | 1 |
| Gradient hard stops or coincident offsets | 1 | 1 |

Spot inks defined in Lab or RGB alone would add 102 (to 748, 87%), type alone
28 more.

The Illustrator 7 copy is unchanged at 99. No document met "separate fill and
stroke transparency" or layer transparency: an import never makes them.

#### Still open

- PROOF's canvas composites transparency in RGB; Illustrator shows a CMYK
  document composited in CMYK, as it prints. With the canvas's CMYK
  gradients, a display item for the colour work.
- The canvas, and so the PDF page, draw a knockout group as an ordinary group
  (the import says so); the records keep it, and Illustrator draws it.
- Transparency on layers, in Illustrator's own form, once studied.
- An appearance with separate fill and stroke opacities.

Checks: all **13 unit suites, 0 failures** (reader 74 cases with one opt-in
skip, importer 160 with two, gradient writer 30); **22 export groups, 739
checks**, the transparency control **18/18**. Three gradient smokes asserted
refusals the modern form no longer makes (a stop's opacity, a gradient
stroke's opacity under a fill, now named as separate fill and stroke
transparency); their expectations were changed and they pass, 20/20, 29/29,
13/13. Unit checks: `WrittenTransparencyReadsBackOnEachObjectAndGroup`,
`GradientObjectsKeepTheirOpacityInTheModernFormOnly`,
`StopsKeepTheirOpacityInTheNewerFormOnly`, and the refusal lists.

### Spot inks defined in Lab or RGB in the modern form (2026-10-21)

Illustrator defines an ink in CMYK, RGB or Lab, and its Pantone books define
theirs in Lab. The import keeps the definition (`proof:ink-alternate`, in the
space the file used); until now both writers refused any ink not defined in
CMYK. The modern records now write all three as Illustrator does. Illustrator 7
had no Lab inks, and its copy refuses Lab and RGB inks by name ("spot inks
defined in RGB or Lab (Illustrator 7)").

**How Illustrator writes them**, counted in the owner's cached records (228
files with a Lab ink; 382 CMYK, 62 RGB and 741 Lab ink definitions):

- An ink's colour carries its CMYK first, then for RGB and Lab the definition,
  the name, the tint taken from 1 and its type: `c m y k L a b (name) tint 2
  Xx` for Lab (L from 0 to 100, then a and b), `c m y k r g b (name) tint 1 Xx`
  for RGB, `XX` on a stroke. `x` knows only CMYK, so an RGB or Lab ink is `Xx`
  at full strength too.
- **The CMYK beside an RGB or Lab ink is that of the colour at the tint used**:
  55 Lab inks used at more than one tint, every one so (Lab 84 12 70 is
  `0 0.2238 0.8364 0` at full strength and `0 0.1034 0.3588 0` at 45%); a CMYK
  ink's numbers are its definition at every tint (38 of 38). Paper, at tint 0,
  is `0 0 0 0`.
- The header names every ink in `%%DocumentCustomColors`, lists CMYK inks by
  their CMYK in `%%CMYKCustomColor` and RGB and Lab inks by their RGB in
  `%%RGBCustomColor`, that list after the other (98 files with only the RGB
  list, 95 with only the CMYK one, 53 with both in that order; every ink in
  its list).
- A gradient stop of an RGB or Lab ink is written twice: a stand-in in colour
  style 3, the ink's full-strength CMYK with its name and tint (`… %_BS`), then
  the exact stop, `%_c m y k L a b (name) tint 2 4 opacity 6 midpoint ramp
  Bs`, the CMYK at its tint (22 stops, all so). Every stop of such a gradient is
  in the newer form, as all of Illustrator's are.

**What PROOF writes** is that, through one helper both writers share
(`ai-native-writer-ink.{h,cpp}`), so one ink name keeps one definition across
paints and stops ("one ink name with two definitions" otherwise). The CMYK
beside an RGB or Lab ink is converted through the document's CMYK profile, or
without one the working CMYK space, by the working rendering intent (relative
colorimetric with black point compensation unless changed), as Illustrator
converts with its colour settings; a tint's is the tinted colour's
(`SpotInk::display_color`, toward paper white). An RGB definition keeps its
numbers; other RGB-like spaces are written as their sRGB. Refused by name: an
ink defined in another space ("spot inks defined in other colour spaces"), or
whose definition is out of range.

**The reader** took a Lab ink's gradient stop for RGB: colour style 4 is the
1998 specification's named RGB, and the type operand Illustrator writes after
the tint (2) was overridden by the style. A type after the tint now decides.
Where the ink is also in the Swatches panel or paints an object, those
replaced the stop's reading, so the import was right; an ink used only in
gradient stops came in as an RGB colour made of its L, a and b.

#### The control in Illustrator

`ai-spot-ink-output-smoke.py` (new, in the focused suites as
`ai-spot-ink-output`): a CMYK document in a press profile with two Lab inks,
one RGB ink and one CMYK ink at full strength and at tints, as fills, strokes,
a filled path with a stroke of another ink, and three gradients (Lab to 30% to
process cyan, RGB to 0%, Lab to the CMYK ink). PROOF: the records in
Illustrator's form, reopened from its own records with each ink's definition
and tints and the document's profile byte for byte, drawn as the source at
every probe, written the same twice more, and Illustrator 7 refusing the
inks. 17/17.

Illustrator 29.2.1 (one MCP operation at a time, ten idle seconds after each
open, export, save and close):

- **Read** every ink as a spot with its tints as written: Lab red at 100%, 35%
  and 30% (a stop), Lab blue at 100% and 60% (a stroke), RGB green at 100%,
  50% and 0% (a stop), the CMYK ink at 100% and 40%.
- **It takes the definitions, not the CMYK beside them**: the CMYK it reports
  for the Lab and RGB inks is its own conversion (Lab red 7.7/98.9/72.9/1.7
  where PROOF wrote 8.4/99.8/81.3/1.0).
- **Saved** as a modern copy that keeps them: Lab inks as type 2 with the same
  Lab, the RGB ink as type 1 with the same RGB, the same header lists, the same
  tints. It writes an RGB ink's gradient stops in colour style 4 with type 1
  (`1 1 4`), not the 5 four older stops in the owner's files have; PROOF now
  writes 4 for both, and Illustrator read the control both ways. The copy
  **reopens in PROOF natively** with every ink, definition and tint the same.
- **Drawn**: the same shapes, tints and ramps. Colours differ by up to 23 per
  channel between Illustrator's picture and PROOF's canvas, the plain CMYK ink
  as much as the Lab ones (22): Illustrator draws a CMYK document's spot inks
  as their CMYK through its own colour settings, PROOF's canvas shows Lab
  directly and CMYK through the document's profile. No single profile
  reproduces Illustrator's export.

Local evidence: `artifacts\spot-inks-20261021\` (`in\`, `out\`, `reopen\`,
`records.py`, `smoke-*`).

#### Found on the way, and fixed: a profile changed when it was saved

The control's third write differed from its second in the CMYK beside the
Lab inks. A document's profile was written into SVG and PDF through lcms,
which writes back the tables a conversion has parsed in its own encoding:
the press profile's 8-bit tables (`mft1`) came back 16-bit (`mft2`), 654 KB
became 945 KB under the same name, and it converted differently (Lab 48 68 36
to 7.8% cyan instead of 7.1%). So a document's profile changed on its first
save after any conversion. A profile now leaves as it came: the bytes it was
read from, or its file while that still holds it (the same checksum); only
profiles made in memory are encoded by lcms (`CMS::Profile::dumpData`, in the
shared colour code; `ColorCmsProfile.dumpDataKeepsTheBytesItCameFrom`). See
`PROOF-document-colors.md`. A profile taken from a PDF through Poppler
(`create_from_copy`) is still re-encoded.

#### Real documents

The writer census (`tools\ai-writer-census.py`, the round's corpus run, 13
minutes) over the owner's 871 staged copies, 862 read natively: the modern
form writes **748**, every one read back (646 before), the 102 files these
inks alone held. No file is refused for an ink's definition now. **748 of 862
(87%).** What holds the rest:

| Modern form, 2026-10-21 night | Files | Only reason |
|---|---|---|
| Type | 60 | 46 |
| Clipping paths other than one path | 19 | 16 |
| Gradient colours other than standard RGB or CMYK | 19 | 12 |
| Patterns | 15 | 14 |
| Opacity masks | 7 | 4 |
| Linked pictures missing or unreadable | 6 | 0 |
| Noncanonical Registration ink definitions | 4 | 4 |
| Mesh gradients | 3 | 1 |
| Gradient hard stops or coincident offsets | 1 | 1 |

Type alone would add 46 (to 794, 92%). The Illustrator 7 copy is unchanged at
99; it refuses an RGB or Lab ink in 138 files, 25 for that alone.

#### Still open

- Lab or RGB inks in Illustrator 7: the ink's CMYK could stand in for its
  definition, the plate unchanged (the owner's call).
- Inks defined in grey, which a PDF import can make: Illustrator has no grey
  definition; refused by name.
- The canvas shows an RGB ink in a CMYK document with a profile through that
  profile, a Lab ink as its Lab; Illustrator draws both as their CMYK.
- PDF output still writes spot inks as their screen colour.

Checks: all **13 unit suites, 0 failures** (reader 74 cases with one opt-in
skip, importer 161 with two, gradient writer 32); **23 export groups, 756
checks**, the spot-ink control **17/17**; the colour profile suite 9/9. Unit
checks: `WrittenRecordsKeepInksDefinedInLabAndRgb`,
`InkStopsDefinedInLabOrRgbAreWrittenAsIllustratorWritesThem`,
`TheCmykBesideALabInkComesThroughTheDocumentsProfile`,
`ColorCmsProfile.dumpDataKeepsTheBytesItCameFrom`, and the refusal lists.

### Compound clipping paths written (2026-10-21, after spot inks)

Both writers now keep a clip made from **one path object with several
contours**, including holes. The records are `q`, the clip's winding rule
(`XR`), `*u`, each closed contour with `W n`, `*U`, the artwork, and `Q`.
Single-contour clips keep their existing form. Open clip contours are closed
explicitly, including their closing side: Illustrator otherwise folds their
last anchor into their first. The clip's own transform and the clipped
object's transform compose before conversion to artboard coordinates.

**The clip uses its computed `clip-rule`, independently of the artwork's
`fill-rule`.** The writer updates the shared `XR` cache, so artwork and nested
clips restore their own rules. Nonzero holes, even-odd holes, disjoint islands,
reflections, nonuniform scaling, nested clipping-group intersections,
gradients, strokes, embedded pictures and modern group opacity are covered.
Smooth/corner point handling still uses the ordinary path writer.

Separate objects inside an SVG `clipPath` are a union, and concatenating
their paths could turn overlaps into holes. They remain refused by name,
as do object-bounding-box clips. A clip on the clip's own geometry is now
explicitly refused instead of being dropped. Empty clip geometry is refused.
Nested clipping **groups** remain supported: they intersect as before.

#### Canvas and PDF corrections found by the control

- The canvas read `clip_rule.value`, ignoring inheritance. It now reads the
  computed rule; an explicit rule on the path still overrides its parent's.
- PDF export read the clipped artwork's rule and reversed the two PDF
  operators. A single clip object's own computed rule now selects `W*` for
  even-odd and `W` for nonzero; rectangle clips default to nonzero. The
  pre-existing flattening of multiple clip objects is outside this round.

These are shared display/PDF changes, relevant to the collaborator as well
as `.ai` output. They are needed because the native Save As also carries a
fresh PDF page, and the reader compares the two before accepting native art.

#### Qualification

`ai-compound-clip-output-smoke.py` generates a synthetic control with nine
clip instances, eight compound and one simple, plus a modern variant whose
clipped gradient/stroke group has 60% opacity. Both formats reopen natively,
retain every contour and rule, and match all sampled canvas pixels exactly;
a second write keeps the clips. Unsupported clips refuse without replacing
an existing file. The modern PDF preserves every sampled clip/hole and flat
RGB colour. CMYK-gradient colour conversion differs between canvas and
Poppler, so its coverage is checked independently of those colour values.

Illustrator **29.2.1** opened the modern, Illustrator 7 and transparent
controls. All holes, islands and intersections drew correctly; its saved
copies reopen natively with all nine clips and four even-odd rules. Flat
colour and coverage probes match exactly; CMYK-gradient colour differences
are at most 7 channel levels (5 for the transparent variant). Actual
Illustrator 7 remains untested. No production artwork is in these controls.

The owner's same **871 hash-verified staged copies**, active review paths
and exclusions checked, still import as 862 native and nine page fallbacks.
The modern writer writes **764 of 862** (748 before), and the Illustrator
7 copy **100** (99 before); every accepted record stream reads back. All
19 documents held by compound clips lose that refusal; the 16 held by it
alone now write. This census checks record acceptance, separately from the
synthetic Illustrator rendering gate.

The remaining modern families include gradient colours other than standard
RGB or CMYK (19 documents, now 13 held by nothing else), patterns (15, 14
alone), opacity masks (seven, four alone), and type (60, 46 alone).

Checks: **13 unit suites, zero failures; 24 export groups, 775 checks**,
including the new control **19/19**. Reader 74 cases (one opt-in skip), importer
164 (two), gradient writer 32, clip/blend renderer six. New unit cases cover
compound rules/transforms/closure, nested clip state, refusal of a clip on
clip geometry, and inherited/overridden canvas clip rules.

Next: inspect the actual colour spaces behind the remaining gradient-colour
refusals before implementation; then patterns and opacity masks. Type stays
last. Multiple-object clip unions remain a separate feature.

### Profile-tagged sRGB gradient stops written (2026-10-21)

The gradient-colour inventory found standard RGB stops assigned to the
document's ICC profile. These were refused because their in-memory space was
CMS rather than plain RGB, including ordinary RGB gradients on a second save.
Both writers now keep their original channel values as native RGB stops
(style 2). Linear/radial fills, supported strokes, transforms, compound holes,
clips and modern stop/object opacity retain the existing writing rules.

The supported profiles are the lcms built-in sRGB encoding and the classic
IEC sRGB v2.1 encoding with its 1024-entry transfer tables. Matching compares
their original colour-tag bytes, profile class/version, PCS and illuminant,
allowing different descriptions, creation dates and profile IDs. For the
classic encoding, non-transform viewing/measurement metadata is also ignored.
An altered curve or matrix, an extra colour/LUT tag, another RGB profile or
an unavailable profile remains refused by name. This deliberately bounded
recognition does not classify profiles by their name or an approximate colour
comparison. Other sRGB encodings may remain refused; the ICC registry
[lists several sRGB profile variants](https://registry.color.org/rgb-registry/srgbprofiles).

The classic reference is generated from its colour definition; the application
does not need an installed system ICC file. Matching uses the preserved input
bytes because lcms can re-encode a parsed tag after conversion. The modern
document's profile remains byte for byte as supplied; the Illustrator 7 copy
still has no retained document ICC profile under its published contract.
Mixed RGB/CMYK gradients, coincident stops and other unsupported semantics
remain refused without replacing an existing file.

#### Qualification

The new synthetic `ai-srgb-gradient-output-smoke.py` checks both formats,
original RGB channels, linear/radial geometry, strokes, compound holes/clips,
native reopen, byte-identical modern profiles, second/third writes, canvas
and PDF samples within three channel levels, and atomic refusal of a profile
whose transfer curve changed. Description and date changes are positive
controls. The read-only Windows IEC profile supplies an independent classic
control when available; it is not copied into the repository. The smoke is
**41/41** here (**25** checks without that optional system control).

Illustrator **29.2.1** opened four controls: modern built-in sRGB, modern
classic IEC sRGB, the classic Illustrator 7 copy, and a modern gradient stroke
with stop/object opacity. All sampled colours and holes agree within two
channel levels. Every Illustrator-saved copy reopens natively with all eight
gradients (nine in the stroke control), keeps the three RGB colours and the
stroke opacity, and writes/reopens natively again in PROOF. Actual Illustrator
7 remains untested; no production artwork is part of these controls.

The same **871 hash-verified staged copies** still import as **862 native,
nine page fallbacks**. Modern writing rises from **764 to 775 of 862**;
Illustrator 7 remains **100**. Every accepted record stream reads back, with
no lost import or writer acceptance and no changed input copy. All 19 modern
gradient-colour refusals are removed; 11 documents newly write. The other
eight retain type/mesh restrictions or expose a mixed-space gradient or
coincident stops. Record acceptance and Illustrator rendering are separate
checks.

All **13 unit suites pass; 25 export groups, 816 checks**. The gradient-writer
suite has 35 cases, including renamed/dated sRGB, classic IEC and altered or
missing-profile refusals. Remaining modern families: type 60 (50 alone),
patterns 15 (14 alone), opacity masks seven (four alone), missing linked
pictures six, noncanonical Registration definitions four, meshes three,
coincident stops two, mixed gradient spaces one. Next: patterns, then opacity
masks; type stays last by the owner's agreed order.

### Patterns written (2026-10-21, after sRGB gradients)

The modern form writes a pattern on a fill or a stroke as Illustrator 9 and
later write one. Each tile is defined once in the setup, after the gradients:

```text
%AI3_BeginPattern: (name)
(name) 0 0 width height
%_0 A
%_0 Xw
%_... the tile's art, every line hidden, in a state of its own ...
E
%AI3_EndPattern
```

and each object painted with it says where its tiles lie:

```text
(name) 0 0 1 1 0 0 0 0 0 [a b c d tx ty] p      (P on a stroke)
```

The nine numbers are the placement of Illustrator 8 and older, at rest; the
matrix carries the pattern's own space onto the art. PROOF's pattern space is
the tile's with y up, its box from 0,0, so the matrix is the tile flipped
about its height, then SVG's own placement (`translate(x, y)` and the pattern
transform, inherited along the `href` chain), then the object's placement in
art space. Its numbers and the tile's size are written to eight decimals: a
tile repeats, and at four a tile a thousand repeats from its origin would lie
a point off.

**What is written.** Patterns whose tile and content are in the user space of
what they paint: what the import makes of Illustrator's patterns, and what
PROOF's own pattern library holds (259 patterns, all of this form; some
offset by `x` and `y`, most placed through a wrapper with its own transform).
One definition is written for each tile and size, however many objects and
wrappers use it. It takes the tile's label (the import keeps Illustrator's
swatch name there), else its id, numbered when two tiles would share a name.
The tile's art is written as the document's is: paths and compound paths,
groups, clipping groups, process and spot paint with tints, strokes and
transparency. A pattern no object paints with is not written (as for
gradients), so swatch-only patterns are not kept.

**Refused by name:** patterns sized to their object (`objectBoundingBox`
tiles or content), patterns with a view box, patterns inside patterns,
gradients and pictures in a tile (where Illustrator places a gradient in a
pattern's space is not qualified), and empty patterns. The Illustrator 7 copy
refuses `patterns (Illustrator 7)`: its patterns are PostScript procedures.

#### The control in Illustrator

`ai-pattern-output-smoke.py` makes an asymmetric tile (a red quarter and a
ring), so that a tile placed anywhere else shows, and a second one offset by
`x` and `y` with a compound path, a half-transparent group, a clipped group
and a spot ink at 50%. They paint eight objects: directly, turned and scaled
through a wrapper, moved through a wrapper, on an ellipse, on a compound path
with a stroke, as a stroke, inside a turned and scaled group, and with a fill
opacity of its own.

Illustrator **29.2.1** read all eight as written: seven pattern fills and one
pattern stroke under PROOF's names, the fill's own opacity as 60%. Drawn at
96 dpi beside PROOF's canvas, **every tile lies where PROOF puts it** (the
best match is unshifted in all eight areas). Pattern matrices, unlike a
gradient's `Bg`, are not applied on the old 8,640-point canvas. Flat
features inside the tiles agree within **three** channel levels (the red
quarter, the ring, the transparent green over ground and over the CMYK ring,
the clipped circle, the spot ink at 50%), and the turned tile and the stroke
exactly. Its saved copy keeps every matrix and both names exactly, and wraps
each tile's art in a group of its own, adding no box rectangle. That copy
opens natively in PROOF with all eight objects and both tiles, within one
channel level of the original. Saved again by PROOF, it reopens natively
within one again.

#### Found on the way, and fixed: PDF patterns and soft tiles

The native Save As carries PROOF's own PDF page, and the reader opens a file
natively only when its records draw as that page. The first control fell
back to the page: **PDF output placed patterns as SVG does not** (shared PDF
code, `pdfoutput/build-patterns.cpp`). It took the tile's box only from the
pattern holding the art, though a wrapper may set it; drew the art from the
pattern's origin while its box began at `x`, `y`, so offset tiles were cut in
the wrong place; used the wrapper's own transform instead of the one
inherited along the chain; fitted a view box with whatever the tile's last
canvas fit had left; and ignored sizes in the object's units. It now builds
each tiling pattern as the canvas does (`SPPattern::update_view`): the box
from 0,0, the placement `translate(x, y)` and the inherited transform, the
art fitted by the chain's view box or scaled to the object's box. Its matrix
is relative to the painted object's own space, which is where each object is
drawn (its own transparency group). The canvas, unlike SVG, doesn't move a
tile sized to its object to the object's corner; PDF now matches the canvas.

Checking the page needed care: Poppler's splash renderer rounds a turned
tile's cell to whole pixels, so its tiles drift a pixel or two from the
origin, and its Cairo renderer darkens transparency inside a tile. The smoke
reads the tiling patterns' matrices and boxes from the PDF itself, then asks
each renderer only what it draws right: Cairo matches the canvas within 2 on
every opaque tile (14 on the CMYK cyan, as in earlier rounds), splash the
transparent one.

**The canvas drew a tile read back from a file softer.** It sizes its tile
cache in whole pixels, rounding up, and a tile of 36 pixels read back from
eight decimals came out at 36.00000002, was drawn at 37 and resampled. It now
rounds up only past 0.0001 of a pixel (`display/drawing-pattern.cpp`), so the
turned tile reopens identical. Patterns imported from Illustrator, whose
numbers carry the same noise, are drawn sharper too. Both are shared
rendering changes; the collaborator should know.

#### Real documents

Patterns held 15 of the owner's 862 native documents, 14 for nothing else.
In their records: 92 pattern definitions, 43 of them painted with. The other
49 are only swatches, among them Illustrator's default patterns, some holding
gradients and transparency. What their art paints with, as the import makes
it: fills on paths only (no pattern on a stroke), tiles of
groups, paths and compound paths in CMYK, RGB or grey, spot inks with tints,
strokes and one clipping group.

All 14 now write. Each copy saved as `.ai` reopens natively, and draws as
the original import within 22 channel levels, with no pixel more than 24
apart; the inputs are unchanged. The 15th is held by an opacity mask. The round's census (`tools/ai-writer-census.py`, the same 871
copies, 11 minutes): import unchanged at 862 native and nine pages; the modern
form writes **789 of 862** (was 775), the 14 patterns alone held, none lost,
every accepted stream read back; Illustrator 7 stays 100. What remains: type
60 (50 alone), opacity masks 7 (5), missing linked pictures 6 (0),
noncanonical Registration 4 (4), meshes 3 (1), coincident stops 2 (2), mixed
gradient spaces 1 (1).

#### Still open

- Gradients and pictures inside a tile, and patterns inside patterns:
  Illustrator writes all three; their placement in a pattern's space needs
  its own control.
- Pattern swatches no object paints with are not written.
- Patterns sized to their object and with a view box could be written by
  converting them to the user-space form.
- The Illustrator 7 copy's patterns (PostScript procedures).
- Pattern names outside ASCII are written as UTF-8 bytes, as layer names are;
  Illustrator's reading of them is unchecked (the owner's are all ASCII).

Checks: all **13 unit suites, 0 failures** (reader 74 cases with one opt-in
skip, importer 166 with two, gradient writer 35); **26 export groups, 836
checks**, the pattern control **20/20**. Unit checks: `WrittenPatternsKeepTheirTileArtAndEveryPlacement`,
`PatternsOutsideTheWrittenSubsetAreRefusedByName`, and the refusal lists.

### Opacity masks written (2026-10-22, after patterns)

The modern form writes an object's opacity mask as Illustrator writes one,
straight after the object it masks, in the object's art dictionary on hidden
lines:

```text
... the object: a path's paint operator, a group's U, a clipping group's Q ...
%_/ArtDictionary :
%_/Mask :
%_1 /Bool (Linked) ,
%_0 /Bool (Inverted) ,
%_1 /Bool (Clipping) ,
%_0 /Bool (Disabled) ,
%_X=
%_u
%_... the mask's art, every line hidden, all of its state set ...
%_U
%_9 () XW
%_X+
%_ (Art) ,
%_; (AI9_Mask) ,
%_;
%_
... a group's own state: Xy, 0 0 Xd, 6 () XW (or 9 () XW) ...
```

That is the form of the owner's seven mask documents (one Illustrator group
around the art, `9 () XW` after it, the flags in any order) and of
Illustrator 29's own saved copies of the control. A mask in the mask's art is
written the same way inside it, on lines hidden once. The art is written in a
state of its own, all of it set; after the dictionary the next object sets
all of its own too, whatever a reader puts back.

**Clip.** An SVG mask shows nothing where its art isn't, as Illustrator's
does with Clip on, so a mask is written with Clip on. With Clip off,
Illustrator leaves the object as it is where the mask's art isn't, as an
endless white backdrop would; the import makes such a mask a white rectangle
under the art (`proof:mask-backdrop`, and `proof:ai-mask-clipping="false"` on
the mask). It is written with Clip off, without the rectangle, where that
rectangle covers all that the object paints. Once the object is moved or
grown past it, PROOF draws the object only where the rectangle is, and the
mask is written so: Clip on, the rectangle as art. PROOF ignores a mask's own
region (`x`, `y`, `width`, `height`), in the canvas and in PDF.

**Greys.** In a mask a colour is how much of the object shows: the luminance
PROOF's canvas takes from it (`MaskLuminanceToAlpha`). Mask art is written
in Gray (`g`, `G`), which Illustrator takes as that coverage whatever the
document's colour mode; a colour more than two 8-bit levels from neutral is
refused by name. In a CMYK document a K grey is drawn by PROOF through the
CMYK profile, so 50% K comes out lighter on the canvas than Illustrator's
half and is written as the grey PROOF draws (`0.593 g` in the control); see
Still open. A gradient's stops are Gray in a CMYK document, where Illustrator
makes them tints of black, exactly; **in an RGB document they are written as
neutral RGB**, because Illustrator draws Gray gradient stops there through
its Gray working space (Dot Gain 20%: black becomes 26 of 255, and the dark
end of a mask's ramp lets a tenth through). Flat Gray in an RGB document's
mask is drawn as given.

**What is written:** masks on paths, compound paths, groups, clipping groups,
pictures (after the crop group a picture with a viewport is), hidden and
locked objects (their mask's art is shown and unlocked); art of paths,
compound paths, groups and clipping groups, fills and strokes, linear and
radial gradients with stop opacity, an object's opacity, Normal or Multiply,
nested masks. The import keeps a mask on a group about the object it masks
(`Builder::masked`); that group, when it has nothing else of its own, is not
written, and the mask goes with the object, as Illustrator had it. A mask's
art that is one plain group (as a reopened file's is) is that group, not put
in another each time.

**Refused by name:** colours other than grey, spot inks, patterns and
pictures in a mask's art; blend modes other than Multiply and knockout groups
in it (PROOF's reader takes Normal and Multiply); masks sized to their object
(`maskContentUnits="objectBoundingBox"`), on layers, on the whole document,
on clones, on clipping paths and in patterns; a mask inside itself. The
Illustrator 7 copy says `opacity masks (Illustrator 7)`.

**Isolation.** PROOF's canvas draws a masked group isolated, as SVG says;
Illustrator doesn't unless Isolate Blending is set, so a blend mode inside a
masked group reaches what lies under it there. The writer keeps the group's
own isolation setting, as for opacity; the two draw the same unless a blend
mode is inside (see Still open).

#### The reader: a group's own state after its dictionary

A dictionary puts back the drawing state it found (`Reader::end_obj`), but not
where the object stood: the `9 () XW` that ends a mask's art left the reader
thinking a style marker had followed, and a group's `Xy` after its mask
dictionary was not taken as the group's. Likewise any dictionary (a name)
cleared "a group just closed", so a group's `0 0 Xd` after its name, with no
`Xy` of its own, was lost. Both are now put back with the state.

#### The control in Illustrator

`ai-mask-output-smoke.py` makes ten masked cells, in a CMYK document and an
RGB one: halves of grey (a K grey in CMYK) with Clip on; the import's form of
Clip off (a wrapper, a white backdrop, a black band and a dark grey); a
linear gradient; a radial one whose middle stop has an opacity of its own; a
turned group whose mask's art has a mask of its own; a clipping group; a
picture; a group at 60% after its mask; a stroke for a mask; and an object at
70% beside a hidden masked one.

Illustrator **29.2.1** (one operation at a time, ten seconds after each open,
export, save and close; closed without saving; idle after) read all ten
cells as written: the masked paths as paths (the import's wrapper not
written), the turned and clipping groups, the picture, the group at 60%, the
object at 70% and the hidden one. Its scripting can't see masks; its drawing
and its saved copy show them. At 96 dpi beside PROOF's canvas, **every cell
lines up unshifted**; flat greys, Clip off, the nested mask, the picture, the
stroke and the transparent ones agree but for edge pixels (at most 2.6% of a
cell over 24, means about 1). The first RGB control's gradients didn't (the
ramp a tenth light at its dark end): Illustrator's own page showed why, an RGB
shading from 0.102 to 0.988 for Gray 0 to 1. Written as neutral RGB, they
match within **1** (linear) and **2** (radial). In the CMYK control the
gradients match as written (Gray became Black tints `1 0`); other differences
there are the inks' colour conversion and the RGB picture, which Illustrator
converts to CMYK, as in earlier rounds. Its saved copies keep all twelve
masks, their Clip settings and the art (flat greys as `g`, the gradients'
stops); they open natively in PROOF with all twelve, within **1** of the
original (CMYK: but the picture, converted).

#### Found on the way, and fixed: plain groups in PDF

A real mask document saved as `.ai` reopened from its page: PROOF's page drew
a blue silhouette that its canvas didn't. Not the masks: Illustrator's
Multiply shapes, each in a group of its own inside plain groups. PDF output
wrote every SVG group as a transparency group of its own; a plain one is
non-isolated, which in PDF is the same as drawing it in place, but Poppler's
Cairo backend, which PROOF's check of an `.ai` against its page and its PDF
import draw with, starts every group from nothing. A Multiply in it came out
flat over the colours under the group (Poppler's splash drew it right). A
group with nothing of its own to apply (no opacity, blend mode, isolation,
mask, filter, paint server or clone, and not an anchor, symbol or marker) is
now drawn in place (`pdfoutput/build-drawing.cpp`, `paint_item_group`), as
Illustrator's pages draw one. A layer inside a layer still gets a group of its
own: a layer's marked content (its optional content group) may not nest in
another's in one content stream, and CapyPDF refuses it ("Nesting marked
content is forbidden"). The first form of the change drew those in place too,
and a sampled document with sublayers failed to save; the sample caught it.
Shared code; the collaborator should know. The smoke checks a Multiply in a
layer, a plain group, a clipped group and two plain groups deep over a band
(the previous build was 190 levels off in the three groups, this one exact),
and a layer in a layer, saved as PDF and as `.ai`.

#### Real documents

Opacity masks held 7 of the owner's 862 native documents, 5 for nothing else
(type holds the other two). In their records: some 50 masks at the top level,
more inside them, all linked, none inverted, most with Clip off; their art
mostly greyscale gradients, often set to Multiply, with empty masks of their
own inside (Clip off: they change nothing), groups and clipping groups. All 5
now write, and each copy saved as `.ai` reopens natively and draws as the
original import within 24 channel levels, with no pixel more than 24 apart;
the inputs are unchanged. One of them needed the plain-groups fix above.

A sample of 30 other documents the writer accepts (every 26th by hash, up to
10 MB) saved and reopened: all 30 natively, no pixel more than 24 apart; with
the previous build 29, one falling back to its page for the plain groups. The
14 documents patterns held last round: all 14 still reopen natively.

The round's census (`tools/ai-writer-census.py`, the same 871 copies, 11
minutes, nothing else running): import unchanged at 862 native and nine pages
(no document's acceptance changed with the reader's change); the modern form writes **794 of
862** (was 789), the 5 masks alone held, none lost, every accepted stream read
back; Illustrator 7 stays 100 (it now names `opacity masks (Illustrator 7)`).
What remains: type 60 (52 alone), missing linked pictures 6 (0), noncanonical
Registration 4 (4), meshes 3 (1), coincident stops 2 (2), mixed gradient
spaces 1 (1).

#### Still open

- Coloured mask art: written as its luminance it would draw the same, but
  the colour would change; refused instead.
- A K grey in a CMYK document's mask: PROOF's canvas draws it through the
  CMYK profile (59% shows for 50% K), Illustrator as one less the K (50%).
  The writer keeps what PROOF draws; whether the canvas should take K as
  Illustrator does is a colour question (shared).
- Illustrator's own RGB documents: Gray gradient stops there are drawn
  through its Gray working space, which PROOF's import doesn't do (reader).
- A blend mode inside a masked (or translucent) group: isolated on PROOF's
  canvas, not in Illustrator.
- Inverted masks, unlinked masks, pictures and type in a mask's art, masks
  on layers.

Checks: all **13 unit suites, 0 failures** (reader 75 cases with one opt-in
skip, importer 169 with two, gradient writer 35, PDF output 5); **27 export
groups, 859 checks**, the mask control **23/23**. Unit checks:
`WrittenOpacityMasksReadBackAndDrawAsTheyDid`, `MasksMadeInProofAreWrittenWithClipOn`,
`MasksOutsideTheWrittenSubsetAreRefusedByName`,
`AGroupsOwnStateAfterItsDictionaryIsStillTheGroups` (reader), and the
layer/root refusals, which now name where.

### Registration defined in RGB written (2026-10-22, after opacity masks)

The four documents the census held for "noncanonical Registration ink
definitions" alone (`4760b1df4a`, `a1fd615076`, `bd055cc0ce`, `d2a72bb676`)
are RGB documents from Illustrator CS5 and CS6 (`%AI5_FileFormat` 11 and 13).
In an RGB document Illustrator defines Registration as RGB black:

```text
%%RGBProcessColor: 0 0 0 ([Registration])
...
0.7497 0.6792 0.6705 0.9015 0 0 0 ([Registration]) 0 1 Xz
```

its CMYK equivalent, its RGB, the tint taken from 1, the type of an RGB ink
(1), then `Xz` (`XZ` for a stroke). Their pages draw it `/Separation /All
/DeviceRGB`, paper at 0 and black at 1 (`C0 [1 1 1]`, `C1 [0 0 0]`). The
import read it as the ink `All` defined as RGB black; the writer knew only the
CMYK definition (`1 1 1 1 ([Registration]) tint Xs`) and refused it.

**Written as Illustrator writes it.** The modern writer takes `All` defined as
RGB black, fully opaque, as Registration too: `%%RGBProcessColor: 0 0 0
([Registration])` in the header, and `c m y k 0 0 0 ([Registration]) tint 1
Xz` (`XZ`), the CMYK that of the tinted colour as for any RGB ink
(`ink_operands`). A document using both definitions is refused by name
(`Registration defined in both CMYK and RGB`); any other definition stays
`noncanonical Registration ink definitions`. Illustrator 7 still refuses
Registration.

**PDF output (shared code).** Registration defined in RGB is now `/Separation
/All /DeviceRGB` with the tint transform `{ 1 exch sub dup dup }`, as on
Illustrator's pages; before, it was drawn as plain black, on no plate of its
own. The CMYK definition keeps `/DeviceCMYK` and `{ dup dup dup }`
(`pdfoutput/build-styles.cpp`).

**Illustrator 29.2.1** (one operation at a time, ten seconds after each open,
save and close; closed without saving; idle after). The smoke's RGB control
opened with `[Registration]` on all four paints (fills 100% and 35%, strokes
60% and 100%), defined RGB 0 0 0, once the document also had an ordinary RGB
colour. Painted with Registration alone, it opened as a CMYK document though
its header says RGB (`%AI9_ColorModel: 1`, as the masks round's RGB control,
which opened RGB), with Registration at the same tints. Illustrator's saved
copies, the CMYK one and the RGB one, both keep Registration as
`%%RGBProcessColor` and `Xz`/`XZ` in the same layout, with their own CMYK, and
both pages draw `/All /DeviceRGB`: Illustrator keeps a Registration defined in
RGB in a CMYK document, so the writer takes it there too. Both copies reopen
in PROOF natively, the three tints RGB black; PROOF writes the CMYK one again
as Illustrator did (`%AI9_ColorModel: 2`, Registration in RGB).

**Real documents.** All four now write. Saved as `.ai` and reopened, each
opens natively and draws as the original import within 8 channel levels, no
pixel more than 24 apart; the copies are unchanged; their pages have
`/All /DeviceRGB`.

The round's census, the same 871 copies (12 minutes, nothing else
running): import unchanged at 862 native and nine pages; **the modern form
writes 798 of 862** (was 794), exactly the four, none lost, no other
document's outcome or reasons changed, every accepted stream read back;
Illustrator 7 stays 100. Left for the modern form: type 60 (52 alone),
missing links 6 (0), meshes 3 (1), coincident stops 2 (2), mixed gradient
spaces 1 (1).

Checks: **13 unit suites, 0 failures** (importer 170 with two opt-in skips,
reader 75 with one, PDF output 5); **27 export groups, 867 checks** (was
859; the registration smoke 88, was 80). Unit: `AnRgbDocumentsRegistrationIsWrittenAsIllustratorWritesItThere`
(Illustrator's form in, written, read back, written again the same), and in
`TheWriterRefusesAmbiguousAndMalformedRegistration` RGB not quite black
refused, RGB black in a CMYK document taken, both definitions refused. The
registration smoke **88/88**: the RGB definition in an RGB and in a CMYK
document, records, PDF separation and native reopen.

### Hard stops, and gradient patterns imported in their own space (2026-10-22)

The modern writer now writes coincident stops: a hard transition inside a
linear/radial gradient, two stops at its start, and identical stops at its end.
Linear definitions are listed last first and radial definitions first first,
as Illustrator takes them. The reader reverses a linear list before stable
sorting, so ties keep the order that paints correctly. A midpoint has no
interval to interpolate between two coincident stops.

There is a real endpoint difference: Illustrator extends with the first stop
at 100%, while SVG uses the last. A source SVG whose final stops differ in
colour, ink/tint or opacity is refused (`gradient stops of different colours
at its end`), without replacing an existing file. On import, only Illustrator's
first final stop is kept for painting; unused later final stops are omitted
and explicitly reported in the import notes. Their inactive definitions are
not retained as editable SVG stops. Illustrator 7 hard stops remain refused
(`gradient hard stops (Illustrator 7)`); actual Illustrator 7 is unqualified.

#### Shared PDF output and import fixes

PDF output previously built zero-width stitching intervals between equal
offsets, or bounds equal to the function's endpoints. Poppler rendered those
incorrectly. Interior zero-width pieces are now omitted, retaining the two
colours on either side of the hard edge. At an endpoint a constant interval
at most 0.00001 of the ramp preserves SVG's extension colour with strictly
increasing PDF bounds. Colour and opacity functions use the same construction.

PDF import now evaluates each stitching piece at its own encoded endpoints.
Evaluating the whole function at a shared boundary took the colour on one
side for both stops and lost the transition. Adjacent pieces with different
endpoint colours now keep both stops.

The control also exposed ordinary PDF gradients being placed incorrectly
inside forms, even without hard stops. The gradient builder now uses the
parser's initial page/form/pattern matrix, updated on entry and restored on
exit, instead of assuming a page-wide points-to-pixels scale and flip. This
follows [ISO 32000-1, section 8.7.2](https://developer.adobe.com/document-services/docs/assets/35e4369068f86065372c18787171a17e/PDF_ISO_32000-1.pdf): a pattern matrix maps to its content stream's initial user space.
These are shared PDF changes; the collaborator should know.

#### Qualification and limits

`ai-hard-stop-output-smoke.py` generates six linear/radial cells, checks
native stop order, strictly increasing PDF stitching bounds, native reopening,
a second write, both Poppler renderers, PDF import, endpoint refusal with
and without an existing destination, and the Illustrator 7 refusal. Two
independently authored PDF controls cover nonidentity pattern/form matrices,
nested and repeated forms, page patterns after a form, stroke patterns and a
rotated page. **15/15** checks pass. PDF import matches every sampled pixel
of the six-cell control; second native saves are within one channel level.
Hard-edge renderer comparisons allow the control's two-percent fraction above
24 levels for antialiasing at a transition; the independent form controls
stay below one percent (0.78% and 0.65%). No import gate was relaxed.

Illustrator **29.2.1** draws all six final cells correctly. Differences above
24 levels occupy at most 0.81% of a cell, confined to transition antialiasing.
Its saved copy still opens through PDF: the native/page comparison reports
2.2% colour disagreement against the existing 2% limit. Its PDF's coincident
stitching bounds remain a Poppler comparison limitation. The corrected PDF
import draws that saved copy correctly with editable gradient vectors; this
is **not a qualified native Illustrator-saved return trip**.

The independent cropped-page experiment found an existing page translation
error: old and candidate imports have identical paint coverage, both shifted
from the cropped reference. That separate placement issue remains open.

Both formerly blocked copied documents now save and reopen **natively** in
PROOF; their source/reopen differences peak at 20 and 3 channel levels, with
no pixel above 24 and unchanged input hashes. The same **871 copied inputs**
retain **862 native imports / nine page fallbacks**. Modern record writing
rises **798 -> 800 of 862**, Illustrator 7 stays **100**, no earlier
acceptance is lost, and every accepted stream reads back. Census record
acceptance is separate from the Illustrator rendering/return-trip controls.

Checks: **13 unit suites, zero failures; 28 export groups, 882 checks**.
Reader 76 cases (one opt-in skip), importer 171 (two), gradient writer 36.
Next: inspect the one mixed-gradient-space document, then the remaining mesh
subset; type stays last (60 documents, 52 held by nothing else).

### Gradient stops of different colour models, and inks after the format (2026-10-22, after hard stops)

The one document the census held for "mixed gradient colour spaces" alone
(`c0a2cebfb7`) is an RGB document from Illustrator CS5 with one such gradient,
used once, on art beside the artboard (its page leaves it out): radial, a spot
ink defined in CMYK at 0 (colour style 3), RGB at 90% opacity at 68 (style 2),
the midpoint at 68.7. Illustrator gives every stop a colour style of its own.
Its ramp cache for the gradient (`%_Br` type 4, C, M, Y, K, R, G and B sampled)
follows one curve in R, G and B (0.08, 0.28, 0.59 and 0.83 at the quarters) and
none in C, M, Y and K (K stays 0 to 40%): blended in RGB, the CMYK derived from
that. The import already sampled the midpoint in RGB; the writer refused any
gradient whose stops had more than one model.

**Written as Illustrator writes them.** Each stop keeps its own style: RGB (2)
beside CMYK (1) and spot inks (3, with the exact stop, 4, of an ink defined in
Lab or RGB). A `color-interpolation`, when set, must still match every stop.
Illustrator 7 refuses them by name (`mixed gradient colour spaces (Illustrator
7)`); its reading is unqualified.

**PDF output (shared code; the collaborator should know).** A shading took the
first stop's space, so the owner's gradient would have blended in CMYK. Stops
of more than one colour model now blend in the document's mode, in the space of
its first stop of that mode (a CMYK stop in the document's profile keeps its
numbers); gradients of one model are unchanged (`pdfoutput/build-patterns.cpp`).

**Import.** Midpoint samples between stops of different models (an ink and a
process colour, RGB and CMYK) blend in the document's mode: a CMYK document's
in CMYK, into its profile. Before, they were RGB in any document, which itself
made a CMYK document's gradient mixed. RGB documents read as before.

In a CMYK document with a profile PROOF reads RGB paint as CMYK in it
(`DocumentColors::interpret`), so stops are mixed only in one without; a CMYK
`.ai` reopens with the working CMYK profile, its RGB stops then CMYK.

#### Found on the way, and fixed: the inks before the format

Illustrator 29.2.1 drew every radial gradient of the first control flat, in its
last colour; its saved copy had each centre at y 8594 instead of -135
(`37.5 0 0 37.5 60 8594 Bm`). Eight small controls isolated it. Plain RGB
radials drew in place alone, beside an ink defined in CMYK, Lab or RGB in a
linear or a radial gradient, and beside one ink's fill; beside two inks' fills,
every radial was flat. The modern writer put the ink declarations
(`%%DocumentCustomColors`, `%%CMYKCustomColor`, `%%RGBCustomColor`, the
Registration process colour) before `%AI5_FileFormat`, and with two or more
inks the `%%+` line there leaves Illustrator misreading every radial's `Bm`.
Illustrator writes them after `%AI5_FileFormat`, `%AI12_BuildNumber`,
`%AI3_ColorUsage` and `%AI7_ImageSettings` in all 92 of the owner's documents
with inks of both kinds; the modern writer now writes them after
`%AI3_ColorUsage`, and the same art draws every radial in place. This dates
from the spot-ink rounds, whose control had linear gradients only: 9 of the
documents the modern form writes declare two or more inks and define radial
gradients.
The Illustrator 7 header is unchanged.

#### The control in Illustrator

`ai-mixed-gradient-output-smoke.py`: six cells (CMYK to RGB, a CMYK ink to RGB,
RGB-CMYK-RGB, RGB to a Lab ink, the owner's gradient with its samples, an RGB
ink to 60% black), in an RGB and a CMYK document. Illustrator 29.2.1 (one
operation at a time, ten seconds after each open, export, save and close;
closed without saving; idle after) read every stop as written and kept the
inks; in the RGB document it saved the process CMYK stops as RGB.

- **RGB document.** CMYK to RGB within 1 level of PROOF's canvas: Illustrator
  blends process stops of different models in RGB. Every gradient's ends match
  within 4. Between an ink and a process colour Illustrator draws the ink
  overprinting the process ramp, as the plates print: its page has a DeviceRGB
  shading of the process part and the ink's Separation shading overprinted on
  it (`/MultiChannelPaint`). Mid-ramp that is lighter than PROOF's linear
  blend: 18, 24 and 20 levels at most in the ink cells, the RGB ink to grey
  2.8% of its cell above 24.
- **CMYK document.** Illustrator blends in CMYK (an ink with process colour as
  DeviceN), as PROOF's PDF output now does; PROOF's canvas blends every
  gradient in RGB: CMYK to RGB 31 levels at most mid-ramp (11% above 24),
  yellow to a Lab ink 63 (31%), the others within 22.
- **Illustrator's saved copies.** The CMYK one reopens in PROOF natively,
  within 4 of PROOF's canvas. The RGB one opens through its page (16.6% by
  lightness, 18.5% by colour): Poppler, which PROOF's page check and PDF import
  draw with, doesn't simulate overprint, so the ink cells show the ink's ramp
  alone. That is the cause of the existing "spot-gradient preview
  disagreement". Not a qualified native return for RGB documents with an ink
  beside process colour.

#### Real documents and the census

`c0a2cebfb7` now writes; saved and reopened, it opens natively and draws as
the original import but for 7 isolated pixels of 3.3 million at the tips of
two hairline spikes (coordinate rounding); the copy is unchanged. The census,
the same 871 copies (12 minutes, nothing else running): import unchanged at
862 native and nine pages; **the modern form writes 801 of 862** (was 800),
exactly that document, none lost, no other outcome or reason changed, every
accepted stream read back; Illustrator 7 stays 100. Left for the modern form:
type 60 (52 alone), missing links 6 (0), meshes 3 (1).

Checks: **13 unit suites, 0 failures** (importer 172 with two opt-in skips,
reader 76 with one, gradient writer 37); **29 export groups, 902 checks**,
the new control 20/20. Unit: `StopsOfDifferentModelsKeepTheirOwnColourStyles`
(styles 3, 2 and 1 written and read back; Illustrator 7 refuses),
`MidpointBetweenModelsBlendsInTheDocumentsMode` (CMYK samples in a CMYK
document, RGB in an RGB one), and the inks after `%AI3_ColorUsage` in
`WrittenRecordsKeepInksDefinedInLabAndRgb`.

#### Still open

- Drawing an ink beside process colour as Illustrator does (the ink
  overprinting the process ramp) on the canvas and in PDF output; PROOF's PDF
  still prints spot gradients in process (DeviceN is separate work). Spot and
  CMYK stops, written since the spot-ink rounds, are the same case.
- Reading an RGB page's `/MultiChannelPaint` (overprint in the page check and
  PDF import).
- The canvas blends gradients of CMYK documents in RGB.

### Editable mesh patches written (2026-10-22)

The modern writer now writes supported RGB and CMYK Coons meshes. Corners,
handles, channel values and corner opacity remain editable. Each SVG patch
becomes a one-patch Illustrator mesh; multiple patches are grouped so the
object's opacity, blend mode and supported opacity mask apply once. A shape
whose boundary differs from its mesh becomes a clipping group, including
compound holes. Placement includes item/gradient transforms, reflection and
object-bounding-box units. A complete control-polygon comparison decides
whether the original outline can be omitted; uncertain boundaries keep a clip.

**Why separate patches:** Illustrator smooths colour across the lines of a
multi-patch grid. A saturated RGB control with identical native corner records
drew differently from SVG/Cairo. Independent patches retain SVG's interpolation
in Illustrator. This preserves editable corners and handles, but not one
connected grid on export. The working SVG retains that grid. Import keeps a
compatible Illustrator grid together on its outer outline, cutting full mesh
lines at additional boundary anchors; it marks imported mesh paths as mesh
objects so subsequent mesh edits also move their outlines. Incompatible grids
still come in patch by patch with a note. Original corners retain their own
colour models; only newly interpolated corners require one model.

Refusals are explicit: Illustrator 7 meshes; bicubic smoothing; moved tensor
points; mixed or unsupported corner colour models; strokes on mesh-filled
objects; meshes used as strokes, in patterns or in opacity-mask art; mesh fill
overprint; nonfinite/singular placement; and more than 4096 patches in a mesh.
An ordinary supported mask attached to a mesh object is retained.

**Shared PDF fixes (collaborator take note):** mesh coordinate encoding now
bounds the control polygons in mesh space, rather than translated objects'
bounds. Mesh patterns use each object's bounds and gradient transform, including
object-sized units, and use the same built array as the canvas for references.
A gradient transparency mask paints only its own object in local coordinates,
instead of every object sharing that gradient. Illustrator's mesh transparency
written after `/End` is now attached to that mesh before its style marker;
previously a patch could inherit the preceding object's opacity.

Illustrator 29.2.1 opens both synthetic controls as 21 editable patch meshes.
Its saved RGB control keeps the corner/handle data within 3e-8; CMYK keeps them
exactly, including each colour model and corner opacity. The RGB drawing agrees
within the control's 2% colour tolerance (two outer pixels excluded for clipping
antialiasing). Both saved controls reopen natively after the post-mesh opacity
fix. **CMYK preview remains limited:** Illustrator blends CMYK channels, while
PROOF's canvas blends RGB previews. This is not a CMYK screen-match claim.
Poppler's Splash and Cairo backends also differ on translucent mesh soft masks;
the check uses Cairo for these, and independently checks opaque untransformed
RGB meshes with Splash. Full diagnostics remain local.

The 15 copied Illustrator controls and the owner's mesh-only document
`ca830635ef` write and reopen natively; the other two mesh documents still
contain type. The final census is recorded in the round's workspace handoff.
The synthetic `ai-mesh-output-smoke.py` joins the focused runner and checks
editable patch counts, models, clipping, masks, repeated saves, PDF pages,
source integrity and Illustrator 7's named refusal. Import's visual gates are
unchanged.

Final census: **862 NATIVE of 871, nine PAGE; modern 802/862** (was 801),
Illustrator 7 **100**. No lost import/writer acceptances, unread streams or
changed copy hashes. Remaining modern refusals are type 60 (54 alone) and
missing links six (all also type). **13 focused unit suites, zero failures;
30 export groups, 927 checks**, including the mesh control **25/25**.
Existing opt-in/GUI skips remain; no editing-UI acceptance is claimed.
## Perceptual gradient import guard (2026-10-22)

Illustrator 30.6's extended `Bd` definition is recognised and its extra operands
retained by the records reader. Painted Perceptual gradients now explicitly
fall back to the PDF page; they no longer silently become Classic when the
visual comparison tolerates the difference. Unknown extended settings also
refuse native paint. Unused definitions/state notes do not force fallback.
Oklab canvas/vector PDF rendering and native method round-trip remain open.
See [the records contract](PROOF-ai-native-records.md#perceptual-gradient-settings-are-recognised-2026-10-22).
