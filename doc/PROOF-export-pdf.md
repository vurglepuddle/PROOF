# Export PDF window

PROOF writes PDF through its own **Export PDF** window, laid out as Illustrator's Save Adobe PDF
dialog and filled one option family at a time. The owner, 2026-10-24: "Maybe with a specific
export window for pdf, similar to adobe pdf export (but obviously, for PROOF)". Owner's verdict on
the first version, 2026-10-25: "We want a clean, robust menu doing it's own thing, this looks like
one."

## The window (2026-10-25)

`src/ui/dialog/export-pdf-dialog.{h,cpp}`. Saving or saving a copy as PDF opens it in place of the
generated options dialog; its buttons are **Save PDF** and **Cancel**. In the Export panel (and
its batch rows) the PDF format's gear holds one button, **PDF Options…**, which opens the same
window with **OK**; the panel's export then uses the stored values without a window each time.
The command line never opens it.

- Top: "PDF Preset:" (`[PROOF Default]`, a placeholder), "Standard:" (None, a placeholder) and
  **Compatibility**: "Acrobat 8 (PDF 1.7)" or "PDF 2.0".
- Sections, as Illustrator lists them: General (a description of the compatibility), Compression,
  Marks and Bleeds, Output (each says what will come there; the bleed is each page's own, set with
  the Page tool), Advanced, Security (passwords and permissions aren't offered: a note only, by the
  owner's choice), Summary ("Summary!" when there are warnings).
- Advanced holds what the writer could already do: Fonts, Text: Embed fonts, Convert to outlines,
  or Omit text and write a LaTeX file (`textToPath`); Effects: Rasterize filter effects at N ppi
  (`blurToBitmap`, `resolution`).
- Values are staged in `PdfExportSettings` and written to the extension's parameters only when the
  window is accepted; Cancel writes nothing and cancels the save.

The hook: `Output::prefs()` overrides `Extension::prefs()`. Without a GUI, or with the extension's
GUI flag off (command line, scripted export), it opens nothing; otherwise it asks the
implementation's `output_prefs()` (`nullopt` keeps the generated dialog). Only the CapyPDF writer
(`PdfOutput`) answers it, and the writer itself never opens a window: native `.ai` saving calls it
with its own hidden options.

## Versions

PDF 1.7 is the default (owner, 2026-10-25: "PDF 1.7 (Recommended)"): Illustrator 29.2.1 opens it,
and refuses the PDF 2.0 PROOF wrote before. PDF 2.0 stays a choice. 1.7 is offered only with PROOF's
CapyPDF overlay (`PdfOutput::supports_pdf17()`), which sets the engine's version before writing.
At startup `PdfOutput::init()` turns a saved `PDF-2.0` into `PDF-1.7` once (the old dialog offered
nothing else), marked by `/dialogs/export/pdf/compatibility_migrated`, so a later choice of 2.0
stays.

The command line stays as it was unless asked: CapyPDF 2.0, Cairo 1.4, the older `.ai`
interchange 1.5. `--export-pdf-version=1.7` now writes 1.7 (the option check used to refuse it and
fall back to 1.4 silently); a version the chosen writer can't write fails before writing, the
destination untouched.

CapyPDF writes nothing at 1.7 that 1.7 lacks (a static audit of the pinned source with PROOF's
overlay: UTF-8 strings, associated files, `/UseBlackPtComp`, 2.0 structure types and PRC are
there but unreachable from PROOF's output). The pages of placed PDFs are copied in as they are,
so at 1.7 their objects are checked first (`placed_pages_pdf17_compatible`): `/UseBlackPtComp`,
`/AF` or `/AFRelationship` refuse the export by name, before anything is written ("a placed PDF
uses PDF 2.0 features"). A UTF-8 string is fine: Poppler writes it as UTF-16BE. A refused export
no longer deletes a file of that name it never wrote.

## Checks

- `test_pdf-export-settings` (8): staging, apply, cancel, the startup migration, no window
  without a GUI or with the extension's GUI off. `test_extensions-pdfoutput` 5/5; native `.ai`
  option isolation still passes.
- `testfiles/cli_tests/pdf-version-smoke.py` (54): default 2.0, explicit 1.7, the two drawn alike,
  strict parsing, an unsupported version refused with the destination untouched, Cairo's versions,
  placed pages carrying each 2.0 construct refused at 1.7 and taken at 2.0, a UTF-8 string written
  as UTF-16BE, a placed 1.x page taken at 1.7. `placed-page-export-smoke.py` 69,
  `ai-native-output-smoke.py` 38.
- Illustrator 29.2.1 opens PROOF's PDF 1.7: a CMYK control with three spot inks (kept as spots),
  an RGB control with pictures, a placed page, and a drawing of the owner's saved from the window
  ("It opens directly in Illustrator!").

## Found, open: type comes into Illustrator as outlines

Illustrator opens a CapyPDF PDF's text as paths, not type; the same drawing from the Cairo writer
opens with its text editable. Ruled out one at a time, each variant drawing identically: the font
in `/FontFile3 /OpenType` rather than `/FontFile2` (moved: still paths), the page's isolated
transparency group (removed: still paths), the form holding the page (drawn on the page itself:
still paths). Left: the font as CapyPDF writes it, a CID font by glyph ids whose subset keeps only
`cmap`, `glyf`, `head`, `hhea`, `hmtx`, `loca` and `maxp`. Native `.ai` saving isn't affected:
Illustrator reads its records.

Poppler also warns "Mismatch between font type and embedded font file" for every TrueType font
CapyPDF embeds (`/FontFile3 /OpenType`, valid from PDF 1.6); PDF 1.3 to 1.5 and PDF/X-1a/-3 will
need `/FontFile2`.

## Next

The owner's list for this window and the order: workspace `brief/export-pdf-window.md`.
