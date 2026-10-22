# Placed PDF pages

Illustrator places a PDF, or an `.ai` (of Illustrator 9 or later, whose files are
PDFs too), as a page: a picture of one of its pages on screen, the page itself
in print. Since 2026-10-17 PROOF does the same for a linked `.pdf` or `.ai`, in
an `.ai` it opens and in any drawing that links one.

The owner, on 2026-10-16, about linked Photoshop and Illustrator files: "yes,
they use photoshop more than me, unsurprising."

## On screen

The selected page's crop box, drawn by Poppler (the renderer that also draws the
page an `.ai` is checked against), at 300 pixels an inch up to 4,096 pixels on
its longer side, transparent where the page draws nothing and without its
annotations. A page's drawing is kept for its next placement, keyed by file
checksum, size and page number: one page placed twelve times is drawn once
(`src/extension/internal/pdfinput/placed-page.*`,
reached from `Pixbuf::create_from_own_pixels`).

## In print

CapyPDF, which writes PROOF's PDFs, makes everything it writes and can't take
a page from another PDF. So the export draws a stand-in where each placed page
goes, and replaces it once the file is written
(`src/extension/internal/pdfoutput/placed-pages.*`):

1. The stand-in is a form with the page's crop box, one per file and selected page, drawn
   at every placement with the matrix that puts the crop box on the picture's
   box. It names itself with a path drawn with nothing where no page draws
   (`7700001 7700001 1 1 re n`).
2. Once CapyPDF has written the file, Poppler reads it back and finds each
   stand-in. Each placed file's page resources are copied with every object
   they use, once across all selected pages from that file, renumbered past the file's own,
   and otherwise as they are (Poppler's
   `writeObject`, which copies streams undecoded). Each stand-in is written
   again as a form of the page's content, its resources and its transparency
   group.
3. These are appended as an incremental update, with cross-references of the
   kind the file has: a stream after PDF 1.5, a table before. Nothing touches
   the file until the update is whole.

If that fails, the PDF isn't kept: the export fails and says why, since the
file would print without the placed pages.

So spot inks, DeviceN and indexed pictures, embedded fonts and the page's
transparency go to print as they were in the placed file.

## At `.ai` import

The placement's art dictionary gives the page (`AI11 PlacedPDFPageIndex`, from
1) and the box it is cropped to (`AI11 PlacedPDFCropTo`, a number whose meaning
isn't known). The placed box has the size of that page box, so the box is told
by its size. A linked page is shown when the file reads as a PDF, its selected
page exists, that page isn't turned, and the placed box is the size of its
crop box. Otherwise the file falls back to its page with the reason: "a linked
page can't be shown: it has no page 3 (name)",
"... it is cropped to another of its boxes than its crop box,
which isn't shown yet", "... it can't be read as a PDF" (an `.ai` of Illustrator
8 or older).

Since 2026-10-22 the imported image keeps `proof:pdf-page` (from 1; absent
means page 1). SVG save/reopen, duplicate placements and Undo/Redo
use that selection on screen and in PDF output. Invalid or unavailable page
numbers fail closed; PDF export fails rather than printing page 1 instead.
Rotated pages and other crop boxes remain unsupported.

## Measured (2026-10-17)

The collaborator's `9047eb9b4e` (copies, with the resent links) places four
files 18 times: an `.ai` with a 29,970 by 4,249 DeviceN picture and an indexed
one of 31,205 by 2,716, 12 times; an `.ai` of 1,200 shapes in a spot ink,
4 times; a PDF with type in five embedded fonts and a spot ink;
a PDF technical drawing with an RGB photograph.
All four pages are 1 and cropped to their crop box (`AI11 PlacedPDFCropTo` 3 in
each).

- It opens natively, 0.22% different from its page by lightness and 0.07% by
  colour (it fell back before, for its linked `.ai`).
- Exported to PDF: every picture at its own size and colour space, both spot
  inks by name, the five fonts embedded; no stand-in left; read strictly and
  drawn by Poppler without complaint; 0.75% of pixels apart from the `.ai`'s
  own page at 20 dpi (thin lines).
- **Historical opening time, before the 2026-10-22 fixes: about three minutes.** Drawing the four pages for the
  screen takes 10 seconds (each file once). The rest is the check against the
  file's own page: Poppler takes 28 seconds to draw that page even at 20 dpi,
  decoding the 127-megapixel picture each time, and the page's technical
  drawing (thin lines, 11.3% apart at first) needs the finer second drawing.

On the owner's machine on 2026-10-22, keeping slow reference drawings
(`b49ad078cb`) and drawing the two resolutions concurrently after 3 s
(`e79e3706f8`) brought first opening from 232 s to 137 s, later openings to
33.5 s, with the same 0.2% comparison. Illustrator took about 23 s. Poppler
decodes/converts the two huge pictures twelve times per page render, about
2.6 gigapixels. A custom Poppler remains unchosen.
[Page-check details](PROOF-ai-native-records.md#pages-slow-to-draw-weighed-once-and-kept-2026-10-22).

## Limits

- Only unrotated pages cropped to their crop box. A generic linked PDF defaults
  to page 1; imported Illustrator placements retain their selected page.
- The modern native `.ai` writer refuses linked PDF/AI pages explicitly;
  its supported links are raster pictures. The older Cairo `.ai` export puts
  a placed page as the picture drawn for the screen, not as the page.
- On screen a page is drawn at most 4,096 pixels across: zoomed in far, it
  shows its pixels.
- Layers (optional content) of a placed page aren't listed in the exported
  PDF's catalog; readers show what they hold.
- A placed PDF that opens without a password but is encrypted is copied
  decrypted, its streams decoded (larger, the same content).
- Whether the appended update suits PDF/X hasn't been looked at (PROOF doesn't
  write PDF/X yet).

## Checks

- `AiNativeImportTest.ALinkedPdfPageShowsTheSelectedPageCroppedToItsCropBox`: the
  page and its crop read from the placement, the picture drawn at 300 ppi with
  its colour; page 2 survives SVG save/reopen and changing the selection back
  and forth, with separate cached drawings; invalid/unavailable pages,
  another box and a PostScript `.ai` refused with the reason.
- `testfiles/cli_tests/placed-page-export-smoke.py` (69 checks, in
  `tools\run-focused-suites.ps1`): two PDFs written there, one with a spot ink,
  CMYK numbers and type and a second page with its own crop, CMYK picture and
  transparency group, one with a separate CMYK picture. Pages 1 and 2 are each
  placed twice, as PDF 2.0 and 1.4: one form per repeated page, resources
  shared across pages copied once, its content, the spot ink
  by name and its CMYK, the font, the page's box, the picture's samples, no
  stand-in, an update appended, read strictly, drawn without complaint, and
  where each shows on the printed page and on the canvas; SVG reopen and
  failed export of an unavailable page.
  Seven checks use independent native Illustrator records and a PDF page:
  page 2 passes the visual gate, survives SVG save/reopen, retains its original
  PDF vectors and crop, and draws its colours in position. `--native-only`
  runs just these seven, including for an installed-version check.

## Qualification of selected pages (2026-10-22)

Targeted importer/picture tests: 17 passed. The placed-page control passed
62 checks, then the new native-only control passed seven; the unchanged
CMYK TIFF/Photoshop control passed 91. Two selected lite cases retained their
expected outcomes. No full lite/corpus run or new census was made.

The collaborator's `e1143523ac` now gets past page 2 but correctly falls back
at page 6. Its decoded records contain 33 placements of pages 1 through 33
of the same linked PDF; the supplied companion (`3da18b676b`) has five pages.
All 21 copied inputs remained hash-identical in that check. The owner then
identified the 33-page project as a damaged version of the same catalogue.
The related large source, `e03227b6e7` (3.601 GB), was already in the excluded
review pack: its PDF has 30 pages, saved 2021-12-20; the damaged project was
saved 2021-12-10. The owner supplied a 62.1 MB, 30-page PDF, `179214c23a`,
byte-identical to the PDF already in the copied pack and sharing Adobe
document lineage with the large source. The owner clarifies that the 30 pages
are production files excluding the covers; the interior artwork should
otherwise be identical. Cover content and the mapping of production pages to
the older 33 placements have not been matched. Count and saved dates alone
do not establish an interior-artwork difference. Keep the existing fallback, with any
artwork repair separate; no new native acceptance is claimed. The large
copy was inspected through PDF metadata only and remains outside routine
checks. No asset was automatically substituted or renumbered.
