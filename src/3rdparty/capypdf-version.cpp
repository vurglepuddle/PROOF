// SPDX-License-Identifier: GPL-2.0-or-later
#include "capypdf-version.h"
#include <document.hpp>

namespace capypdf::proof {
bool set_pdf17(CapyPDF_DocumentProperties *properties)
{
    if (!properties) return false;
    auto *doc = static_cast<internal::DocumentProperties *>(properties);
    if (!std::holds_alternative<std::monostate>(doc->subtype)) return false;
    doc->pdf_version = internal::PdfVersion::v17;
    return true;
}
}
