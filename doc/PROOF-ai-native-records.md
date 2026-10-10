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
with a note. A path on a network share is followed like any other, as
Illustrator follows it.

**Patterns.** See the operator table. Illustrator CS and later write the tile
as ordinary art, every line of it hidden, between the box and `E`; the
pattern editor's backing rectangle and the copies it adds along the edges
(`AIPattern_Editor_Backing_Tile_Rect`, `AIPattern_Is_Repeated_Art`) are
ordinary paths. One file's PDF page has the same pattern as a tiling pattern,
and its `/Matrix` is `p`'s matrix moved by the artboard's corner
(`1164.12 −4995.58` against `3283.858 −6686.019` less `2119.739 −1690.435`):
the matrix carries the pattern onto the art, with nothing else to apply. In
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
   composed it. Expand from confirmed root causes. Still to come: area type
   that flows again when edited (the frame and the overflow are kept for it),
   type on a path, justified paragraphs, instances of variable fonts, source
   ICC profiles and the remaining rendering families.
4. **Writer** extensions, in this order: gradients, images, text. They use the
   same tables, and the reader checks the writer's output alongside Illustrator.
