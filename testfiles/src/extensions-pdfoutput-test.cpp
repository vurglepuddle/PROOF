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
#include "ui/dialog/choose-file-utils.h"
#include <giomm/liststore.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/init.h>
#if defined(WITH_CAPYPDF) && defined(WITH_POPPLER)
#include "extension/internal/ai/ai-native-output.h"
#include "extension/internal/pdfoutput/pdf-output.h"
#include "document.h"
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
    EXPECT_EQ(report.summary().find("PDF written with export findings."), 0u);
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
