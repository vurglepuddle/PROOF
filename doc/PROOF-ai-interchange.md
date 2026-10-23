# PROOF AI interchange

Current contract, 2026-10-23. PROOF reads supported editable Illustrator artwork
and writes a bounded native subset without Illustrator installed. Keep SVG as
the working master when using features the native writer cannot yet represent.
The [dated implementation history](PROOF-ai-interchange-history.md) contains
format recipes, Illustrator checks and earlier coverage snapshots.

## Choosing an output

| Format | Use | Limits |
|---|---|---|
| **Save As: Adobe Illustrator (native)** | Fresh editable Illustrator records plus matching PDF 1.7 pages, generated from the current document | Requires PROOF's bundled PDF backend; unsupported features refuse the save by name |
| **Save a Copy: Adobe Illustrator 7** | Standalone editable records for the qualified legacy subset | One artboard, Windows-1252 names, opaque artwork; no PDF preview or retained document ICC profile |
| **Save a Copy: Illustrator interchange (PDF-based)** | Broader RGB PDF artwork that Illustrator can open | Loses native layers, CMYK separations and spot identity; filters rasterize; not a print-production preset |

Native saving regenerates both representations. It never attaches stale
Illustrator records from an earlier version of the document. A refused or
failed native save leaves the previous destination intact. Copy-only formats
are excluded from ordinary Save; a deliberate overwrite through Save a Copy
still uses the usual overwrite prompt. Keep original artwork separately.

CLI native output must select the extension explicitly; a `.ai` suffix alone
selects the older PDF-based RGB interchange route:

```text
inkscape working.svg --export-extension=org.inkscape.output.ai.native --export-filename=native.ai
inkscape working.svg --export-extension=org.inkscape.output.ai.legacy7 --export-filename=legacy.ai
inkscape working.svg --export-filename=rgb-interchange.ai
```

## Reading and acceptance

The native reader reconstructs supported layers, paths, compound clips,
gradients, patterns, meshes, masks, process/spot colours, document profiles,
pictures and editable type. Some live Illustrator objects retain their drawn
result with explicit import notes. A native import is not a claim that every
Illustrator editing control survives.

Every artboard is checked against its PDF page. Unsupported/incomplete records,
mismatched page counts or differences above the existing 1% lightness / 2%
colour limits use PDF import. The limits are not relaxed to accept a feature.
Modern files without usable PDF-compatible content remain unsupported.
The [native-record reference](PROOF-ai-native-records.md) documents the bounded
reader, type semantics, import notes and comparison algorithm.

PDF fallback can change layers, text layout and live appearances; text already
outlined in the PDF cannot become editable type. Embedded foreign PDFs have a
bounded editable vector importer; unsupported forms, including overprint and
mesh shading, still refuse native reconstruction.

Slow PDF reference drawings are cached with source-hash verification. If the
first drawing runs beyond 3 s, a finer drawing starts in its own Poppler document
alongside it. The qualified heavy case improved from 232 s to 137 s on first
opening and takes 33.5 s subsequently. This is a measured case, not a general
opening-time guarantee. [Cache and comparison rules](PROOF-ai-native-records.md#pages-slow-to-draw-weighed-once-and-kept-2026-10-22).

## Modern native writing

| Artwork | Supported form |
|---|---|
| Structure | Named/multiple artboards, layers, groups, supported clones, smooth paths, compound paths and clips with holes/winding rules |
| Solid paint | RGB, CMYK and grey process colour; named CMYK-, Lab- and RGB-defined inks with tints; canonical Registration definitions |
| Gradients | Non-repeating linear/radial fills and strokes, separately or together; midpoints, interior hard stops, stop opacity, mixed colour models, supported spot/ICC stops, and the qualified Oklab method |
| Patterns | User-space tiles on fills/strokes, containing supported vector art |
| Meshes | Supported RGB/CMYK Coons patches, with editable corners, handles and opacity; multiple patches written as independent meshes in a group |
| Masks | Linked, non-inverted greyscale vector opacity masks; Clip on/off, fills/strokes, linear/radial gradients and nested masks; Normal/Multiply blending |
| Transparency | Object/group opacity, sixteen blend modes, isolated blending, knockout records, overprint and supported stroke styles |
| Pictures | Supported embedded pictures and local PNG/JPEG/classic TIFF/PSD links; original process channels retained where supported |
| Type | Point type as Illustrator's own stories: each line a paragraph about one anchor, with its leading; runs of installed fonts (or a missing font's own name), sizes, process fill and stroke, tracking, baseline shift, kerning on/off, standard/contextual ligatures, all caps, small caps, superscript, subscript, underline, strikethrough; any frame matrix. [Point type written](#point-type-written-2026-10-23). Area type in its frame (an import's, or the one shape Inkscape flows type into): paragraphs joined where the frame wrapped them, typed line breaks, alignment, hyphenation, the first-baseline rule, what the frame doesn't show; turned frames. [Area type written](#area-type-written-2026-10-23). Ordinary type on one open or closed path, including wrapped intervals. [Path type written](#path-type-written-2026-10-23) |

Supported does not mean arbitrary combinations of these forms. Important
explicit refusals include:

- Vertical type, type that reads from right to left or holds
  such letters, tabs, justified type, auto-wrapped type (`inline-size`), type
  flowed into several shapes, around others or with padding, or past its shape's
  end, stretched or skewed area type, characters moved or turned one by one,
  spot inks, gradients or patterns on type, translucent character paint, variable
  fonts, fonts that aren't installed under the style's own name, and the
  remaining OpenType and decoration settings (the writer names each); type in
  patterns, opacity masks or the Illustrator 7 copy.
- Unsupported appearances/filters; layer/root transparency;
  separate fill and stroke opacity when both paint.
- Patterns sized to their object, view-box patterns, and gradients, pictures or
  nested patterns inside a tile.
- Coloured, inverted, unlinked, picture or type masks; masks on layers, clones,
  the root, inside patterns or with object-sized units.
- Grey-defined spot inks, conflicting definitions for one ink name and
  unsupported ICC spaces. Supported sRGB-tagged stops keep their RGB numbers;
  this is not arbitrary ICC-gradient support.
- Repeating gradients, unsupported radial-stroke geometry, reversed paint order,
  unsupported endpoint hard stops, and unknown extended gradient settings.
- Bicubic or otherwise unsupported meshes, moved tensor points, mixed
  corner models, mesh strokes and mesh paint inside patterns or mask art.
  With translucent corners, folded, degenerate or unproved non-overlapping
  geometry refuses before saving; the conservative whole-grid check can also
  refuse some valid bent meshes. Opaque writing retains its prior bounds;
  opaque folded surfaces have no new qualification.
  [Mesh geometry bounds](PROOF-ai-interchange-history.md#non-overlapping-mesh-writing-2026-10-23).
- Missing/unreadable links, linked PDF/AI pages in native output, unsupported
  picture formats, native cached link previews and packaging.

The writer reports the actual refusal rather than silently dropping the object.
Read the relevant [historical qualification](PROOF-ai-interchange-history.md)
for exact geometric/format bounds; source checks remain authoritative.

## Colour and rendering contracts

Document ICC profiles are saved byte for byte. Process numbers, spot definitions
and tints remain editable. Illustrator's profile assignment is a separate,
owner-deferred qualification item; preserved profile bytes alone do not prove
Illustrator applies the intended profile in every route.

Classic gradients blend in the document's mode; a CMYK document's canvas uses
profile-converted CMYK samples. CMYK meshes similarly blend their channels
before display conversion. Mixed spot/process gradients follow Illustrator's
overprint appearance on the canvas and in process-colour PDF output. PDF spot
plates and PDF/X remain separate prepress work. Full CMYK transparency
compositing and knockout appearance on PROOF's canvas remain limited; retaining
the native blend/knockout records is not screen parity.

<a id="native-oklab-gradients-2026-10-22"></a>
### Oklab / Perceptual gradients

The qualified `1 0 Bd` setting imports as editable Oklab. SVG retains
`proof:gradient-method="oklab"` and original stops/midpoints; rendering samples
do not become extra editable stops. Linear/radial canvas drawing and vector
PDF output share those samples.

**Owner's CMYK policy:** match Illustrator's intended appearance. Convert CMYK
stops through RGB, blend in Oklab, then separate through the document profile,
including rendered endpoints and its black generation. Editable/native stop
values remain original; printed separations can change. There is no forced
K-free rule.

Modern AI writes the newer method plus a Classic alternate. Illustrator 29.2.1
opens that alternate as editable Classic gradients; saving there loses Oklab.
Illustrator 30.6.0 qualification (2026-10-23, two synthetic controls from
`209a170c14`) retains one Classic and three Perceptual methods per document,
original (2,3,2) editable Perceptual stops, channels, midpoints, radial alpha and
stroke geometry. Both saved returns reopen natively; the CMYK ICC stays
byte-identical. The returned PNGs confirmed that midpoints below 50% need a
mirrored power curve. With that correction, linear interiors differ by at most
1 RGB / 8 CMYK display levels on the native canvas, and 3 / 6 in independently
rendered vector PDF. Radial colour/opacity fields also qualify at the saved
cache's sample coordinates: canvas maximum 6 RGB / 4 CMYK display levels,
opacity within one 8-bit level. Independent PDF-function evaluation agrees
within 6 RGB levels / 1.3 CMYK ink percentage points. The larger radial-stop
PNG discrepancy comes from sampling the 150-by-67 raster preview; page PNG
pixel identity is not asserted. See the
[cache-grid qualification and remaining limits](PROOF-document-colors.md#radial-cache-sampling-qualification-2026-10-23).
Unknown extra settings, Perceptual spot-ink gradients and Perceptual output
inside opacity masks refuse safely; the extra setting is not labelled Dither
without a confirming control.
[Format and measured comparisons](PROOF-ai-interchange-history.md#native-oklab-gradients-2026-10-22).

### RGB PDF overprint

The page comparison and default editable PDF importer now simulate the bounded
`/MultiChannelPaint` pairs Illustrator writes: a Separation axial/radial shading
followed by an overprinted DeviceRGB shading on an explicitly RGB transparency
page. A disposable PDF preview gives that second shading Multiply appearance;
the original file and its separations are untouched. Resource/state dictionaries
are copied per page, leaving soft-mask Forms and other pages unchanged. SVG
import retains both editable ramps and their compositing. Ordinary image XObjects
elsewhere on the page are supported, as are bounded leaf Forms containing
paths, process colours, shadings and ordinary images. A state also used to paint
an image/Form, or looked up by a Form through inherited resources, stays
unchanged. A Form's local states remain original, including when they share an
indirect object with the page state. DeviceGray soft-mask paint stays numeric
coverage rather than being converted to print grey.

This is deliberately limited: at most 100 MB input, 256 pages, 8 MB of content
per page, complete known graphics states and exactly two shadings per marked
pair. Leaf Forms have a separate page-wide budget: at most 64 calls, 8 MB of
additional decoded content and one million tokens. Their called states must be
complete known Normal states with no soft mask; the call itself also needs no
active soft mask. Nested Forms, text/pattern art inside Forms, inline images,
marked pairs inside Forms, nested marked paints, shared states, other
blend/overprint modes and colour spaces retain existing handling. The optional Cairo-to-SVG importer
is unchanged: its compositing-filter bounds need separate work. Embedded foreign
PDF overprint remains outside that importer's bounded subset.

The Illustrator-saved RGB mixed-gradient control now opens natively; canvas
samples differ from Illustrator by at most 13 display levels and the default
PDF import by 11. The visual gate is unchanged. Both concurrent comparison
drawings use the same preview bytes; only corrected documents get a new cache
identity, preserving unrelated heavy-page cache entries.
[Implementation and controls](PROOF-ai-interchange-history.md#rgb-pdf-overprint-2026-10-22).
[Colour contract](PROOF-document-colors.md#an-ink-beside-process-colour-2026-10-22).

<a id="point-type-written-2026-10-23"></a>
### Point type written (2026-10-23)

Native saving writes point type as Illustrator keeps it: the stories in the
records' text documents (`%AI11_BeginTextDocument`, before `%%EndSetup`) and an
`/AI11Text` object naming its story where the text object is. Each line of a
text object is a paragraph; the lines share one anchor, each below the one
before by its leading (automatic where it is Illustrator's 120%). Runs carry the
font by PostScript name, the size, process fill and stroke and the other
features in the table above. The frame's matrix places the story; a uniform
scale goes into the sizes, so 16 px type reads as 12 pt in Illustrator. Type set
in a font standing in for one that isn't installed is written in that font's
name. Lines are the layout's chunks, so an import's lines (each placed by its
own x and y) and PROOF's own lines both write; an import's empty lines stay
empty paragraphs.

Illustrator 29.2.1 lays a story out again from its text, paragraphs and runs
when it opens a file, whatever layout the story keeps: a deliberately wrong
layout changes nothing (type spike, 2026-10-23). PROOF writes the layout for its
own reader only, each line where PROOF sets it, so its own files reopen natively
with every character in place. Illustrator 29.2.1 opened a synthetic control
(fonts, RGB/CMYK, tracking, leading, centring, a turned frame, superscript,
small caps, underline, a stroke without a fill) within anti-aliasing of PROOF's
drawing (a turned line within 0.13 pt); its re-save reopens natively in PROOF
within 12 levels of the original. An owner document with Illustrator type
(`2c7be7744c`, five stories in a font that isn't installed), imported and
written by PROOF, draws in Illustrator within 15 levels of the original file.
(Recorded as Illustrator 30.6 until 2026-10-23: the spike and these checks ran
in this machine's 29.2.1. The normal styles written are 30.6's, from a 30.6
file; 30.6 itself hasn't opened PROOF's type yet.)
[Records and keys](PROOF-ai-native-records.md#point-type-written-2026-10-23).

<a id="area-type-written-2026-10-23"></a>
### Area type written (2026-10-23)

Native saving writes area type as Illustrator's area type: the story in its
frame, which the text object also carries as its `/Art` path, and Illustrator
lays it out again in the frame. Two sources:

- **An import's area type** (`proof:text-frame`, each visible line a positioned
  span). The import now keeps how each line ended (`proof:line-end`: `wrap`
  where the frame broke it, `break` at a line break typed inside the paragraph,
  Illustrator's U+0003; none at a paragraph's end), the paragraph's alignment in
  `text-align` (which the positioned lines don't follow; until now a centred
  paragraph whose lines kept a space at their ends came in as left-aligned), its
  hyphenation (`proof:hyphenate`) and the frame's first-baseline rule
  (`proof:first-baseline`). The writer joins wrapped lines back into their
  paragraphs and writes what the frame was too small to show
  (`proof:text-overflow`) after them.
- **Type Inkscape flows into one shape** (`shape-inside`): the shape's outline is
  the frame, the layout's paragraphs and its typed line feeds end paragraphs,
  and the lines Inkscape wrapped are joined. Its first baseline is written fixed
  where Inkscape set it below the frame's top, since Illustrator's default puts
  it one ascent below (2.2 pt higher for 12 pt Arial).

The lines of point type an import kept with a typed line break are written with
one too. Refused by name: auto-wrapped type (`inline-size`; Illustrator has no
frame that grows yet in what PROOF writes), several shapes, `shape-subtract`,
`shape-padding`, type that overflows its shape (the layout keeps nothing of
it), justified paragraphs, and a frame that is stretched, skewed or not one
outline.

**Checks** (Illustrator 29.2.1). Its own control (three frames: left with a
wrap, centred, right-aligned and turned with fixed leading), imported and
written by PROOF: Illustrator draws the write exactly as its own (no pixel
differs in the two upright frames) and its re-save puts every line where it
laid out the original. Inkscape's type in a rectangle, a centred one with an
empty paragraph, a circle and a turned rectangle with 20 pt leading: Illustrator
breaks every line where Inkscape did, and its re-save puts every line exactly
where Inkscape set it; the drawings differ only at glyph edges. The owner
document `15cad93e92` (four frames, typed line breaks) draws in Illustrator
within 59 levels of the original file, no block moved. Of the 11 documents that
area type held, the 4 it held alone now write; the rest wait on right-to-left
type, tabs, variable fonts and missing links.
[Records and keys](PROOF-ai-native-records.md#area-type-written-2026-10-23).

### Path type written (2026-10-23)

Modern native output writes one line of type on one open or closed path as
Illustrator's editable path type. It retains imported brackets or derives them
from SVG's start offset/alignment, reverses the path for `side="right"`, and
allows a closed path's interval to run past its start once. The reader, glyph
bounds and cursor follow those wrapped intervals too. Unsupported shifts,
decorations, character placement, multi-path or stretched forms refuse by name.

Illustrator 29.2.1 retains all eight synthetic control stories and their exact
brackets on re-save. PROOF's master, native write and Illustrator return agree
in drawn placement: no dark ink beyond two pixels of the other drawing at
144 dpi. Pixel identity is not asserted; maximum edge differences reach 179
levels. The returned file opens natively. Illustrator 30.6 remains untested.

Source `/11` options are retained as `proof:ai-path-options` metadata in SVG.
Finite numeric cache values and validated scalar flags no longer force fallback
solely for differing from the original 12 pt control. Flip, effects and
non-baseline alignment still refuse. New native imports retain automatic curve
spacing as `proof:path-spacing="auto"`: smooth paths use a bounded font-metric
approximation, shared by glyphs, selection, hit testing and cursors. Native
saving emits `/11 << /6 false >>`; Illustrator recalculates its font caches.
Very tight inward bends, sharp corners and collapsed offsets remain explicit
refusals/fallbacks. Older SVGs that hold only source-option provenance retain
their existing arc-length layout.

Illustrator brackets now require whole letters and wrap overflowing words at
a break where possible. Alignment follows the fitted prefix, keeping the full
story editable; this applies to imported classic and automatic path type.
Ordinary SVG paths keep their SVG clipping semantics. A wholly clipped native
story stays live even when Illustrator omitted its cached layout frame.
[Records, limits and qualification](PROOF-ai-native-records.md#path-type-written-2026-10-23).

## Linked PDF/AI pages

Native reading retains the one-based `AI11 PlacedPDFPageIndex` in
`proof:pdf-page` (default 1). Screen/SVG reopen and ordinary PDF output use the
selected page, retaining its vectors, fonts and inks in PDF. Shared resources
are copied once; different pages remain distinct. Missing/invalid pages,
rotated pages and crop choices other than the PDF crop box fail closed.
This does not add linked PDF/AI pages to native AI writing.
[Placed-page contract and checks](PROOF-placed-pages.md).

The damaged 33-placement case `e1143523ac` still has an expected fallback: its
supplied companion lacks requested pages. The related 30-page production PDF
omits covers; correspondence with the older placements has not been mapped.
There is no automatic relinking or inferred difference in interior artwork.

<a id="the-illustrator-7-copy-what-it-keeps-and-refuses-2026-10-21"></a>
## Illustrator 7 copy

The qualified subset keeps editable paths, compound clips, groups/layers,
CMYK-defined inks and tints, supported classic gradients and opaque 8-bit
greyscale/RGB/CMYK embedded pictures. It uses one unnamed artboard and
Windows-1252 names. It does not need the bundled PDF backend.

It refuses transparency, opacity masks, patterns, meshes, type (named "type (Illustrator 7)"), picture links,
Registration, Lab/RGB-defined inks, mixed gradient models, hard stops and
Perceptual gradients, plus unsupported geometry/names. It does not retain the
document ICC profile or an artboard name. The combined subset was checked in
Illustrator 29.2.1 and back into PROOF; actual Illustrator 7 is untested.
[Full legacy subset and qualifications](PROOF-ai-interchange-history.md#the-illustrator-7-copy-what-it-keeps-and-refuses-2026-10-21).

## Qualification and next work

The last broad checkpoint is **2026-10-22 at `fb17863507`**: 862 native imports
from 871 copied files, modern writing 802/862, legacy writing 100. These are
historical counts, not a census of later changes. Writer acceptance/readback
and visual acceptance are separate from complete Illustrator editability.
Subsequent changes use affected units, export controls and copied lite cases;
a new full census waits for an agreed feature-family milestone.

The owner's v1 scope (2026-10-23) is the qualified interchange subset plus
bounded editable type writing. After closing current-subset correctness gaps,
native type is the next and final implementation family for v1. Extended
Oklab settings/freeform/named-ink controls, connected/folded mesh export,
richer masks/patterns, symbols, packaging and general overprint are later
extensions, tackled separately with their present refusals retained.
PDF spot separations, PDF/X, full press soft proofing and physical print matching
are unfinished or unqualified; this v1 plan does not certify press output.

For detailed evidence, use the feature references and dated history:

<a id="writer-recipe-named-and-multiple-artboards-verified-2026-10-02"></a>

- [Native container and artboard recipe](PROOF-ai-interchange-history.md#writer-recipe-named-and-multiple-artboards-verified-2026-10-02).

<a id="patterns-written-2026-10-21-after-srgb-gradients"></a>

- [Pattern format and qualification](PROOF-ai-interchange-history.md#patterns-written-2026-10-21-after-srgb-gradients).

<a id="opacity-masks-written-2026-10-22-after-patterns"></a>

- [Opacity-mask format and qualification](PROOF-ai-interchange-history.md#opacity-masks-written-2026-10-22-after-patterns).

<a id="transparency-and-blend-modes-in-the-modern-form-2026-10-21"></a>

- [Transparency records and remaining rendering limits](PROOF-ai-interchange-history.md#transparency-and-blend-modes-in-the-modern-form-2026-10-21).

<a id="editable-mesh-patches-written-2026-10-22"></a>

- [Mesh format, bounds and Illustrator qualification](PROOF-ai-interchange-history.md#editable-mesh-patches-written-2026-10-22).
- [Document colours](PROOF-document-colors.md), [spot inks](PROOF-spot-inks.md)
  and [native records](PROOF-ai-native-records.md).

Evidence under `artifacts/` is local to the machine that produced it and is not
part of the public repository. Artwork checks use copies; public reports refer
to artwork by a short content hash or generic description.
