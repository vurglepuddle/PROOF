// SPDX-License-Identifier: GPL-2.0-or-later
#include "capypdf-version.h"
#include <document.hpp>
#include <generator.hpp>

namespace capypdf::proof {
bool set_pdf17(CapyPDF_DocumentProperties *properties)
{
    if (!properties) return false;
    auto *doc = static_cast<internal::DocumentProperties *>(properties);
    if (!std::holds_alternative<std::monostate>(doc->subtype)) return false;
    doc->pdf_version = internal::PdfVersion::v17;
    return true;
}

CapyPDF_SeparationId add_icc_separation(CapyPDF_Generator *generator, std::string_view name, CapyPDF_IccColorSpaceId icc,
                                        CapyPDF_FunctionId tint_transform)
{
    auto fail = [](internal::ErrorCode ec) { throw capypdf::PdfException(capy_error_message((CapyPDF_EC)ec)); };
    if (!generator) fail(internal::ErrorCode::ArgIsNull);
    auto ascii = internal::asciistring::from_view(name);
    if (!ascii) fail(ascii.error());
    auto made = static_cast<internal::PdfGen *>(generator)->create_icc_separation(ascii.value(), icc, tint_transform);
    if (!made) fail(made.error());
    return made.value();
}
}
