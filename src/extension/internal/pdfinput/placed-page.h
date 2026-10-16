// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a placed PDF page, for the screen.
 *
 * Illustrator places a PDF, or an .ai (of Illustrator 9 or later, whose files are PDFs too), as
 * a picture of one of its pages, cropped to one of its boxes, and keeps the page as it is: it
 * shows a picture of it and prints the page itself. PROOF does the same, for the first page
 * cropped to its crop box (what every placement read so far shows):
 *
 * - On screen the crop box is drawn by Poppler, the renderer that also draws the page an .ai
 *   is checked against, transparent where the page draws nothing. The drawing of a file is
 *   kept for its next placement: one file placed twelve times is drawn once.
 * - In print the page goes into the PDF as it is, its spot inks, pictures and type untouched
 *   (pdfoutput/placed-pages.h).
 */
#ifndef SEEN_EXTENSION_INTERNAL_PDFINPUT_PLACED_PAGE_H
#define SEEN_EXTENSION_INTERNAL_PDFINPUT_PLACED_PAGE_H

#include <cairo.h>
#include <optional>
#include <string>
#include <string_view>

#include <2geom/rect.h>

namespace Inkscape::Extension::Internal {

/// The longest side, in pixels, of a placed page drawn for the screen; smaller pages are drawn
/// at 300 pixels an inch.
inline constexpr double PLACED_PAGE_PIXELS = 4096;

/// Whether `data` starts as a PDF does (an .ai of Illustrator 9 or later too).
bool looks_like_pdf(std::string_view data);

/// The crop box (PDF units, y up) of the first page of the PDF at `path` (UTF-8), or nothing
/// with the reason in `why`: it can't be read, has no page, or its first page is turned.
std::optional<Geom::Rect> placed_page_crop(std::string const &path, std::string *why = nullptr);

/// The first page of the PDF in `data`, its crop box drawn for the screen: a premultiplied
/// ARGB32 surface the caller owns. Nothing when it isn't a PDF or can't be drawn.
cairo_surface_t *placed_page_surface(std::string_view data);

} // namespace Inkscape::Extension::Internal

#endif
