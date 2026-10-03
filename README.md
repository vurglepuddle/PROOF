# PROOF

Vector & Print Editor with ICC Color Management and Illustrator Interchange

**We have our own Professional Industry Standard at home.**

PROOF is a heavily modified [Inkscape](https://inkscape.org/) fork focused on
professional vector, print, and production workflows. We're proud of those roots
and grateful to the Inkscape community. PROOF is developed independently, with a
cleaner workspace and a focus on precise color, practical editing, and dependable
file exchange.

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
- **Illustrator interchange.** Open PDF-compatible `.ai` artwork and export an
  Illustrator-readable copy through **Save a Copy**. SVG remains the working
  document format.
- **Editing and output fixes.** Smoother object dragging on Windows, corrected
  PDF bleed boxes, and fixes for image transforms and SVG profile imports.

## Still in progress

**Full native `.ai` support is a goal, not a finished feature.** Current `.ai`
export is PDF-based RGB interchange: it does not preserve CMYK separations,
spot-ink identity, native layers, or Illustrator live effects. Modern `.ai`
artwork without PDF-compatible content is not supported yet, and complex text,
masks, and blends can change on import.

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
