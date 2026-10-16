# Placed PDF pages

Illustrator places a PDF, or an `.ai` (of Illustrator 9 or later, whose files are
PDFs too), as a page: a picture of one of its pages on screen, the page itself
in print. Since 2026-10-17 PROOF does the same for a linked `.pdf` or `.ai`, in
an `.ai` it opens and in any drawing that links one.

The owner, on 2026-10-16, about linked Photoshop and Illustrator files: "yes,
they use photoshop more than me, unsurprising."

## On screen

The first page's crop box, drawn by Poppler (the renderer that also draws the
page an `.ai` is checked against), at 300 pixels an inch up to 4,096 pixels on
its longer side, transparent where the page draws nothing and without its
annotations. A file's drawing is kept for its next placement: one file placed
twelve times is drawn once (`src/extension/internal/pdfinput/placed-page.*`,
reached from `Pixbuf::create_from_own_pixels`).

## In print

CapyPDF, which writes PROOF's PDFs, makes everything it writes and can't take
a page from another PDF. So the export draws a stand-in where each placed page
goes, and replaces it once the file is written
(`src/extension/internal/pdfoutput/placed-pages.*`):

1. The stand-in is a form with the page's crop box, one per placed file, drawn
   at every placement with the matrix that puts the crop box on the picture's
   box. It names itself with a path drawn with nothing where no page draws
   (`7700001 7700001 1 1 re n`).
2. Once CapyPDF has written the file, Poppler reads it back and finds each
   stand-in. Each placed file's page resources are copied with every object
   they use, renumbered past the file's own, and otherwise as they are (Poppler's
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
by its size. A linked page is shown when the file reads as a PDF, its first
page is placed, that page isn't turned, and the placed box is the size of its
crop box. Otherwise the file falls back to its page with the reason: "a linked
page can't be shown: it is placed at its page 2, and only a first page is
shown yet (name)", "... it is cropped to another of its boxes than its crop box,
which isn't shown yet", "... it can't be read as a PDF" (an `.ai` of Illustrator
8 or older).

## Measured (2026-10-17)

The collaborator's `9047eb9b4e` (copies, with the resent links) places four
files 18 times: an `.ai` with a 29,970 by 4,249 DeviceN picture and an indexed
one of 31,205 by 2,716, 12 times; an `.ai` of 1,200 shapes in the spot ink
"226085 Kinder ORANGE", 4 times; a PDF with type in five embedded fonts and
the spot ink "Kinder ORANGE"; a PDF technical drawing with an RGB photograph.
All four pages are 1 and cropped to their crop box (`AI11 PlacedPDFCropTo` 3 in
each).

- It opens natively, 0.22% different from its page by lightness and 0.07% by
  colour (it fell back before, for its linked `.ai`).
- Exported to PDF: every picture at its own size and colour space, both spot
  inks by name, the five fonts embedded; no stand-in left; read strictly and
  drawn by Poppler without complaint; 0.75% of pixels apart from the `.ai`'s
  own page at 20 dpi (thin lines).
- **Opening it takes about three minutes.** Drawing the four pages for the
  screen takes 10 seconds (each file once). The rest is the check against the
  file's own page: Poppler takes 28 seconds to draw that page even at 20 dpi,
  decoding the 127-megapixel picture each time, and the page's technical
  drawing (thin lines, 11.3% apart at first) needs the finer second drawing.

## Limits

- Only a first page, and only cropped to its crop box (in a drawing, a linked
  PDF always shows its first page's crop box).
- The `.ai` writer (Cairo) puts a placed page as the picture drawn for the
  screen, not as the page.
- On screen a page is drawn at most 4,096 pixels across: zoomed in far, it
  shows its pixels.
- Layers (optional content) of a placed page aren't listed in the exported
  PDF's catalog; readers show what they hold.
- A placed PDF that opens without a password but is encrypted is copied
  decrypted, its streams decoded (larger, the same content).
- Whether the appended update suits PDF/X hasn't been looked at (PROOF doesn't
  write PDF/X yet).

## Checks

- `AiNativeImportTest.ALinkedPdfPageShowsItsFirstPageCroppedToItsCropBox`: the
  page and its crop read from the placement, the picture drawn at 300 ppi with
  its colour; a second page, another box and a PostScript `.ai` refused with
  the reason.
- `testfiles/cli_tests/placed-page-export-smoke.py` (43 checks, in
  `tools\run-focused-suites.ps1`): two PDFs written there, one with a spot ink,
  CMYK numbers and type, one with a CMYK picture, the first placed twice, as
  PDF 2.0 and 1.4: one form for the file placed twice, its content, the spot ink
  by name and its CMYK, the font, the page's box, the picture's samples, no
  stand-in, an update appended, read strictly, drawn without complaint, and
  where each shows on the printed page and on the canvas.
