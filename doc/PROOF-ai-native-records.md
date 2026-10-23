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
| `u`…`U`, `*u`…`*U`, `q`…`Q` | Group, compound path, clip group. `W` makes the next path clip. A compound path is painted as its last path that has paint. A clipping path can lie in a group of its own at the end of its clip group, and be written three times when it has a fill and a stroke: see "What a plug-in made, clipping paths, compound paths and a group's own state". Several clipping paths in one clip group clip together. | VectorCraft; the rest from the **2026-10-13 round** |
| `m l L c C v V y Y h H` | Path construction. | VectorCraft |
| `N n F f S s B b` | Paint: none, fill, stroke, both. The lower-case forms close the path. `(op) *` paints a guide. | VectorCraft |
| `w J j M d`, `XR` | Stroke width, cap, join, miter limit and dash; fill rule (1 means even-odd). | VectorCraft |
| `O`, `R` | Overprint the fill or the stroke. | VectorCraft |
| `Xw`, `A` | Hidden, locked. | VectorCraft |
| `mode opacity isolate knockout shape Xy` | Transparency. The modes run Normal, Multiply, Screen, Overlay, Soft Light, Hard Light, Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Hue, Saturation, Color, Luminosity. It is written only where it differs from what the last object was written with. After a group closes, it applies to the group. After a compound path closes it is the next object's. | VectorCraft; the last two sentences from the **2026-10-13 round** |
| `a b Xd`, then `6 () XW` | Ends the state a group is written with after it closes. The group's transparency is what holds at `Xd`, whether an `Xy` came before it or not. A group that ends with `9 () XW` has no state of its own. | VectorCraft (`939bc02f`), checked on **2026-10-13** |
| `(plug-in) a b n XP`, then n bytes on `%` lines | Ends an object a plug-in keeps live. Two groups come before it: what the object was made from, on `%_` lines, and what the plug-in drew. Only the second is art. Seen: `Pathfinder Suite` (compound shape), `Adobe Path Blends`, `Adobe Calligraphic Brush Tool`, `Adobe ArtOnPath Brush Tool`, `Adobe PatternOnPath Brush Tool`, `Adobe Deform Plugin` (envelope). | **2026-10-13 round**; the envelope from the second gauntlet |
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
| `[flag] Bb`, `Bg`, `Xm`, `Bm`, `Bc`, `Bh`, `BB` | Gradient on an object, written around the path's paint operator. See "Gradient placement" below: with `Xm` or `Bm` present that matrix is the whole placement, and `Bg`'s origin, angle, length and matrix are not applied as well. `Bb`, `Bg`, `BB` with no path between them is a note an object keeps of its gradient, written with its own state: it paints nothing. | VectorCraft read `Bg` × `Bm`; corrected in the **2026-10-10 trial**. The note: VectorCraft (`77120ada`), checked on **2026-10-13** |
| `%AI5_BeginRaster`, `/Space XN`, `[m] … w h bits type alpha … XI`, `XH`, `N` | Embedded image. The samples stay in their colour space; CMYK is common. The image is an object once its samples are read: the `XH` and the `N` (which paints no path) after it don't end what follows an object, so its name and a `1 () XW` still reach it. | VectorCraft; the tail from the **second 2026-10-10 trial** |
| `0 /DeviceGray XC` in place of `/Space XN` | A grey picture coloured with a paint: the paint's numbers, then its space. Illustrator 29 saves a grey picture it opened from a version 7 file coloured black, which prints as the grey picture and is read as one. Any other paint (two of the owner's record sets colour a grey picture with a Pantone ink) is refused by name. | Illustrator 29.2.1's save of the version 7 control, **2026-10-21** |
| `%AI3_BeginPattern: (name)`, `(name) llx lly urx ury`, the tile's art on `%_` lines, `E`, `%AI3_EndPattern` | Pattern. The box is the tile, in a space of the pattern's own (y up); art past it is cut off there. | **second 2026-10-10 trial**. VectorCraft skips the section. |
| `(name) px py sx sy angle rf r k ka [a b c d tx ty] p` (`P` for a stroke) | Paint with a pattern. The matrix carries the pattern's space onto the art, the same space the paths are in. The nine numbers before it are how Illustrator 8 and older moved, scaled, turned, mirrored and slanted a pattern; later versions leave them at `0 0 1 1 0 0 0 0 0`. Like a colour, it stays the paint until another is set. | **second 2026-10-10 trial**; the matrix checked against a file's own PDF pattern |
| `%AI26_BeginPlacedObjectPreview`, `w h row-bytes 32 17 ` and the rows, `%AI26_EndPlacedObjectPreview`, in a placed file's section | The small picture Illustrator keeps of a placed file, for when the file can't be found. See "A linked picture that is gone". | **2026-10-11** |
| `%AI5_BeginPlace`, `%%FileType: n`, `[a b c d tx ty] llx lly urx ury … (path)` then `` ` ``, `%AI28_BeginPlacedRelativePath` `(folder)` `%AI28_EndPlacedRelativePath`, `~`, `%AI5_EndPlace` | Placed (linked) file. The box is where the picture lies in a space of its own whose y points down (its top edge is `lly`); the matrix carries that onto the art. The path is as the saving machine knew it; the folder is where the file was found from the document's own, empty when it is the same. | **second 2026-10-10 trial** |
| `/AI11Text : /FreeUndo … /StoryIndex …` | Text object. Its text lives in `%AI11_BeginTextDocument`. The section holds two documents: `/AI11TextDocument`, what was typed, and `/AI11UndoFreeTextDocument`, the type appearances drew. With `FreeUndo` 1 the object is drawn type and its `StoryIndex` counts in the second: see "Type an appearance drew" below. | VectorCraft; the second document from the **second 2026-10-10 trial** |
| `n Ar` | The old Attributes panel's output resolution (300, 800). No visual effect. | **census** |
| `1 An` | CS5 only, after every object. Almost certainly *Align to Pixel Grid*. No visual effect. | **census** |
| `(hex) Xt n XD` | Brush metadata inside drawn looks, with n running 0–4. No geometry or paint. | **census** |
| `Ae AE Ap As Xr XG Xh XH XF D X= X+ Bc Np TE TZ Xt Xi XI ` LB2 Lc Xv XV Xq XQ Xg Xn Bn` | Ignored by VectorCraft. (It ignores `Xm`, `Xd` and `XP` too; PROOF reads them.) | VectorCraft |

### Not read at the first census (2026-10-09)

This table records the original census. Later sections describe delivered
support; the README gives the current aggregate results.

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

A PostScript `.ai` (Illustrator 8 and older) has no PDF page, so it can't be
weighed at all: it opens from its records alone, with a note that its art
wasn't checked against a page, and the corpus checker counts it apart as
`NATIVE_UNCHECKED`. What its records hold that isn't read still refuses it.
See "PostScript files with no layers and no page" below.

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


### The document's own colour profile, and how black shows (2026-10-11)

The question was whether 100% black looks the same in PROOF as in Illustrator
with its Appearance of Black set to "Display All Blacks Accurately", where
100K is the dark grey its profile makes of it and not the black of the screen.

**PROOF shows every colour by the document's profile**, 100K included: there
is no rich-black display. That is Illustrator's "accurately". Both convert
with the relative colorimetric intent and black point compensation unless told
otherwise. So the two agree whenever they go by the same profile, and they did
not always.

**The profile was the wrong one for most files.** An import was given PROOF's
working profile of its colour mode whatever the file said. Illustrator shows a
document by the profile it was saved with. The records don't hold that profile
or name it. It is in the file's PDF part: with the profile embedded,
Illustrator draws every process colour of a page in a colour space based on
it, listed among the page's resources:

```
/Resources << /ColorSpace << /CS0 [/ICCBased 50 0 R] /CS1 [/Separation /Ink [/ICCBased 50 0 R] ...] ... >>
```

| What the first page names | Files (of 879) |
|---|---|
| One CMYK profile | 588: U.S. Web Coated (SWOP) v2 533, Coated FOGRA27 31, Coated FOGRA39 20, Coated GRACoL 2006 3, ISO Coated v2 300% 1 |
| An RGB profile and no CMYK one | 107: sRGB IEC61966-2.1 104, Adobe RGB (1998) 2, Generic RGB 1 |
| Neither | 183 |
| More than one profile of one colour model | none |
| A PDF part that can't be read | 1 |

A Lab profile ("Lab D50", 95 files) is there for spot inks defined in Lab. It
has three channels like an RGB profile, and its own header says which it is.

The opener now takes the profile from the PDF part before the document is
built (`document_profile` in `ai-native-open.cpp`): the ICC stream a page's
colour space is based on, directly or as what a spot colour falls back to,
with as many channels as the document's mode and a header that says CMYK or
RGB. The import assigns it (`Source::profile`); a file that names none is in
the working space of whoever opens it, as in Illustrator, and one whose pages
name two is left in the working space too.

**What 100K is.** PROOF's value, and what Illustrator exported for the same
files in the check of art beside the artboards:

| Document's profile | PROOF | Illustrator |
|---|---|---|
| U.S. Web Coated (SWOP) v2 | 35, 31, 32 (`#231f20`) | 33, 31, 32 |
| Coated GRACoL 2006 | 27, 26, 26 | 26, 26, 26 |

Before this, a SWOP document's 100K was GRACoL's 27, 26, 26 on this machine,
and every other colour of it was converted by GRACoL as well. A level or two
remain between the two colour engines.

Not done: Illustrator's other setting, "Display All Blacks as Rich Black";
pictures with profiles of their own.

### A compound path with its paths in a group (2026-10-11)

A compound path that has a style of its own is written with its paths in a
group inside it, and the style's marker after the group:

```
*u
u
1 D
0.72 0.66 0.64 0.74 0.125 0.125 0.129 Xa
… f
0 D
… f
U
9 () XW
*U
```

The reader gave a compound path the paint of its first member. Here that is
the group, which has none, so the whole shape came in with neither fill nor
stroke: a character's trousers, a car's body, a desk. Where a shadow in
Multiply lay over it, the shadow was all that showed. The paint is now that of
the first path, however deep; a transparency the path lacks is taken from the
group it lies in.

50 corpus files have such a compound path (181 of them): 11 of the 36 that
drew differently, 2 on their pages for other reasons, and 37 that passed the
visual check with a shape missing, since a missing shape under one part in a
hundred of an artboard passes.

### An artboard the records only let one guess at (2026-10-11)

A document from Illustrator CS3 or older, or kept in that form, lists no
artboard. Its records give a size (`%AI5_ArtSize`) and a centre
(`%AI3_TemplateBox`), and the reader makes the one artboard of those, with its
left edge rounded down and its top edge rounded up. 196 of the 879 corpus
files are like this.

The guess is often not where the art was drawn for. The file's PDF part says
where it is: its page is the artboard, and its `/ArtBox` gives the bounds of
the art on that page, which the records give in art space
(`%%HiResBoundingBox`). Where the two bounds are as large as each other they
are the same art, and the page's corner follows:

| The guess, against the page | Files (of 196) |
|---|---|
| The same (under 0.01 pt) | 63 |
| Under 0.25 pt off | 13 |
| 0.25 to 1 pt off | 83 |
| 250 pt off | 1 |
| The two bounds differ in size on one axis or both | 36 |

So in 83 files the art lay a quarter to one point from where it belonged on
its artboard, and 75 of them passed the visual check, which allows a pixel of
play at about 0.6 pixels a point. For trimming to an artboard a point matters.

`place_board_by_page` (in `ai-native-reader.cpp`, called by the opener before
the document is built) moves such an artboard to where the page lies, along
each axis on which the bounds agree to within 0.05 pt. An axis on which they
differ stays as guessed: one of the bounds then counts art the other doesn't
(hidden art), or the page cuts it. A document with listed artboards or crop
marks isn't touched.

### Two ways of weighing, earlier (2026-10-11)

The visual check weighs an artboard that fails again from pictures drawn four
times larger and averaged down, with thin lines kept a pixel wide on both
sides, and takes the better result. An artboard that passed at first sight
never got the second weighing, so a file of thin lines at 0.98% was reported
as differing in 1% of it, where the same file a shade worse was weighed again
and reported at 0.0%. After the profile change four files crossed that line
downwards and looked as if they had got worse. From half of what is allowed
(`FINER_FROM`), both weighings are now made. A file can't fail for it: the
better of the two counts, as before.

Lifting the blacks of both pictures to the document's 100K was tried for the
files that differ by 1 to 2%, on the thought that Poppler draws grey-scale
black as the screen's black where the import has the profile's dark grey. It
changed none of them by more than 0.1% and was dropped.

### Results of the three (2026-10-11)

**Tests.** `ai-native-reader-test`: a compound path with its paths in a group,
and an artboard put by its page (both axes, one axis, a page of another size,
crop marks). `ai-native-import-test`: such a compound path painted, and a
document's own profile from `Source::profile` and from a PDF page's colour
space, with a profile of another colour model and bytes that are no profile
passed over. Reader 51, import 76.

**Corpus**, all 879 files, one worker, once, with the profile and the compound
path changes and the missing pictures of the section before: **816 native, 63
on their pages**, none lost. Against the 802 before the day's work: the four
with missing pictures, and ten that drew differently (nine with such compound
paths). 37 files that passed before had such a compound path and now draw the
shape that was missing. The 208 files the artboard change can touch (the 196
and those still differing) were then run again: 4 more native, among them the
one whose artboard was 250 pt off, 72 closer to their pages, none lost.
**820 of 879** by that count. The seventeen trial files, the gauntlet and
`minion-check.ai` are native on the last build.

What is left on its page, of 59: 22 that draw differently, 11 with gradient
meshes, 7 with opacity masks (two of them with art kept in another format as
well, one of those with symbols too), 3 with art kept in another format, 1
with symbols, 10 for their type, 2 whose artboard and page counts differ, 2
with an image that can't be read, 1 that can't be read.


### Where type lies on the canvas (2026-10-11)

Two of the files that drew differently seemed to have lost letters: an eye
chart's, and STOP on a sign. The letters were there, as type, 74 points from
where they belonged in one file and 40 in the other.

A story's place is written in the coordinates of Illustrator's canvas, 16383
points square with y down, and has to be carried into art space. The import
took the middle of the canvas (8191.5, 8191.5) to be the document's centre
(`%AI3_TemplateBox`). That holds while the artboard sits in the middle of the
canvas. Once it has been moved, or resized from one side, it doesn't.

The text section says where the art's 0,0 is on the canvas, in an entry of
its own between the two text documents:

```
/AI11TextDocument : /ASCII85Decode ,
%…~>
7579 8587 /RulerOrigin ,
;
/AI11UndoFreeTextDocument : /ASCII85Decode ,
```

Art x is canvas x less the first number; art y is the second number less
canvas y. In 764 of the 878 corpus files with records the entry agrees with
the centre to a hundredth of a point (a 2025 file: centre 306.5, -396.5, entry
7885 7795). In 114 it differs by a point or more. Most of those have no type
that shows, which is why only a few were caught.

`read_text_document` now places stories by the entry, and by the centre only
when the section has none.

**Results.** The 114 files, those still differing and the trial set, 162 in
all, run again: the eye chart's file is native at 0.0%, the sign's file went
from 2.7% to 2.1% (its letters are in place; other things differ), 8 native
files are closer to their pages, 121 are the same, none is further and none is
lost. **821 of 879** by count, 21 still drawing differently.

### Inherited character styles and clipped vector opacity masks (2026-10-11)

This records the first mask subset. The later
[unclipped, gradient and nested mask work](#unclipped-gradient-and-nested-opacity-masks-2026-10-14)
expands it; the refusal list below describes the initial implementation.

A story can omit its character-run list when every character uses the
document's default style. This is valid Illustrator data, previously rejected
as an incomplete dictionary. The reader now inherits that style for the whole
story, counting UTF-16 units including surrogate pairs and end markers. An
explicitly present but malformed or empty list is still refused. Tabs are now
reported as unsupported tab stops, separately from malformed line data.

Opacity masks have a first, bounded implementation. An active `AI9_Mask` on
an artwork dictionary is read when it is clipped, non-inverted and linked
(or has no Linked flag), and consists of solid neutral vector fills. Gray,
neutral RGB and K-only CMYK are coverage values. Their paths, compounds,
ordinary groups, clipping and opacity become editable SVG mask artwork. Empty
active masks hide their objects; disabled masks do nothing. The SVG mask uses
document coordinates and its own wrapper so a text or image transform is not
applied twice. Its region covers the mask artwork, including off-page art.

Mask paint uses an embedded sRGB space. In a print document an untagged RGB
grey can otherwise be converted to the document's CMYK profile and change
coverage: 50% grey must remain 50%, and 100K must hide the object completely.
This does not alter the colour policy of the artwork being masked.

Other forms still cause PDF fallback: non-clipped or inverted masks, unlinked
masks, raster/gradient/coloured/stroked mask art, text/symbol/foreign artwork
inside a mask, nested masks, unsupported blend/knockout, masks on compound
components or clipping paths, and masks on a styled source whose appearance
replaces it. Mask dictionaries are inspected while opened, since the drawing
operators can consume dictionary values before their outer dictionary closes.
Unsupported contents must not silently turn into an empty mask. The normal
visual gate and its 1% lightness / 2% colour limits are unchanged.

Synthetic checks cover inherited styles versus explicit runs, supplementary
Unicode, malformed lists, a text edit after SVG reopen, retained mask artwork
and state, rejection of unsupported masks, an editable mask after SVG reopen,
and mask rendering against independently authored PDF artwork (white, grey,
K-only, black and empty masks, plus a scaled image with and without a mask).
The final focused run passes 52 reader and
80 importer tests, with one opt-in corpus case skipped in each suite. These
save/edit checks concern SVG; they do not establish an Illustrator native-mask
write/read round trip.

### Named paragraph-style resolution (2026-10-13)

A paragraph run's `/0 /0` can be a numeric reference instead of an inline
definition. Its resource is the text document's `/0 /6 /0` array; each entry's
`/0` holds a definition, `/5` its sparse paragraph properties, and optional
`/6` the parent style index. Resolution follows the parent chain, then the
main dictionary's `/3` paragraph defaults. Invalid indices, missing resources,
cycles and chains beyond 64 definitions refuse the story. A per-story work
budget bounds both inherited layers and resolved character runs.

Paragraph property `/32` carries character defaults. Explicit character-run
properties win over the nearest paragraph definition, its ancestors and the
document paragraph defaults, with main `/2` character defaults last. Omitted
character runs inherit that chain. A run crossing paragraphs is split at their
UTF-16 boundaries; cuts through surrogate pairs, malformed lists and unsupported
inherited features still refuse reconstruction. Recorded line origins and
existing alignment/direction and character-scale restrictions remain in force.
The typed and appearance-drawn text documents resolve their own resources.

Accepted text remains editable SVG text, with inherited font, paint and run
formatting. Named paragraph-style definitions and links are expanded inline;
the importer reports this loss and the workspace classifies it as partial
native reconstruction. An SVG save/edit/reopen check verifies editable content
and inherited CMYK paint. This does not implement named-style editing or a
native Illustrator style write/read round trip.

All six new synthetic cases pass, together with the existing native suites:
86 importer and 52 reader cases, with one opt-in corpus case skipped in each.
The six-file text/spot/image smoke set keeps its native acceptances. Three of
the four affected development files now import natively; the fourth gets past
paragraph parsing but retains PDF fallback for a 22.0% lightness / 21.3% colour
disagreement on its first artboard. The 1% lightness / 2% colour limits are
unchanged; no unseen holdout is claimed for these already examined files.

The complete 879-file development regression rises from 821 to 824 native
acceptances, with 55 PDF fallbacks, zero lost acceptances, errors or changed
source hashes. Of the native results, 439 are partial reconstruction and 385
remain unverified for complete editability. The new loss report moves 22
previously native files into the partial category; these retain acceptance.
The local evidence is `artifacts/ai-native-workbench/round11/corpus-final/` in
the workspace. Artwork, paths and generated reports remain outside Git.

### What a plug-in made, clipping paths, compound paths and a group's own state (2026-10-13)

Twenty-three corpus files still drew differently from their pages. Each of the
first few was traced from its picture to its records, the rule found there was
counted over all 879 files, and then it was changed. Seven rules came of it.
Three of them VectorCraft's agents had found in the two days before
(`0d0ea352`, read as source; see the workspace's
`audit-results/vectorcraft-delta-2026-10-13.md`). They have no corpus of real
files, so each was checked against ours first.

**An object a plug-in keeps live** is written as two groups and a marker:

```
%_u … %_U            what it was made from, always on %_ lines
%_9 () XW
u … U                what the plug-in drew of it
9 () XW
(Adobe ArtOnPath Brush Tool) 1 0 34 XP
%0403020100…         34 bytes of the plug-in's own, on % lines
```

Only the second group is art: Illustrator draws and prints that, and so does
the file's page. The reader kept both, and the importer drew both. The path a
brush follows lay over its stroke as a line of its own, and the shapes of a
compound shape lay over what they add up to in their own colours: a dog's body
blue where the shape is white, a black picture on a wall under a pale box. An
object with an appearance is on `%_` lines altogether, marker included, after
its drawn look; the paint its style gives it follows the marker, and then
`1 () XW`, which settles it like any object with a look.

| Plug-in | What it is | In the corpus |
|---|---|---|
| `Pathfinder Suite` | Compound shape | 1,058 objects in about 80 files |
| `Adobe ArtOnPath Brush Tool`, `Adobe PatternOnPath Brush Tool` | Art and pattern brush strokes | 2,854 in about 290 files |
| `Adobe Calligraphic Brush Tool` | Calligraphic brush strokes | 1,882 in 48 files |
| `Adobe Path Blends` | Blend | 214 in 16 files |

Where such an object is itself the drawn look of another, the reader dropped
the first group already. Drawn twice were 1,051 compound shapes in 79 files,
947 brush strokes in 40 and 76 blends in 10.

What the plug-in drew comes in as a group marked `proof:plugin` with the
plug-in's name. What it was made from isn't kept, and the document says how
many such objects it has. A plug-in that wasn't met in a file is left as it
was read. The transparency written after the marker is the object's own: a
blend is a knockout group there.

**A mask's art is written like the art around it**: only what differs from the
object before is set again. A mask whose first shape is white, after a white
object, sets no colour. The reader began every dictionary with a fresh state,
whose fill is black, and a black mask hides what it masks: one panel of four
was missing from a file. The state now carries on into a dictionary, and is
put back when the dictionary ends. So, since 2026-10-22, is where the object
before it stood: the `9 () XW` that ends a mask's art no longer makes a
group's `Xy` after its mask dictionary nobody's, and a name dictionary no
longer loses a group's `0 0 Xd` after it (PROOF-ai-interchange.md, "Opacity
masks written").

**A clipping path in a group of its own.** The path that clips a clip group
can be written after what it clips, in a group that holds nothing else: a
compound shape that clips, or a page-sized background that also clips. Left in
its group it clipped nothing and painted over everything. It now clips the
whole group. Ten files, 131 groups.

**A clipping path that is painted.** Its fill lies under what it clips; the
importer did that already. Its stroke lies over all of it, whole, and that it
did not: the stroke came in under the art and cut to half its width. A
clipping path with both is written three times in a row, to be filled
(`W f`), to clip (`W n`) and to be stroked (`W s`); the three are one path
now, where they were joined into a clipping path with no paint at all. In the
document the clipped art goes in a group of its own, with the stroked path
after it.

**A compound path is painted as its last path.** A gradient is written around
the last path of a compound path alone:

```
*u
0 g  … f             black, for readers that know no gradients
0 g  … f
…  Bb  1 (Gradient) … Bg  … Xm  f  0 BB
*U
```

1,279 compound paths in 222 files are written so, and none with the gradient
around the first path. The reader took the first path's paint: black. Without
a gradient every path of a compound path is painted alike, so nothing else
changes.

**What is set after a compound path ends is the next object's.** A compound
path has the transparency of its paths, set inside it, and no state of its own
after `*U`. An `Xy` after `*U` was given to the compound path, which made a
shape set to Overlay at 20% opaque and normal: a row of buildings in flat
orange.

**A group's own state** is written after it closes and ended by `Xd` and
`6 () XW`. An `Xy` comes before those only when the group's transparency
differs from what the last object was written with. 1,246 groups in 371 files
are written with an explicit normal `Xy` there, so the state does carry over
where none is written. A group whose last member is at 29% in Multiply, and
which is at 29% in Multiply itself, sets nothing, and came in opaque.

**A gradient noted with an object's state.** `Bb`, `Bg`, `BB` with no path
between them, after a group's `Xd`, is a note of the group's gradient. The
reader painted the next path with it, at a placement that meant nothing: a
rail in cream where the page has it grey.

**Results.** All 879 files, one worker: **841 native, 38 on their pages**
(824 and 55 before), seventeen gained and none lost. Of the 841, 492 are
partial and 349 unverified for complete editability. 46 files that already
passed moved closer to their pages and three read further by the numbers; all
three were looked at and are closer in the picture (which of the check's two
weighings counted changed, see "Two ways of weighing"). The reader's suite has
57 tests and the importer's 88, one corpus case skipped in each.

**What is left of the files that draw differently**: six. Five have something
with a blend mode inside a clipping group, and it is the display that differs
there, not the reader: PROOF's canvas blends such a thing with nothing, where
Illustrator blends it with what lies under the group. The sixth is a hair off
at every outline, with an artboard that is only guessed at.

**Not kept, and said so in the document's notes.** The paths a compound shape,
a blend or a brush stroke was made from. The stroke comes in as the outline
the brush drew, not as a path with a brush.

### Blend modes in clipping groups, and several clipping paths in one (2026-10-13, later)

Five of the six files left had something with a blend mode inside a clipping
group. Their records were read as written. It was the canvas that drew them
differently, and the exported PDF did the same for any group or layer: a shape
set to Multiply met nothing on its own surface and came out flat. That is the
editor's fault and is mended there: see
[blend modes under a group](PROOF-clip-blending.md).

**Several clipping paths in one clip group clip together.** Two posters kept a
grey field where the page has a dark green one. Each picture in them lies in
four clip groups, one in the other, with the clipping path after the picture,
and the outermost has three clipping objects of its own:

```
q q q q
%AI5_BeginRaster … %AI5_EndRaster  N
… W n  Q
… W n  Q
… W n  Q
… W n                a path
*u … W N … W N … W N … W N *U     a compound path of four pieces
… W n                a wider path
Q
```

The reader joined the three into one clipping path, which lets through what
any of them does: here the widest, and the picture spread over the field. What
shows is where all of them let it through, as with one clip set after another
on a page. 84 clip groups in 8 files have more than one clipping object, and in
none of them are two of the objects apart from each other, which they would be
if they were meant to add up. A compound path of clipping paths is still one
clipping path of several pieces.

The first clips the group. Each of the others clips a group of its own inside
the one before (`Node::clip_chain`), so the document has one clipped group in
another, as it would for clip groups written that way.

**Results.** All 879 files, one worker: **846 native, 33 on
their pages** (841 and 38 after the round before), 5 gained and
none lost. 497 are partial, 349 unverified for complete
editability. One file still draws differently from its page: a hair off at
every outline, with an artboard that is only guessed at. The reader's suite has
57 tests, the importer's 88, and `test_clip-blend` 5.

### Gradient meshes: bounded native reading (2026-10-14)

`/Mesh X!` opens a mesh and `/End X!` closes it. Entries end with `X#`:
`2 /Version`, `/Cartesian /Type`, `[rows columns] /Size`, `/Data`, then each
patch's `[row column] /P`, grid parameter `/R`, colour space `/CS`, boundary
nodes and `/E`. The colour space is DeviceGray when omitted, or DeviceRGB or
DeviceCMYK. The grid must be complete, with unique, in-range patch positions.

Each `/N` corner holds colour components, opacity, its position, the handle
towards the next corner and its weight, then the handle towards the previous
corner and its weight. Four corners with unit weights become an editable SVG
mesh gradient per patch, preserving the cubic sides, colour channels and
opacity. Patches are kept in the order the file writes them. The source layout
reference is VectorCraft's `ai/mesh.rs` at `0d0ea352`, under its MIT licence
(`LICENSES/MIT-VectorCraft.txt`); its code and dependencies were not run.

**Boundary nodes cannot be dropped.** `/S` and `/A` add positions and two
handles between corners. Their handles are written previous first, then next,
the opposite of `/N`. Across 879 files, 11 have meshes: 585 meshes including
envelope controls, 798 patches, all version 2 Cartesian and all corner weights
1. Of those patches, 592 have extra boundary nodes. The donor ignores these
nodes, changing the outline. PROOF keeps every cubic segment. When all four
corner colours and opacities are equal, that exact outline can be painted
uniformly: it becomes an editable path marked `proof:mesh-outline`, and the
import notes explicitly say mesh editing was not retained. It counts as a
partial native import. Varying-colour or varying-opacity patches with extra
boundary nodes still fall back to PDF; their interior interpolation is not
guessed.

Unknown entries, unsupported spaces or weights, missing patches, incomplete
meshes, nonnumeric or non-finite data, and excessive dimensions or boundary
data fail closed. Limits are checked before patch allocation. Meshes cannot
be flattened into compound paths. An envelope's non-art control mesh is still
discarded in favour of what the envelope drew, before unsupported artwork
meshes are rejected.

Regression fixtures check the boundary-node handle order, malformed records,
grid completeness and limits, exact CMYK channels, editable corners and
handles after saving and reopening, an independently encoded PDF Type 6 Coons
shading, and refusal when the native outline and PDF page disagree. Visual
thresholds and per-artboard fallback are unchanged.

**Milestone result:** 854/879 native, 25 PDF fallback; eight gained and
none lost from the preceding 846/33 result. 505 imports are explicitly
partial, 349 pass the appearance check without complete-editability
certification. Three mesh files still require nonuniform interpolation
with extra boundary nodes. All nine unit suites and both export smoke
tests passed; one full corpus regression was run for this milestone.

### Embedded foreign PDF vectors (2026-10-14)

A `/ForeignObject` dictionary keeps a PDF that Illustrator placed or pasted,
with `Version`, `RTransform`, `Origin`, `Bounds` and ASCII85 `Data` entries.
The census found 30 objects in five development files. All use version 1 and
one embedded PDF page. Four files contain axial CMYK shadings with sampled
functions; one also contains symbols. The fifth contains tensor mesh shading
with overprint and remains outside this first subset.

The decoder requires the `~>` terminator, checks partial groups and overflow,
and strips exactly one `%` at a data line's start. A following `_` or `%` is
data. Total decoded foreign PDFs are limited to 64 MiB per document. Unknown
or duplicate record keys, invalid dimensions/transforms and foreign artwork
inside a compound path or another dictionary are refused.

The layout reference is VectorCraft `ai/read.rs::foreign_art` at `0d0ea352`
(MIT, credited in `LICENSES/MIT-VectorCraft.txt`), read as source only. PROOF
keeps the full affine rather than fitting painted bounding boxes. For a PDF
point relative to its page's low corner, first add the low corner of `Bounds`,
then apply the canvas origin/y flip, then `RTransform`, then the document
transform. Blank page margins, stroke extents, rotation and shear survive.
The PDF page is clipped at its own bounds; a nonzero page origin is normalized
without moving its painted content.

The embedded page is imported through PROOF's existing vector PDF parser.
This first subset accepts paths, strokes, clipping, device Gray/RGB/CMYK
paint, axial/radial shadings in supported process/ICC spaces, Normal/Multiply
and fill/stroke opacity. It checks the content operators and used graphics
states before importing: text, images/forms, patterns, mesh/function shadings,
overprint, soft masks, transfer functions, page transparency groups,
annotations, rotated/cropped or multiple pages and unknown operators,
including inside `BX`/`EX`, retain PDF fallback. Hidden unsupported objects
are omitted with a note. Tiny unsupported visible objects cannot evade this
structural refusal through the artboard's visual tolerance.

Each imported object is an editable SVG group marked `proof:ai-foreign="pdf"`.
Its original PDF is retained in `proof:foreign-pdf` as a data URI. Editing the
SVG paths/gradient stops does **not** update that saved source PDF. The import
notes state this boundary, and these imports count as partial reconstruction;
no native Illustrator writer or foreign-object write/read round trip is
claimed. SVG resource IDs remain independent between imported objects.

The print checks also found a general PDF-import paint bug: the internal Gray
editing space has no CSS serialization, so unconverted grey strokes disappeared
and grey fills inherited other paint. Unprofiled PDF grey now becomes K-only
CMYK; a carried greyscale ICC profile retains its own space.

Synthetic regressions cover exact decoded bytes and limits, full affine
placement against independent PDF content, blank margins and clipping,
nonzero page origins, separate gradients/resources, retained original PDF
bytes, CMYK/black-channel paint, hidden and tiny unsupported artwork, and SVG
save/edit/reopen. `ai-foreign-pdf-smoke.py` consumes fixtures generated with
`PROOF_AI_FOREIGN_DIAG`; `--case` selects one while implementing. Its canvas
and PDF checks also require vector output and exact CMYK fill/stroke channels.

**Milestone result:** 861/879 native, 18 PDF fallback; three gained and none
lost from 858/21. 511 imports are explicitly partial and 350 remain unverified
for full editability. All nine unit suites and four export smokes pass,
including 44/44 foreign-PDF checks; one full corpus regression was run.

**Mesh investigation:** extra boundary anchors in the three remaining native
meshes still have no established interior interpolation here. The pinned
donor drops those anchors, and the
[SVG working-group discussion](https://www.w3.org/Graphics/SVG/WG/wiki/SVG2GradientsComments)
describes why Illustrator colour smoothing cannot generally be reproduced
by one bilinear Coons patch. Neither is a specification for these native
records; keep the fallback until a synthetic reference establishes the rule.

### Unclipped, gradient and nested opacity masks (2026-10-14)

The next mask census found sixteen documents with mask dictionaries, including
four whose only remaining blocker was an active, linked, non-inverted,
unclipped greyscale mask. Their masks use greyscale gradients, Normal or
Multiply artwork, and empty nested masks. A nested empty mask with Clip off
is opaque white, so it leaves its target visible. With Clip on it hides it.
[Illustrator's transparency reference](https://helpx.adobe.com/illustrator/using/transparency-blending-modes-alt.html)
describes the white backdrop when Clip is off and the black backdrop when it
is on. Grey is coverage, independent of the document's printing profile.

The reader now retains the clipping flag and accepts the bounded non-inverted
greyscale vector subset: filled or stroked paths, compound paths, groups,
linear/radial gradients, nested masks, and Normal or Multiply. Every gradient
stop must be Gray, neutral RGB or K-only CMYK, without named inks. Flags are
validated before attachment. An ArtDictionary inside a mask attaches to its
preceding artwork before it is stored as metadata; otherwise a nested mask
could disappear when the dictionary ends. Drawing state still inherits into
the dictionary and restores on exit, including an inherited object's opacity.

The importer builds editable SVG luminance masks. Unclipped masks have an
explicit white backdrop; its region includes the target's painted bounds,
strokes and off-artboard artwork, with a small guard beyond the painted edge.
Mask colours use embedded sRGB, and gradient vectors are cached separately
for ordinary colour and mask coverage. A gradient used in both contexts keeps
its original CMYK channels outside the mask. Nested construction restores the
previous coverage-colour context rather than unconditionally leaving it.

**Editing limit:** the mask region and white backdrop are finite, sized at
import time. Moving or transforming the wrapper carries them with the art;
later anchor or stroke edits that expand beyond the initial region can need
the region/backdrop enlarged. Save/reopen and mask colour edits are checked;
this is not a certification of every subsequent editing operation.

Inverted or unlinked masks, coloured/named-ink paints, raster, pattern, mesh,
text, symbol or foreign artwork in a mask, other blend modes and knockout
remain explicit refusals. Masks on compound components or clipping paths,
and unsupported masks in replaced styled source artwork, still fail closed.
No PDF comparison thresholds or per-artboard fallback rules changed.

The print checks also exposed two PDF exporter faults: ICC colour objects
could abort gradient-function serialization, and a mask nested directly in
mask artwork was omitted. Shadings now pass the RGB/CMYK device components
expected by CapyPDF, preserving channel values; ICC-tagged shading output is
still a separate limitation. Nested mask artwork receives its own soft mask,
and luminosity mask groups explicitly use RGB for blending so translucent
white contributes its alpha to coverage.

Regressions cover flag/attachment refusal, nested inherited/reset opacity,
white-backdrop compositing, Multiply through groups, strokes, off-artboard
extent, shared ordinary/mask gradients, mask colour edits and SVG reopen.
Independent PDF shadings check clipped and unclipped coverage. The
[installed opacity-mask export check](../testfiles/cli_tests/ai-opacity-mask-smoke.py)
uses importer-generated synthetic SVGs: canvas and PDF pixels, vector-only
output and exact ordinary CMYK gradient channels. No private artwork is
included in those fixtures.

**Milestone result:** 858/879 native, 21 PDF fallback; four gained and none
lost from 854/25. 508 imports are explicitly partial and 350 are not certified
for complete editability. All sixteen mask-bearing files were checked during
implementation; the four mask-only fallbacks gained native import and the
other twelve retained their previous acceptance/fallback. All nine unit
suites passed, with affected suites rerun after the PDF fixes. Installed
export checks passed: opacity masks 59/59, `.ai` 19/19 and blends 32/32. One
full corpus regression ran at the stable milestone, with no errors/timeouts,
source hash differences or native losses. No previously accepted native file
moved by more than 0.2 percentage points in either visual measure.

### PostScript files with no layers and no page (2026-10-16)

The collaborator's 1,395 files had 38 that fell back with "its own data has
no layers" (none among the owner's 879). Every one was written by Photoshop's
"Export Paths to Illustrator" (`%%Creator: Adobe Photoshop(TM) Pen Path
Export 7.0`):

- Illustrator 3 PostScript (`Adobe_IllustratorA_AI3` procset), no `%PDF-`
  head, so no PDF part and no page; every line ends in a bare CR.
- No `%AI5_BeginLayer`: the art follows `%%EndSetup` directly.
- Each path component is a compound path of its own, closed and unpainted:
  `%AI3_Note:<< /operation /xor /defaultFill false >>`, `n XR`, `m`, curves,
  `n`, `*U`, inside `%Adobe_Photoshop_Path_Begin` and `_End` comments. The
  note carries Photoshop's path operation; with no paint it draws nothing.
- The setup line `0 0 0 1 k 0 i 0 J 0 j 1 w 4 M []0 d` uses `i`, flatness,
  which no record of the 879 has.

Until now PROOF couldn't open such a file at all: the native path refused it,
and the fallback handed PostScript to Poppler. Now:

| Rule | Where |
|---|---|
| Records with no `%AI5_BeginLayer` are one layer, "Layer 1", opened at `%%EndSetup` and closed at the end, as Illustrator opens an Illustrator 3 file. Records with layers still refuse art outside them | `Reader::comment`, `Reader::run` |
| `i` (flatness) is passed over: it says how finely a printer splits curves, and draws nothing | `IGNORED` |
| A file without a `%PDF-` head opens from its records with no page and no weighing, and the note "it is a PostScript file of Illustrator 8 or older, with no PDF page: its art wasn't checked against a page". A PDF-based file whose records have no layers still falls back | `open_checked`, `pdf_file` |

This applies to every PostScript `.ai`, the Illustrator 5 to 8 ones with
layers too, which used to fail with "its PDF part can't be read". The pack had
one: an Illustrator 8 file exported by CorelDRAW X8 (`%%Creator: Adobe
Illustrator (TM) 8.000000 Exported from CorelDRAW X8`), one layer holding one
compound path of 62 subpaths filled `0 0 0 1 k` with `1 XR`. It came in as one
path of 62 subpaths, the CMYK black kept (`icc-color(…, 0, 0, 0, 1)`),
even-odd, no stroke, its points' extent equal to the source's to 0.0000 pt.

**Checked without a page.** For all 38, an independent count of the source
(Python, not PROOF's reader) against the imported SVG: subpaths and segments
equal in every file (155,849 paths, 0 differing), the extent of every point,
control points included, equal to the source's to 0.000 pt in document space,
and every path `fill:none;stroke:none`, as Illustrator has them. One file has
no paths and opens as an empty layer on its artboard. Tests:
`AiNativeReader.RecordsWithoutLayersAreOneLayer`,
`AiNativeImportTest.APostScriptFileWithoutLayersOpensFromItsRecordsUnchecked`.

**A warning when it opens** (owner, 2026-10-16: "I think for ancient files
that open unchecked, a warning is good enough. Illustrator 2025 also actually
warns you about them!"). The import marks the root `proof:unchecked=
"postscript"` (`AI_UNCHECKED_ATTRIBUTE`); when such an `.ai` opens in a window,
a warning comes before the missing links and fonts: "This file is in the
format of Illustrator 8 or older. It has no PDF copy of its pages, so PROOF
couldn't check that its artwork came in as it was drawn. Look it over before
you use it for print." (`ui/dialog/font-substitution.cpp`, `showUnchecked`).
The wording is PROOF's own.

**The pack, files up to 500 MB** (150 of 153): 39 open unchecked (the 38
and the CorelDRAW file), 104 fall back for the reasons they did on the
collaborator's machine, and 7 that depend on links open as before (their
links aren't beside the pack's copies); no errors, timeouts or changed
sources. All nine unit suites and the four export checks pass.

### Linked CMYK TIFFs, and a page older than its links (2026-10-16)

A file whose links are CMYK TIFFs with transparency fell back: PROOF's screen
drew them on white boxes, paler, and so the import couldn't look like the
page. PROOF now reads such TIFFs itself, in the document's CMYK (Preserve
Numbers), and prints them right; see
[PROOF-cmyk-pictures.md](PROOF-cmyk-pictures.md). The collaborator's file
went from 33.4% different to 0.00%. The import names a linked CMYK TIFF whose
own profile differs from the document's.

**A page can be older than its links**: a photograph updated in Photoshop
after the `.ai` was last saved shows as it is now in Illustrator, while the
file's page keeps it as it was, and weighing the import against that page
refuses a right import. The records hold no date of a link (the placed record
is its matrix, four numbers and its path; the XMP manifest has
`stRef:filePath` only). They hold the picture Illustrator made of each link
when saving (`%AI26_BeginPlacedObjectPreview`, up to 512 pixels, straight
alpha). Since 2026-10-16 a found link that no longer looks like it is left out
of the weighing, as a missing link is, and the notes say so: "1 linked picture
looks different from what the document kept of it when it was saved (name):
the page shows it as before, so where it lies wasn't compared with the page"
(`link_changed` in `pdfinput/ai-native-open.cpp`). Both on white, the file at
the kept picture's size (a 4 x 4 grid of samples a pixel), each pixel allowed
to match a neighbour; changed when more than 15% of it differs by more than
48, or its shape by more than 10%. On the collaborator's 70 links with kept
pictures: unchanged ones differ in at most 6.4%, shape at most 3.3%; one
replaced after saving (in a file refused for other reasons) in 25-31%, shape
24%. The four numbers of the placed record are not always the file's pixel
size: equal in one file, a tenth of it in another whose pictures were
exported again larger with the same content, so they aren't used.
Test: `ALinkedPictureThatChangedSinceTheSaveIsLeftOutOfTheComparison`.

**Correction, the same day.** This section first said that the file whose
Illustrator screenshots showed PROOF right ("Some of the linked objects have
been updated") had a page older than its links. It hasn't: its 13 links match
the pictures kept of them. **Its PDF part is broken**: page 2 draws its
pictures off the page (at y 405-412 and 525-731 on a page 319 high, one at x
836-1042), each frame inside a knockout group whose clip lies where the frame
belongs, so nothing of it shows; page 1 is shifted likewise. Illustrator wrote
a page that doesn't match its own art. The import is right; what serves that
file is opening the editable version anyway (the owner's decision of
2026-10-14), not the link check. Illustrator's message can come from file
dates alone, which copying changes.

### Opening the editable version anyway, and a crash after the comparison (2026-10-16)

The owner, 2026-10-14, on a file whose own art draws differently from its
page: "Yes, perhaps we should offer this option, but we should obviously
strive to have native everything at some point (not a rush obviously)".

- When the records read in full and only the drawing differs, the PDF import
  dialog that comes up for the page says so, how far, and offers the art:
  "**Its own art doesn't match its page.** PROOF read this Illustrator file's
  own art in full, but on artboard 1, about 27% of it looks different from the
  PDF page saved with it: part of the art may be misread, or the page out of
  date or damaged. OK imports the page shown here. Open Editable Version opens
  the art as PROOF read it, with its layers, type and colours as Illustrator
  saved them." The button sits apart from Cancel and OK, which work as before.
- The share is the larger of the two measures, lightness and colour, on the
  first artboard found to differ; the artboards after it aren't drawn
  (`AiKeepDiffering::First`), so asking costs no more time than refusing did.
- Only that refusal is offered. Records that hold something not read, or a
  page that can't be weighed against (turned, another size, another count of
  artboards), open from the page as before: nothing says how far they differ.
- From the command line nothing is asked: the page, as before.
- `cursed.ai` (`2c1e4b6cdb`), whose PDF part is broken while its art is right:
  27% on artboard 1 (21.9% by lightness, 26.6% by colour).
- Code: `open_ai_native(..., AiKeepDiffering)` and `AiNativeOpen::differing`
  in `pdfinput/ai-native-open.*`; `PdfImportDialog::offerEditable` and
  `PdfInput::open` in `pdfinput/pdf-input.cpp`. Test:
  `ArtThatDrawsDifferentlyCanBeOfferedWithHowFar`.

**A crash after the comparison, fixed.** Poppler's glib side, which draws the
page for the comparison, makes Poppler's settings (`globalParams`) for the
first document it opens and drops them when its last one closes
(`GlobalParamsIniter` in Poppler 26.08). After every comparison the settings
were gone, and the page import that follows a refusal read the page's first
font without them: an access violation in `GlobalParams::mapNameToUnicodeText`,
in the editor and from the command line. So every `.ai` that fell back after
its page was drawn crashed PROOF: those whose art draws differently, and those
whose page is turned, of another size or another count of artboards. A file
refused before the drawing (records not read) didn't. The workbench never saw
it, as it calls the reader directly and not the editor's open path. Found when
a copy of `cursed.ai` crashed the editor; round 19's build crashes on it from
the command line (`C0000005`), this one imports its page. `open_ai_native`
makes the settings again on its way out. Tests:
`PopplersSettingsOutliveTheCheckAgainstThePage`; the `.ai` smoke opens two
files with records from the command line, one that matches its page and one
that differs from it with type on the page (round 19's build fails the
second).

### Linked Photoshop files (2026-10-17)

A placed `.psd` or `.psb` shows its merged image, read by PROOF
(`display/psd-pixels.h`; `PROOF-cmyk-pictures.md`, "Photoshop files"). At
import, `Builder` reads a linked Photoshop file's header and resources: one
whose merged image can't be read refuses the file with the reason, "a linked
Photoshop file can't be shown: it was saved without its merged image
(Photoshop's Maximize Compatibility) (name)". Before, every linked Photoshop
file refused it ("a linked file is of a kind PROOF can't show yet"), as
gdk-pixbuf has no Photoshop reader. The profile note of linked CMYK TIFFs
covers Photoshop files too.

The collaborator's two files refused for it, with their links (copies):
`f16f351946` (three Photoshop files) is native at 0.01%; `e95eaa172c` (two
among 20 links) is native at 0.08%, its one link replaced after the save
(`Vector 3506.png`) left out of the comparison and named, as round 19 meant.
Test: `ALinkedPhotoshopFileShowsItsMergedImage`.

### Linked `.ai` and `.pdf` files (2026-10-17)

A placed PDF or `.ai` is a page, not a picture: Illustrator shows a picture of
it and prints the page itself. PROOF now does both
(`PROOF-placed-pages.md`). The reader keeps the placement's page and crop
(`Placed::page` from `AI11 PlacedPDFPageIndex`, `Placed::crop_to` from
`AI11 PlacedPDFCropTo`, in the art dictionary after `%AI5_EndPlace`); the
placed box (always with its top-left at 7893, 7770 in the four placements
read) has the size of the page box it is cropped to. The import shows the
selected page cropped to its crop box (page 1 was the initial limit, removed
on 2026-10-22). The selection is retained as `proof:pdf-page` on the image
through SVG save/reopen and PDF output; unavailable or invalid pages still
fail closed. Other crop boxes and rotated pages remain unsupported.

The collaborator's `9047eb9b4e`, refused for its linked `.ai` before, opens
natively at 0.22% in the original round. Its slow page check was addressed
on 2026-10-22: first opening 137 s, later ones 33.5 s; see
"Pages slow to draw: weighed once and kept" below. Test:
`ALinkedPdfPageShowsTheSelectedPageCroppedToItsCropBox`.

### Arrays nested too deeply (2026-10-17)

The reader builds an array at its `]` without recursion, but a chain of arrays
inside arrays is freed by recursion, and nothing bounded how deep they nest
but the million values the stack may hold. Half a million `[` then as many `]`
overflowed the stack: PROOF crashed on opening such a file. Found by reading
VectorCraft's changes (their nesting caps; workspace
`audit-results/vectorcraft-delta-2026-10-17.md`).

Open arrays are now limited like nested objects (`Limits::max_depth`, 256),
and such a file is refused: "its arrays are nested too deeply". Illustrator's
records nest arrays three or four deep. A mark an operator has cut from the
stack isn't counted, by the same test `]` already applied, so files that read
before read the same. Test: `ArraysNestedTooDeeplyAreRefused`, which crashed
the suite before the cap.

### Gradient meshes whose edges bend (2026-10-17)

A mesh patch's edge can bend through anchor points (Illustrator's square points
on a mesh line, which take no colour), written `/A` or `/S` between the
patch's corners (`/N`). PROOF drew such a patch only when its colour was
uniform, as its outline, and joined the anchors in file order. Fifteen control
meshes saved by Illustrator 29.2.1 for this (the owner's Codex; corner inks C,
M, Y and K, one feature each: an anchor on each edge, two on one, one slid
along an edge, one turned into a corner, a deleted line, a star) showed how
they are written and drawn. Their PDF parts, where Illustrator writes each
mesh as finer tensor patches, gave the drawing:

- **Order.** Corners run from the lower left (y up) with the patch's turn. An
  edge's anchors follow the corner it starts from; the last edge's come before
  the first corner. They are listed along the mesh's grid, the first handle
  towards the anchor listed before: with the turn on edges 0 and 1, against it
  on edges 2 and 3. File order drew those two edges wrongly (a zigzag, or a
  lone anchor's handles swapped). Every run of two or more anchors in the
  owner's and the collaborator's mesh files fits.
- **`/S` and `/A`** are drawn alike. `/S` is left where a mesh point was (a
  deleted line's points; a Mesh tool click into a star), `/A` is added. The
  last number is 1 for a smooth point and 0 for a corner.
- **The inside** is the Coons patch of the four edges, its colour the four
  corners' blended by the patch's parameters (within 2/255 of Illustrator's).
- **Along a bent edge** each cubic takes a share of the parameter as long as
  its control polygon: an anchor slid to 80% of a straight edge, its handles
  kept, sits at 63% (79.875 pt of 126.42), and its colour is 63%.
- **Colour spaces**: Illustrator 29 writes pure process inks as per-point
  `[ [c m y k /DeviceCMYK (Cyan) /Separation] ... n /NChannel ]`, and a named
  process colour as `[r g b /DeviceRGB (name) /Process]` with a tint per point
  (tinted as elsewhere: inks scale, RGB blends towards paper). Both are read;
  spot inks in a mesh are not.

So a patch whose colour changes is imported as a mesh gradient cut into cells
at every anchor (`Builder::mesh_cells`): each cell's sides are exact cubics of
the same Coons surface, so the cells draw it exactly, and each anchor becomes
a mesh point with the colour it had between the corners (a note says so).
Uniform patches stay outlines, now in the right order.

Checks: `MeshAnchorsAreListedAlongTheGrid` (the control's layout, a corner
anchor, the `/Process` tint, a spot ink refused),
`MeshExtraBoundaryNodesOutlineAUniformPatchAndCutAChangingOne`,
`ABentMeshPatchIsCutAtItsAnchorsWithTheSharesIllustratorGives` (read back
through Inkscape's mesh). All 15 controls open natively against their own
pages. The owner's three mesh refusals open (0.37%, 0.37%, and 0.13% for the
243 MB file); the 8 native mesh files stay or come closer to their pages. Of
the collaborator's 12, 8 open; three now fail only for opacity masks, and
`f9e2bddc55` draws 1.4% differently, though the same mesh opens in three
other files at 0.00 to 0.28%.

## Plan

1. **Native reader** (`src/extension/internal/ai/`), delivered. It covers the operator table
   above, is bounded, and fails closed. It produces a structure (layers, objects,
   inks, gradients, images, artboards) for both the importer and round-trip tests
   of the writer.
2. **Native import**, delivered for the checked subset above. It builds the PROOF document from that structure:
   - CMYK colours and spot inks stay exact.
   - Lab alternates are kept.
   - CMYK images keep their samples, and print them since 2026-10-16 ([PROOF-cmyk-pictures.md](PROOF-cmyk-pictures.md)).
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
   wider path options and character scaling (the ordinary path subset is below),
   justified paragraphs, linked frames and the remaining rendering families. (The document's own ICC profile is read since
   2026-10-11.)
4. **Writer** extensions, in this order: gradients, images, text. They use the
   same tables, and the reader checks the writer's output alongside Illustrator.

## Left-aligned paragraph tabs (2026-10-17)

Adobe's [Tabs documentation](https://helpx.adobe.com/illustrator/desktop/measure-and-align/grids-and-guides/align-graphic-objects-with-tabs.html)
defines paragraph stops and left, centre, right and character alignment.
The first implementation reads **left-aligned stops**, including a default
interval, explicit stops, paragraph indents and different rulers in different
paragraphs. Area type retains its recorded lines, as before; this does not
add area-text reflow or a Tabs panel.

**Records.** Small controls saved by Illustrator 29.2.1 with ArialMT
(Arial Regular 7.00, 2,048 units/em) establish the keys: paragraph feature
**31** is the default
interval in points; **30** is a dictionary whose **0** array holds stops.
Each stop's **0** is its position in points, **1** its alignment (0 left,
1 centre, 2 right, 3 character), **2** its leader string, and **3** its
alignment character. Stops inherit through the existing bounded paragraph
style resolver, then the text document's defaults. A missing interval uses
36 points. Empty stop dictionaries mean defaults alone.

The ruler's zero is the layout frame F's anchor, rather than the first
indented glyph. After the last explicit stop, defaults resume on the original
interval grid, not a grid beginning at that explicit stop. The controls and
the ten copied tab-stop cases establish these forms; the owner's 871-file
cached-record census finds exactly one tabbed story, `6724e115c1`. No original
artwork was checked.

**Editable layout.** Each imported line retains its tab characters and
carries `proof:tab-origin`, `proof:tab-interval` and `proof:tab-stops` (a
space-separated list of left-stop positions). Common character scaling is
compensated in the ruler too. The layout engine measures each tab from the
current pen position; it is a selectable character with an advance and no
drawn glyph. Illustrator chooses a stop before the preceding letter's
tracking, then applies its letter-to-space kerning and tracking after the advance. A tab
itself has no tracking. This explains why consecutive tabs after a kerned
letter can reach a different stop from two leading tabs. Text edits keep
the fields aligned. Ruler-attribute changes invalidate layout, and the attributes survive writing and reopening a text
or span. These are PROOF SVG extensions; other SVG readers need not honour
them.

The font preflight excludes a tab from missing-glyph detection: it must not
mark an otherwise installed font as incomplete and remove the story from
the page comparison. The edit/reopen test explicitly checks that the story
is still compared. Ordinary text without these tab attributes keeps its
existing layout behaviour.

**Bounds and refusals.** At most 1,024 finite, strictly increasing nonnegative
explicit stops; finite positive default spacing and a finite origin.
Malformed/unknown active tab settings refuse the import. Right, centre or
character stops, leaders and non-left-aligned or bidirectional paragraphs
still refuse with a specific reason. Existing unsupported frame/segment
rules still apply. The layout extension currently applies to horizontal left-to-right, unwrapped
text; imported area text uses its already recorded individual lines.

**Focused checks.** Six targeted tests cover UTF-16 text, paragraph-style
inheritance, common scales, two area paragraphs, edits, ruler changes,
glyph/character counts, selection rectangles, caret movement, tab-only
stories, safe bounds after attaching type to a path, reopening, kerning and
tracking. The importer has 115 tests including its opt-in corpus skip; the reader has 68 including its
opt-in corpus skip. Thirteen independent synthetic copies: eight supported
forms open natively, five unsupported forms fall back for the expected
reasons, no substituted fonts. Three further independent area/scale controls,
a seven-story leading/consecutive-tab control and four independent tracking
controls test the same positions against Illustrator's PDF and PROOF's own
exported PDF. Exported type remains vector text with embedded fonts and
extractable contents. Final milestone validation is recorded below.

Ten affected copied cases: eight partial-native, two (`72ffd60cdf`,
`fa084545fa`) now parse their tabs but still fall back at the unchanged
appearance gate (about 1.5% lightness on artboard 1). The cause of that
remaining difference is unestablished. Of the eight native files, five
substitute fonts in at least one tabbed story, so that type's original
appearance is not qualified. `e24cb1edf7` has 36 missing linked pictures;
about 48.5% of one artboard is excluded for those links. `5906b294bd` has
seven missing links and one other substituted-font story; its maximum
excluded area is about 82%. `6724e115c1` substitutes fonts in two other text
objects. These exclusions remain unqualified. The counts describe reader
opening and the stated comparison scope, not whole-document appearance or
complete editability certification.

The extra upstream ligature/cursor suite initially fails six of twelve with
system Fontconfig substituting the requested Arabic/CJK fonts. All twelve
pass in an isolated test profile with the repository's rendering-test fonts
and Noto Sans CJK JP at `notofonts/noto-cjk` revision
`f8d157532fbfaeda587e826d4cd5b21a49186f7c`. No system/Illustrator font
installation or corpus font environment was changed.

The eleven standard suites pass (importer 115 and reader 68, one opt-in corpus
skip each); interchange/export checks pass 23, 32, 59, 44, 91 and 43
(`artifacts/focused-suites/20261017-033732`). After the final selection and
bounds fix for characters without glyphs, the complete importer (114 passes, one skip), reader
(67 passes, one skip) and isolated ligature/cursor suite (12 passes) were
rebuilt and rerun. The sixteen native synthetic control documents retain
vector text in PDF and match both their Illustrator PDFs and PROOF canvas:
zero unmatched foreground ink beyond the two-pixel tolerance at 144 dpi.
**Final 871-copy regression:** 860 native (510 partial, 350 unverified),
11 PDF fallbacks; `6724e115c1` gained, none lost, no errors, timeouts or changed
inputs. All 859 earlier native cases keep their classification, notes and
lightness/colour values exactly; the other eleven fallback reasons and
comparison values are unchanged too. All copies were hash-verified first,
with the exclusions ledger respected; five larger files remain outside this
approved under-50-MB subset. One worker, four of sixteen logical CPUs, Below
Normal priority; 870 comparisons executed and one valid comparison reused,
10 min 19 s. Run: `artifacts/ai-native-workbench/runs/20261017-004520-233642`;
authoritative SHA-256 comparison: `artifacts/round23-tabs/owner-comparison-final.json`.
The original 879-file milestone and excluded cases remain separate.
No real mouse/keyboard trial was run; the last live baseline is still 2026-10-14.


## Ordinary type on a path (2026-10-17, round 24)

A single horizontal, left-to-right paragraph with Illustrator's ordinary
baseline-aligned Rainbow path layout imports as editable SVG `textPath`.
Left, centre and right alignment retain their anchors when the text changes.
The full cubic path is retained in the definitions, referenced by the text;
it is not cut to the occupied portion or converted to glyph outlines.

The frame kind is `/2 /0 2`, its outline `/1 /0` and its interval `/2 /6`.
The interval is segment index plus cubic parameter, not a length or a fraction
of the path. Even a straight segment whose handles equal its endpoints keeps
that parameterisation: parameter 0.25 lies 31.25 points along a 200-point
segment. Convert those bounds to distances before simplifying any geometry.
The frame matrix and the text document's canvas/ruler placement still apply.
The path layout's F node has a box rather than a point-type anchor. Its /10
matched the referenced frame index in the controls and the owner's three
path stories; that index must not be treated as a path effect or alignment.

`startOffset` and `text-anchor` carry the start, middle or end anchor. PROOF
also keeps `proof:path-start` and `proof:path-end` as distances in the path's
local coordinates. They keep excess characters hidden while the full text
remains editable. Closed paths do not repeat overflow around the loop. Text
edits, path edits and bracket-attribute edits invalidate layout; the bracket
attributes survive save/reopen and construction of a fresh textPath node.
Malformed bracket metadata hides the text instead of exposing overflow.
The path object remains alive for cursor queries. Hidden glyphs stay out of
outline conversion as well as the canvas and PDF.

**Editing limitation:** Illustrator's original parameter brackets become
fixed distances. After changing the curve, they do not retain their original
segment/parameter positions. This is reported as partial native import.
The active end bracket is a PROOF extension: another SVG reader can ignore it
and show the excess text. PDF export uses PROOF's layout and hides it.

The first subset accepts absent path options or the default values observed
in Illustrator's independent controls: `/11` numeric keys 4 and 18 equal 10,
key 22 equals 0.025, flags 6 and 7 are false. Their meanings beyond these
controls are not established; other values or keys fail closed. No options
are silently discarded. Wrapped intervals, disconnected paths, multiple or
justified paragraphs, tabs, bidirectional text, non-unit character scales,
baseline shifts, superscripts/subscripts and decorations remain unsupported.
Existing character/paragraph inheritance and UTF-16 boundary checks apply.

Adobe's behaviour reference is [Type on a path](https://helpx.adobe.com/illustrator/using/creating-type-path.html).
The donor's `ate.rs` at `0d0ea352` confirms the segment/parameter interval,
but its reader clamps bad bounds and ignores the options dictionary. PROOF
does neither. The donor's `ai/frames.rs` also warns that an object's own Art
path can differ from its text resource's frame after copying. That wider
case is not claimed here; every imported document still passes the existing
independent per-page comparison or falls back to PDF.

**Validation.** Five new tests cover cubic-parameter conversion, malformed and
unsupported input, alignment, path/text edits, save/reopen, bracket mutation,
closed-loop overflow, outline conversion and end-caret queries. The full
importer passes 119 of 120 (the corpus case is skipped), reader 67 of 68
(the same skip), and all eleven standard unit suites pass their enabled
checks. Twelve upstream layout/cursor tests pass with the existing isolated
font profile; no system fonts were installed or changed.

Seven independently authored Illustrator controls (straight, curve, circle,
offset, centre, right and a short path) pass native import, retain vector
fonts in the PDF and extract the same visible text as Illustrator. The
short path retains ABCD but draws/exports only A. Canvas/PDF foreground
comparison has zero unmatched ink after a two-pixel allowance at 144 dpi.
Against Illustrator, the maximum is 1.613% (circle), within the separate 2%
foreground test; curve is 0.199%, the other five zero. These measurements do
not establish exact glyph placement for every curve. The installed editor's
own native .ai open path preserves textPath and the full story for straight
and short controls.

The PDF check caught and fixed an existing layout/export disagreement:
path fitting hid a character's glyph reference for the canvas, but did not
set the glyph visibility flag read by PDF output. A short path printed the
hidden letters as well. Fitting now hides both, including text used to clip.
The six-case `testfiles/cli_tests/path-type-export-smoke.py` covers full,
short, closed-loop, malformed-end, edited-end and ordinary SVG path text.
It checks extracted text, vector fonts, blank output and canvas/PDF ink.
The initial generic fixture assumed the fallback font's A advance and
expected too few visible letters. It was corrected to use a wide W with a
clear margin; the final six cases passed separately. Production code did
not change after the eleven-suite run. The workspace's focused runner now
includes this check. The six earlier export suites pass 23/32/59/44/91/43.
No real mouse/keyboard acceptance was performed.

All nine affected copied files still use PDF: eight have unestablished path
option values; the owner's copy has common horizontal character scale
0.99412 with vertical scale 1. Do not implement that with the point-type
story's global affine: on a curve it would warp the path and glyph normals.
Those settings need further independent, paced Illustrator controls. This
round claims no new native corpus acceptance.

A cached-record census gives the next investigation a lead: option keys 4
and 18 vary roughly with font size (500 -> 427, 450 -> 384, 170 -> 145).
They may be cached font/layout metrics; this is a hypothesis, not a mapped
meaning. A minimal same-font control at a second size should distinguish
that from a user path setting. Keep the nondefault dictionary refused until
that is established; do not discard the other flags with it. Census:
`artifacts/round24-path-text/options-and-sizes.json`.

Local evidence: `artifacts/round24-path-text/`, seven-control native run
`20261017-015213-398927`, nine-case run `20261017-015216-201608`, standard
suite run `artifacts/focused-suites/20261017-050040/`, final six-case check
`artifacts/round24-path-text/path-export-final/`, PDF comparison
`pdf-summary.json` and installed native-open check `open-smoke.py`.
**Milestone regression.** All 871 owner copies were hash-verified: 860
native (510 partial, 350 unverified), 11 PDF fallbacks; counts unchanged,
all old native notes/classifications and comparison values identical. The
150-file collaborator failure pack, with 51 verified linked companions,
remains 12 checked native (8 partial, 4 unverified), 39 unchecked and 99 PDF
fallbacks; old native/unchecked notes and comparison values are identical.
Only path refusals become more precise (one owner, nine collaborator).
No lost acceptances, errors, timeouts or changed inputs/assets. The three
inputs over 500 MB were excluded before content reads; the original full
1,395-file collaborator baseline was not rerun.

One worker, affinity [12,13,14,15] of 16 CPUs, Below Normal; no build alongside
checks. Owner 624 s, friend 457 s (600 s per-file guard for the known
slow linked-PDF case). Runs `20261017-020810-715882` and
`20261017-021838-755831`; authoritative comparisons and matching source/installed
binary hashes: `artifacts/round24-path-text/`. Broad cohorts run at stable
milestones; affected cases and focused tests are used between changes.

Illustrator automation is left idle after the owner's flicker report. Future
controls require announced, serial document operations with at least ten
seconds idle after completion, rather than rapid create/save/close batches.
The old rapid helpers are disabled; saved controls remain intact.

## Gradient records as Illustrator reads them, and rules from a VectorCraft delta (2026-10-20)

Counted over the owner's 871 staged copies and, separately, the collaborator's
failure pack (140 copies under 100 MB), then put to Illustrator 29.2.1 for the
radial writer (`PROOF-ai-interchange.md`, "Radial gradients"):

- **Stop order.** A linear definition lists its stops last first (6,906
  definitions), a radial one first first (4,482), without exception, and
  Illustrator takes them in that order. The reader pairs each colour with its
  own ramp point and sorts, so it reads both; anything that writes must follow
  the order.
- **`Bh` on every radial instance**, right after `Bb` (13,194 and 53,128
  instances), never on a linear one. Its `x y` is the focal offset in art space;
  its angle and share, through `Bm`, give the same offset in all 191 off-centre
  instances that have `Bm` (187 and 4): "Gradient placement" above holds.
- **`Bg`'s eleventh number** is the ellipse's aspect ratio, y radius over x.
  The reader doesn't need it: every radial art instance in both sets has `Bm`,
  which is whole.
- **Illustrator reads `Bg`, not `Bm`, and reads `Bg` two ways by `%%Creator`.**
  Its own files have `Bg`'s origin and length relative to the object's bounds
  (origin `0 0` is the box's centre, length 1 is √((half-width² +
  half-height²)/2)) and its matrix on its y-down canvas; files from other
  applications have art-space values, no turn from the angle, and the canvas
  centred at 4320. The reader takes `Xm`/`Bm` where they are and `Bg` as art
  space only without them, which suits files from other applications; an
  Illustrator file with an art instance lacking both would need the relative
  reading, and none has turned up.
- **After `%AI5_EndRaster`** Illustrator ends the raster with a paint operator:
  `N` in 292 and 95 files, `F` in 4 of the pack's (Illustrator 17 and 24). No
  path is open there, so neither paints. VectorCraft found that without one,
  Illustrator dropped a clipped picture and all the art after it (`e5c90543`).
- **Feather in an art style** is `(Adobe Fuzzy Mask) 1 0 /Filter`,
  `(FuzzyEffect.aip) /PluginFileName`, `(Feather) /Title`, `<points> /Real
  (Radius)`: 600 and 70 files. VectorCraft's `8f7486b9` looks for `Adobe
  Feather` and `rad`, which neither set has. For the art-styles round.
- **The variable library** (`variableSets` under `/Document`) is in every
  modern file, with one empty `variableSet`; none of either set has a
  `variable` or `sampleDataSet`. Reading it, or saying it wasn't read, needs
  Illustrator control files.

Census tools: `tools\ai-records-census.py` (now with `--max-mb`, default the
everyday 100) and the delta report, `audit-results\vectorcraft-delta-2026-10-20.md`
in the workspace.

### Editable mesh patches written (2026-10-22)

The reader now retains `/Size` and each patch's `/P` position. Compatible
patches sharing corners, colours and sides import as one SVG mesh on its
outer boundary. Extra boundary anchors cut complete grid lines, within 4096
cells; incompatible grids retain the per-patch form and report it. Mesh paths
carry `proof:mesh-object="true"`, joining outline synchronization to mesh edits.
Original corners keep their own colour models, including an uncut patch with
mixed models. A single patch needs no wrapper group.

`Xy` immediately after a mesh's `/End`, before its style marker, is that mesh's
own transparency. Illustrator writes this form when a previous object's state
is still current; reading only the preceding state gave the wrong opacity.
The new reader regression covers post-record opacity and the settled marker.

The writer emits separate `[1 1]` Cartesian meshes for SVG patches because
Illustrator smooths colours across a larger grid's lines. Its saved controls
retain all 21 patches, corners, handles, opacity and device colour models.
Since 2026-10-23, meshes with translucent corners need a conservative
non-overlap proof using all Coons control-point derivative directions across
the complete grid. Folded, degenerate or unproved translucent geometry refuses
by name before output is replaced; some valid strongly bent grids also refuse.
Opaque meshes keep their prior writing bounds; object/group opacity applies
once to the complete object. Qualified curved/reflected/translated controls
still save. This is a writer guard, not connected/folded-grid export support
or a reader change.
[Interchange details and limits](PROOF-ai-interchange.md#editable-mesh-patches-written-2026-10-22).

### Perceptual gradient settings are recognised (2026-10-22)

The interim fallback at `517da8bec5` is superseded by native Oklab support below.

Two copied Illustrator 30.6 controls, `5051fa0707` (RGB) and `5aa616098d`
(CMYK), write `(name) 0 3 1 0 Bd` inside
`%AI17_Begin_Content_if_version_gt:24 16`, followed by a Classic definition
in `%AI17_Alternate_Content`. The reader selects the newer block and now
retains the additional `Bd` operands. It does not reinterpret the second
operand as Dither without a qualified control.

Until Oklab interpolation is implemented, a path painted with this definition
explicitly refuses native import as "Perceptual gradients" and uses the PDF
page where available. Previously it silently drew Classic and could pass the
visual gate, whose tolerance exceeds that colour difference. Unknown extended
settings also refuse native paint. Unused definitions and state notes without
paint do not force fallback; ordinary Classic definitions remain supported.

This is recognition and safe fallback, not editable Perceptual support. The
approved remaining work is to retain the method in the working document,
render it in Oklab through the document's CMYK profile with exact ink endpoints,
and write vector PDF and versioned AI definitions with a Classic alternate.

### Native Oklab and the Classic alternate (2026-10-22)

For the qualified `1 0` setting the reader now retains the method and imports
the original stops as an Oklab vector. The Classic linear ramp cache is
compatibility data, not the newer gradient's interpolation. The writer emits
the observed versioned `Bd` form and a Classic alternate, without replacing
editable stops with rendering samples. Unknown extended settings still fail
closed. Perceptual stop midpoints are retained on their original SVG stops.

The two supplied files each have three gradients, two Perceptual and one
Classic. **Correction to the initial CMYK interpretation:** gradient 150's
6,666,724 raster pixels all have K=0. The ellipse's original endpoints are
`79.8 96 55.1 0` and `100 42 71 49`; its roughly 23-47% K sample row is not
evidence of black generated between two K-free stops. Its first pixel already
has about 23% K because Illustrator re-separates even the original K-free
endpoint. The profile controls black generation; the bright gradient alone
cannot establish whether a K-free constraint exists.

The owner chose Illustrator's RGB/Oklab/profile route, including rendered
endpoints, while retaining original editable stop values. This supersedes the
initial proposal to keep printed endpoint channels exact. PROOF's vector CMYK
functions match the supplied raster samples within 1.9 percentage points per
channel. A deliberately dull blend between K-free endpoints would give a
further direct black-generation control.
[Rendering, writing and qualification](PROOF-ai-interchange.md#native-oklab-gradients-2026-10-22).

## Pages slow to draw: weighed once and kept (2026-10-22)

A document from the collaborator's corpus (`9047eb9b4e`, 11 MB, with five
linked `.ai` and PDF files beside it) took four minutes to open and timed out
their census at 300 s. It opens natively; almost all the time was the page
check. Its one page is 7,739 by 2,339 points with 61 embedded objects and 18
placements, most of a 17.5 MB linked `.ai`, and Poppler takes about 98 s to
draw it at any size (600 px and 2,400 px alike: the content, not the pixels).
It fails the first weighing and passes the finer one (0.2%), so both drawings
were needed. In October the reader refused its linked `.ai` before the check,
in 0.7 s. The owner chose two remedies, and a look at why Poppler is slow
afterwards:

- **Kept for the next opening.** A page Poppler took longer than
  `SLOW_PAGE` (10 s) to draw is kept, with its finer drawing when one is
  made, in the user's cache (`proof-ai-pages` under Inkscape's cache folder):
  a folder per file, found by its path, size and time, Poppler's and Cairo's
  versions and `PAGE_STORE_FORMAT`, and used only while the file's SHA-256 is
  the one it was kept for, so a changed file is drawn again. Each picture
  carries its checksum and how long it took. At most 64 files are kept; the
  folder least lately written goes first, and only folders and files the store
  names are removed. Quick pages are never kept, so ordinary files don't hash
  or write anything. The test runner's own cache directories work as before,
  now keeping the finer drawing and the time too.
- **Weighed once when slow and passing.** A slow page that passes at first
  sight is not drawn again finer: in that band (`FINER_FROM` up to the limit)
  the finer weighing changes only the figure reported, never whether the art
  opens. The import then notes that the page was compared once. A slow page
  that fails at first sight is still weighed finer, since that decides.

For this document: first opening 232 s, every later one **33.5 s**, the same
0.2% each time. Its first opening only gets faster with Poppler, the next
step. Tests: `PagesSlowToDrawAreKeptForTheNextOpening` (every page counted
slow through `PROOF_AI_SLOW_PAGE_SECONDS`, a store of its own through
`PROOF_AI_PAGE_STORE`: kept, reused, ignored and kept anew when the checksum
isn't the file's, a quick page not kept); the importer suite (182); the whole
lite set, 23 of 23 as before.

**Two drawings at once (same day, the owner's choice of remedy 3).** Why
Poppler takes 98 s: the placed `.ai` holds two soft-masked pictures of 29,970
by 4,249 (DeviceN) and 31,205 by 2,716 pixels, each drawn 12 times, so every
drawing of the page decodes and colour-converts about 2.6 gigapixels; Poppler
converts each source pixel before scaling, hence the same time at any size.
(Poppler's Splash renderer takes 28.8 s, but the check is calibrated on Cairo,
and Splash misdraws translucent soft masks.) Illustrator itself takes about 23
s to open the file, the owner measured. Now a page's first drawing runs alone
for `FINER_HEAD_START` (3 s); still running then, the page is heavy, and its
finer drawing starts beside it on a thread of its own, in a Poppler document of
its own (`FinerBeside`). Pages that draw within 3 s never start a second
thread. A page that turns out not to need the finer drawing waits for the rest
of it, begun 3 s after the first. **The timeout document's first opening:
137 s, was 232 s** (98.3 and 98.4 s side by side); later openings 33.5 s; the
same 0.2% throughout. Test: `AFinerDrawingBesideTheFirstGivesTheSameResult`
(a page of hairlines that needs the finer weighing: the same result with the
finer drawing beside the first as after it, and the one used is the one drawn
beside; `PROOF_AI_FINER_HEAD_START` for tests). Importer suite 183; the whole
lite set 23 of 23.

## RGB overprint in the page comparison (2026-10-22)

The page check now uses a disposable appearance PDF for the supported
`/MultiChannelPaint` Separation/DeviceRGB pairs, with the same preview bytes in
both drawing threads. The original input and the visual thresholds are unchanged.
Only corrected documents get a new reference-cache identity; ordinary slow-page
entries remain usable. Default editable PDF import uses the same correction.
The Illustrator-saved RGB mixed-gradient control now returns natively.
[Current scope](PROOF-ai-interchange.md#rgb-pdf-overprint) and
[format/qualification](PROOF-ai-interchange-history.md#rgb-pdf-overprint-2026-10-22).

### Ordinary pictures beside the RGB overprint pairs (2026-10-22)

Image XObject calls outside the marked pairs no longer exclude a page from the
preview. Images cannot execute a nested graphics-state resource lookup; Forms
can, and remain excluded. Any changed state also used to paint an image remains
ineligible. The cache version changes only for corrected documents. Five affected
import/cache/thread tests and 57 picture checks pass, including masks, original
image colour, editable gradients and independent PDF output. No corpus run.

### Leaf Forms beside RGB overprint pairs (2026-10-23)

The disposable page comparison/default editable PDF import now permits bounded
leaf Form XObjects with paths, process colours, shadings and ordinary images.
It scans Forms after the page parser finishes, avoiding stream-alias interference.
Inherited graphics-state names count as shared page uses; local Form states
remain untouched, even when they reference the same original indirect state.
A changed state used to invoke a Form remains ineligible. The Form call and its
known complete Normal states must have no soft mask.

Forms have a separate aggregate budget of 64 calls, 8 MB decoded content and
one million tokens per page. Nested Forms, text, patterns, inline images,
uninspected operators and mask-bearing/partial states retain the prior refusal.
Marked pairs inside Forms are not part of this extension. Page state copies
remain local; the source and original Form resources are never rewritten.
The corrected-page cache suffix is rgb-overprint-3; unrelated heavy-page entries
remain valid. Seven focused import/cache/thread tests and 133 checks across
seven exact synthetic controls pass. No corpus, census, lite or live UI run.

<a id="point-type-written-2026-10-23"></a>
## Point type written (2026-10-23)

PROOF's native writer (`ai/ai-native-writer-text.cpp`) writes point type in the
form PROOF's reader reads and Illustrator opens.

**In the records.** Before `%%EndSetup`, after the gradients and patterns:

```text
%AI11_BeginTextDocument
/AI11TextDocument : /ASCII85Decode ,
%<ASCII85 lines, each after %>~>
x y /RulerOrigin ,
;
/AI11UndoFreeTextDocument : /ASCII85Decode ,
%<...>~>
;
%AI11_EndTextDocument
```

`x y` is where art space's 0,0 lies on the 16383-point canvas, y down: the
artboards' own `RulerOrigin` before it is rounded down. In the body, where the
text object is, after its transparency:

```text
/AI11Text :
0 /FreeUndo ,
0 /FrameIndex ,
n /StoryIndex ,
2 /TextAntialiasing ,
;

0 0 Xd
6 () XW
```

**The typed text document** (version `/98 << /0 14 >>`, Illustrator 30.6's;
Illustrator 29.2.1 writes 11 and opens 14):
resources `/0` hold the fonts (`/1`: MyriadPro-Regular for the normal style,
AdobeInvisFont, then each font used, `<< /0 << /99 /CoolTypeFont /0 << /0 (name)
/2 truetype? >> >> >>`, with `/4 [16.16 axis values]` for a missing variable
instance), the `[Normal Character Style]` (`/5`) and `[Normal Paragraph Style]`
(`/6`) with every feature as Illustrator 30.6 saves them, and the frames (`/8`):
`<< /0 [anchor] /2 << /2 [a b c d e f] >> >>`, the anchor in the story's own
space and the matrix to the canvas. The main dictionary `/1` holds the
document's superscript, subscript and small-caps settings (`/0`, keys 3 to 7),
the stories (`/1`) and the character and paragraph defaults (`/2`, `/3`). The
second document has the same resources and settings and no stories.

**A story:** `/0` the model: the text (UTF-16BE after its mark, a return ending
each paragraph, parentheses and backslashes escaped and other bytes as they
are, as Illustrator writes them), paragraph runs (`/5`; each `/0 << /0 align >>`
under the normal paragraph style `/6 0`, one per paragraph: a single run over
two paragraphs draws them as one line in Illustrator), character runs (`/6`; each
the normal style `/5 0` and its own features `/6`), and `/10 << /0 4 /2 true >>`
as Illustrator writes it. `/1` the view: its frame and the layout PROOF's reader
needs: `PC`, `F` (the frame and its anchor), `R`, `R`, then a line `L` per
paragraph (`/0` down from the anchor) with one segment `S` (`/15 /0` its UTF-16
count, `/0` where its first character starts across from the anchor).

**Features written** (run `/6`): 0 font, 1 size (points), 4 automatic leading,
5 leading, 8 tracking (thousandths of the size the characters are set at),
9 baseline shift (points, up), 11 kerning (0 none, 1 the font's), 12 capitals
(0, 1 small caps, 2 all caps), 13 position (0, 1 superscript, 2 subscript),
15 strikethrough, 16 underline, 18 standard ligatures, 20 contextual
alternates, 53/56 fill and its flag, 54/57 stroke and its flag, 63 stroke width,
64 miter limit. A paint is `<< /99 /SimplePaint /0 << /0 model /1 [1.0 ...] >> >>`:
model 1 RGB, 2 CMYK, the first value 1.

**What Illustrator does with it** (type spike, 2026-10-23, Illustrator 29.2.1
through its scripting interface; recorded as 30.6 until the owner pointed out
that this machine's is 29.2.1): it reads the stories, fonts, sizes, colours
in their own model, tracking, leading and alignment as editable type, and lays
each story out again from the model; the layout in the view is ignored, a
deliberately wrong one included. Without any layout it draws the same.

**Where the lines come from.** Inkscape's layout gives plain SVG text (an
import's: each line a tspan with its own x and y) one layout line, its later
chunks moved down by their `y`; Inkscape's own lines are layout lines of one
chunk each. The writer takes each chunk as a line, at the x its alignment is to
and its baseline (`Layout::chunkPlacement`, added for this). A chunk's
`chunkAnchorPoint` measures with the letter spacing after its last letter,
which the layout leaves out when it aligns the chunk; the placement doesn't.

<a id="area-type-written-2026-10-23"></a>
## Area type written (2026-10-23)

The writer writes area type in the form Illustrator 29.2.1 saves it, learned
from a control it saved (three frames) and from area type in the owner's
copies.

**The text object** carries its frame as an unpainted path in art space between
its story number and its antialiasing:

```text
/AI11Text :
0 /FreeUndo ,
0 /FrameIndex ,
n /StoryIndex ,
/Art :
X=
4 As
50 631.89 m
50 781.89 L
...
n
X+
; /ConfiningPath ,
2 /TextAntialiasing ,
;
```

`As` is the path's number of segments (4 for a rectangle, 5 for one curved
frame of the owner's). Illustrator writes the path's state before it when it
changes (`0 D`, `Xy`, stroke settings); PROOF writes none: the path is
unpainted. In 255 area objects of the owner's copies the `/Art` path and the
frame in the text document agree; nothing showed a copied object keeping its
original's frame (VectorCraft's note in `frames.rs`).

**The frame** (text document resource `/8`): `<< /0 << /1 << /0 [points] >> /2
<< /0 1 /2 [a b c d e f] /7 18.0 /8 18.0 [/10 << /0 rule /1 minimum >>] >> >> >>`.
The points are the outline, each segment four points (start, two handles, end;
a straight side its ends twice), in the story's own space; Illustrator starts a
rectangle at its bottom-left corner. `/0 1` is area type, `/7 /8` the gutters
(18 when untouched), the matrix as for point type; Illustrator 29.2.1 leaves the
matrix out when it is the identity.

**The first baseline** (`/10`), worked out by having Illustrator 29.2.1 lay out
nine frames of one text, each with another rule and a minimum of 0, and reading
its re-save (Myriad Pro 12 pt, leading 15):

| Rule | Illustrator's name | First baseline below the frame's top |
|---|---|---|
| 0 | Fixed | the minimum (0) |
| 1 | Ascent, the default; Illustrator then leaves `/10` out | 8.52 |
| 2 | Cap Height | 8.09 |
| 3 | Leading | 15 |
| 4 | x Height | 5.81 |
| 5 | Em Box Height | 10.04 |
| 6 | Legacy | 11.42 |

7 and 8 aren't rules (one moved the line above the frame). 167 of the 248 area
frames in the owner's copies say `/10 << /0 5 /1 0 >>` (Em Box), the rest
nothing. The reader keeps the rule (`PointText::first_baseline`, the import's
`proof:first-baseline`) and the writer writes it back; written without it,
Illustrator would set such frames by Ascent, about 1.5 pt higher. Type Inkscape
flowed into a shape is written with rule 0 and the distance from the frame's
top to Inkscape's first baseline: Inkscape sets the first line by its line
height, Illustrator by the font's ascent (Arial 0.716 em), 2.2 to 4.4 pt apart.

**The story.** What was typed, the lines joined: a return ends a paragraph, a
line break typed inside one is U+0003, a line the frame wrapped ends with
nothing. Illustrator ends a paragraph run at each U+0003 as at each return (83
stories of the owner's copies, every one); the writer does the same. Each
paragraph run says its alignment (`/0`) and, where the import says so,
hyphenation (`/9 true`; the normal style written has it off, as Inkscape sets
type). Leading is each line's distance from the one before. What the frame
was too small to show follows the last line, in its last character's features.
The view holds, for PROOF's reader, the frame node `F` at the frame's top-left
corner and a line `L` for each line, down from it, its segment's count and
where it starts across. Illustrator 29.2.1 also writes the story's line count
per paragraph (`/1 /1`) and the program that wrote it (`/1 /4`); neither is
needed.

**Reading it.** The reader marks each line's end (`TextLine::end`: a return, a
U+0003, or the frame's wrap), its paragraph's alignment and hyphenation, all
before it decides the anchors: an area line whose paragraph is centred but
which keeps a space at its end is anchored at its start, as before, and now
also carries the centring (`text-align`), which the writer writes. Nothing
else changed in how lines are placed.

**Inkscape's type in a shape.** `shape-inside` with one shape: its curve with
the shape's own transform is the frame, in the text's coordinates. Inkscape
keeps a typed line feed (`white-space:pre`) as a character inside its layout
paragraph, not as a new one, so the writer ends a paragraph after a line whose
chunk holds one. The layout measured that line feed with the font's width for
it, though it draws it with none: a centred or right-aligned line before one
was set off its anchor by that width (3.2 pt for 14 pt Arial). It is measured
with none now (`Layout-TNG-Compute.cpp`, `_measureUnbrokenSpan`), and the line
sits where Illustrator sets it.

## Path type written (2026-10-23)

The third bounded type-writing slice follows point and area type. A text object
holding one `textPath` becomes Illustrator path type: frame kind `/0 2`, its
matrix `/2`, cubic path points in `/1 /0` and brackets `/6 [start end]`.
Its `/AI11Text` object also carries that path as `/Art` before `/ConfiningPath`.
Original stories, Unicode and supported character runs stay editable.

The referenced shape's transform is included and `side="right"` reverses its
path. Each segment is cubic; a line's handles lie on its endpoints. Brackets
are segment index plus cubic time, found from arc length. Imported
`proof:path-start/end` must agree with `startOffset`. Ordinary SVG type derives
its interval from start offset and start/middle/end alignment; it must fit.
Closed paths can run once beyond their own start. The reader accepts an end
past the final segment or normalized before the start, within one turn; a
nonzero whole-turn range can normalize to equal places. Identical raw brackets
remain invalid. An import's hidden overflow stays in the story and remains
hidden on canvas/PDF; native saving keeps its active interval.

The reader measures distances on the original cubics before `Path::close()`
can replace an endpoint-handled closing cubic with a line. This fixes timing
on a closed path's final straight side. Layout fitting, character bounds,
nearest-cursor lookup and cursor shape wrap round a closed path consistently.
Existing short/invalid/end-edited intervals still hide the same glyphs in PDF.

### Source path options and spacing

The frame's `/11` dictionary is bounded and validated. Keys 4 and 18 accept
finite numbers (Illustrator recalculates them); 22 accepts a finite number and
6 a Boolean. The later controls correct the earlier key inference: explicit
`6 false` enables automatic spacing even alone; `6 true`, or omitting 6,
does not. Illustrator drops bare 4/18 without 6 and recalculates caches when
automatic spacing is requested. False 0
and 7 and zero 1/2 are accepted defaults. Flip (`0 true`), nonzero effects (1),
non-baseline alignment (2), `7 true`, malformed types and unknown keys retain
explicit refusal. A changed cache number alone no longer rejects the story.
This is not a mapping of all type-on-path settings.

Validated options survive SVG save/reopen on `textPath` as
`proof:ai-path-options`, a canonical dictionary body of scalar values.
Appearance-story comparisons conservatively distinguish different source
options. New native imports also write `proof:path-spacing="auto"` for explicit
`6 false`. Native output regenerates that supported mode as `/11 << /6 false >>`,
with no stale cache numbers; Illustrator recomputes them. Older working SVGs
with source provenance alone keep their existing arc-length layout.

The bounded renderer uses the first run's live font: reference height in points
is `round(size * (1 + cap-height/em) / 2)`, then converted to local coordinates.
This matches eight independent Arial, Times New Roman, Courier New and Myriad
Pro controls at 12/24 pt. A monotone sampled metric approximates spacing along
an offset baseline: one fifth of the height on outward bends, half on inward
bends. Selection, cursor placement and hit testing use the same inverse map.
Straight paths retain their ordinary placement. These are empirical rules from
Illustrator 29.2.1, not Adobe's published implementation or a universal 1.93 pt
constant. The inward range is qualified only while height times curvature is
at most .4; corners, tighter inward bends and collapsed/nonmonotone offsets
refuse native saving and native import rather than pretending to match.

Imported bracketed path type clips a whole cluster if its end does not fit.
Overflow chooses a word break when possible; centre/right alignment uses the
visible prefix, excluding its trailing space. The complete story remains in
the SVG/native records. This corrects classic bracketed type too, while ordinary
SVG type without imported brackets retains SVG midpoint clipping. A completely
clipped path story may have only `/PC /5 0`, with no cached `/F` or `/L`: its
geometry and style runs still make an editable hidden story. Extending its end
bracket makes it visible. Decimal closed-loop brackets one turn apart are
normalized as a full turn even when floating-point remainders differ by ulps.

The original Arial 12 pt / 1.93 pt raised-path fit remains historical evidence,
superseded by this font-metric approximation and its stated bounds. The visual
gate is unchanged. Adobe's public
[Type on a path reference](https://helpx.adobe.com/illustrator/using/creating-type-path.html)
describes user-facing effects/alignment/spacing; the numbered keys above come
from the independent saved controls, not from that guide.

### Refusals and qualification

Unsupported forms include several/partial paths, a missing or multi-piece
path, invalid/moved brackets, several lines, overflow of ordinary SVG type,
path type inside an area frame, stretched/skewed placement, shifted baselines,
character x/y placement, underline/strikethrough and other unsupported type
features. Modern saving names the actual limitation; Illustrator 7 still
refuses all type.

Illustrator 29.2.1 opens the eight-case control as editable path type: left,
centre, right, reversed side, straight line, centred across a circle's start,
a whole turn, and the final corner of a rectangle. Re-save preserves every
story and bracket exactly, including `[0 4]`, `[2 6]` and
`[3.32635 7.32635]`; its return opens natively. At 144 dpi, master/native/return
have zero dark ink pixels farther than two pixels from Illustrator's drawing;
page component mean difference is about .132, maximum 179 at edges. One extra
PROOF column is provably white and excluded; images are neither shifted nor
rescaled. These are placement/edge bounds for these controls, not pixel identity
or automatic-spacing qualification, and not an Illustrator 30.6 check.

Follow-up validation: nine focused path-type units (including metadata
save/reopen, conservative refusals and native normalization), six path/PDF
export controls, and the exact nine copied fallback cases. Four now pass the
native gate (`0b58f4b842`, `35e3486b58`, `56dacaefaf`, `b6c58bcfdd`); all four
native writes still refuse missing links. No companions were available beside
the intake copies or in their recorded relative folders. The other five retain
fallback for Flip (one), character scaling (three) or linked frames (one).
No new artwork writer acceptance or lite case is claimed. Earlier importer
202, reader 79, layout 2/2 and native smoke 37/37 remain the prior slice's
evidence; they are not fresh reruns. No full corpus/census, whole lite set or
new Illustrator session in this qualification follow-up.

### Curve spacing follow-up (2026-10-23)

The bounded spacing and clipping follow-up is qualified below. Fifty-one
supported synthetic controls cover four fonts/two sizes, mixed runs, outward/
inward/S curves, circles, alignment, option normalization, wrapped starts and
overflow. At 144 dpi, more than 99% of dark ink is within two pixels of the
saved Illustrator page in every supported control (no image registration).
This is an approximation/edge bound, not pixel identity. Tight inward, cusp and
corner controls remain guarded; six extreme single-glyph controls save in
Illustrator with no visible glyphs. The owner's gauntlet was inspected only
as a hash-verified copy; it has no live path type, so synthetic controls were
used at the owner's request.

Sixteen focused path units pass, including eight numeric Illustrator placement
references and fully hidden story recovery; shared layout suites pass 2/2.
Eleven canvas/PDF cases pass, checking extraction, font vectors and hidden
glyphs. The three-file native Save As qualification covers 29 stories and 25
automatic modes; its final report is recorded in the workspace handoff.

Correctness tightening changes the four former partial imports 0b58f4b842,
35e3486b58, 56dacaefaf and b6c58bcfdd to PDF fallback for their unqualified tight
path spacing. This supersedes the preceding four-of-nine acceptance statement;
their native writers were already blocked by missing links. No broad totals
were recomputed. Illustrator 30.6 and tighter inward spacing remain open.
