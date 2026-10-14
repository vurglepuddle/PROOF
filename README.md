# PROOF

Vector & Print Editor with ICC Color Management and Illustrator Interchange

**We have our own Professional Industry Standard at home.**

PROOF is a heavily modified [Inkscape](https://inkscape.org/) fork focused on
professional vector, print, and production workflows. We're proud of those roots
and grateful to the Inkscape community. PROOF is developed independently, with a
cleaner workspace and a focus on precise color, practical editing, and dependable
file exchange.

![PROOF's window: a short tool strip on the left, a document in the middle, and the Properties, Stroke and Layers panels on the right](doc/screenshots/proof-workspace.png)

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
  against its PDF pages, and export an Illustrator-readable copy through
  **Save a Copy**. SVG remains the working document format.
- **Editing and output fixes.** Smoother object dragging on Windows, corrected
  PDF bleed boxes, and fixes for image transforms and SVG profile imports.

## Illustrator .ai progress

**Verified on 2026-10-14:** 858 of 879 private corpus files open from
Illustrator's native records; 21 use their PDF pages. This round gained
4 native imports and lost none. The corpus contains real documents; source
names, filenames, artwork and individual reports are kept private.

| Capability | Current state |
|---|---|
| Native reading | Layers, paths, clipping, compound paths, gradients, patterns, bounded gradient meshes and greyscale opacity masks, process/spot colours, document ICC profiles, images, linked-image previews and supported editable type |
| Safety check | Every artboard is compared with its PDF page. Unsupported or incomplete records, mismatched page counts and excessive visual differences use PDF import |
| Editability | 508 native imports are explicitly partial; 350 pass the visual check but have not been certified for complete editability |
| Writing `.ai` | Save a Copy writes PDF-based RGB interchange. Native Illustrator records, CMYK/spot separations and a complete editable return trip are still planned |

Opacity masks now support the linked, non-inverted greyscale vector subset:
Clip on or off, fills/strokes, linear/radial gradients, nested masks and
Normal/Multiply blending. Unsupported mask forms retain PDF fallback. Mask
regions are finite; geometry edits beyond the imported extent can require the
region and white backdrop to be enlarged. The PDF exporter also retains nested
mask coverage and avoids a crash on ICC-tagged gradient colours.

Four-corner mesh patches remain editable mesh gradients.
Uniform patches with extra boundary nodes retain their exact outlines as paths
and are reported as partial imports. Three more complex mesh files still use
PDF fallback. Other remaining families include foreign-format
objects, advanced type, symbols and unreadable images. Brushes, blends,
compound shapes and many appearances currently keep their drawn result rather
than all of Illustrator's live controls.

For agents and contributors, start with the
[native-record reference and implementation history](doc/PROOF-ai-native-records.md)
and [interchange/export limits and writer plan](doc/PROOF-ai-interchange.md).
The [native reader and importer](src/extension/internal/ai/) build the editable
document; [the PDF comparison and fallback](src/extension/internal/pdfinput/ai-native-open.cpp)
decide whether it is accepted. Regression fixtures are in
[the reader tests](testfiles/src/ai-native-reader-test.cpp) and
[the importer tests](testfiles/src/ai-native-import-test.cpp), with
[canvas/vector PDF mask checks](testfiles/cli_tests/ai-opacity-mask-smoke.py).

This milestone passed all nine relevant unit suites, the `.ai` export smoke
checks (19/19), blend/PDF export checks (32/32), opacity-mask export checks
(59/59), and one full 879-file corpus
regression. A visual pass is evidence of appearance, not a certificate of full
editability or a native Illustrator round trip.

## Still in progress

**Full native `.ai` support is a goal, not a finished feature.** Current `.ai`
export is PDF-based RGB interchange: it does not preserve CMYK separations,
spot-ink identity, native layers, or Illustrator live effects. Modern `.ai`
artwork without PDF-compatible content is not supported yet. Unsupported
native records use PDF fallback; many appearances and live objects currently
retain their drawn result with explicit import notes.

Spot inks can be imported and edited, but spot separations are not exported yet.
ICC-managed vector previews work; complete press soft proofing, overprint
simulation, and physical print matching remain unqualified. PROOF is in active
development, so check output on real jobs before relying on it for production.

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
