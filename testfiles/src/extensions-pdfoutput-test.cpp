// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Test Inkscape::Extensions::Internal::PdfOutput
 */
/*
 * Authors:
 *   Martin Owens
 *
 * Copyright (C) 2025 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "attributes.h"
#include "style.h"

#include "extension/internal/pdfoutput/remember-styles.h"
#include "extension/internal/cairo-render-context.h"
#include "extension/internal/cairo-renderer.h"
#include "extension/internal/cairo-renderer-pdf-out.h"
#include "extension/internal/ai/ai-legacy-output.h"
#include "extension/db.h"
#include "extension/output.h"
#include "extension/missing-link-save.h"
#include "ui/dialog/choose-file-utils.h"
#include <giomm/liststore.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/init.h>
#if defined(WITH_CAPYPDF) && defined(WITH_POPPLER)
#include "extension/internal/ai/ai-native-output.h"
#include "extension/internal/pdfinput/ai-private-data.h"
#include "extension/internal/pdfoutput/pdf-output.h"
#include "document.h"
#include "document-undo.h"
#include "display/raster-pixels.h"
#include <lcms2.h>
#include "inkscape.h"
#include "xml/repr.h"
#include "util/scope_exit.h"
#include <glib/gstdio.h>
#endif

#include <limits>
#include <algorithm>

#include <gtest/gtest.h>

using Inkscape::Extension::Internal::StyleMemory;

// Save As uses the guarded native writer. The older PDF-only format stays copy-only.
Inkscape::Extension::Output *get_output_extension_for_save(
    std::string filename, bool allow_save_copy, Inkscape::Extension::Output *selected = nullptr);

TEST(AiInterchangeTest, NativeSaveIsAvailableAndPdfInterchangeRemainsCopyOnly)
{
    using namespace Inkscape::Extension;
    if (!db.get("org.inkscape.output.ai.legacy7")) Internal::AiLegacyOutput::init();
    auto const legacy = dynamic_cast<Output *>(db.get("org.inkscape.output.ai.legacy7"));
    ASSERT_NE(legacy, nullptr);
    EXPECT_TRUE(legacy->savecopy_only());
    EXPECT_TRUE(legacy->causes_dataloss());
    EXPECT_STREQ(legacy->get_mimetype(), "application/illustrator");
    if (!db.get("org.inkscape.output.ai.pdf")) {
        Internal::CairoRendererPdfOutput::init();
    }
#if defined(WITH_CAPYPDF) && defined(WITH_POPPLER)
    if (Internal::PdfOutput::supports_pdf17()) {
        if (!db.get("org.inkscape.output.ai.native")) {
            Internal::PdfOutput::init();
            Internal::AiNativeOutput::init();
        }
        auto native = get_output_extension_for_save("original.ai", false);
        ASSERT_NE(native, nullptr);
        EXPECT_FALSE(native->savecopy_only());
        EXPECT_STREQ(native->get_id(), "org.inkscape.output.ai.native");
    } else {
        EXPECT_EQ(get_output_extension_for_save("original.ai", false), nullptr);
    }
#else
    EXPECT_EQ(get_output_extension_for_save("original.ai", false), nullptr);
#endif
    auto copy = get_output_extension_for_save("deliverable.ai", true);
    ASSERT_NE(copy, nullptr);
    EXPECT_TRUE(copy->savecopy_only());
    EXPECT_TRUE(copy->causes_dataloss());
    EXPECT_STREQ(copy->get_id(), "org.inkscape.output.ai.pdf");
    EXPECT_EQ(get_output_extension_for_save("deliverable.ai", true, legacy), legacy);
    EXPECT_EQ(get_output_extension_for_save("deliverable.AI", true, legacy), legacy);
    EXPECT_EQ(get_output_extension_for_save("original.ai", false, legacy), nullptr);
    EXPECT_EQ(get_output_extension_for_save("working.svg", true, legacy), nullptr);
    EXPECT_EQ(get_output_extension_for_save("deliverable.ai", true, copy), copy);

    Gtk::init_gtkmm_internals();
    std::vector<Output *> outputs;
    auto filters = Inkscape::UI::Dialog::create_export_filters(true, true, &outputs);
    ASSERT_EQ(filters->get_n_items(), outputs.size());
    ASSERT_FALSE(outputs.empty());
    EXPECT_EQ(outputs.front(), nullptr); // All Files keeps filename-based dispatch.
    auto legacy_it = std::find(outputs.begin(), outputs.end(), legacy);
    auto copy_it = std::find(outputs.begin(), outputs.end(), copy);
    ASSERT_NE(legacy_it, outputs.end());
    ASSERT_NE(copy_it, outputs.end());
    EXPECT_NE(filters->get_item(legacy_it - outputs.begin())->get_name(),
              filters->get_item(copy_it - outputs.begin())->get_name());
    auto save_filters = Inkscape::UI::Dialog::create_export_filters(true, false, &outputs);
    EXPECT_EQ(save_filters->get_n_items(), outputs.size());
    EXPECT_EQ(std::find(outputs.begin(), outputs.end(), legacy), outputs.end());
    EXPECT_EQ(std::find(outputs.begin(), outputs.end(), copy), outputs.end());
}

TEST(CairoPdfTransformTest, SmallInvertibleImageScaleIsRetained)
{
    using namespace Inkscape::Extension::Internal;
    CairoRenderer renderer;
    auto context = renderer.createContext();
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
    ASSERT_TRUE(context.setSurfaceTarget(surface, false));
    cairo_surface_destroy(surface);
    context.transform(Geom::Scale(1.0 / 3456, 1.0 / 2160));
    auto matrix = context.getTransform();
    EXPECT_DOUBLE_EQ(matrix[0], 1.0 / 3456);
    EXPECT_DOUBLE_EQ(matrix[3], 1.0 / 2160);
    context.transform(Geom::Scale(0, 1));
    EXPECT_EQ(context.getTransform(), matrix);
    context.transform(Geom::Scale(std::numeric_limits<double>::infinity(), 1));
    EXPECT_EQ(context.getTransform(), matrix);
    EXPECT_TRUE(context.finish());
}

#if defined(WITH_CAPYPDF) && defined(WITH_POPPLER)
namespace {
class ExportFailureTest : public ::testing::TestWithParam<char const *>
{};

TEST_P(ExportFailureTest, PdfIsWrittenWithFindingsAndAiReplacementIsRefused)
{
    using namespace Inkscape;
    using namespace Extension;
    if (!Application::exists()) Application::create(false);
    if (!Internal::PdfOutput::supports_pdf17()) GTEST_SKIP();
    if (!db.get("org.inkscape.output.pdf.capypdf")) Internal::PdfOutput::init();
    auto *pdf = dynamic_cast<Output *>(db.get("org.inkscape.output.pdf.capypdf"));
    ASSERT_TRUE(pdf);
    pdf->set_gui(false);
    std::string const previous_text = pdf->get_param_optiongroup("textToPath");
    auto const previous_strokes = pdf->get_param_bool("strokesToPath");
    auto const previous_fault = g_getenv("PROOF_TEST_PDF_FAILURE");
    std::string const old_fault = previous_fault ? previous_fault : "";
    auto restore = scope_exit([&] {
        pdf->set_param_optiongroup("textToPath", previous_text.c_str());
        pdf->set_param_bool("strokesToPath", previous_strokes);
        if (previous_fault) g_setenv("PROOF_TEST_PDF_FAILURE", old_fault.c_str(), true);
        else g_unsetenv("PROOF_TEST_PDF_FAILURE");
    });
    std::string const kind = GetParam();
    pdf->set_param_optiongroup("textToPath", "embed");
    pdf->set_param_bool("strokesToPath", kind == "outline");
    std::string const body = kind == "font" ?
        "<text id='target' x='10' y='40' style='font-family:Arial;font-size:20px'>Test</text>" :
        kind == "picture" ?
        "<image id='target' x='10' y='10' width='20' height='20' href='data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=='/>" :
        "<path id='target' d='M10 20H80' fill='none' stroke='black' stroke-width='4'/>";
    auto document = [&] {
        return SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>" +
            body + "<rect id='after' x='10' y='60' width='20' height='20' fill='red'/></svg>");
    };
    auto doc = document();
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto const original = sp_repr_save_buf(doc->getReprDoc());
    auto const dir = g_dir_make_tmp("proof-export-failure-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const folder = dir, file = folder + "/result.pdf", ai = folder + "/previous.ai";
    g_free(dir);
    g_setenv("PROOF_TEST_PDF_FAILURE", (kind + ":target").c_str(), true);
    ASSERT_NO_THROW(pdf->save(doc.get(), file.c_str()));
    auto const &report = pdf->export_report;
    ASSERT_EQ(report.findings.size(), 1u) << report.summary();
    EXPECT_EQ(report.findings[0].object, "target");
    EXPECT_EQ(report.findings[0].severity, Severity::Error);
    EXPECT_FALSE(report.findings[0].reason.empty());
    EXPECT_TRUE(report.has_errors());
    EXPECT_TRUE(report.written);
    EXPECT_EQ(report.summary().find("PDF written with export findings. Some content may be missing or not converted."), 0u);
    EXPECT_NE(report.summary().find("Error [target]:"), std::string::npos);
    EXPECT_TRUE(g_file_test(file.c_str(), G_FILE_TEST_IS_REGULAR));

    ASSERT_TRUE(g_file_set_contents(ai.c_str(), "existing destination", -1, nullptr));
    // The native format normally uses fixed embed options. Use the PDF options module
    // here to exercise the same pre-rename guard for outline requests as well.
    auto page_doc = document();
    ASSERT_TRUE(page_doc);
    try {
        Internal::AiNativeOutput().save(pdf, page_doc.get(), ai.c_str());
        FAIL() << "incomplete PDF page must refuse replacement";
    } catch (std::runtime_error const &e) {
        EXPECT_NE(std::string(e.what()).find("The existing file was left unchanged."), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("Error [target]:"), std::string::npos);
    }
    EXPECT_FALSE(report.written);
    EXPECT_TRUE(report.has_errors());
    EXPECT_EQ(report.summary().find("Export failed."), 0u);
    gchar *contents = nullptr;
    ASSERT_TRUE(g_file_get_contents(ai.c_str(), &contents, nullptr, nullptr));
    EXPECT_STREQ(contents, "existing destination");
    g_free(contents);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), original);

    g_unsetenv("PROOF_TEST_PDF_FAILURE");
    ASSERT_NO_THROW(pdf->save(doc.get(), (folder + "/clean.pdf").c_str()));
    EXPECT_TRUE(report.written);
    EXPECT_TRUE(report.findings.empty()) << report.summary();
    EXPECT_EQ(report.summary(), "Export completed."); // No stale warning from an earlier save.
}
INSTANTIATE_TEST_SUITE_P(Faults, ExportFailureTest, ::testing::Values("font", "picture", "outline", "object"));

TEST(MissingLinkSaveTest, CancelDiscardsRelinksAndIgnoreIsExplicit)
{
    using namespace Inkscape;
    using namespace Extension;
    if (!Application::exists()) Application::create(false);
    if (!db.get("org.inkscape.output.pdf.capypdf")) Internal::PdfOutput::init();
    auto *pdf = dynamic_cast<Output *>(db.get("org.inkscape.output.pdf.capypdf"));
    ASSERT_TRUE(pdf);
    auto const dir = g_dir_make_tmp("proof-link-save-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const folder = dir, target = folder + "/result.pdf", replacement = folder + "/replacement.png";
    g_free(dir);
    std::string const png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
    gsize size = 0;
    auto *bytes = g_base64_decode(png.c_str(), &size);
    ASSERT_TRUE(g_file_set_contents(replacement.c_str(), reinterpret_cast<char *>(bytes), size, nullptr));
    g_free(bytes);
    auto doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' "
        "xmlns:proof='urn:x-proof:document:1' width='100' height='100'><image id='lost' width='20' height='20' "
        "preserveAspectRatio='none' href='file:///PROOF-does-not-exist.png' proof:link-preview='data:image/png;base64," +
        png + "'/></svg>");
    ASSERT_TRUE(doc);
    ASSERT_EQ(sp_images_missing(doc.get()).size(), 1u);
    auto const original = sp_repr_save_buf(doc->getReprDoc());
    {
        MissingLinkSave staged(doc.get());
        EXPECT_FALSE(staged.accept(MissingLinkAction::RequirePresent));
        EXPECT_TRUE(staged.accept(MissingLinkAction::Ignore));
        EXPECT_FALSE(staged.changed);
        ASSERT_EQ(sp_images_relink(sp_images_missing(staged.document())[0], replacement), 1u);
        EXPECT_FALSE(staged.accept(MissingLinkAction::Cancel));
        EXPECT_FALSE(staged.changed);
    }
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), original);
    ASSERT_TRUE(g_file_set_contents(target.c_str(), "untouched", -1, nullptr));
    EXPECT_THROW(pdf->save(doc.get(), target.c_str(), false, MissingLinkAction::Cancel), Output::save_cancelled);
    gchar *previous = nullptr;
    ASSERT_TRUE(g_file_get_contents(target.c_str(), &previous, nullptr, nullptr));
    EXPECT_STREQ(previous, "untouched");
    g_free(previous);
    EXPECT_FALSE(pdf->export_report.written);
    Internal::PdfOutput().save(pdf, doc.get(), target.c_str());
    ASSERT_EQ(pdf->export_report.findings.size(), 1u);
    EXPECT_EQ(pdf->export_report.findings[0].severity, Severity::Error);
    ASSERT_NO_THROW(pdf->save(doc.get(), target.c_str(), false, MissingLinkAction::Ignore));
    ASSERT_EQ(pdf->export_report.findings.size(), 1u);
    EXPECT_EQ(pdf->export_report.findings[0].severity, Severity::Warning);
    EXPECT_NE(pdf->export_report.findings[0].reason.find("Linked picture is missing: "), std::string::npos);
    EXPECT_EQ(pdf->missing_link_action, MissingLinkAction::RequirePresent); // Choice never leaks to another save.
    auto *image = sp_images_missing(doc.get())[0];
    image->getRepr()->setAttribute("proof:samples", "unreadable");
    ASSERT_NO_THROW(pdf->save(doc.get(), target.c_str(), false, MissingLinkAction::Ignore));
    EXPECT_TRUE(pdf->export_report.has_errors()); // Ignore does not relax the retained-samples rule.
    image->getRepr()->removeAttribute("proof:samples");
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), RC_("Undo", "Prepare missing link"), "");
    auto export_copy = doc->copy();
    MissingLinkSave staged(export_copy.get());
    ASSERT_EQ(sp_images_relink(sp_images_missing(staged.document())[0], replacement), 1u);
    EXPECT_FALSE(staged.accept(MissingLinkAction::Cancel, doc.get()));
    EXPECT_EQ(sp_images_missing(doc.get()).size(), 1u);
    EXPECT_TRUE(staged.accept(MissingLinkAction::RequirePresent, doc.get()));
    EXPECT_TRUE(staged.changed);
    DocumentUndo::done(doc.get(), RC_("Undo", "Relink pictures that can't be found"), "");
    EXPECT_TRUE(sp_images_missing(doc.get()).empty());
    EXPECT_TRUE(sp_images_missing(export_copy.get()).empty());
    EXPECT_FALSE(image->getRepr()->attribute(SPImage::LINK_PREVIEW_ATTRIBUTE));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(sp_images_missing(doc.get()).size(), 1u);
    EXPECT_TRUE(image->getRepr()->attribute(SPImage::LINK_PREVIEW_ATTRIBUTE));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_TRUE(sp_images_missing(doc.get()).empty());
}

TEST(MissingLinkSaveTest, DiscardAndRelinkCancelOrMirrorAsOneUndoStep)
{
    using namespace Inkscape;
    using namespace Extension;
    if (!Application::exists()) Application::create(false);
    if (!db.get("org.inkscape.output.pdf.capypdf")) Internal::PdfOutput::init();
    if (!db.get("org.inkscape.output.ai.native")) Internal::AiNativeOutput::init();
    auto *native = dynamic_cast<Output *>(db.get("org.inkscape.output.ai.native"));
    ASSERT_TRUE(native);
    auto *dir = g_dir_make_tmp("proof-discard-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const folder = dir, replacement = folder + "/replacement.png", target = folder + "/saved.ai";
    g_free(dir);
    gsize size = 0;
    auto *bytes = g_base64_decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", &size);
    ASSERT_TRUE(g_file_set_contents(replacement.c_str(), reinterpret_cast<char *>(bytes), size, nullptr));
    g_free(bytes);
    auto doc = SPDocument::createNewDocFromMem(std::string_view("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<g id='container'><image id='gone' width='20' height='20' preserveAspectRatio='none' "
        "href='file:///PROOF-gone-no-preview.png'/></g>"
        "<image id='relink' x='30' width='20' height='20' preserveAspectRatio='none' "
        "href='file:///PROOF-relink-no-preview.png'/></svg>"));
    ASSERT_TRUE(doc);
    auto const before = sp_repr_save_buf(doc->getReprDoc());
    {
        MissingLinkSave staged(doc.get());
        ASSERT_TRUE(staged.discard(cast<SPImage>(staged.document()->getObjectById("gone"))));
        ASSERT_EQ(sp_images_relink(cast<SPImage>(staged.document()->getObjectById("relink")), replacement), 1u);
        EXPECT_EQ(sp_images_missing(staged.document()).size(), 0u);
        EXPECT_FALSE(staged.accept(MissingLinkAction::Cancel));
        EXPECT_FALSE(staged.changed);
    }
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    ASSERT_TRUE(g_file_set_contents(target.c_str(), "untouched", -1, nullptr));
    try {
        native->save(doc.get(), target.c_str(), false, MissingLinkAction::Ignore);
        FAIL() << "A missing link without a preview must still refuse.";
    } catch (std::runtime_error const &error) {
        EXPECT_NE(std::string(error.what()).find("Relink or Discard the missing picture in the Missing Links window"), std::string::npos);
    }
    gchar *previous = nullptr;
    ASSERT_TRUE(g_file_get_contents(target.c_str(), &previous, nullptr, nullptr));
    EXPECT_STREQ(previous, "untouched");
    g_free(previous);

    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), RC_("Undo", "Prepare missing links"), "");
    auto export_copy = doc->copy();
    MissingLinkSave staged(export_copy.get());
    ASSERT_TRUE(staged.discard(cast<SPImage>(staged.document()->getObjectById("gone"))));
    ASSERT_EQ(sp_images_relink(cast<SPImage>(staged.document()->getObjectById("relink")), replacement), 1u);
    auto unmatchable = doc->copy();
    unmatchable->getObjectById("relink")->getRepr()->removeAttribute("id");
    auto const unmatched_before = sp_repr_save_buf(unmatchable->getReprDoc());
    EXPECT_FALSE(staged.accept(MissingLinkAction::RequirePresent, unmatchable.get()));
    EXPECT_EQ(sp_repr_save_buf(unmatchable->getReprDoc()), unmatched_before);
    EXPECT_TRUE(export_copy->getObjectById("gone")); // Validation fails before any partial deletion.
    ASSERT_TRUE(staged.accept(MissingLinkAction::RequirePresent, doc.get()));
    EXPECT_TRUE(staged.changed);
    DocumentUndo::done(doc.get(), RC_("Undo", "Relink or discard missing pictures"), "");
    for (auto *document : {doc.get(), export_copy.get()}) {
        EXPECT_FALSE(document->getObjectById("gone"));
        auto *container = document->getObjectById("container");
        ASSERT_TRUE(container);
        EXPECT_TRUE(container->children.empty()); // Do not cascade into empty containers.
        EXPECT_TRUE(sp_images_missing(document).empty());
    }
    ASSERT_NO_THROW(native->save(doc.get(), target.c_str(), false, MissingLinkAction::RequirePresent));
    EXPECT_TRUE(native->export_report.written);
    EXPECT_TRUE(native->export_report.findings.empty());
    std::string error;
    auto const records = Internal::read_ai_native_records(target, {}, 64 << 20, error);
    ASSERT_TRUE(records) << error; // Inspect the saved destination, not a second writer call.
    EXPECT_EQ(records->text.find("PROOF-gone-no-preview"), std::string::npos);
    EXPECT_EQ(records->text.find("(gone)"), std::string::npos);
    EXPECT_NE(records->text.find("%%IncludeFile:"), std::string::npos);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); // One undo restores both operations.
    EXPECT_TRUE(doc->getObjectById("gone"));
    EXPECT_EQ(sp_images_missing(doc.get()).size(), 2u);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_FALSE(doc->getObjectById("gone"));
    EXPECT_TRUE(sp_images_missing(doc.get()).empty());
}

TEST(MissingLinkSaveTest, DiscardProtectsClipsMasksAndUnmatchableCopies)
{
    using namespace Inkscape;
    using namespace Extension;
    auto doc = SPDocument::createNewDocFromMem(std::string_view("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<defs><clipPath id='clip'><g><image id='inclip' width='20' height='20' href='file:///PROOF-inclip.png'/></g></clipPath>"
        "<mask id='mask'><image id='inmask' width='20' height='20' href='file:///PROOF-inmask.png'/></mask></defs>"
        "<g id='group' clip-path='url(#clip)' mask='url(#mask)'><image id='content' width='20' height='20' "
        "href='file:///PROOF-content.png'/></g><image id='idless' width='20' height='20' href='file:///PROOF-idless.png'/></svg>"));
    ASSERT_TRUE(doc);
    doc->getObjectById("idless")->getRepr()->removeAttribute("id");
    auto const before = sp_repr_save_buf(doc->getReprDoc());
    MissingLinkSave staged(doc.get());
    for (auto const *id : {"inclip", "inmask"}) {
        EXPECT_FALSE(staged.discard(cast<SPImage>(staged.document()->getObjectById(id))));
        EXPECT_FALSE(discard_missing_picture(cast<SPImage>(doc->getObjectById(id))));
    }
    for (auto *image : sp_images_missing(staged.document())) {
        if (image->linkedFile().find("PROOF-idless") != std::string::npos) EXPECT_FALSE(staged.discard(image));
    }
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    DocumentUndo::done(doc.get(), RC_("Undo", "Prepare missing links"), "");
    ASSERT_TRUE(discard_missing_picture(cast<SPImage>(doc->getObjectById("content"))));
    DocumentUndo::done(doc.get(), RC_("Undo", "Discard missing pictures"), "");
    EXPECT_TRUE(doc->getObjectById("group")); // Clipped/masked content is safe; its containers stay.
    EXPECT_FALSE(doc->getObjectById("content"));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_TRUE(doc->getObjectById("content"));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_FALSE(doc->getObjectById("content"));
}

TEST(PrepressColorTest, LabFallbacksAndEmptyNamesAreFindings)
{
    using namespace Inkscape;
    using namespace Extension;
    if (!Application::exists()) Application::create(false);
    if (!db.get("org.inkscape.output.pdf.capypdf")) Internal::PdfOutput::init();
    auto *pdf = dynamic_cast<Output *>(db.get("org.inkscape.output.pdf.capypdf"));
    ASSERT_TRUE(pdf);
    auto const dir = g_dir_make_tmp("proof-colour-export-XXXXXX", nullptr);
    ASSERT_TRUE(dir);
    std::string const file = std::string(dir) + "/colours.pdf";
    g_free(dir);
    auto save = [&](std::string const &body) {
        auto doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' "
            "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' xmlns:proof='urn:x-proof:document:1' "
            "width='200' height='100'>" + body + "</svg>");
        if (!doc) throw std::runtime_error("test document did not load");
        doc->ensureUpToDate();
        pdf->save(doc.get(), file.c_str());
    };
    ASSERT_NO_THROW(save("<rect width='80' height='80' fill='lab(55 20 -30)' stroke='lab(65 -12 31)'/>"
        "<text x='10' y='30' style='font-family:Arial;fill:lab(50 0 0)'>A"
        "<tspan style='fill:lab(70 10 20);stroke:lab(60 -5 10)'>B</tspan></text>"));
    EXPECT_TRUE(pdf->export_report.findings.empty()) << pdf->export_report.summary();
    ASSERT_NO_THROW(save("<defs><linearGradient id='g'><stop stop-color='lab(55 20 -30)'/>"
        "<stop offset='1' stop-color='red'/></linearGradient></defs>"
        "<rect id='first' width='80' height='80' fill='url(#g)'/>"
        "<rect id='second' x='100' width='80' height='80' fill='url(#g)'/>"));
    auto const &findings = pdf->export_report.findings;
    ASSERT_EQ(findings.size(), 2u) << pdf->export_report.summary();
    EXPECT_EQ(findings[0].object, "first");
    EXPECT_EQ(findings[1].object, "second");
    for (auto const &finding : findings) {
        EXPECT_EQ(finding.severity, Severity::Warning);
        EXPECT_EQ(finding.reason, "Lab colour was converted to RGB.");
    }
    ASSERT_NO_THROW(save("<defs><linearGradient id='bad' inkscape:swatch='solid' proof:ink='' "
        "proof:ink-alternate='device-cmyk(0 1 0 0)'><stop stop-color='red'/></linearGradient></defs>"
        "<rect id='unnamed' width='80' height='80' fill='url(#bad)'/>"));
    ASSERT_EQ(findings.size(), 1u) << pdf->export_report.summary();
    EXPECT_EQ(findings[0].object, "unnamed");
    EXPECT_EQ(findings[0].severity, Severity::Error);
    EXPECT_EQ(findings[0].reason, "Spot ink name is empty.");
    EXPECT_TRUE(pdf->export_report.written);
}

TEST(PictureProfileTest, CachedGrayTransformsKeepProfilesDepthAndAlphaDistinct)
{
    using namespace Inkscape;
    auto profile = [](double gamma) {
        auto curve = cmsBuildGamma(nullptr, gamma);
        auto handle = cmsCreateGrayProfile(cmsD50_xyY(), curve);
        cmsFreeToneCurve(curve);
        cmsUInt32Number size = 0;
        cmsSaveProfileToMem(handle, nullptr, &size);
        std::string bytes(size, '\0');
        cmsSaveProfileToMem(handle, bytes.data(), &size);
        cmsCloseProfile(handle);
        return bytes;
    };
    RasterPixels pixels;
    pixels.model = RasterPixels::Model::Gray;
    pixels.width = pixels.height = 1;
    pixels.colour = {128};
    auto read = [&](int want, int tolerance = 1) {
        auto *surface = raster_surface(pixels, {});
        EXPECT_NE(surface, nullptr);
        if (!surface) return;
        auto const value = *reinterpret_cast<std::uint32_t *>(cairo_image_surface_get_data(surface));
        for (unsigned shift : {0, 8, 16}) EXPECT_NEAR((value >> shift) & 255, want, tolerance);
        cairo_surface_destroy(surface);
    };
    read(128, 0); // Untagged samples retain their values.
    pixels.profile = profile(1.0);
    read(188); // sRGB encoding of linear light 128/255.
    auto const linear = pixels.profile;
    pixels.profile = profile(2.2);
    read(129);
    pixels.profile = linear;
    read(188); // Reusing the first profile must not reuse the second profile's transform.
    pixels.alpha = {64};
    read(47);
    pixels.alpha.clear();
    pixels.bits = 16;
    pixels.colour = {0, 128}; // 32768 in native byte order on this target.
    read(188);
    pixels.model = RasterPixels::Model::RGB;
    pixels.colour = {0, 128, 0, 128, 0, 128};
    EXPECT_EQ(raster_surface(pixels, {}), nullptr); // Mismatched profile/sample models cannot transform.
}

} // namespace
#endif

TEST(StyleMemeoryTest, MapFiltersStyle)
{
    SPStyle *style = new SPStyle(); // No document style
    style->mergeString("opacity:1.0;fill:black;stroke:red");

    auto memory = StyleMemory({SPAttr::OPACITY, SPAttr::FILL});
    auto map = memory.get_changes(style);
    EXPECT_TRUE(map.contains(SPAttr::OPACITY));
    EXPECT_TRUE(map.contains(SPAttr::FILL));
    EXPECT_FALSE(map.contains(SPAttr::STROKE));

    EXPECT_EQ(map[SPAttr::OPACITY], "1");
    EXPECT_EQ(map[SPAttr::FILL], "black");
}

TEST(StyleMemoryTest, MapContainsUnsetStyle)
{
    SPStyle *style = new SPStyle(); // No document style
    style->mergeString("fill:black;stroke:red");

    auto memory = StyleMemory({SPAttr::OPACITY, SPAttr::FILL});
    auto map = memory.get_changes(style);
    EXPECT_TRUE(map.contains(SPAttr::OPACITY));
    EXPECT_TRUE(map.contains(SPAttr::FILL));

    EXPECT_EQ(map[SPAttr::OPACITY], "1");
    EXPECT_EQ(map[SPAttr::FILL], "black");
}

TEST(StyleMemoryTest, MemoryState)
{
    SPStyle *style = new SPStyle(); // No document style
    style->mergeString("fill:black;");

    auto memory = StyleMemory({SPAttr::OPACITY, SPAttr::FILL});
    ASSERT_EQ(memory.get_state().size(), 0);

    auto map = memory.get_changes(style);
    ASSERT_EQ(map.size(), 2);

    {
        auto scope = memory.remember(map);
        ASSERT_EQ(memory.get_state().size(), 2);
        ASSERT_EQ(memory.get_state().find(SPAttr::FILL)->second, "black");
        ASSERT_EQ(memory.get_state().find(SPAttr::OPACITY)->second, "1");

        // Nothing has changed, so nothing should change
        ASSERT_EQ(memory.get_changes(style).size(), 0);

        style->clear(SPAttr::FILL);
        style->mergeString("fill:red");
        auto map2 = memory.get_changes(style);
        ASSERT_EQ(map2.size(), 1);
        ASSERT_EQ(map2[SPAttr::FILL], "red");

        {
            auto scope2 = memory.remember(map2);
            ASSERT_EQ(memory.get_state().find(SPAttr::FILL)->second, "red");
            ASSERT_EQ(memory.get_state().find(SPAttr::OPACITY)->second, "1");
            ASSERT_EQ(memory.get_changes(style).size(), 0);
        }

        ASSERT_EQ(memory.get_state().find(SPAttr::FILL)->second, "black");
        ASSERT_EQ(memory.get_state().find(SPAttr::OPACITY)->second, "1");
        ASSERT_EQ(memory.get_changes(style).size(), 1);
    }
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
