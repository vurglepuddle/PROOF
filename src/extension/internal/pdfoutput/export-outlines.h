// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_EXTENSION_PDF_EXPORT_OUTLINES_H
#define INKSCAPE_EXTENSION_PDF_EXPORT_OUTLINES_H

#include <string_view>
#include "extension/export-report.h"

class SPDocument;

namespace Inkscape::Extension::Internal::PdfBuilder {

bool is_cut_name(std::string_view name);
void outline_for_export(SPDocument *document, bool text, bool strokes, ExportReport *report = nullptr);

} // namespace Inkscape::Extension::Internal::PdfBuilder

#endif
