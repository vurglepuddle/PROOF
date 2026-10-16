// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: placed PDF pages (linked .pdf and .ai files) into an exported PDF, as they are.
 *
 * Illustrator prints a placed page as the page itself. CapyPDF, which writes PROOF's PDFs, draws
 * only what it makes and can't take a page from another PDF. So the export draws a stand-in
 * where each placed page goes: a form that names itself in a path drawn with nothing, where no
 * page draws ("7700001.000000 7700001.000000 1.000000 1.000000 re n" for the first), its box
 * the page's crop box. Once CapyPDF has written the file, the stand-ins are replaced by their
 * pages in an update appended to it (a PDF's incremental update, with cross-references of the
 * kind the file has): each page's content
 * becomes its form's content, and its resources are copied from its file with every object
 * they use, renumbered and otherwise as they are. Spot inks, DeviceN pictures, type and
 * transparency go to print untouched. See pdfinput/placed-page.h for the screen.
 */
#ifndef SEEN_EXTENSION_INTERNAL_PDFOUTPUT_PLACED_PAGES_H
#define SEEN_EXTENSION_INTERNAL_PDFOUTPUT_PLACED_PAGES_H

#include <string>
#include <vector>

namespace Inkscape::Extension::Internal::PdfBuilder {

struct PlacedPage
{
    std::string file;   ///< The placed PDF, its first page (UTF-8).
    std::string marker; ///< What the stand-in's content holds and no other's does.
};

/// The stand-ins in the PDF at `path` replaced by their pages. When that fails the file is left
/// as it was written and `error` says why.
bool splice_placed_pages(std::string const &path, std::vector<PlacedPage> const &pages, std::string &error);

} // namespace Inkscape::Extension::Internal::PdfBuilder

#endif
