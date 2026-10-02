// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF spot inks: swatch storage, tint display and palette spot flags.
 */
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>
#include <gtest/gtest.h>

#include "colors/color.h"
#include "colors/spaces/base.h"
#include "colors/spaces/enum.h"
#include "doc-per-case-test.h"
#include "document.h"
#include "gradient-chemistry.h"
#include "object/sp-defs.h"
#include "object/sp-gradient.h"
#include "object/sp-stop.h"
#include "spot-ink.h"
#include "ui/dialog/ai-palette.h"
#include "ui/dialog/global-palettes.h"
#include "xml/repr.h"

using namespace Inkscape;
using Colors::Color;
using Colors::Space::Type;
using namespace std::literals;

namespace {

constexpr auto EMPTY_SVG = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><defs/></svg>)"sv;

// Big-endian writers for hand-built palette files.
void put16(std::string &out, unsigned value)
{
    out += static_cast<char>((value >> 8) & 0xff);
    out += static_cast<char>(value & 0xff);
}

void put32(std::string &out, uint32_t value)
{
    put16(out, value >> 16);
    put16(out, value & 0xffff);
}

void put_float(std::string &out, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    put32(out, bits);
}

/// UTF-16BE string with a terminating null, preceded by its length in code units.
void put_utf16(std::string &out, std::string const &text, bool short_length)
{
    auto const units = text.size() + 1;
    short_length ? put16(out, units) : put32(out, units);
    for (unsigned char ch : text) {
        put16(out, ch);
    }
    put16(out, 0);
}

std::string write_temp(std::string const &name, std::string const &bytes)
{
    auto path = std::string(g_get_tmp_dir()) + G_DIR_SEPARATOR_S + name;
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
}

/// Stop colours carry an opacity channel; compare colour channels only.
Color without_opacity(Color color)
{
    color.enableOpacity(false);
    return color;
}

class SpotInkTest : public DocPerCaseTest
{
protected:
    void SetUp() override { doc = SPDocument::createNewDocFromMem(EMPTY_SVG); }
    std::unique_ptr<SPDocument> doc;
};

} // namespace

TEST(SpotInkIdTest, ReadableStems)
{
    EXPECT_EQ(SpotInk::id_stem("PANTONE 185 C"), "ink-pantone-185-c");
    EXPECT_EQ(SpotInk::id_stem("CutContour"), "ink-cutcontour");
    EXPECT_EQ(SpotInk::id_stem("  RAL 3020 / Verkehrsrot "), "ink-ral-3020-verkehrsrot");
    EXPECT_EQ(SpotInk::id_stem("***"), "ink-unnamed");
}

TEST(SpotInkDisplayTest, CmykTintScalesInk)
{
    auto shown = SpotInk::display_color(Color(Type::CMYK, {0, 0.9, 0.8, 0}), 0.3);
    ASSERT_EQ(shown.getSpace()->getType(), Type::CMYK);
    EXPECT_NEAR(shown[0], 0.0, 1e-9);
    EXPECT_NEAR(shown[1], 0.27, 1e-9);
    EXPECT_NEAR(shown[2], 0.24, 1e-9);
    EXPECT_NEAR(shown[3], 0.0, 1e-9);
}

TEST(SpotInkDisplayTest, LabTintBlendsTowardsPaper)
{
    // PANTONE 2004 CP as Illustrator writes it: L 87.8431, a 0, b 59.
    auto const span = 255.0;
    Color alternate(Type::LAB, {0.878431, (0 + 128) / span, (59 + 128) / span});
    auto shown = SpotInk::display_color(alternate, 0.3);
    ASSERT_EQ(shown.getSpace()->getType(), Type::LAB);
    EXPECT_NEAR(shown[0] * 100, 100 - 0.3 * (100 - 87.8431), 1e-6);
    EXPECT_NEAR(shown[1] * span - 128, 0, 1e-6);
    EXPECT_NEAR(shown[2] * span - 128, 0.3 * 59, 1e-6);
    auto paper = SpotInk::display_color(alternate, 0.0);
    EXPECT_NEAR(paper[0], 1.0, 1e-9);
}

TEST_F(SpotInkTest, EnsureCreatesReusesAndSeparatesTints)
{
    SpotInk::Ink red{"PANTONE 185 C", Color(Type::CMYK, {0, 0.93, 0.79, 0}), 1.0};
    auto const id = SpotInk::ensure(doc.get(), red);
    EXPECT_EQ(id, "ink-pantone-185-c");
    EXPECT_EQ(SpotInk::ensure(doc.get(), red), id) << "same ink and tint must reuse the swatch";

    auto tinted = red;
    tinted.tint = 0.3;
    auto const tint_id = SpotInk::ensure(doc.get(), tinted);
    EXPECT_EQ(tint_id, "ink-pantone-185-c-t30");

    auto grad = cast<SPGradient>(doc->getObjectById(tint_id));
    ASSERT_TRUE(grad);
    EXPECT_TRUE(grad->isSwatch());
    EXPECT_TRUE(grad->isSolid());
    EXPECT_STREQ(grad->getRepr()->attribute("inkscape:label"), "PANTONE 185 C 30%");

    auto ink = SpotInk::read(grad->getRepr());
    ASSERT_TRUE(ink);
    EXPECT_EQ(ink->name, "PANTONE 185 C");
    EXPECT_NEAR(ink->tint, 0.3, 1e-9);
    EXPECT_TRUE(ink->alternate.isClose(red.alternate, 1e-6));
}

TEST_F(SpotInkTest, SameNameDifferentDefinitionGetsItsOwnSwatch)
{
    SpotInk::Ink a{"CutContour", Color(Type::CMYK, {0, 1, 0, 0}), 1.0};
    SpotInk::Ink b{"CutContour", Color(Type::CMYK, {1, 0, 0, 0}), 1.0};
    auto const first = SpotInk::ensure(doc.get(), a);
    auto const second = SpotInk::ensure(doc.get(), b);
    EXPECT_NE(first, second);
    EXPECT_EQ(second, "ink-cutcontour-2");
}

TEST_F(SpotInkTest, InkSurvivesSaveAndReload)
{
    SpotInk::Ink ink{"PANTONE 2004 CP", Color(Type::LAB, {0.878431, 128 / 255.0, 187 / 255.0}), 0.5};
    auto const id = SpotInk::ensure(doc.get(), ink);

    std::string const saved = sp_repr_save_buf(doc->getReprDoc()).raw();
    EXPECT_NE(saved.find("xmlns:proof=\"urn:x-proof:document:1\""), std::string::npos) << saved;
    EXPECT_NE(saved.find("proof:ink=\"PANTONE 2004 CP\""), std::string::npos) << saved;

    auto reloaded = SPDocument::createNewDocFromMem(saved);
    ASSERT_TRUE(reloaded);
    auto obj = reloaded->getObjectById(id);
    ASSERT_TRUE(obj);
    auto back = SpotInk::read(obj->getRepr());
    ASSERT_TRUE(back);
    EXPECT_EQ(back->name, ink.name);
    EXPECT_NEAR(back->tint, 0.5, 1e-9);
    EXPECT_EQ(back->alternate.getSpace()->getType(), Type::LAB);
    EXPECT_TRUE(back->alternate.isClose(ink.alternate, 1e-4));
}

TEST(SpotInkEditTest, AlternateForInvertsDisplay)
{
    for (auto alternate : {Color(Type::CMYK, {0, 0.93, 0.79, 0.1}),
                           Color(Type::LAB, {0.878431, 128 / 255.0, 187 / 255.0}),
                           Color(Type::RGB, {0.129, 0.369, 0.561})}) {
        auto shown = SpotInk::display_color(alternate, 0.3);
        auto back = SpotInk::alternate_for(shown, 0.3);
        EXPECT_TRUE(back.isClose(alternate, 1e-6)) << alternate.toString() << " -> " << back.toString();
    }
    auto full = Color(Type::CMYK, {0.2, 0.4, 0.6, 0.8});
    EXPECT_TRUE(SpotInk::alternate_for(full, 1.0).isClose(full, 1e-9));
}

TEST_F(SpotInkTest, EditingATintRedefinesTheWholeInk)
{
    SpotInk::Ink base{"PANTONE 185 C", Color(Type::CMYK, {0, 0.9, 0.8, 0}), 1.0};
    auto tinted = base;
    tinted.tint = 0.3;
    SpotInk::Ink other{"CutContour", Color(Type::CMYK, {0, 1, 0, 0}), 1.0};
    auto base_id = SpotInk::ensure(doc.get(), base);
    auto tint_id = SpotInk::ensure(doc.get(), tinted);
    auto other_id = SpotInk::ensure(doc.get(), other);

    // The user picks a new colour for the 30% swatch, as Fill & Stroke would.
    auto tint_swatch = cast<SPGradient>(doc->getObjectById(tint_id));
    ASSERT_TRUE(tint_swatch);
    sp_change_swatch_color(tint_swatch, Color(Type::CMYK, {0.03, 0.3, 0.15, 0}));

    auto base_ink = SpotInk::read(doc->getObjectById(base_id)->getRepr());
    auto tint_ink = SpotInk::read(doc->getObjectById(tint_id)->getRepr());
    ASSERT_TRUE(base_ink && tint_ink);
    auto expected = Color(Type::CMYK, {0.1, 1.0, 0.5, 0});
    EXPECT_TRUE(base_ink->alternate.isClose(expected, 1e-6)) << base_ink->alternate.toString();
    EXPECT_TRUE(tint_ink->alternate.isClose(expected, 1e-6)) << "tints share the new definition";
    EXPECT_NEAR(tint_ink->tint, 0.3, 1e-9);
    auto tint_stop = cast<SPGradient>(doc->getObjectById(tint_id))->getFirstStop()->getColor();
    EXPECT_TRUE(without_opacity(tint_stop).isClose(SpotInk::display_color(expected, 0.3), 1e-3))
        << tint_stop.toString() << " vs " << SpotInk::display_color(expected, 0.3).toString();

    auto untouched = SpotInk::read(doc->getObjectById(other_id)->getRepr());
    EXPECT_TRUE(untouched->alternate.isClose(other.alternate, 1e-9)) << "other inks are not affected";
}

TEST_F(SpotInkTest, EditingKeepsTheDefinitionSpace)
{
    SpotInk::Ink pantone{"PANTONE 2004 CP", Color(Type::LAB, {0.878431, 128 / 255.0, 187 / 255.0}), 1.0};
    auto id = SpotInk::ensure(doc.get(), pantone);
    sp_change_swatch_color(cast<SPGradient>(doc->getObjectById(id)), Color(Type::RGB, {0.9, 0.8, 0.3}));
    auto ink = SpotInk::read(doc->getObjectById(id)->getRepr());
    ASSERT_TRUE(ink);
    EXPECT_EQ(ink->alternate.getSpace()->getType(), Type::LAB) << "a Lab ink edited in RGB stays Lab";
}

TEST_F(SpotInkTest, RenamingRenamesTheInkAndKeepsTintLabels)
{
    SpotInk::Ink base{"Spot 1", Color(Type::CMYK, {0, 1, 0, 0}), 1.0};
    auto tinted = base;
    tinted.tint = 0.5;
    auto base_id = SpotInk::ensure(doc.get(), base);
    auto tint_id = SpotInk::ensure(doc.get(), tinted);

    // Renaming from the tint swatch, as typed into the swatch editor's name field.
    sp_rename_swatch(cast<SPGradient>(doc->getObjectById(tint_id)), "CutContour 50%");
    EXPECT_EQ(SpotInk::read(doc->getObjectById(base_id)->getRepr())->name, "CutContour");
    EXPECT_STREQ(doc->getObjectById(base_id)->getRepr()->attribute("inkscape:label"), "CutContour");
    EXPECT_EQ(SpotInk::read(doc->getObjectById(tint_id)->getRepr())->name, "CutContour");
    EXPECT_STREQ(doc->getObjectById(tint_id)->getRepr()->attribute("inkscape:label"), "CutContour 50%");
}

TEST_F(SpotInkTest, SpotSwitchTurnsSwatchesIntoInksAndBack)
{
    auto swatch = sp_document_default_gradient_vector(doc.get(), Color(Type::CMYK, {0, 0, 0, 0.2}), 1, true);
    ASSERT_TRUE(swatch);
    swatch->setSwatch();
    ASSERT_TRUE(SpotInk::make_spot(doc.get(), swatch->getRepr(), "Varnish"));
    auto ink = SpotInk::read(swatch->getRepr());
    ASSERT_TRUE(ink);
    EXPECT_EQ(ink->name, "Varnish");
    EXPECT_NEAR(ink->tint, 1.0, 1e-9);
    EXPECT_TRUE(ink->alternate.isClose(Color(Type::CMYK, {0, 0, 0, 0.2}), 1e-6));

    SpotInk::make_process(swatch->getRepr());
    EXPECT_FALSE(SpotInk::read(swatch->getRepr()));
    EXPECT_TRUE(without_opacity(swatch->getFirstStop()->getColor()).isClose(Color(Type::CMYK, {0, 0, 0, 0.2}), 1e-6))
        << swatch->getFirstStop()->getColor().toString();
}

TEST(SpotPaletteTest, AseKeepsSpotFlag)
{
    std::string ase = "ASEF";
    put16(ase, 1);
    put16(ase, 0);
    put32(ase, 2); // two colour blocks
    for (auto [name, type] : {std::pair{"PANTONE 185 C"s, 1u}, std::pair{"Process Red"s, 2u}}) {
        std::string block;
        put_utf16(block, name, true);
        block += "CMYK";
        for (float v : {0.0f, 0.93f, 0.79f, 0.0f}) {
            put_float(block, v);
        }
        put16(block, type);
        put16(ase, 0x0001);
        put32(ase, block.size());
        ase += block;
    }
    auto result = UI::Dialog::load_palette(write_temp("proof-spot-test.ase", ase));
    ASSERT_TRUE(result.palette) << result.error_message;
    auto const &colors = result.palette->colors;
    ASSERT_EQ(colors.size(), 2u);
    auto spot = std::get_if<UI::Dialog::PaletteFileData::SpotColor>(&colors[0]);
    ASSERT_TRUE(spot);
    EXPECT_EQ(spot->color.getName(), "PANTONE 185 C");
    EXPECT_NEAR(spot->color[1], 0.93, 1e-6);
    auto process = std::get_if<Color>(&colors[1]);
    ASSERT_TRUE(process) << "type 2 (normal) must stay a plain colour";
    EXPECT_EQ(process->getName(), "Process Red");
}

TEST(SpotPaletteTest, AseLabUsesActualAB)
{
    // ORACAL 641 "010 (641)" (white) exactly as stored: L as a fraction, a and b as actual values.
    std::string ase = "ASEF";
    put16(ase, 1);
    put16(ase, 0);
    put32(ase, 1);
    std::string block;
    put_utf16(block, "010 (641)", true);
    block += "LAB ";
    for (float v : {0.9249f, -0.157f, -2.6159f}) {
        put_float(block, v);
    }
    put16(block, 0); // global
    put16(ase, 0x0001);
    put32(ase, block.size());
    ase += block;

    auto result = UI::Dialog::load_palette(write_temp("proof-lab-test.ase", ase));
    ASSERT_TRUE(result.palette) << result.error_message;
    auto white = std::get_if<Color>(&result.palette->colors[0]);
    ASSERT_TRUE(white);
    ASSERT_EQ(white->getSpace()->getType(), Type::LAB);
    EXPECT_NEAR(white->get(0) * 100, 92.49, 1e-3);
    EXPECT_NEAR(white->get(1) * 255 - 128, -0.157, 1e-3);
    EXPECT_NEAR(white->get(2) * 255 - 128, -2.6159, 1e-3);
    auto rgb = white->converted(Type::RGB);
    ASSERT_TRUE(rgb);
    for (int i = 0; i < 3; ++i) {
        EXPECT_GT(rgb->get(i), 0.85) << "channel " << i << " should be near white, not cyan";
    }
}

namespace {

// Palette records as Illustrator 29 writes them (modelled on KZ_Colors.ai and Difficult_Spot.ai).
constexpr auto AI_PALETTE = R"(%AI5_BeginPalette
0 0 Pb
%AI17_Begin_Content_if_version_gt:24 15
1 1 1 1 ([Registration]) 0 Xs
([Registration])
Pc
%AI17_Alternate_Content
0 0 0 1 (Fallback Should Not Appear) 0 0 Xk
(Fallback Should Not Appear)
Pc
%AI17_End_Versioned_Content
0 0 0 0 k
(White)
Pc
Bb
2 (White, Black) 0 0 0 1 1 0 0 1 0 0 1 Bg
0 BB
(White, Black)
Pc
1 (Brand) 1 Pg
0.05 1 0.45 0.22 (Brand Red) 0 0 Xk
(Brand Red)
Pc
0.5 0.36 0.34 0.09 0.129 0.369 0.561 (Signalblau) 0 1 Xx
(Signalblau)
Pc
0.05 0.1 0.68 0 87.8431 0 59 (PANTONE 2004 CP) 0 2 Xx
(PANTONE 2004 CP)
Pc
0.05 0.1 0.68 0 87.8431 0 59 (PANTONE 2004 CP) 0.5 2 Xx
(PANTONE 2004 CP 50%)
Pc
0.2 0.2 0.5 0 0.792 0.749 0.557 (Gr\374nbeige) 0 1 Xx
(Gr\374nbeige)
Pc
PB
%AI5_EndPalette
)"sv;

/// The expected palette, shared by the parser test and the real-file tests.
void expect_ai_fixture_palette(UI::Dialog::PaletteFileData const &palette)
{
    using Data = UI::Dialog::PaletteFileData;
    auto const &c = palette.colors;
    ASSERT_EQ(c.size(), 7u) << "White, group, Brand Red and 4 spot inks; registration, gradient and fallback skipped";

    auto white = std::get_if<Color>(&c[0]);
    ASSERT_TRUE(white);
    EXPECT_EQ(white->getName(), "White");
    EXPECT_EQ(white->getSpace()->getType(), Type::CMYK);

    auto group = std::get_if<Data::GroupStart>(&c[1]);
    ASSERT_TRUE(group);
    EXPECT_EQ(group->name, "Brand");

    auto global = std::get_if<Color>(&c[2]);
    ASSERT_TRUE(global) << "global process colours are plain colours";
    EXPECT_EQ(global->getName(), "Brand Red");
    EXPECT_NEAR((*global)[3], 0.22, 1e-9);

    auto rgb_spot = std::get_if<Data::SpotColor>(&c[3]);
    ASSERT_TRUE(rgb_spot);
    EXPECT_EQ(rgb_spot->color.getName(), "Signalblau");
    EXPECT_EQ(rgb_spot->color.getSpace()->getType(), Type::RGB);
    EXPECT_NEAR(rgb_spot->color[2], 0.561, 1e-9);
    EXPECT_NEAR(rgb_spot->tint, 1.0, 1e-9);

    auto lab_spot = std::get_if<Data::SpotColor>(&c[4]);
    ASSERT_TRUE(lab_spot);
    EXPECT_EQ(lab_spot->color.getName(), "PANTONE 2004 CP");
    EXPECT_EQ(lab_spot->color.getSpace()->getType(), Type::LAB);
    EXPECT_NEAR(lab_spot->color[0] * 100, 87.8431, 1e-6);
    EXPECT_NEAR(lab_spot->color[2] * 255 - 128, 59, 1e-6);

    auto tint_spot = std::get_if<Data::SpotColor>(&c[5]);
    ASSERT_TRUE(tint_spot);
    EXPECT_EQ(tint_spot->color.getName(), "PANTONE 2004 CP") << "tint swatches keep the ink's name";
    EXPECT_NEAR(tint_spot->tint, 0.5, 1e-9) << "AI tint operand 0.5 means 50%";

    auto umlaut = std::get_if<Data::SpotColor>(&c[6]);
    ASSERT_TRUE(umlaut);
    EXPECT_EQ(umlaut->color.getName(), "Gr\xc3\xbcnbeige") << "Windows-1252 names decode to UTF-8";
}

} // namespace

TEST(AiPaletteTest, ParsesIllustratorPaletteRecords)
{
    UI::Dialog::PaletteFileData palette;
    ASSERT_TRUE(UI::Dialog::parse_ai_palette(AI_PALETTE, palette));
    expect_ai_fixture_palette(palette);

    UI::Dialog::PaletteFileData empty;
    EXPECT_FALSE(UI::Dialog::parse_ai_palette("%!PS-Adobe-3.0\n%%EOF\n", empty));
}

TEST(AiPaletteTest, LoadsZstandardAndZlibSwatchLibraries)
{
    for (auto name : {"proof-ai-swatches-zstd.ai", "proof-ai-swatches-zlib.ai"}) {
        SCOPED_TRACE(name);
        auto result = UI::Dialog::load_palette(std::string(INKSCAPE_TESTS_DIR) + "/data/" + name);
        ASSERT_TRUE(result.palette) << result.error_message;
        EXPECT_EQ(result.palette->name.raw(), std::string(name).substr(0, std::string(name).size() - 3));
        expect_ai_fixture_palette(*result.palette);
    }
}

// Diagnostic, not part of the normal run: dump a real library loaded by PROOF so it can be
// compared with Illustrator's Swatches panel. Run with --gtest_also_run_disabled_tests and
// PROOF_AI_SWATCH_FILE=<path to .ai/.ase/.acb>.
TEST(AiPaletteTest, DISABLED_DumpPaletteFromEnvironment)
{
    auto const path = g_getenv("PROOF_AI_SWATCH_FILE");
    ASSERT_TRUE(path) << "set PROOF_AI_SWATCH_FILE";
    auto result = UI::Dialog::load_palette(path);
    ASSERT_TRUE(result.palette) << result.error_message;
    for (auto const &entry : result.palette->colors) {
        if (auto group = std::get_if<UI::Dialog::PaletteFileData::GroupStart>(&entry)) {
            std::printf("GROUP|%s\n", group->name.c_str());
        } else if (auto color = std::get_if<Color>(&entry)) {
            std::printf("COLOR|%s|%s\n", color->getName().c_str(), color->toString(false).c_str());
        } else if (auto spot = std::get_if<UI::Dialog::PaletteFileData::SpotColor>(&entry)) {
            std::printf("SPOT|%s|%s|%g\n", spot->color.getName().c_str(), spot->color.toString(false).c_str(),
                        spot->tint);
        }
    }
}

TEST(AiPaletteTest, RejectsNonIllustratorFiles)
{
    auto result = UI::Dialog::load_palette(write_temp("proof-not-ai.ai", "%PDF-1.4\nnot really a pdf\n"));
    EXPECT_FALSE(result.palette);
    EXPECT_FALSE(result.error_message.empty());
}

TEST(SpotPaletteTest, AcbSpotBookMarksEveryColour)
{
    auto book = [](std::string const &marker) {
        std::string acb = "8BCB";
        put16(acb, 1);    // version
        put16(acb, 3000); // book id
        put_utf16(acb, "$$$/Title=PROOF Test Book", false);
        put_utf16(acb, "$$$/Prefix=PROOF ", false);
        put_utf16(acb, "$$$/Suffix= C", false);
        put_utf16(acb, "", false);
        put16(acb, 1); // colour count
        put16(acb, 7); // columns
        put16(acb, 0); // page offset
        put16(acb, 7); // Lab
        put_utf16(acb, "185", false);
        acb += "185C  ";
        acb += static_cast<char>(0x7f); // L ~ 50
        acb += static_cast<char>(0xd0); // a ~ +80
        acb += static_cast<char>(0xb0); // b ~ +48
        return acb + marker;
    };

    auto spot = UI::Dialog::load_palette(write_temp("proof-spot-test.acb", book("spflspot")));
    ASSERT_TRUE(spot.palette) << spot.error_message;
    ASSERT_EQ(spot.palette->colors.size(), 1u);
    auto ink = std::get_if<UI::Dialog::PaletteFileData::SpotColor>(&spot.palette->colors[0]);
    ASSERT_TRUE(ink);
    EXPECT_EQ(ink->color.getName(), "PROOF 185 C");
    EXPECT_EQ(ink->color.getSpace()->getType(), Type::LAB);

    auto process = UI::Dialog::load_palette(write_temp("proof-process-test.acb", book("spflproc")));
    ASSERT_TRUE(process.palette) << process.error_message;
    EXPECT_TRUE(std::get_if<Color>(&process.palette->colors[0]));
}
