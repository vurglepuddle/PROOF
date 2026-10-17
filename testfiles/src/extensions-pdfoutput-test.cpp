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
#include "extension/db.h"
#include "extension/output.h"
#if defined(WITH_CAPYPDF) && defined(WITH_POPPLER)
#include "extension/internal/ai/ai-native-output.h"
#include "extension/internal/pdfoutput/pdf-output.h"
#endif

#include <limits>

#include <gtest/gtest.h>

using Inkscape::Extension::Internal::StyleMemory;

// Save As uses the guarded native writer. The older PDF-only format stays copy-only.
Inkscape::Extension::Output *get_output_extension_for_save(std::string filename, bool allow_save_copy);

TEST(AiInterchangeTest, NativeSaveIsAvailableAndPdfInterchangeRemainsCopyOnly)
{
    using namespace Inkscape::Extension;
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
