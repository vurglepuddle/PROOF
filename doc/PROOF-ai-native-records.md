# Illustrator native records

A `.ai` file carries two copies of the artwork. One is the PDF page that every
other application reads. The other is Illustrator's own copy, which it reads and
everyone else ignores. This page describes that own copy and how much of it
PROOF can read. It is the shared reference for PROOF's native reader and its
native `.ai` writer ([AI interchange](PROOF-ai-interchange.md)).

## Where the records are

| File | Where |
|---|---|
| PDF-based `.ai` (Illustrator 9 and later) | Page 1's `/PieceInfo /Illustrator /Private` streams `AIPrivateData1…N`, joined |
| Illustrator EPS | After `%%EOF`, between `%AI9_PrivateDataBegin` and `%AI9_PrivateDataEnd` |
| PostScript `.ai` (Illustrator 8 and older) | The file itself |

The joined streams may start with plain header comments and a thumbnail. After
that comes `%AI12_CompressedData` followed by zlib (CS2 to CC 2019), or
`%AI24_ZStandard_Data` followed by Zstandard (2020 and later). Older files keep
plain records. The marker can be more than 4 KB into the data, so search the
whole data for it. Illustrator pads Zstandard frames with zeros.

## The records

The records are a PostScript-dialect program. Nothing in them has to be
executed. A reader tokenizes them and acts on a fixed table of operators.

- **Lines starting with `%_`** are tokens hidden from a PostScript interpreter.
  They hold Illustrator-only data, such as the object behind a drawn look,
  object dictionaries and mask art.
- **Other `%` lines** are section markers, such as `%AI5_BeginLayer`. So is a
  `%` straight after `%_`: an object written on hidden lines keeps its markers
  and its image data there (`%_%AI5_BeginRaster`, `%_%%BeginData: n`). The
  samples that follow are never prefixed.
- **Dictionaries are written postfix:**
  - `/Type :` opens one.
  - `values /ValueType (Key) ,` adds an entry.
  - `;` closes it.
- **Image samples** follow `%%BeginData: n` and `XI`.

The **coordinate space** is the art space, with y pointing up. The first
artboard's top-left corner is PROOF's origin. See the writer recipe in
[AI interchange](PROOF-ai-interchange.md) for the canvas and `%AI3_TemplateBox`
arithmetic.

### Operator table

Unless the "Source" column says otherwise, the meanings come from VectorCraft's
reader (`crates/eps/src/import/ai/` at `4cf912f`, MIT OR Apache-2.0). Rows marked
**census** were worked out from real files on 2026-10-09 (see below).

| Operators | Meaning | Source |
|---|---|---|
| `%AI5_BeginLayer`, `… Lb`, `(name) Ln`, `LB` | Layer. The `Lb` operands are: 0 visible, 1 preview, 2 unlocked, 3 printing, 4 dimmed, 7 colour index, 8–10 RGB, 12 dim %. | VectorCraft |
| `u`…`U`, `*u`…`*U`, `q`…`Q` | Group, compound path, clip group. `W` makes the next path clip. | VectorCraft |
| `m l L c C v V y Y h H` | Path construction. | VectorCraft |
| `N n F f S s B b` | Paint: none, fill, stroke, both. The lower-case forms close the path. `(op) *` paints a guide. | VectorCraft |
| `w J j M d`, `XR` | Stroke width, cap, join, miter limit and dash; fill rule (1 means even-odd). | VectorCraft |
| `O`, `R` | Overprint the fill or the stroke. | VectorCraft |
| `Xw`, `A` | Hidden, locked. | VectorCraft |
| `mode opacity isolate knockout shape Xy` | Transparency. The modes run Normal, Multiply, Screen, Overlay, Soft Light, Hard Light, Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Hue, Saturation, Color, Luminosity. After a group closes, it applies to the group. | VectorCraft |
| `n (style) XW` | Style marker. `1` means the object's drawn look came first and the object itself follows on `%_` lines. | VectorCraft |
| `/ArtDictionary : … ;` | Follows an object. `AIArtName`, or `AI10_ArtUID` (an XML id: `_` is a space, `_xHH_` an escaped character), gives its name. `BBAccumRotation` is the turned box's angle. | VectorCraft; angle from PROOF's transform-box work |
| `g`/`G` | Grey, where 1 is white. | VectorCraft |
| `k`/`K` | CMYK. | VectorCraft |
| `Xa`/`XA` | RGB: `c m y k r g b`, where the CMYK values are its equivalent. | VectorCraft |
| `x`/`X` | Named CMYK colour: `c m y k (name) tint`. **Tint 0 is the full colour.** | VectorCraft |
| `Xx`/`XX` | Spot colour: `c m y k v1 v2 v3 (name) tint type`. Type 0 is CMYK, 1 is RGB, and **2 is Lab, where v1–v3 are L, a, b**. PANTONE swatches use type 2. | **census**. VectorCraft reads type 2 as CMYK and loses the Lab definition. |
| `Xk`/`XK` | **Global process colour**, in the same layout as `Xx`. Type 0 is CMYK with 6 numbers; type 1 is RGB with 9. | **census** (blocked about 200 of 877 files under VectorCraft's table) |
| `Xs`/`XS` | Registration: `c m y k ([Registration]) tint`. | **census**. VectorCraft ignores `Xs`, so Registration fills keep the previous fill. |
| `Xz`/`XZ` | Registration, in the `Xx` layout. | **census** |
| `(name) type n Bd` … `Bs` … `BD` | Gradient definition. Each stop is `colour style [opacity 6] midpoint ramp Bs`. The styles are 0 grey, 1 CMYK, 2 RGB with CMYK, 3 and 4 named, and **5 named with its type, in the `Xx`/`Xk` layout** (`c m y k [v1 v2 v3] (name) tint type`). | VectorCraft; style 5 from the 2026-10-10 trial |
| `… %_BS`, `… %_Bs`, `n %_Br` | **Operators ending a line after `%_`.** Each stop is written twice: `c m y k 1 opacity 6 midpoint ramp %_BS`, a process stand-in, then the exact stop on its own `%_… Bs` line. Old files end the one line `%_Bs`. `%_Br` is the ramp count. Read as comments, their operands pile up in front of the next operator. | **2026-10-10 trial**. VectorCraft skips them, and then misreads every named stop. |
| `[flag] Bb`, `Bg`, `Xm`, `Bm`, `Bc`, `Bh`, `BB` | Gradient on an object. See "Gradient placement" below: with `Xm` or `Bm` present that matrix is the whole placement, and `Bg`'s origin, angle, length and matrix are not applied as well. | VectorCraft read `Bg` × `Bm`; corrected in the **2026-10-10 trial** |
| `%AI5_BeginRaster`, `/Space XN`, `[m] … w h bits type alpha … XI`, `XH`, `N` | Embedded image. The samples stay in their colour space; CMYK is common. The image is an object once its samples are read: the `XH` and the `N` (which paints no path) after it don't end what follows an object, so its name and a `1 () XW` still reach it. | VectorCraft; the tail from the **second 2026-10-10 trial** |
| `%AI3_BeginPattern: (name)`, `(name) llx lly urx ury`, the tile's art on `%_` lines, `E`, `%AI3_EndPattern` | Pattern. The box is the tile, in a space of the pattern's own (y up); art past it is cut off there. | **second 2026-10-10 trial**. VectorCraft skips the section. |
| `(name) px py sx sy angle rf r k ka [a b c d tx ty] p` (`P` for a stroke) | Paint with a pattern. The matrix carries the pattern's space onto the art, the same space the paths are in. The nine numbers before it are how Illustrator 8 and older moved, scaled, turned, mirrored and slanted a pattern; later versions leave them at `0 0 1 1 0 0 0 0 0`. Like a colour, it stays the paint until another is set. | **second 2026-10-10 trial**; the matrix checked against a file's own PDF pattern |
| `%AI26_BeginPlacedObjectPreview`, `w h row-bytes 32 17 ` and the rows, `%AI26_EndPlacedObjectPreview`, in a placed file's section | The small picture Illustrator keeps of a placed file, for when the file can't be found. See "A linked picture that is gone". | **2026-10-11** |
| `%AI5_BeginPlace`, `%%FileType: n`, `[a b c d tx ty] llx lly urx ury … (path)` then `` ` ``, `%AI28_BeginPlacedRelativePath` `(folder)` `%AI28_EndPlacedRelativePath`, `~`, `%AI5_EndPlace` | Placed (linked) file. The box is where the picture lies in a space of its own whose y points down (its top edge is `lly`); the matrix carries that onto the art. The path is as the saving machine knew it; the folder is where the file was found from the document's own, empty when it is the same. | **second 2026-10-10 trial** |
| `/AI11Text : /FreeUndo … /StoryIndex …` | Text object. Its text lives in `%AI11_BeginTextDocument`. The section holds two documents: `/AI11TextDocument`, what was typed, and `/AI11UndoFreeTextDocument`, the type appearances drew. With `FreeUndo` 1 the object is drawn type and its `StoryIndex` counts in the second: see "Type an appearance drew" below. | VectorCraft; the second document from the **second 2026-10-10 trial** |
| `n Ar` | The old Attributes panel's output resolution (300, 800). No visual effect. | **census** |
| `1 An` | CS5 only, after every object. Almost certainly *Align to Pixel Grid*. No visual effect. | **census** |
| `(hex) Xt n XD` | Brush metadata inside drawn looks, with n running 0–4. No geometry or paint. | **census** |
| `Ae AE Ap As Xd Xr XG Xh XH XF D X= X+ Bc XP Np TE TZ Xt Xi XI ` LB2 Lc Xv XV Xq XQ Xg Xn Bn` | Ignored by VectorCraft. (It ignores `Xm` too; PROOF reads it.) | VectorCraft |

### Not read yet (the file opens from its PDF page)

| Feature | Records | Files (of 877) |
|---|---|---|
| Gradient mesh | `/Mesh X!` … `/End X!`, with `value /Key X#` pairs (Version, Type, Size, the patch data) | 11 |
| Patterns of Illustrator 8 and older | The tile as procedures of the printing format (`[ %AI3_Tile (…) @ … ] E`), or a `p` whose nine numbers aren't at rest | not counted |
| Placed files that aren't pictures | A placement without the `` ` `` operator (embedded EPS); a linked Photoshop, PDF or Illustrator file, which nothing here draws | not counted |
| Symbols | `/SymbolInstance` | 2 |
| Opacity masks | `/Mask : (Clipping) (Inverted) (Disabled) (Linked) ,` inside the masked object's ArtDictionary, with the mask art on `%_` lines | 3 in `Typical.ai` |

A pattern whose tile holds something that isn't read costs only the objects
painted with it: the definition is read in a frame of its own, and whatever
goes wrong in it stays the pattern's.

**VectorCraft drops opacity masks silently.** It discards art built inside a
dictionary, so a masked group comes in unmasked, without an error. PROOF must
read masks or refuse the native path for them. More generally, reading without
an error is not the same as reading faithfully. The importer compares its
render with the PDF page and uses the page when they differ.

## Census of real files (2026-10-09)

`tools/ai-census.py` decodes the records and walks them with one of two tables:
VectorCraft's (`--rules vectorcraft`) or PROOF's extended one (`--rules proof`).
It never executes anything, and duplicates are counted once. It runs only on
approved sources (see `APPROVED` in the script), because some production files
are under NDA.

The corpus has **877 unique files** from Illustrator CS3 to 2025. They are mostly
illustration and print work. The five `spike/files` test files are among them,
and so is a released 2020–2023 print job.

| | Reads fully native |
|---|---|
| VectorCraft's table | 589 of 877 (67%); 2 of the 5 spike files |
| PROOF's table | **839 of 877 (95.7%)**; all 5 spike files |

`Xk`/`XK` kept the other 3 spike files and most of the 2020–2023 files from
VectorCraft's table (one spike file also had `Ar`). Under that table, the 2020–2023
print job read 15 of 44 files; under PROOF's, it reads 43.

| Version | Files | PROOF | VectorCraft |
|---|---:|---:|---:|
| CS3 | 9 | 9 | 4 |
| CS5 | 281 | 277 | 201 |
| CS6 | 71 | 70 | 59 |
| CC | 113 | 112 | 66 |
| CC 2014 | 35 | 33 | 23 |
| CC 2015 / 2015.3 | 144 | 130 | 106 |
| CC 2017 | 110 | 108 | 78 |
| 2019–2021 | 32 | 31 | 24 |
| 2022–2023 | 64 | 57 | 21 |
| 2025 | 14 | 12 | 7 |

The remaining 38 files are:
- the features in the table above: meshes 11, pattern fills 15, placed files 6
  and symbols 2
- three files whose records decode past the 256 MB limit
- one file whose groups don't nest (`Q` without `q`)
- one file whose image data isn't delimited

The census also shows how common these features are:
- transparency in 700 files
- drawn looks in 503 (objects with several fills or strokes, effects or brushes)
- spot or global colours in 248
- overprint in 97
- text in 67
- embedded images in 56, 46 of them CMYK
- hidden objects in 38
- hidden layers in 16

Every file had layers and a `/Document` dictionary.

## PROOF's reader (2026-10-09)

`src/extension/internal/ai/ai-native-lexer.{h,cpp}` and
`ai-native-reader.{h,cpp}` contain the scanner and the reader. The operator
meanings are ported from VectorCraft, with the MIT notice in
`LICENSES/MIT-VectorCraft.txt`. The reader produces plain structures in art space:
- layers with their options
- groups, compound paths and clipping groups
- paths with fills, strokes, overprint and transparency
- inks
- gradients
- images, with their samples kept as stored (CMYK stays CMYK)
- text slots
- artboards and the art-to-document matrix

Beyond the VectorCraft port:
- **The palette is read.** `%AI5_BeginPalette` registers each swatch's ink with
  its kind (`Xx` spot, `Xk` global process) and its Lab or RGB definition, used
  or not. A gradient stop names its colour only in CMYK form, so it links to the
  swatch of that name. Otherwise a global process colour used only in a gradient
  would become a spot plate.
- **Lab spot definitions are kept** (`Xx` type 2).
- **Registration** is read (`Xs`/`XS`, `Xz`/`XZ`).
- **Opacity masks fail closed.** A `/Mask` that isn't disabled sends the file
  to the page instead of being dropped silently.
- **Census operators are ignored on purpose:** `Ar`, `An`, `XD`.

A gradient on an object is written as the path, then `Bb` (no operands), `Bg`,
optional `Bm`/`Bh`, then the paint operator (`f`), then `BB`. The paint uses the
instance, and `BB` paints nothing. One instance can hold two `Bm` matrices; the
last one wins. That still has to be checked against the page.

`testfiles/src/ai-native-reader-test.cpp` covers each operator row with
hand-written records, plus fail-closed cases, hostile input and limits. With
`PROOF_AI_CORPUS` set to `;`-separated folders of approved files, its corpus
case reads every file and prints which ones read natively.

**Decoder fixes** in `pdfinput/ai-private-data.cpp`, found by the census:
- **The compression marker** was looked for only at byte 0. CS-era files keep a
  plain header and thumbnail before `%AI12_CompressedData`, so they were treated
  as uncompressed, and `.ai` swatch libraries from them read nothing.
- **Zstandard decoding** stopped after the first frame. It could also stop while
  the decoder still held output, after the last input. Both would truncate the
  records silently. It now reads every frame, skips Illustrator's zero padding,
  and drains the decoder.

## Native import (2026-10-10)

Opening `.ai` now tries its native records first. `ai-native-import.{h,cpp}`
builds the document and `pdfinput/ai-native-open.{h,cpp}` checks it against the
PDF page. The preference `/options/aiimport/native` can disable this path.
PDF files, including PDFs carrying Illustrator records, keep their existing
import path.

The document keeps process CMYK channels in its assigned working profile,
Lab/RGB/CMYK spot definitions and tints, Registration as the ink `All` (labelled
`[Registration]`), and global process colours as solid swatches. Unused named
inks are retained too. Global process tints are separate swatches; they do not
retain a live parent link. Grey in a CMYK document becomes black ink only.

Artboards become named pages in points, with the first artboard's top-left at
the origin; the root dimensions use the original ruler unit. Layer names,
visibility, locking and highlight colours are kept. Print, preview and dim
options are stored as `proof:layer-*`. Objects keep opacity, blend mode,
isolation and turned-box angles. Drawn looks become groups, compounds become
paths, and clip groups become SVG clip paths. Straight guides become guides;
curved guide segments are omitted with a note.

Embedded images display as PNG. CMYK samples additionally survive in
`proof:samples`, as a Deflate CMYK TIFF (including unassociated alpha when
present). The PNG is converted through the working profile. These original
samples are preserved on SVG save/reopen, but exporters do not use them yet.

### Corrections verified during import validation

- **Painted clipping paths.** `W` schedules clipping; it does not cancel `f`
  or `B`. The reader formerly discarded that paint, and the builder discarded
  the clipping object. Both now preserve it. A hand-written PDF/native fixture
  reproduces the missing background and checks the corrected render.
- **Gradient midpoints.** Stops arrive in descending ramp order. A stop's
  midpoint controls its segment to the next higher stop. Illustrator uses the
  power curve `v = u^N`, with `N = ln(0.5) / ln(midpoint)`; its PDF uses Type 2
  functions with that exponent. PROOF adds adaptive sample stops, within 0.5%
  of the curve at subdivision midpoints. VectorCraft's piecewise linear
  interpolation around the midpoint is different. Spot sample stops keep
  ink/tint metadata.
- **Every artboard is checked.** Artboard and PDF page counts must agree;
  missing, rotated, invalid or differently sized pages refuse native import.
  Each page's independent lightness and colour limits apply, including when
  choosing a retry with non-printing layers hidden.
- **Known omissions refuse import.** Missing gradient definitions, unreadable
  images and images that cannot be placed use the PDF import. A small omitted
  object must not slip through the visual tolerance merely because it covers
  little of the page.

### Visual check and its limits

Poppler draws each PDF TrimBox and PROOF draws the matching artboard, on white,
at a longest side of 600 pixels (at most four pixels per point). Each pixel
can match a neighbour within one pixel, in both directions. Differences are
counted separately by lightness (48/255) and by any RGB channel (80/255).
Native import is refused if more than **1% by lightness** or **2% by colour**
differs on any artboard. The initial guesses of 2% and 5% accepted visible
fill changes in the corpus; the limits were tightened after inspecting those
cases. They remain practical rendering tolerances, not proof of exact visual
identity: small details, low-contrast changes and differences below the sampled
resolution can escape them. An artboard over a limit is weighed once more from
larger pictures, for lines thinner than a pixel; see the 2026-10-10 trial below.

Non-printing layers are retried hidden for comparison, then their visibility
is restored. PDF optional-content screen and print views can differ; this
check uses Poppler's screen view. Bleed comes from the PDF BleedBox relative
to its TrimBox. Comparison pictures and the native SVG can be saved by the
opt-in corpus test (`PROOF_AI_PICTURES` and `PROOF_AI_DUMP`), for diagnosis.

Point text now reads the bounded ASCII85 text document: UTF-16 character-run
lengths, Unicode content, editable font/style runs, explicit line positions,
full frame affines, tracking, common horizontal/vertical character scaling,
left/centre/right paragraph anchors, and process RGB/CMYK/grey fill and stroke.
The exact PostScript font must be installed and contain the characters; missing
fonts or glyphs refuse native import rather than silently substituting. The
source font names and character styles are resolved before building the SVG.
Area type in one frame, baseline shifts and a run's kerning and ligature
switches are read since the second trial of 2026-10-10 (below). Type on a path,
linked frames, varying character scales within a story, justification and
unsupported character features still refuse visible text.
The parser bounds decoded bytes, nesting, value count, story size and run lengths;
malformed text or a run splitting a surrogate pair also refuses the native path.
Hidden text is retained when supported; unsupported hidden text/content may be
omitted with notes. Knockout groups draw as ordinary
groups; their state and overprint are stored as PROOF attributes. Export does
not yet honour overprint, knockout, original CMYK image samples or non-printing
layers. The source's embedded ICC profile is not decoded; the chosen working
profile is used. This is an import feature, with print/export work still to do.

`test_ai-native-import` covers mapping, exact channel/sample storage and SVG
save/reopen, plus independently written PDF/native pairs. It verifies native
acceptance, stale-art fallback, painted clipping paths, bleed, non-printing
layers, page counts, page geometry and independent limits on multiple pages.
Its opt-in `Corpus` case accepts `PROOF_AI_CORPUS` (approved folders separated
by `;`) or `PROOF_AI_FILE` (one approved file) and prints the decision,
maximum per-artboard lightness/colour difference and import time. Corpus paths,
logs, diagnostic artwork and pictures stay outside Git.

The pre-point-text baseline on the approved corpus was **704 native / 222 page imports across 926
files**, or **667 / 212 across 879 distinct SHA-256 hashes**. All 926 isolated
test processes completed without a crash or timeout. The import step's median
was 233 ms and maximum 9.1 s on this machine (excluding process startup).
Of the 821 files reaching the drawing comparison, 117 exceeded a visual limit;
the other 105 used the page for unsupported records/text, incomplete artwork,
decode limits or page geometry/count checks. These counts describe this corpus,
not arbitrary Illustrator files.

At that baseline, all **19 native-import cases** passed, including the SVG save/reopen assertions.
The six existing focused suites also pass (**173 cases**, with their two
pre-existing disabled cases and the reader's opt-in corpus case excluded).
The installed CLI native/fallback checks and all **19 AI interchange smoke
checks** pass. A live open of a disposable spot-colour sample displayed its
artwork in a CMYK document, without an import dialog, and closed normally.
This does not establish extended editing or print/export acceptance.

### Point-text continuation and low-load validation (2026-10-10)

Development uses one process/worker, 1-2 real representatives per issue and small
independent fixtures. The 52 distinct files previously refused first for visible
text were checked once in chunks of at most eight, then only the 12 affected
alignment cases were checked after that feature. Twenty distinct former fallbacks
(21 original paths) reached the native visual gate: ten with no reported structural
loss, and ten partial imports because appearances came in as drawn groups. A visual
pass is not a complete editable-reconstruction certificate. No new overall-corpus
percentage is claimed, and neither the 222 fallback set nor the 926-file corpus
was rerun. Remaining text blockers include exact fonts, unsupported frames/styles,
invalid/incomplete stories and unresolved visual differences.

Independent PDF text operators verify visible glyphs, baseline placement, common
character scaling and centre/right anchors. Displaced text is rejected. Unicode
runs, missing fonts/glyphs, editable styles, a text-content edit and SVG save/reopen
are tested separately. Explicit SVG line positions deliberately omit
`sodipodi:role="line"`: Inkscape's reflow would discard their single x/y pairs.
The continuation passes 29 active native-import cases, 30 active reader cases,
10 cache/selection checks and all 19 installed AI interchange smoke checks.
The two opt-in corpus tests are excluded from those active counts. Installed
CLI checks preserve two editable sign stories and use PDF fallback for a missing
exact font. Extended live editing, Illustrator round trips and print acceptance
have not been repeated for this continuation.

The optional `AiNativeCache` is for the local development runner, off for ordinary
imports. The caller supplies trusted source/dependency-keyed directories for
decoded records and PDF reference PNGs. Size/dimension/digest checks turn damaged
entries into cache misses. The test reports `AI_CACHE` reuse counters and `AI_NOTE`
capability-loss notes. A warm test verifies identical comparison results and
recovery from damaged records/PNG entries.

Workspace-local `tools/ai-native-workbench.py` builds its SHA-deduplicated manifest
from existing reports, keeps reviewed root-cause groups/representatives/fixtures,
supports `--affected --offset N --limit 8`, and stores all private data under
`artifacts/ai-native-workbench/`. Decoded records, PDF references and comparisons
have separate keys. An unchanged comparison skips its process entirely; changing
only the importer preserves decoded/PDF stages. Reports distinguish
`NATIVE_UNVERIFIED`, `PARTIAL_NATIVE`, `PDF_FALLBACK`, errors and timeouts. No file
is labelled complete from visual acceptance alone. The gallery reuses actual
comparison PNGs. `--all` requires an explicit milestone reason. Existing corpus
files are already seen; an independent holdout awaits newly approved files.

### Dallas designer trial: text appearances and dropped artboards (2026-10-10)

Illustrator can write a text-only drawn appearance with `FreeUndo=1` and a
`StoryIndex` different from the following commented, editable `FreeUndo=0`
object. The Dallas map exposed labels apparently swapping layers: the reader
had kept the appearance's text instead of the canonical story. Text-only
appearances now use the canonical text in its original layer. An otherwise
empty, anonymous appearance wrapper is removed; named wrappers and wrappers with
transparency or object state remain without applying opacity twice. Appearance
markers also retain the association needed to attach a
following `AIArtName` dictionary to its group.

Dropping an `.ai` file onto a canvas with no artwork uses the normal File > Open
operation, preserving source colour mode, units and artboard coordinates.
Empty layers count as blank; hidden or locked artwork counts as existing
content. Explicit File > Import still imports. Opening a modified blank
document preserves it in its tab through the existing open operation.

For actual page imports, both documents are updated before calculating physical
coordinates, page labels are copied, and `importDefs` excludes page definitions.
Previously it copied each `svg:view` again at its untransformed source position,
leaving an extra set of artboards detached from the imported artwork. Distinct
overlapping artboards remain distinct.

The Dallas file remains a **partial native import**. Its saved visible artwork
passes the PDF comparison, but hidden layers include unsupported pattern fills.
Matching the saved PDF cannot verify artwork that those layers hide. Pattern
reconstruction and an independent user check of the updated drag operation are
still required.

The focused suites pass 32 active native-import cases and 33 active reader cases;
their two opt-in corpus cases are skipped. Small fixtures cover physical page/art
placement across differing document scales, overlapping pages, names after
appearance markers, canonical text stories, named text wrappers and opacity.
The six-file real smoke suite and 20 previously recovered text files retain their
native decisions, with two effect-family representatives rechecked after the
final wrapper guard. Installed CLI open and SVG save/reopen of Dallas retain RGB,
six artboards, 58 editable text objects and the correct tree/label stories. The
installed interchange smoke passes 19/19; none of these is a live drag-gesture
or hidden-layer fidelity certificate.

### Designer trial of 2026-10-10: gradients, hairlines, swatch stops, tiles

A first hand trial of current work files sent about three in four to the PDF
import. Seven of them were handed over; six open natively now. The causes were
few and general, and the full corpus was rerun afterwards (see the end).

**Gradient placement.** The reader applied `Bg`'s origin, angle and length and
then `Bg`'s matrix and `Bm`, as VectorCraft does. That is right only for old
files, and for the trivial `0 0 0 1` that newer files usually write. Counted
over the gradient instances drawn in 66 decoded files:

- *Linear.* Every instance that has a matrix has two: `Xm`, then `Bm` (one per
  ramp, with `Bc` caps around them). `Xm` carries the unit ramp (0 to 1 along x)
  onto the art, whole. In all 221 instances with their own `Bg` values and an
  identity `Bg` matrix, `Xm`'s x axis points exactly along `Bg`'s angle: the
  angle, length and origin are already in it. `Bg`'s own values are the same
  placement relative to the object's bounds (`-0.026 0 -90 1.54`), and its
  matrix the accumulated transformation (`CAIGradientTformMatrix`, with
  translations in the thousands); neither is to be applied again.
- `Bm` is the matrix for PostScript, which draws ramp by ramp. In all 1,092
  linear instances `Bm` equals `Xm` cut down to the span between the first two
  stops (`Bm.x = (s1 − s0)·Xm.x`, `Bm.origin = Xm.origin + s0·Xm.x`). Read as
  the whole ramp it squeezed every gradient whose second stop isn't at 100%,
  and the last `Bm` of a three-stop gradient is its *last* ramp. Without `Xm`
  (older files) the first `Bm` is used and the ramp runs from
  `−s0/(s1−s0)` to `(1−s0)/(s1−s0)` in its space.
- *Radial.* Only `Bm`, and it is whole: the unit circle about the origin is the
  full ramp. The file's own PDF page paints the same matrix with
  `Coords [fx fy 0 0 0 1]`. `Bh` is `hx hy angle length`: the highlight's offset
  in art space, and again in the unit space as an angle and a share of the
  radius, `(length·cos a, −length·sin a)`; through `Bm` that is `hx hy` in all
  78 instances that have one.

**Stops written twice.** See the operator table: `… %_BS` then `%_… Bs`. As
comments, the stand-in's nine numbers stayed in front of the exact stop's
operands. Plain stops survived that (their colour is read from the end);
named ones (styles 3 to 5) took their CMYK from the wrong numbers, which is
how a logo's red-to-dark-red swatch gradient came in black. Gradients are
also defined *before* the Swatches panel is listed, so a stop meets a
swatch's name first: that first sight is now replaced by the swatch's own
definition (kind and colour) when it comes, in place of a second ink of the
same name.

**Transparency after a style marker.** The previous round let a name
dictionary after `n () XW` still reach the object before it, by no longer
ending the "after an object" state there. `Xy` uses the same state to decide
whether it belongs to a group that has just closed, so `U`, `6 () XW`,
`1 0.15 0 0 0 Xy` gave the whole closed group the next path's 15% multiply.
A marker now ends that for transparency only. Real files also leave such a
state standing across text objects without resetting it (12 texts in 6 of the
66 files): type does not take its transparency from `Xy`.

**Hairlines in the visual check.** Poppler, drawing for a screen, widens any
stroke thinner than a device pixel to one pixel and snaps it to the pixel
grid (`CairoOutputDev::updateLineWidth`, when not printing). PROOF draws
strokes as thin as they are. At the check's size a page of 1 pt outlines, or
a 0.1 pt cut line down a 59 inch banner, then differs in "3%" of its pixels
although nothing is missing. An artboard that fails is now weighed a second
time from pictures drawn four times larger and averaged down, with PROOF's
side keeping lines thinner than a pixel one pixel wide (the renderer's
"visible hairlines"): both sides then show the same ink for the same line,
and the limits stay 1% and 2%. Poppler's printing mode would also stop the
widening but draws images unfiltered, so it isn't used. When the art still
differs and the caller won't keep it, the remaining artboards aren't drawn.

**Tiles.** An expanded gradient is a clip group of strips that meet along
whole edges. Each strip is antialiased on its own, so a pixel on a joint is
partly covered by each and the background shows through: a light hairline at
every joint, at most zooms. Illustrator's page and print show none. Shapes
that tile (flat colour, no stroke, straight edges, a whole edge shared with a
sibling) are imported with `shape-rendering:crispEdges`, which shares the
pixels out exactly, but only when their other edges can't turn ragged for it:
those must be horizontal or vertical, or lie where the group's clipping path
cuts them away. Slanted strips that show their ends keep the joints.

**Text style.** A story's style is now on the `<text>` element, and a run's
span carries only what differs from it. With everything on the spans, the
object's own fill and stroke were unset: the Appearance controls showed "?"
and what they set was overridden by the spans.

**Results.** The opt-in corpus case over all 879 distinct approved files, one
worker: **768 pass the visual check natively (385 with no reported loss, 383
with appearances as drawn groups) and 111 use the page**, with no error or
timeout. Against the 667 of the first baseline that is 101 gained (78 that
drew differently, 23 from point type) and none lost. Of the 111, 38 still
draw differently (20 of them within 1.0 to 1.8% by lightness), 15 have
pattern fills, 15 unknown operators (meshes among them), 7 opacity masks, and
about 30 text the reader refuses (exact fonts not installed, mostly). The
same limits of a sampled comparison apply as before.

### Second designer trial of 2026-10-10: patterns, linked pictures, area type

After the first trial about one more file in ten opened natively, and the most
common failure left was the offer of the PDF import. Eleven such files were
handed over (`spike/files/Fail/`). Every one was refused before anything was
drawn, for five causes; behind those, the visual check and the corpus found
more. Ten of the eleven open natively now, and the eleventh needs a font this
machine doesn't have installed (see the end).

**Images on hidden lines.** A picture with an effect is written twice: what
the effect draws, then the picture itself on `%_` lines, markers and all
(`%_%AI5_BeginRaster` … `%_%%BeginData: 786436`, then `XI` and the samples,
unprefixed). The scanner took `%` after `%_` for a comment inside a line and
then read the samples as operators (`{`, `}`, runs of bytes). Comments and
data after `%_` are tokens now, marked hidden. Of the reader's sections only
images and placed files are acted on from hidden lines; a hidden marker of
any other section is still passed over, as before, and doesn't count when a
skipped section looks for its end.

**Placed files.** See the operator table. Four of the ten files link PNG or
JPEG pictures. The box starts at the artboard's ruler origin on Illustrator's
canvas (`7885 7795` in one file, whose `RulerOrigin` is `7885 7795`); the
matrix puts it on the art. That `lly` is the top edge was settled by the
document's own bounding box, and then by a file whose page shows its pictures. The picture stays a link in PROOF
(`xlink:href` as a `file:` address, `sodipodi:absref` beside it): the file is
looked for where the document says, then in the folder the document gives
from its own, then beside the document, as Illustrator looks. The name is
tried in UTF-8, in the machine's own encoding and in Windows-1252. One that
shows and can't be found, or isn't a picture (Photoshop, PDF), refuses the
native import with its name; one that doesn't show keeps its link as written,
with a note. (Since 2026-10-11 one that can't be found no longer refuses: see
"A linked picture that is gone" below.) A path on a network share is followed like any other, as
Illustrator follows it.

**Patterns.** See the operator table. Illustrator CS and later write the tile
as ordinary art, every line of it hidden, between the box and `E`; the
pattern editor's backing rectangle and the copies it adds along the edges
(`AIPattern_Editor_Backing_Tile_Rect`, `AIPattern_Is_Repeated_Art`) are
ordinary paths. One file's PDF page has the same pattern as a tiling pattern,
and its `/Matrix` is `p`'s matrix moved by the artboard's corner
(`1164.12 −4995.58` against `3283.858 −6686.019` less `2119.739 −1690.435`):
the matrix carries the pattern onto the art. It places the tile from its
lower-left corner, wherever the tile's box lies in the pattern's own space
(found on 2026-10-11 from tiles that start at 5.76, 5.76; see "A second
gauntlet" below. Until then the import placed the box at its own coordinates,
which is the same thing only for a box that starts at 0,0). In
PROOF the tile is a `<pattern>` holding the art in the tile's own space (its
top-left corner at 0,0, y down), kept once and listed under the pattern's
name; each object that paints with it gets a `<pattern>` of its own that
refers to the tile and carries that object's matrix. Patterns nothing paints
with are kept too, as Illustrator lists them among the swatches; one whose
tile couldn't be placed is taken out again instead of refusing the document.
A hand-written pair checks the phase, the flip and a quarter turn against
squares the PDF page draws one by one.

**Area type.** A frame's entry in the text document says its kind in what it
carries: nothing for point type, `/0 1` for area type, beside the gutters
(`/7`, `/8`, 18 points when untouched) and the first-baseline rule (`/10`).
Where point type has its point (`/0 [x y]`), area type has its outline (`/1`):
segments one after another, each four points. The story's layout is the same
tree as point type's, and it holds what Illustrator composed: under the frame
node (`F`, its anchor the frame's top-left corner) come rows and columns
(`R`), lines (`L`, each with its offset and its box) and in each line a
segment (`S`) whose `/15 /0` is the number of characters in it, line end
included, and whose glyph runs (`G`) give their extent. So a paragraph is
broken where Illustrator broke it, without setting it again: each line is a
span at its recorded place, holding its recorded characters. The frame's
outline is kept in `proof:text-frame`, in the text's own coordinates; nothing
flows the text into it again yet, so the lines stay put when the text is
edited. Characters past the last line are what the frame is too small to
show: empty paragraphs are dropped, real text is kept as plain text in
`proof:text-overflow` with a note. A frame nothing was typed in has no layout
at all and is left out, with a note. Type on a path (`/0` other than 1),
linked frames and tabs are still refused.

**One anchor a text object.** What sets text in PROOF takes the alignment of
a whole `<text>` from its first line: the lines are chunks of one paragraph.
Lines anchored differently in one object (a centred paragraph after a blank
line, a centred line beside one set from its start) were therefore all set by
the first, and moved by half their width. A story now has one anchor for all
its lines. It is the middle or the right end only when every line with
characters is aligned that way and says where its anchor is: point type by
its segment's offset from the story's anchor, area type by the middle or
right end of its line's box, when the glyphs' own extent agrees. A line that
keeps a space at its end doesn't qualify: Illustrator centres the space with
the line and PROOF's text setting leaves it out, half a space's width apart.
Otherwise every line is set from where its glyphs start, which is exact.

**Baseline shift.** Character feature 9, in points, up when positive; the
glyph runs of a shifted story are offset by it downwards
(`/0 [0 1.00112]` for `−1.00112`). It becomes `baseline-shift` on the span
that holds the characters and never on the text object: a shift is taken from
the baseline of what holds the span and added to that one's own.

**Kerning and substitutions.** Character feature 11 is how pairs are kerned:
0 not at all, 1 by the font's own kerning (the default), 2 optically, 3 by
the font's for Roman only. A headline with kerning off came in 2.6% narrower,
because the font's kerning was applied regardless. Features 18 and 20 are
standard ligatures and contextual alternates, both on unless a run switches
them off. They become `font-feature-settings:'kern' 0` and
`font-variant-ligatures`, written only in a story where some run differs from
the font's own behaviour. Optical kerning is Illustrator's own measure of the
shapes: the font's kerning stands in for it, and the import says so in a
note.

**Type an appearance drew.** The Dallas trial found type in a drawn look
written with "another story's number". The number is right; it counts in
another document. The text section holds two: `/AI11TextDocument`, what was
typed, and after it `/AI11UndoFreeTextDocument`, the type that appearances
drew. A text object with `FreeUndo` 1 names its story in the second (one file:
109 such objects, numbered 0 to 108, and 109 stories there; all 15 corpus
files with such objects have the second document). Read against the first,
a heading came out as story 36 of the typed ones, a different text elsewhere,
and ten pages of headings were missing, each replaced by a second copy of
some other story drawn over the first.

Both documents are read now (`TextDocument::stories`, `::drawn`). A look that
holds type is kept with the object it stands for until the stories are read.
When the look is nothing but type, in groups that add nothing, and each piece
sets what the object's own type sets (the same characters, styles and
places), the object stands in the look's place, editable as it was typed.
Otherwise the look stands and draws its own stories, which is what the
appearance made of the type (`proof:ai-drawn-story`). Drawn type whose story
isn't in the records takes the object's typed story when there is one a
piece or one for all (records without a second document, and the Dallas
rule as it was); with nothing to say what it sets, it refuses the native
import where it shows. An object with an appearance *inside* one with an
appearance is on `%_` lines altogether, its look, itself and the `1 () XW`
between them; that marker is acted on too now, where it used to be passed
over. So is a marker whose style has no name (`1 () XW`), which one file
writes throughout.

**Characters that set nothing.** A story can hold a NUL (a paste leaves one
before a paragraph's end). It counts as a character in the run and line
lengths and sets nothing; such stories were refused whole.

**Foreign objects.** `/ForeignObject : … /Data ,` is art of another format
that Illustrator keeps as it came (from a placed or pasted PDF, mostly) and
can't edit either: a transform, bounds and a blob in ASCII85 lines that
start with a percent sign. Nothing acted on the dictionary, so the art was
dropped without a word, and a data line that happened to begin `%_(` opened a
string that swallowed the rest of the layer ("it ends inside a layer"). The
blob is skipped as a blob now and the object refuses the native import where
it shows. Four corpus files have one; none of them read natively before.

**The text document's size.** One of the first test files has 560 typed
stories and 340 drawn ones: 357,536 and 263,695 values, against a limit of
250,000 to a document. The limit is two million.

**Size.** The records hold every embedded image uncompressed. A two-page file
with 283 CMYK images is 19 MB on disk and 564 MB of records (435 MB of
samples, one picture eight times over), and the limit was 256 MB. It is
2 GB now. The Zstandard decoder makes room for a frame that says its size at
once, running out of memory falls back to the page instead of ending the
program, and a picture that occurs several times is converted and packed
once. That file takes about 20 seconds to pass the check on this machine:
2.4 s for the records, 0.5 s to read them, 7.4 s to build the document and
10.4 s to draw it and the two pages for the comparison. The opener reports
those parts (`AiNativeOpen::timings`; `AI_TIME` lines from the corpus case).

**What the visual check doesn't see.** It compares artboards. Four of the
eleven files keep the art in question beside the artboard (the pattern of
`Gradient_Fail.ai`, the pictures of two others), where the page has nothing to
compare with. Those were rendered whole and looked at instead; the pattern
and the placement rules above rest on the files whose pages do show them and
on the hand-written pairs.

**Not opened natively: one file, for its font.** Its type is set in
`MinionConceptRoman-DisplayMedium`, an instance of Minion Variable Concept.
That font ships inside Illustrator's own folder
(`Support Files\Required\Fonts`), where no other program is offered it, and
it isn't installed for Windows. Reading it would take the font being
installed, or PROOF being pointed at that folder, and then naming instances
of variable fonts by their PostScript names, which PROOF doesn't do yet.

**Results.** The corpus case over all 879 distinct approved files, one
worker, once: **785 pass the visual check natively and 94 use the page**,
with no error or timeout; against the 768 of the first trial, 17 gained (14
that had pattern fills, 2 over the size limit, 1 whose pattern section
"didn't end") and none lost, and none of the 768 drawn differently by more
than 0.2% of an artboard. That run was made before the two text documents
were read as described above, before NULs, foreign objects and the larger
text limit. Those changes can touch 27 of the files (the ones with drawn
type, nameless look markers, foreign objects, or last refused for what the
changes read); run again, 2 more read natively and none was lost or drawn
differently: **787 and 92**, 19 gained on the first trial and none lost. The
run also showed 128 files with a new note about knockout groups, which came
from Illustrator's stock pattern swatches, kept now though nothing paints
with them. What a tile nothing paints with holds is no longer worth a note;
12 of the 128 were run again and are without it. With the other 116 counted
the same way, 392 of the 787 have no reported loss and 395 have notes
(appearances as drawn groups, mostly).

Of the 92: 37 still draw differently; 18 need a font that isn't installed;
11 have operators that aren't read (meshes among them), 6 opacity masks or
symbols, 4 foreign objects; 4 set type in All Caps (character feature 12 is
2; one of them superscript as well, feature 13), which PROOF's text setting
doesn't do yet; 3 link a picture that isn't where the document says; 3 have a text
document with entries missing; and the rest are one or two each (artboard
and page counts that differ, an image that can't be read, type on a path, a
file that can't be opened).

### Third round of 2026-10-10: font instances, capitals, fonts that aren't installed, turned pictures

One trial file was left on its page after the second trial, for its font. Its
owner set it in Acumin Variable Concept instead and installed that font. It
then needed three things read, and showed a fourth that was wrong.

**Instances of variable fonts.** The text document lists each font by a
PostScript name. An instance of a variable font has none of its own, so
Illustrator makes one up, as Adobe's Technical Note 5902 describes:

| The instance | Its name |
|---|---|
| A named one | The font's prefix, a hyphen, the style: `AcuminConcept-ExtraCondUltraBlack` |
| Any other | The prefix, then `_`, a value and an axis tag for each axis off its default: `AcuminConcept_396.000wght_64.000wdth_4.200slnt` |
| The font's default | The font's own PostScript name: `MinionVariableConcept-Roman` |

The prefix is the font's name 25, else its family name, with everything but
letters and digits dropped. It isn't the family's PostScript name: Acumin
Variable Concept is `AcuminVariableConcept`, and its instances start
`AcuminConcept`.

The name alone doesn't say which instance it is. Two versions of one font name
the same instance differently: the corpus has `AcuminConcept-ExtraCondensedSemibold`
where the installed font calls it `AcuminConcept-ExtraCondSemibold`. The system
lists a named instance that equals the font's default under the font's own
name only (Bahnschrift's Regular is `Bahnschrift`, never
`Bahnschrift-Regular`). And a name too long to spell out ends in a checksum.

What says it is beside the name. A font's entry in the list (`/0 /1 /0`, each
`/0 /0`) holds:

| Key | Value |
|---|---|
| `0` | The PostScript name |
| `2` | The kind of font: 0 or 1 |
| `4` | For an instance: its value on each axis, in the font's own order, in 16.16 fixed point. `[25952256 4194304 275251]` is weight 396, width 64, slant 4.2 |
| `5` | The font's version string |

So the name finds the font, and key 4 is the instance. `ai-native-fonts.cpp`
does it in this order:

1. A face the system knows by exactly that name. If its file has no axes, it
   is an ordinary font, as before. If it has, the instance is key 4 when it has
   a value for each axis, else the named instance the system numbers it, else
   the font's defaults.
2. No face by that name: the installed variable font whose prefix the name
   starts with. The instance is key 4, else the named instance of that name
   (the name the font gives it, or the one made from its style), else the
   values the name spells out.

The text gets the font's family and every axis in `font-variation-settings`,
default or not: Pango picks a face of the family by weight, width and slant,
that face can be a named instance, and it starts from the instance's own
values. `font-weight` and `font-stretch` follow the weight and width axes. The
face Pango then loads is checked to be that font at those values. A family
with ordinary fonts beside its variable one can give one of those instead.
When the name is one the system knows a face by, that ordinary font is taken,
as it was before instances were told apart; when the font was only found by
its prefix, it counts as not installed.

A count over the corpus's records: 15 files set type in a font with key 4; one
of them names an instance by its values; no font name has an underscore
without key 4.

**All Caps.** Character feature 12 says how a run is capitalised: 0 as typed,
1 small caps, 2 all caps. The story keeps what was typed. A run in all caps
gets `text-transform:uppercase`, which PROOF's text layout now applies
(`Layout-TNG-Input.cpp`): it had been parsed and ignored. The layout keeps a
transformed copy of each source's text with one character for each typed one,
so a place in the layout is still a place in the text and the cursor and
editing work as before; `uppercase`, `lowercase` and `capitalize` all go
through it. A letter whose capital is two letters (ß to SS) stays as typed for
that reason. Small caps (1) still refuse the story.

**Superscript and subscript.** Character feature 13 says where a run stands:
0 on the line, 1 superscript, 2 subscript. Illustrator makes both from the
run's own font, smaller and moved, by settings the document keeps (Document
Setup > Type). They are in the settings of the text document's main part
(`/1 /0`), each a share of the type's size:

| Key | Setting | Illustrator's own |
|---|---|---|
| `3`, `4` | Superscript: size, and how far above the baseline | 0.583, 0.333 |
| `5`, `6` | Subscript: size, and how far below | 0.583, 0.333 |
| `7` | Small caps: size | 0.7 |

Such a run says where it stands with `font-variant-position` (`super` or
`sub`) and keeps its own size; a shift of its own (feature 9) stays in
`baseline-shift` and adds to it. The text layout sets the run smaller and off
the baseline by the document's settings (since 2026-10-11, see "The editor's
side of type" below; until then the run was a span at a plain size and shift).

**A font that isn't installed** no longer sends the file to its page. As in
Illustrator, another font sets the type: Myriad Pro when it is installed, else
Arial, else the system's sans serif. The text or span carries the missing
font's PostScript name in `proof:font-missing` (and an instance's axis values
in `proof:font-missing-axes`), and its declarations name no
`-inkscape-font-specification`. That last part is how a later choice is told
apart: choosing a font for the text writes one, and the text is then simply in
the font chosen. Lines stay where Illustrator put them.

When a document opens in the editor, text whose font has been installed since
gets it back (`restore_fonts`), and the fonts that still aren't are listed in
the Missing Fonts window, which can replace each across the document and
selects the text still set in another font on request
([PROOF-type.md](PROOF-type.md)). That list shows once for a document and by
default; Preferences > Text has the switch (`/options/font/missingdlg`).

Such type can't look like the page, so the visual check leaves it out. Each
run of glyphs in the layout tree (`G`) has the box Illustrator set it in, in
its key `8` (left, top, right, bottom, in the layout's space). Those boxes, and
the bounds of the same text as it is set here, are painted white in both
pictures before they are weighed. Everything else is still compared: of two
files that came this far and still differ, one has a group drawn in a flat
colour and one a blend mode missing, both refused as they should be. When more
than a quarter of an artboard is left out, a note says so.

Characters a font doesn't have are treated alike: another font draws them, the
text is marked `proof:glyphs-missing`, and the check leaves it out.

**Pictures that are turned** were placed wrongly. An image's matrix is written
for rows counted upwards, and a picture's rows count down. For an upright
picture it makes no difference which side of the matrix the rows are turned
over on, and the import had it on the wrong one. The corner tiles of a pattern
brush in the trial file settle it, since Illustrator writes the same picture
turned each way inside one tile box:

| Matrix | 201 x 106 pixels land in |
|---|---|
| `[0.24 0 0 -0.24 -7884 7768.56]` | x -7884 to -7835.76, y 7768.56 to 7794 |
| `[0 0.24 0.24 0 -7848.21 7746.76]` | x -7873.65 to -7848.21, y 7746.76 to 7795 |
| `[0 -0.24 -0.24 0 -7873.55 7795]` | x -7873.55 to -7848.11, y 7746.76 to 7795 |

All inside the tile's box of -7885 to -7836.76 by 7746.76 to 7795, which the
old reading left. A pixel (x, y) goes to (a·x − c·y + tx, b·x − d·y + ty).

**Tests.** `ai-native-import-test` has cases for each: a font entry's axis
values; Bahnschrift's instances by name, by values and by a name that only
the values explain, and their widths as drawn; text-transform against the same
capitals typed, with letters that change length in bytes, and the cursor's
place; all caps from a run's features; superscript and subscript at
Illustrator's sizes and at a document's own; pictures turned each way; a font
that isn't installed, its name kept per span, and given back when it is found; and
three against an independently written PDF page (the type left out, other art
beside it still refused, and a page whose type lies elsewhere refused).

**Results.** The trial file opens natively, at most 0.6% of an artboard
from its page, and with it all seventeen trial files (the installed console
build, each exit 0, none from its page). The corpus case over all 879 distinct
approved files, one worker, once, on the final build: **802 pass the visual
check natively and 77 use the page**, with no error or timeout. Against the
787 of the second trial, 15 gained and none lost, and none of the 787 is drawn
further from its page by more than 0.2% of an artboard. Of the 15, 12 had been
refused for a font, 2 for pictures that are turned, and 1 for all caps (it
also names a font that isn't installed). 13 of the 802 have type in a font
that isn't installed, the furthest of them 0.5% from its page, one with more
than a quarter of an artboard left out of the check. 399 of the 802 have no
reported loss and 403 have notes. The run took about ten minutes.

Of the 77: 36 draw differently (two of them reached the comparison only now);
11 have operators that aren't read; 7 opacity masks; 4 link a picture that
isn't where the document says; 4 have a text document with entries missing; 3
foreign objects; and one or two each: a control character in a story,
artboard and page counts that differ, an image that can't be read, a justified
paragraph, type on a path, character scales that differ within a story, linked
text frames, symbols, a file that can't be opened.

### A gauntlet of character options (2026-10-10, late)

The user made a file with one text object for each option of the Character
panel, changed one at a time, in an instance of Acumin Variable Concept:
plain, all caps, small caps, superscript, subscript, no break, Hebrew in
three paragraphs read from the right, strikethrough, strikethrough with
underline, tracking tight and wide, and four character scales with optical
kerning. It was refused at the first strikethrough. It opens natively now,
0.02% from its page at the check's size.

**Small caps** (character feature 12 is 1). Acumin has no small capitals of
its own, and Illustrator makes them: the layout tree has the capital in one
run of glyphs and the small letters in the next, whose box is 0.7 of the
first's (`[0 -7.148 8.139 1.972]` against `[0 -10.212 3.636 2.817]`), the
document's setting 7. The story keeps what was typed. The run gets
`font-variant-caps:small-caps`, and the text layout now sets it either way: a
font that has small capitals uses its own, as before; for one that hasn't, a
small letter is laid out as its capital at 0.7 of the size
(`InputStreamTextSource::reduced`), in the same one-character-for-one copy
that all caps uses. The size is the document's setting 7 (kept with the
document since 2026-10-11; before that a setting other than 0.7 refused the
story).

**Strikethrough and underline** (features 15 and 16, each 0 or 1) become
`text-decoration-line` on the span of the run. An underline's box is in the
line's segment (`S`, key 18: left, top, right, bottom); for 12 point type it
is 0.6 thick and 0.9 below the baseline, which is the font's own underline.
Nothing says where a strikethrough goes. A line under or through type stops at
the line's last letter in Illustrator; the layout drew it on through the
letter-spacing that follows that letter (or stopped short of the letter when
the spacing was negative), and now stops where Illustrator does.

**Paragraphs read from the right** (paragraph feature 35 is 1) get
`direction:rtl`. Their lines start at the same end as before: a line's origin
is its left end, its right end or its middle whichever way it reads, and only
the name of that end changes (left is the end of a line read from the right).
Illustrator has two composers. The one for these scripts puts the letters in
order and leaves what it worked out in the segment's character count (key 15
of `S`, its key 7). The other sets Hebrew from the left as it was typed, which
shows it backwards. A sample of the backwards case came with the second
gauntlet and confirmed the rule; such a story is now set the way Illustrator
shows it (below). Until then it was refused.

No break (feature 52) changes nothing that is drawn and is not read.

**Looked at closer than the check does.** The check draws a letter page at
about 0.76 pixels a point, where 12 point type is nine pixels high. The built
document and the page were also drawn at 300 dpi and laid over each other.
Every object lies where the page has it, small caps, superscript and
subscript included; the lines under and through type end where the page's do;
what differs is the Hebrew, drawn by another font, and the edges of letters by
a fraction of a pixel.

**Results.** Both new files open natively: the gauntlet, and the earlier
trial file in Minion Variable Concept, with Myriad Pro standing in for its
three instances (306 text objects). The fifteen trial files opened with the
installed console build all take the native route. One corpus file holds
anything these changes read (letters read from the right, not put in order);
it used its page before and does now, so the corpus was not run again and
stays at 802 of 879.

### The editor's side of type (2026-10-11)

Nothing new is read from the records in this round. What the import writes for
three options changed, so that the editor's Character buttons and the imported
type are one thing ([PROOF-type.md](PROOF-type.md)).

**Superscript and subscript** (character feature 13) are now
`font-variant-position:super` or `sub` on the run, at the run's own size. The
text layout makes them: the run's characters at the document's size for them,
moved off the baseline by the document's distance, each a share of the type's
size. A shift of the run's own (feature 9) stays in `baseline-shift` and adds
to it. Before, the import worked the size and the shift out and wrote them as
plain values, so the size shown for such type was the reduced one and a later
change of size left the shift behind.

**The document's settings** (the text document's `/1 /0`, keys 3 to 7) are
kept with the document, on its root element: `proof:superscript` and
`proof:subscript` (size, distance) and `proof:small-caps` (size). What is
Illustrator's own (0.583 and 0.333, 0.583 and 0.333, 0.7) isn't written. The
layout reads them when it sets such type.

**Small caps at a size other than 0.7** no longer refuse the story: the size
is one of those settings.

**Type that appearances drew** is in a text document of its own
(`AI11UndoFreeTextDocument`), with settings of its own. A document has one
set. Where the second text document's differ, its stories that use the
setting in question are refused, as any type that can't be set as it is; no
file has been seen where they differ.

**Checked.** The gauntlet file, drawn at 300 dpi by the build before this
change and by this one, is the same in every pixel: small caps, superscript
and subscript lie where they lay. The trial files' distances from their pages
are unchanged to the last digit. Two corpus files have superscript or
subscript (one native, 0.22% from its page before and now; one on its page for
another reason); none has small caps or settings of its own, so the corpus was
not run again and stays at 802 of 879.

**Not seen in a file.** Small caps and superscript on the same run (each makes
the letters smaller in its turn here), and settings other than Illustrator's
own. Both are covered by unit tests only.

### A second gauntlet (2026-10-11)

The user's gauntlet grew to 60 text objects: twelve instances of one font,
every scale and tracking, stroked and outlined type, type painted with a
gradient and with patterns, the four anti-aliasing settings, Hebrew set up for
its direction and not, and type under an envelope. It was refused at the first
of these to be met. It opens natively now, 0.27% from its page by lightness
and 0.11% by colour at the check's size.

**An envelope** (Object > Envelope Distort) is an object a plug-in made, ended
by `(Adobe Deform Plugin) 1 0 0 XP`. Before that marker come, on `%_` lines, a
group holding the mesh it distorts with, and in that group's dictionary the
object it distorts (art between `X=` and `X+`, under `AIDeformArtKey`, beside
the warp's settings); then, plainly, a group of what the envelope drew. Art in
a dictionary was never kept. The mesh group is now dropped too, and the drawn
group comes in as a drawn look. What was distorted, type or art, isn't kept:
the document says so in a note.

**A mesh** is `/Mesh X!`, lines of `X#`, `/End X!`. These were operators
nobody knew; they are known now, and a mesh that is art (a gradient mesh)
still refuses the file, under its own name. Only the mesh of an envelope,
found as above, is left out without that. Eleven corpus files have gradient
meshes; their reason reads "gradient meshes" where it listed the operators.

**Justified paragraphs** are paragraph feature 0 at 3 to 6: justified with the
last line left, right, centred, or justified too. A line of point type has no
measure to be justified to, so it is set as that last line: from its left end
for 3 and 6, from its right end or its middle for 4 and 5 where the layout
says where that is, and from where its glyphs start otherwise. Justified area
type is still refused: its lines are stretched to the frame.

**Hebrew that isn't set up for its direction.** The sample is four Hebrew
letters in a paragraph that reads from the left (feature 35 is 0), and its
line's segment has a character count (key 15) with `/5 false` and no key 7:
Illustrator's other composer set it, letter after letter from the left as
typed, which shows the word backwards. The story is marked for it and comes in
with `direction:ltr; unicode-bidi:bidi-override`, which the text layout now
knows: the text is what was typed, and it shows as it does in Illustrator.
Such letters in a paragraph that reads from the right, or put in order in one
line and not in another, still refuse the story.

**Paint by name.** A character's fill or stroke is a `/SimplePaint`, or a
`/CAITextPaint` that names a style of the document's beside its colour:
`/5 (Anon …)`. The styles are between `%AI9_BeginArtStyles` and
`%AI9_EndArtStyles`, each a `/KnownStyle :` with its `/Name` and a
`/SimpleStyle` whose operators set the paint: a colour, `Bg` for a gradient,
`p` or `P` for a pattern. For a gradient or a pattern the colour kept beside
the name is black, whatever the paint looks like.

| The style's paint | Illustrator's page | Here |
|---|---|---|
| A colour | That colour | The colour beside the name, as before |
| A gradient | Black: a gradient isn't drawn on type | Black, with a note that says why |
| A pattern | The pattern | The pattern, placed by the style's matrix as on a path |

Before this the gradient case was right by accident and the pattern case came
in black without a word; the check let it through, since two words are a
small part of a page.

**Where a pattern's tile lies.** The page has the two patterns as tiling
patterns whose cells run from 0,0 (`/BBox [0 0 28.8 28.8]`), under the very
matrix the style gives (moved by the artboard's corner), and the art inside is
moved to match: a dot the records have at 12.96, 12.96 in a tile box that
starts at 5.76, 5.76 is at 7.2, 7.2 there. So the matrix places the tile from
its lower-left corner. The import placed the box at its own coordinates, which
put these tiles out of phase by the matrix times that corner. It is the same
for a path's pattern, and corrected for both. No corpus file paints art on its
layers with a pattern whose tile box starts elsewhere than 0,0, so none of
them changes.

**Anti-aliasing** (`/TextAntialiasing` in the text object, 0 to 4) is a hint
for the screen and isn't read.

**Looked at closer.** Drawn at 200 dpi and laid over the page, every object
lies where the page has it, the dots of both patterns included. What differs:
the Hebrew, drawn by another font; letter edges by a fraction of a pixel; and
the black of the gradient-painted type, which the page has as a grey and
Poppler draws darker than the black ink it is here.

**Results.** The trial files are as far from their pages as before, to the
last digit. Two corpus files hold anything these changes read: one with
justified area type, on its page before and now, and one with letters read
from the right, on its page for a picture that is gone. The corpus was not run
again and stays at 802 of 879.

### A linked picture that is gone (2026-10-11)

Four corpus files stayed on their pages because a picture they link to wasn't
where they said. Illustrator opens such a document, says which files it can't
find, and offers to replace them. PROOF now does the same.

**The picture Illustrator keeps.** Since Illustrator 2022 a placed file's
section holds a small picture of the file, after its placement and before `~`:

```
%AI17_Begin_Content_if_version_gt:24 5

%AI26_BeginPlacedObjectPreview
512 336 2048 32 17 <688128 bytes>
%AI26_EndPlacedObjectPreview
%AI17_End_Versioned_Content
```

| Number | Meaning |
|---|---|
| 1, 2 | Width and height in pixels |
| 3 | Bytes to a row: four times the width in every file seen |
| 4 | Bits to a pixel: 32 |
| 5 | 17 in every file seen; not known what else it can be |

One space follows the last number, then the rows, top row first, as bytes of
any value, and a line end. Each pixel is alpha, red, green, blue. The colours
aren't multiplied by the alpha: in the one picture of the corpus with pixels
that are partly clear, 105 of 1,943 such pixels are brighter than their alpha.
All 38 placed files in the corpus (six documents) have one. It is the picture
as large as it lies on the art at 72 to an inch when that is small, and
otherwise 512 or 256 pixels on its longer side or its width; which of the two
wasn't worked out.

The scanner used to pass this section over by looking for the marker that ends
it. It goes by the count now, since the bytes can be anything, and falls back
to looking for the marker when the marker doesn't follow the count. The reader
keeps the picture with its placed file (`Placed::preview`, as red, green,
blue, alpha). A picture that isn't 32 bits with that 17, or is cut short, or is
over 2,048 pixels on a side, isn't kept, and the file is placed all the same.

**A file that can't be found** no longer sends the document to its page. The
picture is an `<image>` whose link stays as the document wrote it, with the
kept picture beside it as a PNG in `proof:link-preview`. PROOF's picture object
shows that in the file's place while the file can't be found, in the box the
file's own picture lay in. A document that keeps no picture of it (Illustrator
before 2022) gets Inkscape's marked box there. The editor lists such pictures
when the document opens and links them again
([PROOF-links.md](PROOF-links.md)).

What stands in can't look like the page, which has the file's own picture at
full size. So the box is left out of the visual check on both sides, as type
set in a stand-in font is, and everything else is still compared.

Still refused, where it shows:

| Case | Why |
|---|---|
| A linked file that is there and isn't a picture that can be read (Photoshop, PDF, Illustrator) | Illustrator shows it and PROOF can't: the page has it |
| A missing picture inside a pattern's tile | It shows wherever the pattern paints, so there is no one box to leave out of the check. A pattern nothing paints with is taken out, as before |
| A link whose name can't be read in any of the three encodings tried | Nothing to look for or to show |

A missing picture that doesn't show is kept as a link and counted in a note, as
before; it now carries its kept picture too.

**What the check can no longer see.** A box that is left out hides whatever
else lies in it. One of the four files is a single picture over its whole
artboard, so nothing of that artboard is compared, and the notes say so
("about 100% of the artboard is linked pictures that can't be found, and
wasn't compared with the page"). Comparing what stands in with the page at the
kept picture's own resolution would close that; it isn't done.

**Tests.** `ai-native-reader-test`: the count of a preview against bytes that
spell its end marker, a count the marker doesn't follow, and the picture kept
with its placed file or not kept when it is written another way.
`ai-native-import-test`: a missing picture with and without a kept picture (the
link, the PNG to the pixel, what the picture object shows, the note, a long
name cut short in the note, a tile, a pattern nothing paints with); linking
again from one folder, a file that isn't a picture, a picture of another shape,
and a relink taken back; one that doesn't show; and one against an
independently written PDF page (its box left out, art beside it still refused,
a page with its picture elsewhere refused).

**Results.** All four files open natively: two with nothing differing outside
the boxes left out, the others 0.7% and 0.9% from their pages by lightness
(the limit is 1.0%). Between them they have 10 missing pictures that show and
14 that don't. Six corpus files place files at all, and those six are the only
ones this round's changes can touch. The other two are on their pages for
other reasons, as before. With the seventeen trial files they were run again:
20 results are the same as last round to the last digit, notes included, and
the four are gained. The corpus was not run whole and stands at **806 of 879**
by that count.


## Plan

1. **Native reader** (`src/extension/internal/ai/`), delivered. It covers the operator table
   above, is bounded, and fails closed. It produces a structure (layers, objects,
   inks, gradients, images, artboards) for both the importer and round-trip tests
   of the writer.
2. **Native import**, delivered for the checked subset above. It builds the PROOF document from that structure:
   - CMYK colours and spot inks stay exact.
   - Lab alternates are kept.
   - CMYK images keep their samples.
   - Hidden objects and layers come in hidden.

   Drawn looks come in as groups. Supported text slots get editable point type. The
   render is compared with the PDF page, and the page is used whenever they
   differ or anything is unread.
3. **Point type**, delivered for the bounded subset above, with VectorCraft's
   `ate.rs` as the numeric-key reference, and **area type** as Illustrator
   composed it. Instances of variable fonts, all caps and small caps,
   superscript and subscript (with the document's sizes for them), underline
   and strikethrough, and paragraphs read from the right are read, and a font
   that isn't installed is stood in for.
   Expand from confirmed root causes. Still to come: area type
   that flows again when edited (the frame and the overflow are kept for it),
   type on a path, justified paragraphs, linked frames, source ICC profiles
   and the remaining rendering families.
4. **Writer** extensions, in this order: gradients, images, text. They use the
   same tables, and the reader checks the writer's output alongside Illustrator.
