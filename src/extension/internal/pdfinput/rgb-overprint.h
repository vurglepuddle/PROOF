// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_EXTENSION_INTERNAL_RGB_OVERPRINT_H
#define INKSCAPE_EXTENSION_INTERNAL_RGB_OVERPRINT_H

#include <optional>
#include <string>

class PDFDoc;

namespace Inkscape::Extension::Internal {

/** A disposable PDF preview of Illustrator's bounded RGB MultiChannelPaint pairs.
 * Changes only page-local state dictionaries in `pdf`, never its source file.
 * Returns no bytes when there is no supported pair. This is appearance simulation,
 * not a replacement for the original PDF's separations or general overprint.
 */
std::optional<std::string> rgb_overprint_preview(PDFDoc &pdf);

} // namespace Inkscape::Extension::Internal
#endif
