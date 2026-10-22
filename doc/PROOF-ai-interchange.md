# PROOF AI interchange

Current contract, 2026-10-22. PROOF reads supported editable Illustrator artwork
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

Supported does not mean arbitrary combinations of these forms. Important
explicit refusals include:

- Live type; unsupported appearances/filters; layer/root transparency;
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
byte-identical. **Appearance is not fully qualified:** the off-centre linear
interiors differ from Illustrator's 72-ppi PNGs by up to 23 RGB / 27 CMYK display
levels. Native page acceptance is not a PNG parity assertion. See the
[midpoint qualification limits](PROOF-document-colors.md#illustrator-306-return-qualification-2026-10-23).
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
elsewhere on the page are supported; a state also used to paint an image is left
unchanged. DeviceGray soft-mask
paint remains numeric coverage rather than being converted to print grey.

This is deliberately limited: at most 100 MB input, 256 pages, 8 MB of content
per page, complete known graphics states and exactly two shadings per marked
pair. Pages containing Form XObject calls or inline images, nested marked paints,
states shared with other artwork, other blend/overprint modes and other colour
space combinations retain existing handling. The optional Cairo-to-SVG importer
is unchanged: its compositing-filter bounds need separate work. Embedded foreign
PDF overprint remains outside that importer's bounded subset.

The Illustrator-saved RGB mixed-gradient control now opens natively; canvas
samples differ from Illustrator by at most 13 display levels and the default
PDF import by 11. The visual gate is unchanged. Both concurrent comparison
drawings use the same preview bytes; only corrected documents get a new cache
identity, preserving unrelated heavy-page cache entries.
[Implementation and controls](PROOF-ai-interchange-history.md#rgb-pdf-overprint-2026-10-22).
[Colour contract](PROOF-document-colors.md#an-ink-beside-process-colour-2026-10-22).

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

It refuses transparency, opacity masks, patterns, meshes, type, picture links,
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

Next: Oklab off-centre appearance parity and remaining extended-setting
controls; additional connected/folded mesh
qualification; then agree the native-type writing subset. Type remains last.
PDF spot separations, PDF/X, full press soft proofing and physical print matching
are unfinished or unqualified.

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
