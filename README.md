# PROOF

Vector & Print Editor with ICC Color Management and Illustrator Interchange

**We have our own Professional Industry Standard at home.**

PROOF is a heavily modified [Inkscape](https://inkscape.org/) fork focused on
professional vector, print, and production workflows. We're proud of those roots
and grateful to the Inkscape community. PROOF is developed independently, with a
cleaner workspace and a focus on precise color, practical editing, and dependable
file exchange.

![PROOF with its editable launcher artwork open on the canvas, tools on the left and Properties, Stroke and Layers panels on the right](doc/screenshots/proof-workspace.png)

## V1 interchange milestone

**Qualified on 24 October 2026.** PROOF's first bounded native Illustrator
interchange milestone is complete: open supported artwork, edit it, and save
fresh native `.ai` records together with their PDF pages.

| Copied artwork qualification | Result |
|---|---|
| Native imports | **862 of 871 documents** |
| Complete modern `.ai` saves and native reopening | **862 of 862 documents**, covering **1,330 PDF pages** |
| Existing PDF fallbacks | **9 documents**, unchanged |

All copied input hashes remain unchanged. Representative Illustrator controls
qualify editability separately. See the [qualification and limits](doc/PROOF-ai-interchange.md#qualification-and-next-work).

## What's changed

- **A workspace focused on prepress.** Compact dark chrome, a reduced tool strip,
  contextual Properties, and dockable Stroke and Layers panels.
- **Document ICC color management.** RGB and CMYK working spaces, profile
  assignment and conversion, rendering intents, and black-point compensation.
  Embedded profiles and process channels survive PROOF SVG saves.
- **CMYK editing that keeps your numbers.** Color controls and copy/paste
  preserve process channels during editing.
- **Spot inks and swatch libraries.** Named inks, linked tints, and `.ase`,
  `.acb`, and `.ai` palettes.
- **Native Illustrator interchange.** Supported layers, paths, inks, gradients,
  transparency, patterns, meshes, masks, pictures and type remain editable.
- **Editable type and live PDF text.** Point, area and ordinary path type,
  bounded Arabic/Hebrew, and supported variable-font instances. PDF text stays
  selectable and searchable, including supported CFF and variable fonts.
- **Editing and output fixes.** Smoother object dragging on Windows, corrected
  PDF bleed boxes, and fixes for image transforms and SVG profile imports.

## Illustrator file exchange

PROOF reads and writes its supported native subset without Illustrator installed.
Imported artboards are compared with their PDF pages; unsupported or visibly
different content uses PDF import.

| Route | Current support |
|---|---|
| **Open native `.ai`** | Supported editable artwork and type, with PDF fallback |
| **Save As: Adobe Illustrator (native)** | Fresh editable native records with PDF 1.7 pages |
| **Save a Copy: Adobe Illustrator 7** | A limited opaque subset on one artboard; no PDF preview or retained document ICC profile |
| **Save a Copy: Illustrator interchange (PDF-based)** | Broader RGB PDF artwork; native layers and print-ink identity are not retained |

Unsupported native output is refused by name, preserving the previous file.
Keep SVG as the working master for features the writer cannot save yet.
Route limits, CLI options, rendering behavior and Illustrator-version checks
are in the [interchange reference](doc/PROOF-ai-interchange.md).

## Still in progress

Advanced type, gradients/meshes, richer masks/patterns, symbols, link packaging
and general overprint follow this milestone one feature at a time. Modern AI
without usable PDF-compatible content remains unsupported.

PDF spot plates, PDF/X, full press soft proofing and physical print matching
remain separate prepress work. The v1 milestone qualifies the documented
interchange subset.

## More information

- [Document color management](doc/PROOF-document-colors.md)
- [Spot inks](doc/PROOF-spot-inks.md)
- [AI interchange and its limits](doc/PROOF-ai-interchange.md)
- [Type and fonts](doc/PROOF-type.md)
- [Placed PDF/AI pages](doc/PROOF-placed-pages.md)
- [Native records and import performance](doc/PROOF-ai-native-records.md)
- [Building from source](doc/building/readme.md) — inherited Inkscape build guides;
  local build scripts and dependencies may differ.
- [Inkscape](https://inkscape.org/) · [Upstream source](https://gitlab.com/inkscape/inkscape)

PROOF is free and open source. Thanks to Inkscape and the many projects behind
our tools and format research: see [Credits](CREDITS.md) and the inherited
[AUTHORS](AUTHORS) list. Licensing is documented in [COPYING](COPYING).
