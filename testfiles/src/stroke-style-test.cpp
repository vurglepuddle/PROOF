// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Refreshing stroke controls and leaving untouched entries must not edit the document.
 * GTK needs a display; opt in with INKSCAPE_TEST_GUI=1. These tests open no windows.
 */

#include <cstdlib>
#include <string>
#include <vector>

#include <glibmm/main.h>
#include <glibmm/i18n.h>
#include <gtest/gtest.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/gridview.h>
#include <gtkmm/init.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/singleselection.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "extension/init.h"
#include "inkscape.h"
#include "style.h"
#include "ui/widget/dash-selector.h"
#include "ui/widget/generic/spin-button.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/ink-property-grid.h"
#include "ui/widget/paint-attribute.h"
#include "ui/widget/stroke-options.h"
#include "util/scope_exit.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::UI::Widget;

namespace {

void pump()
{
    auto context = Glib::MainContext::get_default();
    for (int i = 0; i < 500 && context->pending(); ++i) {
        context->iteration(false);
    }
}

template <typename T>
std::vector<T *> collect(Gtk::Widget &widget)
{
    std::vector<T *> result;
    if (auto match = dynamic_cast<T *>(&widget)) result.push_back(match);
    for (auto child = widget.get_first_child(); child; child = child->get_next_sibling()) {
        auto matches = collect<T>(*child);
        result.insert(result.end(), matches.begin(), matches.end());
    }
    return result;
}

void leave_entry(InkSpinButton &spin)
{
    auto entries = collect<Gtk::Entry>(spin);
    ASSERT_EQ(entries.size(), 1u);
    entries.front()->set_visible(true);
    auto controllers = gtk_widget_observe_controllers(spin.gobj());
    for (unsigned i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_FOCUS(controller)) {
            g_signal_emit_by_name(controller, "leave");
        }
        g_object_unref(controller);
    }
    g_object_unref(controllers);
}

class StrokeStyleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GTK widget test: set INKSCAPE_TEST_GUI=1 (no windows are opened)";
        }
        static bool const initialized = [] {
            if (!Application::exists()) Application::create(false);
            gtk_init();
            Gtk::init_gtkmm_internals();
            register_all();
            Extension::init();
            return true;
        }();
        (void)initialized;
    }
};

class DashSelectorTest : public StrokeStyleTest, public ::testing::WithParamInterface<bool> {};

TEST_P(DashSelectorTest, RefreshIsSilent)
{
    DashSelector selector(GetParam());
    unsigned changes = 0;
    selector.changed_signal.connect([&](auto) { ++changes; });

    for (int i = 0; i < 3; ++i) {
        selector.set_dash_pattern({1, 2, 3, 4}, 0.123456789);
        selector.set_dash_pattern({3, 7}, 2.7654321);
        selector.set_dash_pattern({}, 0);
        pump();
        EXPECT_TRUE(selector.get_dash_pattern().empty());
        EXPECT_EQ(selector.get_offset(), 0);
    }
    EXPECT_EQ(changes, 0u);
}

TEST_P(DashSelectorTest, ChoosingSolidEmitsOneDashChange)
{
    DashSelector selector(GetParam());
    selector.set_dash_pattern({1, 2, 3, 4}, 2.5);
    std::vector<DashSelector::Change> changes;
    selector.changed_signal.connect([&](auto change) { changes.push_back(change); });

    auto menu = dynamic_cast<Gtk::MenuButton *>(selector.get_first_child());
    ASSERT_TRUE(menu);
    ASSERT_TRUE(menu->get_popover());
    auto grids = collect<Gtk::GridView>(*menu->get_popover());
    ASSERT_EQ(grids.size(), 1u);
    auto selection = std::dynamic_pointer_cast<Gtk::SingleSelection>(grids.front()->get_model());
    ASSERT_TRUE(selection);
    selection->set_selected(0); // Solid in the dash palette.
    g_signal_emit_by_name(grids.front()->gobj(), "activate", 0u);

    EXPECT_EQ(changes, (std::vector<DashSelector::Change>{DashSelector::Dash}));
    EXPECT_TRUE(selector.get_dash_pattern().empty());
    EXPECT_EQ(selector.get_offset(), 0);
}

TEST_P(DashSelectorTest, EditingOffsetStillEmits)
{
    DashSelector selector(GetParam());
    selector.set_dash_pattern({1, 2, 3, 4}, 1.25);
    std::vector<DashSelector::Change> changes;
    selector.changed_signal.connect([&](auto change) { changes.push_back(change); });

    InkSpinButton *spin = nullptr;
    if (GetParam()) {
        auto menu = dynamic_cast<Gtk::MenuButton *>(selector.get_first_child());
        ASSERT_TRUE(menu);
        ASSERT_TRUE(menu->get_popover());
        auto spins = collect<InkSpinButton>(*menu->get_popover());
        ASSERT_EQ(spins.size(), 1u);
        spin = spins.front();
    } else {
        spin = dynamic_cast<InkSpinButton *>(selector.get_last_child());
    }
    ASSERT_TRUE(spin);
    spin->set_value(2.5);
    EXPECT_EQ(changes, (std::vector<DashSelector::Change>{DashSelector::Offset}));
    EXPECT_EQ(selector.get_offset(), 2.5);
}

INSTANTIATE_TEST_SUITE_P(Layouts, DashSelectorTest, ::testing::Bool());

TEST_F(StrokeStyleTest, UntouchedSpinEntryPreservesFullPrecision)
{
    InkSpinButton spin;
    spin.set_digits(2);
    spin.set_value(1.23456789);
    unsigned changes = 0;
    spin.signal_value_changed().connect([&](double) { ++changes; });

    leave_entry(spin);
    EXPECT_DOUBLE_EQ(spin.get_value(), 1.23456789);
    EXPECT_EQ(changes, 0u);
}

TEST_F(StrokeStyleTest, EditedSpinEntryStillCommits)
{
    InkSpinButton spin;
    spin.set_value(1.23456789);
    auto entries = collect<Gtk::Entry>(spin);
    ASSERT_EQ(entries.size(), 1u);
    unsigned changes = 0;
    spin.signal_value_changed().connect([&](double) { ++changes; });
    entries.front()->set_text("2 + 0.75");

    leave_entry(spin);
    EXPECT_DOUBLE_EQ(spin.get_value(), 2.75);
    EXPECT_EQ(changes, 1u);
}

TEST_F(StrokeStyleTest, StrokeOptionsRefreshIsSilentAndDoesNotMultiplyHandlers)
{
    StrokeOptions options;
    unsigned joins = 0, caps = 0, miters = 0, orders = 0;
    options._join_changed.connect([&](auto) { ++joins; });
    options._cap_changed.connect([&](auto) { ++caps; });
    options._miter_changed.connect([&](auto) { ++miters; });
    options._order_changed.connect([&](auto) { ++orders; });

    SPStyle style;
    for (int i = 0; i < 3; ++i) {
        style.clear();
        style.mergeString("stroke-miterlimit:7.123456;stroke-linejoin:round;stroke-linecap:square;paint-order:stroke fill markers");
        options.update_widgets(style);
        style.clear();
        style.mergeString("stroke-miterlimit:4;stroke-linejoin:miter;stroke-linecap:butt;paint-order:normal");
        options.update_widgets(style);
    }
    EXPECT_EQ(joins, 0u);
    EXPECT_EQ(caps, 0u);
    EXPECT_EQ(miters, 0u);
    EXPECT_EQ(orders, 0u);

    auto spins = collect<InkSpinButton>(options);
    ASSERT_EQ(spins.size(), 1u);
    spins.front()->set_value(8);
    EXPECT_EQ(miters, 1u);
    auto paint_orders = collect<PaintOrderWidget>(options);
    ASSERT_EQ(paint_orders.size(), 1u);
    paint_orders.front()->signal_values_changed().emit();
    EXPECT_EQ(orders, 1u);
}

TEST_F(StrokeStyleTest, PaintAttributeRefreshAndUntouchedEntriesPreserveDocumentAndUndo)
{
    // Marker controls create a shared ColorNotebook that expects an active desktop.
    // Keep its document separate: an unshown desktop has no title widget for save notifications.
    auto desktop_document = SPDocument::createNewDoc(nullptr, true);
    ASSERT_TRUE(desktop_document);
    SPDesktop desktop(desktop_document->getNamedView());
    INKSCAPE.add_desktop(&desktop);
    auto unregister_desktop = scope_exit([&] { INKSCAPE.remove_desktop(&desktop); });

    std::string const svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <rect id="solid" width="10" height="10"
        style="fill:#123456;fill-opacity:0.876543;stroke:#654321;stroke-width:1.23456789;stroke-miterlimit:7.123456;opacity:0.734567"/>
      <rect id="dashed" x="20" width="10" height="10"
        style="fill:none;stroke:#123456;stroke-width:2;stroke-dasharray:3,7;stroke-dashoffset:0.23456789;stroke-linecap:round;paint-order:stroke fill markers"/>
    </svg>)";
    auto document = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    InkPropertyGrid grid;
    PaintAttribute paint(PaintAttribute::AllParts, 0);
    paint.insert_widgets(grid);
    paint.set_document(document.get());

    DocumentUndo::setUndoSensitive(document.get(), true);
    DocumentUndo::clearUndo(document.get());
    DocumentUndo::clearRedo(document.get());
    document->setModifiedSinceSave(false);
    auto before = sp_repr_save_buf(document->getReprDoc());
    for (auto id : {"dashed", "solid", "dashed", "solid"}) {
        paint.update_from_object(document->getObjectById(id));
        pump();
        for (auto spin : collect<InkSpinButton>(grid)) {
            leave_entry(*spin);
        }
        pump();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
        EXPECT_FALSE(document->isModifiedSinceSave());
    }
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::redo(document.get()));
}

} // namespace
