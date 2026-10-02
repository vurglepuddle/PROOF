// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: a CMYK colour must survive the colour picker unchanged.
 *
 * Reproduces a user report: corporate CMYK 5/100/45/22 showed as 0/100/42/26 and drifted when
 * edited. Drives the real picker widgets; needs a display, so it only runs with INKSCAPE_TEST_GUI=1.
 */
#include <cstdlib>
#include <string>
#include <vector>

#include <glibmm/main.h>
#include <gtest/gtest.h>
#include <gtkmm/init.h>
#include <gtkmm/window.h>

#include "colors/color-set.h"
#include "colors/document-cms.h"
#include "colors/spaces/cms.h"
#include "doc-per-case-test.h"
#include "document.h"
#include "extension/init.h"
#include "gradient-chemistry.h"
#include "inkscape.h"
#include "object/sp-gradient.h"
#include "object/sp-item.h"
#include "object/sp-stop.h"
#include "style.h"
#include "colors/color.h"
#include "colors/spaces/base.h"
#include "colors/spaces/enum.h"
#include "preferences.h"
#include "ui/widget/color-picker-panel.h"
#include "ui/widget/edit-operation.h"
#include "ui/widget/paint-switch.h"
#include "ui/widget/generic/spin-button.h"

using namespace Inkscape;
using Colors::Color;
using Colors::ColorSet;
using Colors::Space::Type;
using UI::Widget::ColorPickerPanel;
using UI::Widget::InkSpinButton;

namespace {

void pump()
{
    auto ctx = Glib::MainContext::get_default();
    for (int i = 0; i < 500 && ctx->pending(); ++i) {
        ctx->iteration(false);
    }
}

void collect_spins(Gtk::Widget &widget, std::vector<InkSpinButton *> &out)
{
    if (auto spin = dynamic_cast<InkSpinButton *>(&widget)) {
        out.push_back(spin);
    }
    for (auto child = widget.get_first_child(); child; child = child->get_next_sibling()) {
        collect_spins(*child, out);
    }
}

std::string describe(ColorSet const &set)
{
    return set.isEmpty() ? std::string("(empty)") : set.getAverage().toString();
}

class CmykPickerTest : public ::testing::TestWithParam<ColorPickerPanel::PlateType>
{
protected:
    void SetUp() override
    {
        char const *gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI test: set INKSCAPE_TEST_GUI=1";
        }
        static bool const initialized = [] {
            if (!Application::exists()) Application::create(false);
            gtk_init();
            Gtk::init_gtkmm_internals();
            UI::Widget::register_all();
            Extension::init(); // PaintSwitch loads SVG resources through the import extensions
            return true;
        }();
        (void)initialized;
    }
};

} // namespace

TEST_P(CmykPickerTest, CorporateCmykSurvivesShowingAndEditingCyan)
{
    auto colors = std::make_shared<ColorSet>(); // as PaintSwitch and SwatchEditor create it
    colors->set(Color(0x000000ff)); // PaintSwitch starts in RGB before loading the selection
    Color original(Type::CMYK, {0.05, 1.0, 0.45, 0.22});
    original.setOpacity(1.0); // the paint popover adds the object's opacity
    colors->set(original);

    auto panel = ColorPickerPanel::create(Type::CMYK, GetParam(), colors);
    Gtk::Window window;
    window.set_child(*panel);
    window.present();
    pump();

    auto shown = colors->getAverage();
    EXPECT_EQ(shown.getSpace()->getType(), Type::CMYK) << describe(*colors);
    EXPECT_TRUE(shown.isClose(original, 1e-6)) << "showing the picker changed the colour: " << describe(*colors);

    std::vector<InkSpinButton *> spins;
    collect_spins(*panel, spins);
    ASSERT_GE(spins.size(), 4u);
    std::string values;
    for (auto spin : spins) {
        values += std::to_string(spin->get_value()) + " ";
    }
    EXPECT_NEAR(spins[0]->get_value(), 5.0, 0.05) << "cyan field; all fields: " << values;
    EXPECT_NEAR(spins[3]->get_value(), 22.0, 0.05) << "black field; all fields: " << values;

    // The user nudges cyan only.
    spins[0]->set_value(6.0);
    pump();
    auto edited = colors->getAverage();
    EXPECT_EQ(edited.getSpace()->getType(), Type::CMYK) << describe(*colors);
    EXPECT_NEAR(edited[0], 0.06, 1e-4) << describe(*colors);
    EXPECT_NEAR(edited[1], 1.00, 1e-4) << describe(*colors);
    EXPECT_NEAR(edited[2], 0.45, 1e-4) << describe(*colors);
    EXPECT_NEAR(edited[3], 0.22, 1e-4) << describe(*colors);

    // Loading another object and coming back must not retain its RGB space.
    panel->set_color(Color(0x00ff00ff));
    panel->set_color(edited);
    pump();
    EXPECT_TRUE(colors->getAverage().isClose(edited, 1e-6));

    // Merely opening another picker page, including its first map, is read-only.
    unsigned writes = 0;
    sigc::scoped_connection changes = colors->signal_changed.connect([&] { ++writes; });
    for (auto type : {Type::RGB, Type::HSL, Type::CMYK}) {
        panel->set_picker_type(type);
        pump();
        EXPECT_TRUE(colors->getAverage().isClose(edited, 1e-6)) << describe(*colors);
    }
    EXPECT_EQ(writes, 0u);

    window.unset_child();
}

// Document side (no GUI): the user's document had FOGRA39 linked and a CMYK swatch.
class CmykDocumentTest : public DocPerCaseTest
{};

TEST_F(CmykDocumentTest, CmykFillAndSwatchSurviveWithLinkedFogra)
{
    constexpr auto fogra = "C:/Windows/System32/spool/drivers/color/CoatedFOGRA39.icc";
    if (!g_file_test(fogra, G_FILE_TEST_EXISTS)) {
        GTEST_SKIP() << "CoatedFOGRA39.icc not installed";
    }
    auto svg = std::string(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="200" height="100">
  <defs>
    <color-profile id="fogra" name="Coated-FOGRA39" xlink:href="file:///)SVG") + fogra + R"SVG("/>
    <linearGradient id="kz" inkscape:swatch="solid"><stop offset="0" style="stop-color:device-cmyk(0.05 1 0.45 0.22);stop-opacity:1"/></linearGradient>
  </defs>
  <rect id="flat" width="100" height="100" style="fill:device-cmyk(0.05 1 0.45 0.22)"/>
  <rect id="linked" x="100" width="100" height="100" style="fill:url(#kz)"/>
</svg>)SVG";
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    Color const corporate(Type::CMYK, {0.05, 1.0, 0.45, 0.22});
    auto space = doc->getDocumentCMS().getSpace("Coated-FOGRA39");
    ASSERT_TRUE(space && space->hasValidCmsProfile());
    Color const managed(space, {0.05, 1.0, 0.45, 0.22});
    // Linking alone doesn't assign this profile to device-cmyk paints. The same
    // ink values interpreted by FOGRA have a different preview, without drift.
    EXPECT_NE(corporate.toRGBA(), managed.toRGBA());
    auto managed_reload = doc->getDocumentCMS().parse(managed.toString());
    ASSERT_TRUE(managed_reload);
    EXPECT_TRUE(managed_reload->isClose(managed, 1e-6));
    auto strip = [](Color c) {
        c.enableOpacity(false);
        return c;
    };

    auto flat = cast<SPItem>(doc->getObjectById("flat"));
    ASSERT_TRUE(flat && flat->style->fill.isColor());
    EXPECT_TRUE(strip(flat->style->fill.getColor()).isClose(corporate, 1e-6)) << flat->style->fill.getColor().toString();

    auto swatch = cast<SPGradient>(doc->getObjectById("kz"));
    ASSERT_TRUE(swatch && swatch->getFirstStop());
    EXPECT_TRUE(strip(swatch->getFirstStop()->getColor()).isClose(corporate, 1e-6))
        << swatch->getFirstStop()->getColor().toString();

    // The swatch editor's edit: cyan 5 -> 6, everything else unchanged.
    sp_change_swatch_color(swatch, Color(Type::CMYK, {0.06, 1.0, 0.45, 0.22}));
    EXPECT_TRUE(strip(swatch->getFirstStop()->getColor()).isClose(Color(Type::CMYK, {0.06, 1.0, 0.45, 0.22}), 1e-6))
        << swatch->getFirstStop()->getColor().toString();
}

namespace {

Gtk::Widget *find_named(Gtk::Widget &widget, std::string const &name)
{
    if (widget.get_name() == name) {
        return &widget;
    }
    for (auto child = widget.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto found = find_named(*child, name)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

// The user's flow: Properties > Fill > Swatch tab, CMYK picker, nudge cyan, click away, come back.
TEST_F(CmykPickerTest, SwatchEditorKeepsCorporateCmykThroughEditsAndReselection)
{
    auto doc = SPDocument::createNewDocFromMem(std::string(R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="200" height="100">
  <defs>
    <linearGradient id="kz" inkscape:swatch="solid"><stop offset="0" style="stop-color:device-cmyk(0.05 1 0.45 0.22);stop-opacity:1"/></linearGradient>
    <linearGradient id="rgb" inkscape:swatch="solid"><stop offset="0" style="stop-color:#00ff00"/></linearGradient>
  </defs>
  <rect id="linked" width="100" height="100" style="fill:url(#kz)"/>
  <rect id="other" x="100" width="100" height="100" style="fill:#00ff00"/>
  <rect id="rgb-linked" width="10" height="10" style="fill:url(#rgb)"/>
</svg>)SVG"));
    ASSERT_TRUE(doc);
    Preferences::get()->setString("/color-picker/sel-color-type", "DeviceCMYK"); // the user's setting
    Preferences::get()->setInt("/swatch-editor/color-plate", ColorPickerPanel::None);

    auto swatch = cast<SPGradient>(doc->getObjectById("kz"));
    auto linked = cast<SPItem>(doc->getObjectById("linked"));
    auto other = cast<SPItem>(doc->getObjectById("other"));
    auto rgb_linked = cast<SPItem>(doc->getObjectById("rgb-linked"));
    auto stop_color = [&] {
        auto c = swatch->getFirstStop()->getColor();
        c.enableOpacity(false);
        return c;
    };

    auto paint = UI::Widget::PaintSwitch::create(true, true);
    paint->set_document(doc.get());
    std::vector<std::string> emitted;
    paint->get_swatch_changed().connect([&](SPGradient *vector, auto operation, SPGradient *, std::optional<Color> color, auto) {
        if (operation == UI::EditOperation::Change && vector && color) {
            emitted.push_back(color->toString());
            sp_change_swatch_color(vector, *color); // what PaintAttribute does
        }
    });
    auto show_item = [&](SPItem *item) {
        paint->set_mode(item->style->fill.isPaintserver() ? UI::Widget::PaintMode::Swatch : UI::Widget::PaintMode::Solid);
        paint->update_from_paint(item->style->fill);
        pump();
    };

    Gtk::Window window;
    window.set_child(*paint);
    window.present();
    show_item(rgb_linked); // a swatch editor that previously held an RGB swatch
    show_item(linked);

    Color const corporate(Type::CMYK, {0.05, 1.0, 0.45, 0.22});
    EXPECT_TRUE(stop_color().isClose(corporate, 1e-6)) << "after showing: " << stop_color().toString();

    auto editor = find_named(*paint, "SwatchEditor");
    ASSERT_TRUE(editor);
    std::vector<InkSpinButton *> spins;
    collect_spins(*editor, spins);
    ASSERT_GE(spins.size(), 4u);
    std::string fields;
    for (auto spin : spins) {
        fields += std::to_string(spin->get_value()) + " ";
    }
    EXPECT_NEAR(spins[0]->get_value(), 5.0, 0.05) << "fields: " << fields;
    EXPECT_TRUE(emitted.empty()) << "Loading a swatch must not write a colour";

    spins[0]->set_value(6.0);
    pump();
    Color const edited(Type::CMYK, {0.06, 1.0, 0.45, 0.22});
    EXPECT_TRUE(stop_color().isClose(edited, 1e-4)) << "after cyan edit: " << stop_color().toString();

    // Click away to another object and back, as the user did.
    show_item(other);
    show_item(rgb_linked);
    show_item(linked);
    EXPECT_TRUE(stop_color().isClose(edited, 1e-4)) << "after reselecting: " << stop_color().toString();
    ASSERT_EQ(emitted.size(), 1u) << "Only the cyan edit may write a colour";

    std::string log;
    for (auto &e : emitted) {
        log += e + "\n";
    }
    RecordProperty("emitted", log);
    std::printf("swatch colours written:\n%s", log.c_str());
    window.unset_child();
}

INSTANTIATE_TEST_SUITE_P(PlateTypes, CmykPickerTest,
                         ::testing::Values(ColorPickerPanel::None, ColorPickerPanel::Circle, ColorPickerPanel::Rect));
