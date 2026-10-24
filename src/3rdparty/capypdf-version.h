// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_CAPYPDF_VERSION_H
#define PROOF_CAPYPDF_VERSION_H

#include <string_view>

#include <capypdf.hpp>

namespace capypdf::proof {
// Select the engine's PDF 1.7 mode for a document containing compatible features.
// Ordinary documents otherwise keep PDF 2.0; conformance modes cannot be overridden.
bool set_pdf17(CapyPDF_DocumentProperties *properties);

class DocumentProperties : public capypdf::DocumentProperties
{
public:
    bool set_pdf17() { return proof::set_pdf17(_d.get()); }
};

// A Separation for the ink `name` (ASCII), its alternate the ICC-based space `icc` and its tint
// transform the calculator function `tint_transform`. The C API has device alternates only.
// Throws capypdf::PdfException as the C++ wrapper does.
CapyPDF_SeparationId add_icc_separation(CapyPDF_Generator *generator, std::string_view name, CapyPDF_IccColorSpaceId icc,
                                        CapyPDF_FunctionId tint_transform);
}

#endif
