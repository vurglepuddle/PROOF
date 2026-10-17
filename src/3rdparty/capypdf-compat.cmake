# SPDX-License-Identifier: GPL-2.0-or-later
# PROOF's small overlay for the pinned CapyPDF: select PDF 1.7 per document.
# Keep the submodule untouched, and fail on an upstream change needing review.
set(CAPYPDF_PROOF_DIR "${CMAKE_CURRENT_BINARY_DIR}/capypdf-proof")
file(MAKE_DIRECTORY "${CAPYPDF_PROOF_DIR}")

function(proof_capypdf_overlay name needle replacement)
    set(source "${CMAKE_CURRENT_LIST_DIR}/capypdf/src/${name}")
    file(READ "${source}" text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(FIND "${text}" "${needle}" first)
    string(FIND "${text}" "${needle}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "Review PROOF's CapyPDF version overlay: ${name} changed")
    endif()
    string(REPLACE "${needle}" "${replacement}" text "${text}")
    file(CONFIGURE OUTPUT "${CAPYPDF_PROOF_DIR}/${name}" CONTENT "${text}" @ONLY)
endfunction()

proof_capypdf_overlay(document.hpp
    "std::variant<std::monostate, CapyPDF_PDFX_Type, CapyPDF_PDFA_Type> subtype;"
    "std::variant<std::monostate, CapyPDF_PDFX_Type, CapyPDF_PDFA_Type> subtype;\n    // PROOF: explicit compatibility for ordinary documents; PDF/X and PDF/A retain their versions.\n    std::optional<PdfVersion> pdf_version;")
proof_capypdf_overlay(document.cpp
    "return PdfVersion::v20;\n}\n\nbool DocumentProperties::use_rdf_metadata()"
    "return pdf_version.value_or(PdfVersion::v20);\n}\n\nbool DocumentProperties::use_rdf_metadata()")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/capypdf/src/document.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/capypdf/src/document.cpp")
