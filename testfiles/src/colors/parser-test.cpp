// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for color objects.
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/parser.h"

#include <gtest/gtest.h>

#include "../test-utils.h"

using namespace Inkscape;
using namespace Inkscape::Colors;

namespace {

TEST(ColorsParser, legacy_icc_identifiers)
{
    EXPECT_EQ(escape_legacy_icc_names("fill:#ac2045 icc-color(Press-2:2004.v1, .05, 1, .45, .22)"),
              "fill:#ac2045 icc-color(Press-2\\3a 2004\\2e v1, .05, 1, .45, .22)");
    EXPECT_EQ(escape_legacy_icc_names("fill:icc-color(Press:2004, 0);stroke:icc-color(Press.v1, 1)"),
              "fill:icc-color(Press\\3a 2004, 0);stroke:icc-color(Press\\2e v1, 1)");
    for (auto text : {"fill:icc-color(Press-2004, 0)",
                      "fill:icc-color(Press\\:2004, 0)",
                      "content:'icc-color(Press:2004, 0)'",
                      "/* icc-color(Press:2004, 0) */ fill:red",
                      "fill:url(icc-color(Press:2004, 0))",
                      "fill:other-icc-color(Press:2004, 0)"}) {
        EXPECT_EQ(escape_legacy_icc_names(text), text);
    }
}

TEST(ColorsParser, test_prefix_parsing)
{
    std::istringstream tests("#rgb(hsl( color( srgb icc-color(profile");
    ASSERT_EQ(Parser::getCssPrefix(tests), "#");
    ASSERT_EQ(Parser::getCssPrefix(tests), "rgb");
    ASSERT_EQ(Parser::getCssPrefix(tests), "hsl");
    ASSERT_EQ(Parser::getCssPrefix(tests), "srgb");
    ASSERT_EQ(Parser::getCssPrefix(tests), "icc-color");

    std::istringstream fails("rgb fail");
    ASSERT_EQ(Parser::getCssPrefix(fails), "");
}

void testCssNumber(std::istringstream &ss, double in_value, std::string in_unit, bool in_end = false)
{
    double out_value;
    std::string out_unit;
    bool out_end = false;

    ASSERT_TRUE(Parser::css_number(ss, out_value, out_unit, out_end, ',')) << in_value << in_unit;
    EXPECT_NEAR(out_value, in_value, 0.001);
    EXPECT_EQ(out_unit, in_unit);
    EXPECT_EQ(out_end, in_end) << in_value << in_unit << " '" << ss.peek() << "'";
}

TEST(ColorsParser, test_number_parsing)
{
    try {
        std::locale::global(std::locale("C"));
    } catch (std::runtime_error &e) {
        ASSERT_TRUE(false) << "Locale 'C' not available for testing";
    }

    std::istringstream tests("1.2 .2 5turn 120deg 20%,5,5, 2cm ,4 9000) 0.0002 5t) 42  )  ");

    testCssNumber(tests, 1.2, "");
    testCssNumber(tests, 0.2, "");
    testCssNumber(tests, 5, "turn");
    testCssNumber(tests, 120, "deg");
    testCssNumber(tests, 20, "%");
    testCssNumber(tests, 5, "");
    testCssNumber(tests, 5, "");
    testCssNumber(tests, 2, "cm");
    testCssNumber(tests, 4, "");
    testCssNumber(tests, 9000, "", true);
    testCssNumber(tests, 0.0002, "");
    testCssNumber(tests, 5, "t", true);
    testCssNumber(tests, 42, "", true);
}

TEST(ColorsParser, test_alt_locale)
{
    std::locale was;
    try {
        was = std::locale::global(std::locale("de_DE.UTF8"));
    } catch (std::runtime_error &e) {
        GTEST_SKIP() << "Skipping all locale test, locale not available";
    }

    std::istringstream tests("1.2 .2 5turn 120deg 20%,5,5, 2cm ,4 9000) 0.0002 5t) 42  )  ");

    testCssNumber(tests, 1.2, "");
    testCssNumber(tests, 0.2, "");
    testCssNumber(tests, 5, "turn");
    testCssNumber(tests, 120, "deg");
    testCssNumber(tests, 20, "%");
    testCssNumber(tests, 5, "");
    testCssNumber(tests, 5, "");
    testCssNumber(tests, 2, "cm");
    testCssNumber(tests, 4, "");
    testCssNumber(tests, 9000, "", true);
    testCssNumber(tests, 0.0002, "");
    testCssNumber(tests, 5, "t", true);
    testCssNumber(tests, 42, "", true);

    std::locale::global(was);
}

void testCssValue(std::string test)
{
    std::istringstream tests("2.0 200% .3, 20  / 5.0)");
    std::vector<double> output;
    bool end = false;
    ASSERT_TRUE(Parser::append_css_value(tests, output, end, ',', 2)      // Value 1
                && Parser::append_css_value(tests, output, end, ',', 3)   // Value 2
                && Parser::append_css_value(tests, output, end, ',', 0.1) // Value 3
                && Parser::append_css_value(tests, output, end, '/', 5)   // Value 4
                && Parser::append_css_value(tests, output, end));         // Opacity
    ASSERT_TRUE(end);
    ASSERT_EQ(output.size(), 5);
    for (auto i = 0; i < 5; i++) {
        EXPECT_NEAR(output[i], (double)(i + 1), 0.001);
    }
}

TEST(ColorsParser, parse_append_css_value)
{
    testCssValue("2.0 200% .3, 20  / 5.0)");
    testCssValue("2.0 200% .3, 20)");
    testCssValue("360deg 3turn .3, 20)");
}

TEST(ColorsParser, parse_hex)
{
    auto parser = HexParser();
    ASSERT_EQ(parser.getPrefix(), "#");

    bool more = false;
    std::vector<double> output;
    std::istringstream p("000001 icc-profile(foo");

    ASSERT_EQ(parser.parseColor(p, output, more), "");
    ASSERT_TRUE(more);
}

TEST(ColorsParser, parse)
{
     Space::Type space_type;
     std::string cms_name;
     std::vector<double> values;
     std::vector<double> fallback;
     ASSERT_TRUE(Parsers::get().parse("rgb(128, 255, 255)", space_type, cms_name, values, fallback));

     ASSERT_EQ(space_type, Space::Type::RGB);
     ASSERT_EQ(cms_name, "");
     ASSERT_EQ(fallback.size(), 0);
     EXPECT_TRUE(VectorIsNear(values, {0.5, 1.0, 1.0}, 0.01));
}

} // namespace

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
