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

#include <glib/gstdio.h>
#include <stdexcept>
#include <string_view>
#ifdef PROOF_CAPYPDF_PDF17
#include "3rdparty/capypdf-version.h"
#endif

#include "build-document.h"
#include "build-page.h"
#include "export-outlines.h"
#include "colors/document-colors.h"
#include "colors/spaces/enum.h"
#include "document.h"
#include "extension/db.h"
#include "extension/internal/latex-text-renderer.h"
#include "extension/output.h"
#include "extension/system.h"
#include "inkscape-version.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "preferences.h"
#include "rdf.h"
#include "ui/dialog/export-pdf-dialog.h"

namespace Inkscape::Extension::Internal {

bool PdfOutput::supports_pdf17()
{
#ifdef PROOF_CAPYPDF_PDF17
    return true;
#else
    return false;
#endif
}

bool PdfOutput::check(Inkscape::Extension::Extension *)
{
    return Inkscape::Extension::db.get("org.inkscape.output.pdf.capypdf");
}

std::optional<bool> PdfOutput::output_prefs(Inkscape::Extension::Output *module, bool settings)
{
    return UI::Dialog::export_pdf_dialog(*module, settings);
}

/**
 * Save the PDF file
 */
void PdfOutput::save(Inkscape::Extension::Output *mod, SPDocument *doc, char const *filename)
{
#ifdef PROOF_CAPYPDF_PDF17
    capypdf::proof::DocumentProperties opt;
#else
    capypdf::DocumentProperties opt;
#endif
    // Native Illustrator output admits only features compatible with PDF 1.7.
    // Select the engine's version before writing, rather than relabeling a PDF 2.0 file.
    if (mod->get_param_optiongroup_is("PDFversion", "PDF-1.7")) {
#ifdef PROOF_CAPYPDF_PDF17
        if (!opt.set_pdf17()) {
            throw std::runtime_error("The PDF version conflicts with its conformance mode.");
        }
#else
        throw std::runtime_error("This PDF backend cannot write Illustrator-compatible PDF 1.7.");
#endif
    }

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
    pdf.set_pdf17(mod->get_param_optiongroup_is("PDFversion", "PDF-1.7"));
    if (mod->get_param_bool("blurToBitmap")) {
        pdf.set_filter_resolution(mod->get_param_int("resolution"));
    }
    bool const strokes = std::string_view(mod->get_id()) == "org.inkscape.output.pdf.capypdf" &&
                         mod->get_param_bool("strokesToPath");
    PdfBuilder::outline_for_export(doc, mod->get_param_optiongroup_is("textToPath", "paths"), strokes);
    if (mod->get_param_optiongroup_is("textToPath", "LaTeX")) {
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
        // A CMYK document's transparency is composited in CMYK, as it prints and as
        // Illustrator's pages say.
        if (Colors::DocumentColors::mode(doc) == Colors::Space::Type::CMYK) {
            pdf_page.set_blending_space(CAPY_DEVICE_CS_CMYK);
        }

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
    } catch (PdfBuilder::PlacedPagesFailed const &ex) {
        // PROOF: a PDF without the pages placed in it isn't kept: it would print without them.
        g_warning("Couldn't put the placed pages into the PDF: %s", ex.what());
        if (ex.file_written) g_unlink(filename);
        throw Inkscape::Extension::Output::save_failed();
    } catch (std::exception &ex) {
        g_warning("Couldn't save pdf file: %s", ex.what());
        // PROOF: say so. Only warning left the save looking done with no file written.
        throw Inkscape::Extension::Output::save_failed();
    }
}

#include "../clear-n_.h"

void PdfOutput::init()
{
    auto prefs = Preferences::get();
    char const *migrated = "/dialogs/export/pdf/compatibility_migrated";
    if (supports_pdf17() && !prefs->getBool(migrated)) {
        char const *version = "/extensions/org.inkscape.output.pdf.capypdf.PDFversion";
        if (prefs->getString(version) == "PDF-2.0") {
            prefs->setString(version, "PDF-1.7");
        }
        prefs->setBool(migrated, true);
    }

    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>Portable Document Format</name>\n"
            "<id>org.inkscape.output.pdf.capypdf</id>\n"
            "<param name=\"PDFversion\" gui-text=\"" N_("Restrict to PDF version:") "\" type=\"optiongroup\" appearance=\"combo\" >\n"
#ifdef PROOF_CAPYPDF_PDF17
                "<option value='PDF-1.7'>" N_("Acrobat 8 (PDF 1.7)") "</option>\n"
#endif
                "<option value='PDF-2.0'>" N_("PDF 2.0") "</option>\n"
            "</param>\n"
            "<param name=\"textToPath\" gui-text=\"" N_("Text output options:") "\" type=\"optiongroup\" appearance=\"radio\">\n"
                "<option value=\"embed\">" N_("Embed fonts") "</option>\n"
                "<option value=\"paths\">" N_("Convert text to paths") "</option>\n"
                "<option value=\"LaTeX\">" N_("Omit text in PDF and create LaTeX file") "</option>\n"
            "</param>\n"
            "<param name='strokesToPath' gui-text='" N_("Convert strokes to outlines") "' type='bool'>false</param>\n"
            "<param name=\"blurToBitmap\" gui-text=\"" N_("Rasterize filter effects") "\" type=\"bool\">true</param>\n"
            "<param name=\"resolution\" gui-text=\"" N_("Resolution for rasterization (dpi):") "\" type=\"int\" min=\"1\" max=\"10000\">300</param>\n"
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
