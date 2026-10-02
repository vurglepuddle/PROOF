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
#include <glibmm/i18n.h>
#include <gtest/gtest.h>
#include <gtkmm/init.h>
#include <gtkmm/window.h>

#include "colors/color-set.h"
#include "colors/document-cms.h"
#include "colors/document-colors.h"
#include "colors/cms/profile.h"
#include "colors/spaces/cms.h"
#include "doc-per-case-test.h"
#include "document.h"
#include "document-undo.h"
#include "extension/init.h"
#include "gradient-chemistry.h"
#include "inkscape.h"
#include "object/sp-gradient.h"
#include "object/sp-item.h"
#include "object/sp-stop.h"
#include "spot-ink.h"
#include "xml/repr.h"
#include "style.h"
#include "colors/color.h"
#include "colors/spaces/base.h"
#include "colors/spaces/enum.h"
#include "preferences.h"
#include "ui/widget/color-picker-panel.h"
#include "ui/widget/document-color-settings.h"
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

namespace DC = Colors::DocumentColors;

namespace {
std::unique_ptr<SPDocument> color_document() {
    return SPDocument::createNewDocFromMem(std::string(R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
        xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="200" height="100">
        <defs><linearGradient id="process" inkscape:swatch="solid">
        <stop id="stop" offset="0" style="stop-color:device-cmyk(0.05 1 0.45 0.22);stop-opacity:0.7"/>
        </linearGradient></defs>
        <rect id="flat" width="100" height="100" style="fill:device-cmyk(0.05 1 0.45 0.22);fill-opacity:0.4"/>
        <rect id="rgb" x="100" width="100" height="100" fill="#cd2468"/>
        </svg>)SVG"));
}
std::shared_ptr<Colors::CMS::Profile> test_profile() {
    return Colors::CMS::Profile::create_from_uri(INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc");
}
Color fill_color(SPDocument *doc) { return doc->getObjectById("flat")->style->fill.getColor(); }
void expect_channels(Color const &color) {
    EXPECT_EQ(color.getSpace()->getComponentType(), Type::CMYK);
    ASSERT_GE(color.size(), 4u);
    EXPECT_NEAR(color[0], 0.05, 1e-6);
    EXPECT_NEAR(color[1], 1.0, 1e-6);
    EXPECT_NEAR(color[2], 0.45, 1e-6);
    EXPECT_NEAR(color[3], 0.22, 1e-6);
}
}

TEST_F(CmykDocumentTest, AssignEmbedsProfilePreservesInkAndRoundTrips) {
    auto document = color_document();
    auto other = color_document();
    auto profile = test_profile();
    ASSERT_TRUE(profile);
    auto before = fill_color(document.get()).toRGBA();
    ASSERT_TRUE(DC::assign(document.get(), Type::CMYK, profile, Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    auto assigned = DC::assignedSpace(document.get());
    ASSERT_TRUE(assigned);
    expect_channels(fill_color(document.get()));
    EXPECT_NE(fill_color(document.get()).toRGBA(), before);
    EXPECT_EQ(fill_color(other.get()).toRGBA(), before) << "Another window must remain unchanged";
    EXPECT_FALSE(DC::assignedSpace(other.get()));
    EXPECT_NEAR(document->getObjectById("flat")->style->fill_opacity.as_double(), 0.4, 1e-6);
    auto stop = cast<SPStop>(document->getObjectById("stop"));
    expect_channels(stop->getColor());
    EXPECT_NEAR(stop->getColor().getOpacity(), 0.7, 1e-6);
    auto parsed = document->getDocumentCMS().parse("device-cmyk(0.05 1 0.45 0.22)");
    ASSERT_TRUE(parsed);
    expect_channels(*parsed);
    EXPECT_EQ(parsed->getSpace(), assigned);

    auto saved = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_NE(saved.find("base64,"), std::string::npos) << "Assigned ICC must travel with the SVG";
    auto reloaded = SPDocument::createNewDocFromMem(saved);
    ASSERT_TRUE(reloaded);
    expect_channels(fill_color(reloaded.get()));
    EXPECT_EQ(fill_color(reloaded.get()).toRGBA(), fill_color(document.get()).toRGBA());
    ASSERT_TRUE(DC::assignedSpace(reloaded.get()));
    EXPECT_EQ(DC::assignedSpace(reloaded.get())->getProfile()->dumpData(), assigned->getProfile()->dumpData());
}

TEST_F(CmykDocumentTest, AssignUndoRedoAndUnmanagedPreserveChannels) {
    auto document = color_document();
    DocumentUndo::setUndoSensitive(document.get(), true);
    auto before = fill_color(document.get()).toRGBA();
    ASSERT_TRUE(DC::assign(document.get(), Type::CMYK, test_profile(), Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    DocumentUndo::done(document.get(), RC_("Undo", "Assign profile"), "");
    auto managed = fill_color(document.get()).toRGBA();
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_FALSE(DC::assignedSpace(document.get()));
    expect_channels(fill_color(document.get()));
    EXPECT_EQ(fill_color(document.get()).toRGBA(), before);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    ASSERT_TRUE(DC::assignedSpace(document.get())) << "Redo must restore the assigned ICC profile";
    expect_channels(fill_color(document.get()));
    EXPECT_EQ(fill_color(document.get()).toRGBA(), managed);
    ASSERT_TRUE(DC::assign(document.get(), Type::CMYK, {}, Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    EXPECT_FALSE(DC::assignedSpace(document.get()));
    expect_channels(fill_color(document.get()));
    EXPECT_EQ(fill_color(document.get()).toRGBA(), before);
}

TEST_F(CmykDocumentTest, ModeConversionAndWrongProfileValidation) {
    auto document = color_document();
    auto rgb = Colors::CMS::Profile::create_srgb();
    auto original = sp_repr_save_buf(document->getReprDoc());
    EXPECT_FALSE(DC::assign(document.get(), Type::CMYK, rgb, Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), original);
    SpotInk::Ink ink{"Corporate Spot", Color(Type::CMYK, {0.05, 1, 0.45, 0.22}), 0.5};
    auto spot_id = SpotInk::ensure(document.get(), ink);
    auto spot_before = SpotInk::read(document->getObjectById(spot_id)->getRepr());
    ASSERT_TRUE(DC::assign(document.get(), Type::CMYK, test_profile(), Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    auto expected = fill_color(document.get()).converted(Type::RGB);
    ASSERT_TRUE(expected);
    ASSERT_TRUE(DC::assign(document.get(), Type::RGB, rgb, Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    EXPECT_EQ(fill_color(document.get()).getSpace()->getComponentType(), Type::RGB);
    EXPECT_EQ(fill_color(document.get()).toRGBA(), expected->toRGBA());
    auto spot_after = SpotInk::read(document->getObjectById(spot_id)->getRepr());
    ASSERT_TRUE(spot_after && spot_before);
    EXPECT_EQ(spot_after->alternate, spot_before->alternate);
    EXPECT_EQ(spot_after->tint, spot_before->tint);
}

TEST_F(CmykPickerTest, ProfiledCmykSlidersEditInkWithoutRgbRoundTrip) {
    auto document = color_document();
    ASSERT_TRUE(DC::assign(document.get(), Type::CMYK, test_profile(), Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    auto colors = std::make_shared<ColorSet>();
    colors->set(Color(0x000000ff));
    auto panel = ColorPickerPanel::create(Type::HSL, ColorPickerPanel::None, colors);
    panel->set_color(fill_color(document.get()));
    Gtk::Window window;
    window.set_child(*panel);
    window.present();
    pump();
    std::vector<InkSpinButton *> spins;
    collect_spins(*panel, spins);
    ASSERT_GE(spins.size(), 4u);
    EXPECT_NEAR(spins[0]->get_value(), 5.0, 1e-6);
    EXPECT_NEAR(spins[3]->get_value(), 22.0, 1e-6);
    spins[0]->set_value(6.0);
    pump();
    auto edited = colors->getAverage();
    EXPECT_EQ(edited.getSpace(), DC::assignedSpace(document.get()));
    EXPECT_NEAR(edited[0], 0.06, 1e-6);
    EXPECT_NEAR(edited[1], 1.0, 1e-6);
    EXPECT_NEAR(edited[2], 0.45, 1e-6);
    EXPECT_NEAR(edited[3], 0.22, 1e-6);
    window.unset_child();
}

TEST_F(CmykPickerTest, DocumentSettingsFilterProfilesAndApplyToCurrentDocument) {
    auto document = color_document();
    auto other = color_document();
    ASSERT_TRUE(DC::assign(document.get(), Type::CMYK, test_profile(), Colors::RenderingIntent::RELATIVE_COLORIMETRIC).empty());
    UI::Widget::DocumentColorSettings settings;
    settings.set_document(document.get());
    Gtk::Window window;
    window.set_default_size(580, 680);
    window.set_child(settings);
    window.present();
    pump();
    auto mode = dynamic_cast<Gtk::ComboBoxText *>(find_named(settings, "document-color-mode"));
    auto assignment = dynamic_cast<Gtk::ComboBoxText *>(find_named(settings, "document-color-assignment"));
    auto profiles = dynamic_cast<Gtk::ComboBoxText *>(find_named(settings, "document-color-profile"));
    auto apply = dynamic_cast<Gtk::Button *>(find_named(settings, "document-color-apply"));
    ASSERT_TRUE(mode && assignment && profiles && apply);
    EXPECT_EQ(mode->get_active_id(), "cmyk");
    EXPECT_EQ(assignment->get_active_id(), "profile");
    EXPECT_EQ(profiles->get_active_text(), DC::assignedSpace(document.get())->getProfile()->getName());
    for (auto const &profile : DC::profiles(Type::CMYK)) EXPECT_EQ(profile->getColorSpace(), cmsSigCmykData);
    for (auto const &profile : DC::profiles(Type::RGB)) EXPECT_EQ(profile->getColorSpace(), cmsSigRgbData);

    if (auto path = std::getenv("INKSCAPE_COLOR_SETTINGS_SNAPSHOT")) {
        auto paintable = gtk_widget_paintable_new(GTK_WIDGET(settings.gobj()));
        auto snapshot = gtk_snapshot_new();
        GdkRGBA background{1, 1, 1, 1};
        graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, float(settings.get_width()), float(settings.get_height()));
        gtk_snapshot_append_color(snapshot, &background, &bounds);
        gdk_paintable_snapshot(paintable, snapshot, settings.get_width(), settings.get_height());
        auto node = gtk_snapshot_free_to_node(snapshot);
        ASSERT_TRUE(node);
        auto texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window.gobj())), node, nullptr);
        ASSERT_TRUE(texture);
        EXPECT_TRUE(gdk_texture_save_to_png(texture, path));
        g_object_unref(texture);
        gsk_render_node_unref(node);
        g_object_unref(paintable);
    }
    mode->set_active_id("rgb");
    assignment->set_active_id("working");
    g_signal_emit_by_name(apply->gobj(), "clicked");
    pump();
    EXPECT_EQ(DC::mode(document.get()), Type::RGB);
    ASSERT_TRUE(DC::assignedSpace(document.get()));
    EXPECT_EQ(DC::assignedSpace(document.get())->getProfile()->getColorSpace(), cmsSigRgbData);
    settings.set_document(other.get());
    EXPECT_EQ(assignment->get_active_id(), "none");
    EXPECT_FALSE(DC::assignedSpace(other.get()));
    settings.set_document(nullptr);
    EXPECT_FALSE(settings.get_sensitive());
    window.unset_child();
}
