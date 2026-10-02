# PROOF spot inks

Spot inks (Pantone, RAL, Oracal, CutContour and the like) are named inks in a
PROOF document, not flattened colours. First slice: 2026-10-02.

## What works now

- **Opening `.ai` and PDF files keeps spot inks.** Each ink becomes a document
  swatch with its exact name. Objects link to it, so they stay linked. Tints keep
  their percentage, so "PANTONE 185 C 30%" is its own swatch. Previously every
  spot ink became a plain RGB colour.
- **The definition matches the source.** Illustrator defines Pantone inks in Lab,
  and PROOF keeps that Lab value. Inks from RGB libraries (RAL) keep their RGB
  values, and inks defined in CMYK stay CMYK. Unusual definitions fall back to
  CMYK.
- **`.ase` swatch libraries keep the spot flag.** Pantone, RAL and Oracal entries
  saved as spot swatches stay spot. Clicking one applies it as a linked spot ink
  and adds it to the document's swatches on first use.
- **`.acb` colour books** (Photoshop's Pantone books) are spot books when the book
  says so (`spflspot`). Every colour in them is then a spot ink.
- Spot swatches show **Illustrator's spot marker**, a white corner triangle with
  a dot, in the palette bar and the Swatches panel.
- **`.ai` swatch libraries open as palettes** (2026-10-03). Put them in the
  palettes folder, or use the Swatches panel's open button. You get Illustrator's
  swatch order and groups, global colours as plain colours, and spot inks with
  their Lab or RGB definitions. Tint swatches ("PANTONE 2004 CP 50%") apply at
  their tint. Registration, gradient and pattern swatches are skipped.
- **Editing a spot swatch edits its ink** (2026-10-03). Change its colour in Fill
  & Stroke or the Properties panel, and the ink's definition changes; all its
  tint swatches follow. Editing a tint swatch works out the full-strength ink
  for you. The definition keeps its colour space, so a Lab Pantone nudged in an
  RGB picker stays Lab. Renaming renames the ink on all its swatches, keeping
  "NN%" on tints.
- **The swatch editor has a "Spot ink" switch**, Illustrator's Colour Type:
  Spot/Process. Turn it on to make your own inks (CutContour, white, varnish),
  or off to make an ink an ordinary colour. A short line says which ink, tint
  and colour space you are editing.

Not yet:
- **Exporting spot inks.** PDF output still writes them as their screen colour.
  The native `.ai` writer is not built.
- **Choosing a new tint of a spot ink from a slider.**

## How a spot ink is stored

A spot ink is an ordinary solid Inkscape swatch, a single-stop `linearGradient`
with `inkscape:swatch="solid"`, with three attributes in the PROOF namespace
(`xmlns:proof="urn:x-proof:document:1"`, provisional):

This is a real one, imported from `Giant File.ai`:

```xml
<linearGradient id="ink-pantone-475-c-t35" inkscape:swatch="solid"
                inkscape:label="PANTONE 475 C 35%"
                proof:ink="PANTONE 475 C"
                proof:ink-alternate="lab(85.882 10 19)"
                proof:ink-tint="0.35">
  <stop offset="0" style="stop-color:lab(95.059 3.5 6.65);stop-opacity:1"/>
</linearGradient>
```

| Attribute | Meaning |
|---|---|
| `proof:ink` | The exact ink name. Separations and the native `.ai` `x`/`Xx` operators use this string. |
| `proof:ink-alternate` | The full-strength appearance, in the space the source used. |
| `proof:ink-tint` | 0–1; absent means 1. Each tint is its own self-contained swatch, so copy-paste between documents cannot orphan it. |
| stop colour | The tinted alternate. Stock Inkscape and browsers render it correctly without knowing about spot inks. |

**Tint display.** CMYK inks scale with the tint. Lab, RGB and Gray blend toward
paper white, which is the PDF Separation convention: tint 0 is
`lab(100 0 0)`, as Illustrator writes it. Code: `src/spot-ink.{h,cpp}`.

Objects use `fill:url(#ink-…)`. Two sources may define the same name
differently. Each definition then gets its own swatch with a numbered id. Both
share `proof:ink`, so they print on one plate. Reconciling the two definitions
is a future UI step.

## Where it happens

| Source | Code | Behaviour |
|---|---|---|
| PDF / PDF-based `.ai` | `extension/internal/pdfinput/svg-builder.cpp`, `_convertSeparation` | Separation colours: ink name, alternate from the PDF tint function evaluated at 1, and tint rounded to 4 decimals (Poppler gives 16-bit fractions). It looks through `ICCBased` alternates to the profile's data space, because Illustrator wraps every ink definition in an ICC profile: Lab for Pantone, the document RGB profile for RGB libraries such as RAL. Lab, RGB, CMYK and Gray values are kept as the library defined them. `/None` paints nothing. Fill and stroke link the swatch; gradient stops get the tinted colour. The import option "convert colours" still converts everything as before. |
| `.ase` | `ui/dialog/global-palettes.cpp` | Type 1 (spot) becomes `PaletteFileData::SpotColor`. Types 0 (global) and 2 (normal) stay plain colours. Lab entries store L as 0–1 but a/b as real values. Inkscape passed a/b through unscaled, which scrambled every Lab `.ase` (ORACAL white showed as cyan). Fixed 2026-10-02. |
| `.acb` | same | A trailing `spflspot` marks the whole book as spot. |
| `.ai` swatch library | `extension/internal/pdfinput/ai-private-data.cpp`, `ui/dialog/ai-palette.cpp` | Poppler finds page 1's `PieceInfo/Illustrator/Private` streams, which are decompressed (AI24 Zstandard or AI12 zlib, bounded) only as far as `%AI5_EndPalette`. Illustrator 8 PostScript files are read directly. The palette section is tokenised, never executed: `k`/`g`/`Xa` process colours, `Xk` global colours, `Xx` spot inks (type 0 CMYK, 1 RGB, 2 Lab; the tint operand is inverted), `Pg` groups. For `%AI17` versioned blocks only the current branch is used. Names may be UTF-16, UTF-8 or Windows-1252. |
| Palette click | `ui/dialog/color-item.cpp` | Spot entries call `SpotInk::ensure()` and apply `url(#id)`. A spot marker is drawn for palette spot entries and for document swatches carrying `proof:ink`. |

## Evidence

- `test_spot-ink`: 17/17 tests (2026-10-03). The first eight cover id stems, CMYK and Lab tint maths, swatch reuse
  and tint separation, same-name conflicts, a save-and-reload round trip with
  the namespace, and hand-built `.ase` and `.acb` files. Later tests add the
  ORACAL Lab `.ase` case, `.ai` swatch libraries, inverse tint maths,
  editing a tint redefining the whole ink (other inks untouched),
  definition-space preservation, rename with tint labels, and the Spot/Process
  switch. The editing tests drive the same functions the editors call.
- Real files imported with the built PROOF (`tools/check-spot-import.py`;
  workspace `artifacts/spot-import-check-2.txt`):
  - **Spike file:** `CutContour` plus `PROOF Spot Red` at 100% and 30%.
  - **Pantone.ai:** 4 Pantone inks with Illustrator's Lab values, exactly
    (`PANTONE 2004 CP` = `lab(87.843 0 59)`).
  - **Giant File:** 18 inks, 3,198 linked paints, real tints (`PANTONE 1915 C`
    20%, `PANTONE 475 C` 35%), and `PANTONE 367 CVC` correctly kept as its CMYK
    definition.
  - **Typical.ai:** no spot inks, so nothing was created.
- Screen colours of the imported Pantone squares match Poppler's rendering of
  the original to within 1/255 per channel.
- The spot marker was checked in a screenshot of the running build. The
  palette-click path is covered by the swatch-module tests but **has not yet
  been clicked through in the GUI**.

**`.ai` swatch libraries:** `AiPaletteTest` (3 tests) uses records modelled
on Illustrator 29 output and generated Zstandard and zlib fixtures
(`tools/make-ai-swatch-fixtures.py`). Against Illustrator 29.2.1's own Swatches
panel, read through COM (`tools/illustrator-swatch-list.ps1`):

| File | Swatches | Order | Spot flags | Groups | CMYK values |
|---|---|---|---|---|---|
| `KZ_Colors.ai` | 11/11 | same | same | same | identical |
| `Difficult_Spot.ai` | 104/104 | same | 31/31 | same | identical, up to 3-decimal rounding |

The only swatches PROOF leaves out are gradients and patterns. Illustrator does
not open the generated fixtures, because they hold only a palette. Writing `.ai`
swatch files that Illustrator opens is future writer work.

## Known gaps, in suggested order

1. Editing is now ink-aware through the swatch functions every editor calls
   (`sp_change_swatch_color`, `sp_rename_swatch`). It is still possible to
   change a swatch's stop directly with the Gradient tool, which bypasses this,
   so the export should treat `proof:ink-alternate` as authoritative. The
   Swatches panel's right-click "Edit…" still opens Fill & Stroke or the
   Gradient tool rather than a dedicated ink dialog.
2. **Export.** CapyPDF PDF output should write Separation colour spaces from
   these swatches. The native `.ai` writer should emit `x`/`Xx` with the inverted
   AI tint (operand = 1 − tint). Cairo output can only use the display colour.
3. **DeviceN** (several inks mixed in one colour, e.g. in Giant File) still
   falls back to CMYK, with the importer warning. The registration ink `/All` is
   currently just a spot named "All".
4. **Overprint** from PDF graphics states is not imported. SVG has no property
   for it yet. The CutContour workflow needs it.
5. Tint picking in the UI, and Illustrator-style global process swatches (ASE
   type 0) as linked swatches.
