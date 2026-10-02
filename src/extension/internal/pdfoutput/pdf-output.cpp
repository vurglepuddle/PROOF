// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Provide a capypdf exporter for Inkscape
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "pdf-output.h"

#include "build-document.h"
#include "build-page.h"
#include "document.h"
#include "extension/db.h"
#include "extension/internal/latex-text-renderer.h"
#include "extension/output.h"
#include "extension/system.h"
#include "inkscape-version.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "path-chemistry.h"
#include "rdf.h"

namespace Inkscape::Extension::Internal {

bool PdfOutput::check(Inkscape::Extension::Extension *)
{
    return Inkscape::Extension::db.get("org.inkscape.output.pdf.capypdf");
}

/**
 * Save the PDF file
 */
void PdfOutput::save(Inkscape::Extension::Output *mod, SPDocument *doc, char const *filename)
{
    capypdf::DocumentProperties opt;

    auto root = doc->getRoot();
    if (!root) {
        g_error("Couldn't save PDF, no document root");
        return;
    }

    if (char const *title = rdf_get_work_entity(doc, rdf_find_entity("title"))) {
        opt.set_title(title);
    }
    if (char const *author = rdf_get_work_entity(doc, rdf_find_entity("creator"))) {
        opt.set_author(author);
    }
    if (char const *subject = rdf_get_work_entity(doc, rdf_find_entity("description"))) {
        // Nothing to set yet.
    }
    if (char const *keywords = rdf_get_work_entity(doc, rdf_find_entity("subject"))) {
        // Nothing to set yet.
    }
    if (char const *copyright = rdf_get_work_entity(doc, rdf_find_entity("rights"))) {
        // Nothing to set yet.
    }

    auto creator =
        Glib::ustring::compose("Inkscape %1 (https://inkscape.org)", Inkscape::version_string_without_revision);
    opt.set_creator(creator.c_str());

    // TODO: This API currently doesn't work well for us
    // opt.set_colorspace(CAPY_DEVICE_CS_CMYK);
    // opt.set_device_profile(DEVICE_COLORSPACE, icc_profile);

    auto pdf = PdfBuilder::Document(filename, opt);
    if (mod->get_param_bool("blurToBitmap")) {
        pdf.set_filter_resolution(mod->get_param_int("resolution"));
    }
    if (mod->get_param_optiongroup_is("textToPath", "paths")) {
        Inkscape::convert_text_to_curves(doc);
    } else if (mod->get_param_optiongroup_is("textToPath", "LaTeX")) {
        pdf.set_text_enabled(false);
        if (!latex_render_document_text_to_file(doc, filename, true)) {
            throw Inkscape::Extension::Output::save_failed();
        }
    }

    // Step 1. Render EVERYTHING in the document out to a single PDF TransparencyGroup
    // This allows the page "positions" to be stored by the offset of the group.
    auto group_ctx = PdfBuilder::ItemContext(pdf, root);
    auto drawing_id = pdf.add_group(group_ctx);

    // FUTURE: If in the future we want PDF files where the items on a page are rendered only
    // in the group for that page, then we have two mechanisms for page separation.
    //   a. Find out if two pages share the same objects, and if they do, use the same root-group
    //   b. Find out if an object is shared by two pages and force it to use a transparency group
    //      at the most useful level. i.e. all children are shared thus share the parent group.

    // Step 2. Enable pages for this document. It SHOULD be a copy by this stage
    auto &pm = doc->getPageManager();
    pm.enablePages();

    uint32_t page = 0;
    // Step 3. Tell the PDF where to draw that whole plate on the PDF pages
    for (auto const &svg_page : pm.getPages()) {
        auto pdf_page = PdfBuilder::PageContext(pdf, svg_page);

        if (!svg_page->isBarePage()) {
            pdf_page.set_pagebox(CAPY_BOX_BLEED, svg_page->getDocumentBleed());
            pdf_page.set_pagebox(CAPY_BOX_TRIM, svg_page->getDocumentRect());
            pdf_page.set_pagebox(CAPY_BOX_ART, svg_page->getDocumentMargin());
        }

        if (auto label = svg_page->label()) {
            pdf.set_label(page, label);
        }

        if (drawing_id) {
            pdf_page.paint_drawing(*drawing_id, doc->getRoot()->c2p);
        }
        // Page links / anchors / annotations are added in post processing.
        pdf_page.add_anchors_for_page(svg_page);
        pdf.add_page(pdf_page);
        page++;
    }
    try {
        pdf.write();
    } catch (std::exception &ex) {
        g_warning("Couldn't save pdf file: %s", ex.what());
    }
}

#include "../clear-n_.h"

void PdfOutput::init()
{
    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>Portable Document Format</name>\n"
            "<id>org.inkscape.output.pdf.capypdf</id>\n"
            "<param name=\"PDFversion\" gui-text=\"" N_("Restrict to PDF version:") "\" type=\"optiongroup\" appearance=\"combo\" >\n"
                "<option value='PDF-2.0'>" N_("PDF 2.0") "</option>\n"
                // No other value is supported at the moment
            "</param>\n"
            "<param name=\"textToPath\" gui-text=\"" N_("Text output options:") "\" type=\"optiongroup\" appearance=\"radio\">\n"
                "<option value=\"embed\">" N_("Embed fonts") "</option>\n"
                "<option value=\"paths\">" N_("Convert text to paths") "</option>\n"
                "<option value=\"LaTeX\">" N_("Omit text in PDF and create LaTeX file") "</option>\n"
            "</param>\n"
            "<param name=\"blurToBitmap\" gui-text=\"" N_("Rasterize filter effects") "\" type=\"bool\">true</param>\n"
            "<param name=\"resolution\" gui-text=\"" N_("Resolution for rasterization (dpi):") "\" type=\"int\" min=\"1\" max=\"10000\">96</param>\n"
            "<output is_exported='true' priority='4'>\n"
                "<extension>.pdf</extension>\n"
                "<mimetype>application/pdf</mimetype>\n"
                "<filetypename>PDF (*.pdf)</filetypename>\n"
                "<filetypetooltip>PDF File</filetypetooltip>\n"
            "</output>\n"
        "</inkscape-extension>", std::make_unique<PdfOutput>());
    // clang-format on
}

} // namespace Inkscape::Extension::Internal
