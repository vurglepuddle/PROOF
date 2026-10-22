# PROOF

Vector & Print Editor with ICC Color Management and Illustrator Interchange

**We have our own Professional Industry Standard at home.**

PROOF is a heavily modified [Inkscape](https://inkscape.org/) fork focused on
professional vector, print, and production workflows. We're proud of those roots
and grateful to the Inkscape community. PROOF is developed independently, with a
cleaner workspace and a focus on precise color, practical editing, and dependable
file exchange.

![PROOF with its editable launcher artwork open on the canvas, tools on the left and Properties, Stroke and Layers panels on the right](doc/screenshots/proof-workspace.png)

## What's changed

- **A workspace focused on prepress.** Compact dark chrome, a reduced tool strip,
  contextual Properties, and dedicated Stroke and Layers panels. Panels can dock,
  float, and retain their layout; Reset Workspace gets you back to the default.
- **Document ICC color management.** RGB and CMYK working spaces, profile
  assignment, mode conversion, rendering intents, and black-point compensation
  through LittleCMS 2. Embedded profiles and process-color channels survive
  saving and reopening in PROOF SVG.
- **CMYK editing that keeps your numbers.** Fixes for picker refresh,
  profile names, and copy/paste prevent unintended channel changes during editing.
- **Spot inks and swatch libraries.** Import named inks and tints from
  PDF-compatible `.ai` and PDF artwork; load `.ase`, `.acb`, and `.ai` palettes.
  Linked spot swatches retain their ink definitions, and editing an ink updates
  its tints.
- **Illustrator interchange.** Read supported native `.ai` artwork, checked
  against its PDF pages. **Save As** writes native layers, smooth paths, inks,
  transparency and blend modes, supported linear/radial gradient fills and
  strokes, patterns, supported editable mesh patches, greyscale opacity masks, embedded pictures and local
  raster links; **Save a Copy** also
  offers Illustrator 7 records and PDF-based RGB interchange.
  SVG keeps features the native writer cannot write yet.
- **Editing and output fixes.** Smoother object dragging on Windows, corrected
  PDF bleed boxes, and fixes for image transforms and SVG profile imports.

## Illustrator .ai progress

PROOF reads supported native Illustrator artwork and writes a growing editable
subset without Illustrator installed. Every imported artboard is compared with
its PDF page; unsupported or visibly different content uses PDF import. A
native pass can still carry explicit notes about reduced live editing controls.

| Route | Current support |
|---|---|
| **Open native `.ai`** | Layers, paths/clips, process/spot colours, profiles, gradients, patterns, supported meshes/masks, pictures and supported editable type |
| **Save As: Adobe Illustrator (native)** | Fresh editable records and PDF 1.7 pages: named artboards/layers, paths, inks/tints, supported linear/radial and Oklab gradients, patterns, independent editable mesh patches, grey vector opacity masks, object/group transparency and supported pictures/raster links |
| **Save a Copy: Adobe Illustrator 7** | A qualified opaque subset on one artboard, with Windows-1252 names; no PDF preview or retained document ICC profile |
| **Save a Copy: Illustrator interchange (PDF-based)** | Broader RGB PDF artwork; native layers and print-ink identity are not retained |

Unsupported native output is refused by name, preserving the previous file.
The modern writer requires PROOF's bundled PDF backend. Keep SVG as the working
master for features it cannot write yet, including live type. The
[current interchange reference](doc/PROOF-ai-interchange.md) gives each route's
limits and CLI selection; the [implementation history](doc/PROOF-ai-interchange-history.md)
retains the detailed recipes and Illustrator qualification.

Classic gradients and meshes in CMYK documents blend in CMYK through the
document profile. Mixed spot/process ramps follow Illustrator's overprint
appearance on the canvas and in process-colour PDF output. **Perceptual/Oklab**
gradients retain their editable stops and method; rendered CMYK colours follow
RGB/Oklab/profile re-separation, including endpoints and black generation.
Illustrator 29.2.1 opens their Classic alternate and loses Oklab on save;
Illustrator 30.6.0 retains the editable methods, stops, midpoints and CMYK
profile. The two returned controls qualify the gradient colour and opacity
fields, including off-centre midpoints. Low-resolution raster previews can
differ around a radial colour stop; measured limits are documented below.
[Colour and Oklab contract](doc/PROOF-ai-interchange.md#colour-and-rendering-contracts).

Linked CMYK TIFF and Photoshop pictures retain supported process channels,
depth and cut-out transparency in PDF output. Linked PDF/AI files retain their
selected page (first by default) as vectors in ordinary PDF output, with fonts
and inks; the native AI writer still refuses these page links.
[Placed pages](doc/PROOF-placed-pages.md).

Heavy native-import PDF checks cache hash-verified reference drawings and start
a finer drawing concurrently after 3 s. The qualified heavy case improved from
232 s to 137 s on first opening and takes 33.5 s subsequently.
[Measurements and cache rules](doc/PROOF-ai-native-records.md#pages-slow-to-draw-weighed-once-and-kept-2026-10-22).

The latest broad **historical** checkpoint, at `fb17863507` on 2026-10-22,
was 862 native imports from 871 copied files, modern writing 802/862 and legacy
writing 100. Those measure acceptance/readback, not complete Illustrator
editability. Later changes use targeted tests and copied representative cases;
the corpus is not rerun for each edit or agent round.

## Still in progress

Full native `.ai` support remains unfinished. Main writing limits include live
type, layer transparency, separate fill/stroke opacity, broader mask/pattern
forms, advanced meshes and additional linked formats. Modern AI without usable
PDF-compatible content is unsupported. Many imported live appearances retain
their drawn result with an explicit note.

The page comparison and default editable PDF importer now simulate the bounded
RGB `/MultiChannelPaint` ink/process pairs Illustrator writes, including on pages
with ordinary raster pictures. The qualified RGB control returns natively; general overprint and the optional Cairo-to-SVG import
remain outside this correction. Unknown extended gradient settings still fail
closed. Next are extended Oklab settings and additional mesh qualification;
native type writing stays last in the agreed order.

Native AI output retains supported spot names and tints, but PDF spot plates
and PDF/X remain separate work. Full CMYK transparency/knockout appearance,
press soft proofing, general overprint simulation and physical print matching
remain limited or unqualified. Validate production output for the intended job.

## More information

- [Document color management](doc/PROOF-document-colors.md)
- [Spot inks](doc/PROOF-spot-inks.md)
- [AI interchange and its limits](doc/PROOF-ai-interchange.md)
- [Building from source](doc/building/readme.md) — inherited Inkscape build guides;
  local build scripts and dependencies may differ.
- [Inkscape](https://inkscape.org/) · [Upstream source](https://gitlab.com/inkscape/inkscape)

PROOF is free and open source. Thanks to Inkscape and the many projects behind
our tools and format research: see [Credits](CREDITS.md) and the inherited
[AUTHORS](AUTHORS) list. Licensing is documented in [COPYING](COPYING).
