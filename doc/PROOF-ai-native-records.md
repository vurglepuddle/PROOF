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
- **Other `%` lines** are section markers, such as `%AI5_BeginLayer`.
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
| `(name) type n Bd` … `Bs` … `BD` | Gradient definition. Each stop is `colour style [opacity 6] midpoint ramp Bs`. The styles are 0 grey, 1 CMYK, 2 RGB with CMYK, 3 and 4 named. | VectorCraft |
| `[flag] Bb`, `Bg`, `Bm`, `Bh`, `BB` | Gradient on an object: name, origin, angle, length, matrix and focal point. | VectorCraft |
| `%AI5_BeginRaster`, `/Space XN`, `[m] … w h bits type alpha … XI` | Embedded image. The samples stay in their colour space; CMYK is common. | VectorCraft |
| `/AI11Text : /StoryIndex …` | Text object. Its text lives in `%AI11_BeginTextDocument`. | VectorCraft |
| `n Ar` | The old Attributes panel's output resolution (300, 800). No visual effect. | **census** |
| `1 An` | CS5 only, after every object. Almost certainly *Align to Pixel Grid*. No visual effect. | **census** |
| `(hex) Xt n XD` | Brush metadata inside drawn looks, with n running 0–4. No geometry or paint. | **census** |
| `Ae AE Ap As Xd Xr XG Xh XH XF D X= X+ Bc Xm XP Np TE TZ Xt Xi XI ` LB2 Lc Xv XV Xq XQ Xg Xn Bn` | Ignored by VectorCraft. | VectorCraft |

### Not read yet (the file opens from its PDF page)

| Feature | Records | Files (of 877) |
|---|---|---|
| Gradient mesh | `/Mesh X!` … `/End X!`, with `value /Key X#` pairs (Version, Type, Size, the patch data) | 11 |
| Pattern fills | `p`/`P` | 15 |
| Placed (linked) files | `%AI5_BeginPlace` … `%AI5_EndPlace`, with a `~` inside | 6 |
| Symbols | `/SymbolInstance` | 2 |
| Opacity masks | `/Mask : (Clipping) (Inverted) (Disabled) (Linked) ,` inside the masked object's ArtDictionary, with the mask art on `%_` lines | 3 in `Typical.ai` |

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
resolution can escape them.

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
Area/path/linked text, varying character scales within a story, baseline shifts,
justification and unsupported character features still refuse visible text.
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
   `ate.rs` as the numeric-key reference. Expand from confirmed root causes;
   source ICC profiles and remaining rendering families are still future work.
4. **Writer** extensions, in this order: gradients, images, text. They use the
   same tables, and the reader checks the writer's output alongside Illustrator.
