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
refuses multiple artboards, non-ASCII ink/layer names, individually hidden
objects, nested layers and image dimming other than 50%. Hidden layers work.
Artboard names and document ICC profiles are not retained. Corner/smooth flags
use the shared writer described below.
Illustrator opens the legacy control in CMYK and converts its RGB process
colours; the control's original CMYK numbers and CMYK spot tints survive.
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
