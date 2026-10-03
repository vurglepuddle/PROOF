// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Desktop widget implementation
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   MenTaLguY <mental@rydia.net>
 *   bulia byak <buliabyak@users.sf.net>
 *   Ralf Stephan <ralf@ark.in-berlin.de>
 *   John Bintz <jcoswell@coswellproductions.org>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2006 John Bintz
 * Copyright (C) 2004 MenTaLguY
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "desktop-widget.h"

#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/paned.h>
#include <gtkmm/popovermenu.h>

#include "actions/actions-view-mode.h"
#include "colors/document-cms.h"
#include "colors/document-colors.h"
#include "colors/spaces/enum.h"
#include "conn-avoid-ref.h"
#include "document.h"
#include "enums.h"
#include "helper/mathfns.h"
#include "inkscape-version.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-namedview.h"
#include "ui/dialog-run.h"
#include "ui/dialog/dialog-container.h"
#include "ui/dialog/dialog-multipaned.h"
#include "ui/dialog/swatches.h"
#include "ui/popup-menu.h"
#include "ui/themes.h"
#include "ui/toolbar/command-toolbar.h"
#include "ui/toolbar/snap-toolbar.h"
#include "ui/toolbar/tool-toolbar.h"
#include "ui/toolbar/toolbar-constants.h"
#include "ui/toolbar/toolbars.h"
#include "ui/tools/tool-base.h"
#include "ui/tools/tool-data.h"
#include "ui/util.h"
#include "ui/widget/canvas-grid.h"
#include "ui/widget/canvas.h"
#include "ui/widget/desktop-tab-controls.h"
#include "ui/widget/ink-ruler.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/status-bar.h"
#include "util/units.h"

using namespace Inkscape;
using Inkscape::DocumentUndo;
using Inkscape::UI::Dialog::DialogContainer;
using Inkscape::UI::Dialog::DialogMultipaned;
using Inkscape::UI::Dialog::DialogWindow;
using Inkscape::UI::Widget::UnitTracker;

Gtk::PopoverMenu create_toolbar_context_menu()
{
    auto menu = Gio::Menu::create();

    auto section = Gio::Menu::create();
    section->append_item(Gio::MenuItem::create(_("Commands Bar"), "win.canvas-commands-bar"));
    section->append_item(Gio::MenuItem::create(_("Snap Controls Bar"), "win.canvas-snap-controls-bar"));
    section->append_item(Gio::MenuItem::create(_("Tool Controls Bar"), "win.canvas-tool-control-bar"));
    section->append_item(Gio::MenuItem::create(_("Toolbox"), "win.canvas-toolbox"));
    section->append_item(Gio::MenuItem::create(_("Rulers"), "win.canvas-rulers"));
    section->append_item(Gio::MenuItem::create(_("Scroll bars"), "win.canvas-scroll-bars"));
    section->append_item(Gio::MenuItem::create(_("Palette"), "win.canvas-palette"));
    section->append_item(Gio::MenuItem::create(_("Statusbar"), "win.canvas-statusbar"));
    menu->append_section(section);

    auto popovermenu = Gtk::PopoverMenu{menu};
    popovermenu.set_has_arrow(false);
    return popovermenu;
}

//---------------------------------------------------------------------
/* SPDesktopWidget */

SPDesktopWidget::SPDesktopWidget(InkscapeWindow *inkscape_window)
    : Gtk::Box(Gtk::Orientation::VERTICAL)
    , _window{inkscape_window}
{
    set_name("SPDesktopWidget");

    auto prefs = Inkscape::Preferences::get();

    /* Status bar */
    _statusbar = Gtk::make_managed<Inkscape::UI::Widget::StatusBar>();
    _statusbar->set_vexpand(false);
    prepend(*_statusbar);

    /* Swatch Bar */
    _panels = Gtk::make_managed<Inkscape::UI::Dialog::SwatchesPanel>(UI::Dialog::SwatchesPanel::Compact, "/embedded/swatches");
    _panels->set_vexpand(false);
    prepend(*_panels);

    /* DesktopHBox (Vertical toolboxes, canvas) */
    _hbox = Gtk::make_managed<Gtk::Box>();
    _hbox->set_vexpand(true);
    _hbox->set_name("DesktopHbox");

    _tbbox = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    _tbbox->set_vexpand(true);
    _tbbox->set_name("ToolboxCanvasPaned");
    _hbox->append(*_tbbox);

    prepend(*_hbox);

    _top_toolbars = Gtk::make_managed<Gtk::Grid>();
    _top_toolbars->set_name("TopToolbars");

    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_SECONDARY);
    click->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
    click->signal_pressed().connect([this](int, double x, double y) {
        auto menu = create_toolbar_context_menu();
        menu.set_parent(*_top_toolbars);
        UI::popup_at(menu, *_top_toolbars, x, y);
    });
    _top_toolbars->add_controller(click);

    prepend(*_top_toolbars);

    /* Toolboxes */
    tool_toolbars = std::make_unique<Inkscape::UI::Toolbar::Toolbars>();
    _top_toolbars->attach(*tool_toolbars, 0, 1);

    tool_toolbox = Gtk::make_managed<Inkscape::UI::Toolbar::ToolToolbar>(inkscape_window);
    _tbbox->set_start_child(*tool_toolbox);
    _tbbox->set_resize_start_child(false);
    _tbbox->set_shrink_start_child(false);
    auto adjust_pos = [=, this](){
        int minimum_width, natural_width;
        int ignore;
        tool_toolbox->measure(Gtk::Orientation::HORIZONTAL, -1, minimum_width, natural_width, ignore, ignore);
        if (minimum_width > 0) {
            int pos = _tbbox->get_position();
            int new_pos = pos + minimum_width / 2;
            const auto max = 5; // max buttons in a row
            new_pos = std::min(new_pos - new_pos % minimum_width, max * minimum_width);
            if (pos != new_pos) _tbbox->set_position(new_pos);
        }
    };
    _tbbox->property_position().signal_changed().connect([=](){ adjust_pos(); });

    auto toolbox_click = Gtk::GestureClick::create();
    toolbox_click->set_button(GDK_BUTTON_SECONDARY);
    toolbox_click->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
    toolbox_click->signal_pressed().connect([this](int, double x, double y) {
        auto menu = create_toolbar_context_menu();
        menu.set_parent(*_tbbox);
        UI::popup_at(menu, *_tbbox, x, y);
    });
    _tbbox->add_controller(toolbox_click);

    snap_toolbar = std::make_unique<Inkscape::UI::Toolbar::SnapToolbar>(_window);
    _hbox->append(*snap_toolbar); // May be moved later.

    _tb_snap_pos = prefs->createObserver("/toolbox/simplesnap", sigc::mem_fun(*this, &SPDesktopWidget::repack_snaptoolbar));
    repack_snaptoolbar();

    auto tbox_width = prefs->getEntry("/toolbox/tools/width");
    if (tbox_width.isSet()) {
        _tbbox->set_position(tbox_width.getIntLimited(32, 8, 500));
    }

    auto set_toolbar_prefs = [=, this]() {
        int min = Inkscape::UI::Toolbar::min_pixel_size;
        int max = Inkscape::UI::Toolbar::max_pixel_size;
        int s = prefs->getIntLimited(Inkscape::UI::Toolbar::tools_icon_size, min, min, max);
        Inkscape::UI::set_icon_sizes(tool_toolbox, s);
        adjust_pos();
    };

    // watch for changes
    _tb_icon_sizes1 = prefs->createObserver(Inkscape::UI::Toolbar::tools_icon_size,    [=]() { set_toolbar_prefs(); });
    _tb_icon_sizes2 = prefs->createObserver(Inkscape::UI::Toolbar::ctrlbars_icon_size, [this]() { apply_ctrlbar_settings(); });

    // restore preferences
    set_toolbar_prefs();

    /* Canvas Grid (canvas, rulers, scrollbars, etc.) */
    // DialogMultipaned owns it
    auto cg = std::make_unique<Inkscape::UI::Widget::CanvasGrid>(this);
    _canvas_grid = cg.get();

    /* Canvas */
    _ds_sticky_zoom = prefs->createObserver("/options/stickyzoom/value", [this]() { sticky_zoom_updated(); });
    sticky_zoom_updated();

    _version_in_title_observer = Preferences::get()->createObserver("/options/versionintitle/value", [this] {
        _updateTitle();
    });

    /* Dialog Container */
    _container = std::make_unique<DialogContainer>(inkscape_window);
    _columns = _container->get_columns();
    _tbbox->set_end_child(*_container);
    _tbbox->set_resize_end_child(true);
    _tbbox->set_shrink_end_child(true);

    // separator widget in tbox
    auto& tbox_separator = *_tbbox->get_children().at(1);
    tbox_separator.set_name("TBoxCanvasSeparator");

    _canvas_grid->set_hexpand(true);
    _canvas_grid->set_vexpand(true);
    _columns->append(std::move(cg));

    // ------------------ Finish Up -------------------- //
    _canvas_grid->ShowCommandPalette(false);

    snap_toolbar->mode_update(); // Hide/show parts.

    command_toolbar = std::make_unique<Inkscape::UI::Toolbar::CommandToolbar>();
    _top_toolbars->attach(*command_toolbar, 0, 0);

    // Applying the saved settings after all the toolbars have been created.
    apply_ctrlbar_settings();
}

void SPDesktopWidget::addDesktop(SPDesktop *desktop, int pos)
{
    desktop->setDesktopWidget(this);
    _desktops.push_back(desktop);
    _canvas_grid->addTab(desktop->getCanvas());
    _canvas_grid->getTabsWidget()->addTab(desktop, pos);

    switchDesktop(desktop);
}

void SPDesktopWidget::removeDesktop(SPDesktop *desktop)
{
    auto const it = std::find(_desktops.begin(), _desktops.end(), desktop);
    assert(it != _desktops.end());

    auto tabs = _canvas_grid->getTabsWidget();

    if (desktop == _desktop) {
        if (_desktops.size() > 1) {
            auto oldpos = tabs->positionOfTab(_desktop);
            auto newpos = oldpos == _desktops.size() - 1 ? oldpos - 1 : oldpos + 1;
            switchDesktop(tabs->tabAtPosition(newpos));
        } else {
            switchDesktop(nullptr);
        }
    }

    tabs->removeTab(desktop);
    _canvas_grid->removeTab(desktop->getCanvas());
    _desktops.erase(it);
    desktop->setDesktopWidget(nullptr);

    if (_desktops.empty()) {
        InkscapeApplication::instance()->windowClose(_window);
    }
}

void SPDesktopWidget::switchDesktop(SPDesktop *desktop)
{
    if (desktop == _desktop) {
        return;
    }

    _desktop = desktop;
    _canvas = _desktop ? _desktop->getCanvas() : nullptr;

    _canvas_grid->switchTab(_canvas);

    if (_desktop) {
        _canvas->grab_focus();

        // Add the shape geometry to libavoid for autorouting connectors.
        // This needs desktop set for its spacing preferences.
        init_avoided_shape_geometry(_desktop);
    }

    _statusbar->set_desktop(_desktop);

    if (_desktop) {
        auto set_tool = [this] {
            tool_toolbars->setTool(_desktop->getTool());
            tool_toolbars->setActiveUnit(_desktop->getNamedView()->getDisplayUnit());
            apply_ctrlbar_settings(); // Apply size settings after populating the tool_toolbars
        };
        _tool_changed_conn = _desktop->connectEventContextChanged([=] (auto, auto) { set_tool(); });
        set_tool();
    } else {
        tool_toolbars->setTool(nullptr);
    }

    _panels->setDesktop(_desktop);

    if (_desktop) {
        layoutWidgets();

        _updateNamedview(); // sets _dt2r, required by updateRulers()

        // Once desktop is set, we can update rulers
        _canvas_grid->updateRulers();

        auto msgstack = _desktop->messageStack();
        setMessage(msgstack->currentMessageType(), msgstack->currentMessage());

        auto tabs = _canvas_grid->getTabsWidget();
        tabs->switchTab(_desktop);

        // Update window's current active tool, display mode, colour mode, cms mode
        // Todo: These should really be tab- or canvas-level actions.
        if (auto action = dynamic_cast<Gio::SimpleAction *>(_window->lookup_action("tool-switch").get())) {
            action->set_state(Glib::Variant<Glib::ustring>::create(pref_path_to_tool_name(_desktop->getTool()->getPrefsPath().c_str())));
        }
        if (auto action = dynamic_cast<Gio::SimpleAction *>(_window->lookup_action("canvas-display-mode").get())) {
            action->set_state(Glib::Variant<int>::create((int)_desktop->getCanvas()->get_render_mode()));
        }
        if (auto action = dynamic_cast<Gio::SimpleAction *>(_window->lookup_action("canvas-color-mode").get())) {
            action->set_state(Glib::Variant<bool>::create(_desktop->getCanvas()->get_color_mode() == Inkscape::ColorMode::GRAYSCALE));
        }
        if (auto action = dynamic_cast<Gio::SimpleAction *>(_window->lookup_action("canvas-split-mode").get())) {
            action->set_state(Glib::Variant<int>::create((int)_desktop->getCanvas()->get_split_mode()));
        }
        if (auto action = dynamic_cast<Gio::SimpleAction *>(_window->lookup_action("canvas-color-manage").get())) {
            action->set_state(Glib::Variant<bool>::create(_desktop->getCanvas()->get_cms_active()));
        }
    }

    _window->setActiveTab(_desktop);
}

Inkscape::UI::Toolbar::Toolbar *SPDesktopWidget::get_current_toolbar()
{
    return tool_toolbars->get_current_toolbar();
}

void SPDesktopWidget::advanceTab(int by)
{
    auto tabs = _canvas_grid->getTabsWidget();
    auto oldpos = tabs->positionOfTab(_desktop);
    auto newpos = Util::safemod<int>(oldpos + by, _desktops.size());
    switchDesktop(tabs->tabAtPosition(newpos));
}

void SPDesktopWidget::apply_ctrlbar_settings()
{
    auto prefs = Preferences::get();
    int min = Inkscape::UI::Toolbar::min_pixel_size;
    int max = Inkscape::UI::Toolbar::max_pixel_size;
    int size = prefs->getIntLimited(Inkscape::UI::Toolbar::ctrlbars_icon_size, min, min, max);
    Inkscape::UI::set_icon_sizes(snap_toolbar.get(), size);
    Inkscape::UI::set_icon_sizes(command_toolbar.get(), size);
    Inkscape::UI::set_icon_sizes(tool_toolbars.get(), size);
}

void SPDesktopWidget::setMessage(Inkscape::MessageType type, char const *message)
{
    _statusbar->set_message(type, message);
}

/**
 * Called before SPDesktopWidget destruction.
 * (Might be called more than once)
 */
void SPDesktopWidget::on_unrealize()
{
    if (_tbbox) {
        Inkscape::Preferences::get()->setInt("/toolbox/tools/width", _tbbox->get_position());
    }

    _panels->setDesktop(nullptr);

    modified_connection.disconnect();
    _desktops.clear();

    _container.reset();

    parent_type::on_unrealize();
}

SPDesktopWidget::~SPDesktopWidget() = default;

/**
 * Set the title in the desktop-window (if desktop has an own window).
 *
 * The title has form file name: desktop number - Inkscape.
 * The desktop number is only shown if it's 2 or higher,
 */
void SPDesktopWidget::_updateTitle()
{
    auto prefs = Preferences::get();

    if (_window) {
        auto const doc = _desktop->doc();

        std::string Name;
        if (doc->isModifiedSinceSave()) {
            Name += "*";
        }

        Name += doc->getDocumentName();

        if (doc->isTemplate()) {
            Name += " (template)";
        }

        if (auto const v = _desktop->viewNumber(); v > 1) {
            Name += ": ";
            Name += std::to_string(v);
        }
        // PROOF: the document color mode always leads, as in Illustrator's title.
        std::vector<std::string> modes{
            Inkscape::Colors::DocumentColors::mode(doc) == Inkscape::Colors::Space::Type::CMYK ? "CMYK" : "RGB"};

        auto const canvas = _desktop->getCanvas();
        auto const render_mode = canvas->get_render_mode();
        auto const color_mode  = canvas->get_color_mode();

        if (render_mode == Inkscape::RenderMode::OUTLINE) {
            modes.emplace_back(N_("outline"));
        } else if (render_mode == Inkscape::RenderMode::NO_FILTERS) {
            modes.emplace_back(N_("no filters"));
        } else if (render_mode == Inkscape::RenderMode::VISIBLE_HAIRLINES) {
            modes.emplace_back(N_("enhance thin lines"));
        } else if (render_mode == Inkscape::RenderMode::OUTLINE_OVERLAY) {
            modes.emplace_back(N_("outline overlay"));
        }

        if (color_mode == Inkscape::ColorMode::GRAYSCALE) {
            modes.emplace_back(N_("grayscale"));
        } else if (color_mode == Inkscape::ColorMode::PRINT_COLORS_PREVIEW) {
            modes.emplace_back(N_("print colors preview"));
        }

        Name += " (";
        for (std::size_t i = 0; i < modes.size(); ++i) {
            Name += (i ? ", " : "") + modes[i];
        }
        Name += ")";

        Name += " - Inkscape";

        if (prefs->getBool("/options/versionintitle/value", true)) {
            Name += " ";
            Name += Inkscape::version_string;
        }

        _window->set_title(Name);
    }
}

DialogContainer *SPDesktopWidget::getDialogContainer()
{
    return _container.get();
}

void SPDesktopWidget::showNotice(Glib::ustring const &msg, int timeout)
{
    _canvas_grid->showNotice(msg, timeout);
}

/**
 * Callback to realize desktop widget.
 */
void SPDesktopWidget::on_realize()
{
    parent_type::on_realize();

    auto const dark = INKSCAPE.themecontext->isCurrentThemeDark(_window);
    Preferences::get()->setBool("/theme/darkTheme", dark);
    INKSCAPE.themecontext->getChangeThemeSignal().emit();
    INKSCAPE.themecontext->add_gtk_css(true);
}

void SPDesktopWidget::desktopChangedDocument(SPDesktop *desktop)
{
    _canvas_grid->getTabsWidget()->refreshTitle(desktop);
    if (desktop == _desktop) {
        _updateNamedview();
    }
}

void SPDesktopWidget::desktopChangedTitle(SPDesktop *desktop)
{
    _canvas_grid->getTabsWidget()->refreshTitle(desktop);
    if (desktop == _desktop) {
        _updateTitle();
    }
}

void SPDesktopWidget::_updateNamedview()
{
    // Listen on namedview modification
    modified_connection = _desktop->getNamedView()->connectModified([this] (auto, unsigned flags) {
        if (flags & SP_OBJECT_MODIFIED_FLAG) {
            _updateUnit();

            // Update unit trackers in certain toolbars, to address https://bugs.launchpad.net/inkscape/+bug/362995.
            tool_toolbars->setActiveUnit(_desktop->getNamedView()->getDisplayUnit());
        }
    });

    _color_mode_connection = _desktop->doc()->getDocumentCMS().connectAssignmentChanged([this] { _updateTitle(); });

    _updateUnit();
    _updateTitle();
}

void
SPDesktopWidget::setCoordinateStatus(Geom::Point p)
{
    _statusbar->set_coordinate(_dt2r * p);
}

void
SPDesktopWidget::letRotateGrabFocus()
{
    _statusbar->rotate_grab_focus();
}

void
SPDesktopWidget::letZoomGrabFocus()
{
    _statusbar->zoom_grab_focus();
}

Geom::IntPoint SPDesktopWidget::getWindowSize() const
{
    if (_window) {
        return {_window->get_width(), _window->get_height()};
    } else {
        return {};
    }
}

void SPDesktopWidget::setWindowSize(Geom::IntPoint const &size)
{
    if (_window) {
        _window->set_default_size(size.x(), size.y());
    }
}

/**
 * \note transientizing does not work on windows; when you minimize a document
 * and then open it back, only its transient emerges and you cannot access
 * the document window. The document window must be restored by rightclicking
 * the taskbar button and pressing "Restore"
 */
void SPDesktopWidget::setWindowTransient(Gtk::Window &window, int transient_policy)
{
    if (_window) {
        window.set_transient_for(*_window);

        /*
         * This enables "aggressive" transientization,
         * i.e. dialogs always emerging on top when you switch documents. Note
         * however that this breaks "click to raise" policy of a window
         * manager because the switched-to document will be raised at once
         * (so that its transients also could raise)
         */
        if (transient_policy == PREFS_DIALOGS_WINDOWS_AGGRESSIVE) {
            // Without this, a transient window doesn't always emerge on top.
            _window->present();
        }
    }
}

void SPDesktopWidget::presentWindow()
{
    if (_window) {
        _window->present();
    }
}

void SPDesktopWidget::showInfoDialog(Glib::ustring const &message)
{
    if (!_window) return;

    Gtk::MessageDialog dialog{*_window, message, false, Gtk::MessageType::INFO, Gtk::ButtonsType::OK};
    dialog.property_destroy_with_parent() = true;
    dialog.set_name("InfoDialog");
    dialog.set_title(_("Note:")); // probably want to take this as a parameter.
    Inkscape::UI::dialog_run(dialog);
}

bool SPDesktopWidget::warnDialog (Glib::ustring const &text)
{
    Gtk::MessageDialog dialog{*_window, text, false, Gtk::MessageType::WARNING, Gtk::ButtonsType::OK_CANCEL};
    auto const response = Inkscape::UI::dialog_run(dialog);
    return response == Gtk::ResponseType::OK;
}

/**
 * Hide whatever the user does not want to see in the window.
 * Also move command toolbar to top or side as required.
 */
void SPDesktopWidget::layoutWidgets()
{
    Glib::ustring pref_root;
    auto prefs = Inkscape::Preferences::get();

    if (_desktop && _desktop->is_focusMode()) {
        pref_root = "/focus/";
    } else if (_desktop && _desktop->is_fullscreen()) {
        pref_root = "/fullscreen/";
    } else {
        pref_root = "/window/";
    }

    command_toolbar->set_visible(prefs->getBool(pref_root + "commands/state", true));

    snap_toolbar->set_visible(prefs->getBool(pref_root + "snaptoolbox/state", true));

    tool_toolbars->set_visible(prefs->getBool(pref_root + "toppanel/state", true));

    tool_toolbox->set_visible(prefs->getBool(pref_root + "toolbox/state", true));

    _statusbar->set_visible(prefs->getBool(pref_root + "statusbar/state", true));
    _statusbar->update_visibility(); // Individual items in bar

    _panels->set_visible(prefs->getBool(pref_root + "panels/state", true));

    _canvas_grid->ShowScrollbars(prefs->getBool(pref_root + "scrollbars/state", true));
    _canvas_grid->ShowRulers(    prefs->getBool(pref_root + "rulers/state",     true));

    // Move command toolbar as required.

    // If interface_mode is unset, use the same screen-aspect-ratio default as the action.
    bool widescreen = get_widescreen_default();
    widescreen = prefs->getBool(pref_root + "interface_mode", widescreen);

    // Unlink command toolbar.
    remove_from_top_toolbar_or_hbox(*command_toolbar);

    // Link command toolbar back.
    auto orientation_c = GTK_ORIENTATION_HORIZONTAL;
    if (!widescreen) {
        _top_toolbars->attach(*command_toolbar, 0, 0); // Always first in Grid
        command_toolbar->set_hexpand(true);
        orientation_c = GTK_ORIENTATION_HORIZONTAL;
    } else {
        _hbox->append(*command_toolbar);
        orientation_c = GTK_ORIENTATION_VERTICAL;
        command_toolbar->set_hexpand(false);
    }
    // Toolbar is actually child:
    for (auto &widget : Inkscape::UI::children(*command_toolbar)) {
        if (auto toolbar = dynamic_cast<Gtk::Box *>(&widget)) {
            gtk_orientable_set_orientation(GTK_ORIENTABLE(toolbar->gobj()), orientation_c); // Missing in C++interface!
        }
    }

    repack_snaptoolbar();
}

Gtk::Widget *SPDesktopWidget::get_toolbar_by_name(const Glib::ustring &name)
{
    auto const widget = Inkscape::UI::find_widget_by_name(*tool_toolbars, name, false);

    if (!widget) {
        std::cerr << "SPDesktopWidget::get_toolbar_by_name: failed to find: " << name << std::endl;
    }

    return widget;
}

void
SPDesktopWidget::setToolboxFocusTo (const gchar* label)
{
    // Look for a named widget
    auto hb = Inkscape::UI::find_widget_by_name(*tool_toolbars, label, true);
    if (hb) {
        hb->grab_focus();
    }
}

void
SPDesktopWidget::setToolboxAdjustmentValue (gchar const *id, double value)
{
    // Look for a named widget
    auto hb = Inkscape::UI::find_widget_by_name(*tool_toolbars, id, true);
    if (hb) {
        auto sb = dynamic_cast<Inkscape::UI::Widget::SpinButton *>(hb);
        auto a = sb->get_adjustment();

        if(a) a->set_value(value);
    } else {
        g_warning ("Could not find GtkAdjustment for %s\n", id);
    }
}

bool
SPDesktopWidget::isToolboxButtonActive(char const * const id) const
{
    auto const widget = const_cast<Gtk::Widget const *>(
        Inkscape::UI::find_widget_by_name(*tool_toolbars, id, true));

    if (!widget) {
        //g_message( "Unable to locate item for {%s}", id );
        return false;
    }

    if (auto const button = dynamic_cast<Gtk::ToggleButton const *>(widget)) {
        return button->get_active();
    }

    //g_message( "Item for {%s} is of an unsupported type", id );
    return false;
}

/**
 * Choose where to pack the snap toolbar.
 * Hiding/unhiding is done in the SnapToolbar widget.
 */
void SPDesktopWidget::repack_snaptoolbar()
{
    auto prefs = Inkscape::Preferences::get();
    bool is_perm = prefs->getInt("/toolbox/simplesnap", 1) == 2;
    auto &aux = *tool_toolbars;
    auto &snap = *snap_toolbar;

    // Only remove from the parent if the status has changed
    auto parent = snap.get_parent();
    if (parent && ((is_perm && parent != _hbox) || (!is_perm && parent != _top_toolbars))) {
        remove_from_top_toolbar_or_hbox(snap);
    }

    // Only repack if there's no parent widget now.
    if (!snap.get_parent()) {
        if (is_perm) {
            _hbox->append(snap);
        } else {
            _top_toolbars->attach(snap, 1, 0, 1, 2);
        }
    }

    // Always reset the various constraints, even if not repacked.
    if (is_perm) {
        snap.set_valign(Gtk::Align::START);
        return;
    }

    // This ensures that the Snap toolbox is on the top and only takes the needed space.
    _top_toolbars->remove(aux);
    _top_toolbars->remove(snap);
    if (Inkscape::UI::get_n_children(*_top_toolbars) == 3 && command_toolbar->get_visible()) {
        _top_toolbars->attach(aux, 0, 1, 2, 1);
        _top_toolbars->attach(snap, 1, 0, 1, 2);
        snap.set_valign(Gtk::Align::START);
    } else {
        _top_toolbars->attach(aux, 0, 1, 1, 1);
        _top_toolbars->attach(snap, 1, 0, 2, 2);
        snap.set_valign(Gtk::Align::CENTER);
    }
}

void SPDesktopWidget::_updateUnit()
{
    auto const unit = _desktop->getNamedView()->getDisplayUnit();

    _dt2r = 1.0 / unit->factor;

    _canvas_grid->GetVRuler()->set_unit(unit);
    _canvas_grid->GetHRuler()->set_unit(unit);
    _canvas_grid->GetVRuler()->set_tooltip_text(gettext(unit->name_plural.c_str()));
    _canvas_grid->GetHRuler()->set_tooltip_text(gettext(unit->name_plural.c_str()));
    _canvas_grid->updateRulers();
}

// We make the desktop window with focus active. Signal is connected in inkscape-window.cpp
void SPDesktopWidget::onFocus(bool const has_focus)
{
    if (!has_focus) return;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/options/bitmapautoreload/value", true)) {
        auto const &imageList = _desktop->doc()->getResourceList("image");
        for (auto it : imageList) {
            auto image = cast<SPImage>(it);
            image->refresh_if_outdated();
        }
    }
}

// ------------------------ Zoom ------------------------

void
SPDesktopWidget::sticky_zoom_toggled()
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/stickyzoom/value", _canvas_grid->GetStickyZoom()->get_active());
}

void
SPDesktopWidget::sticky_zoom_updated()
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    _canvas_grid->GetStickyZoom()->set_active(prefs->getBool("/options/stickyzoom/value", false));
}

void
SPDesktopWidget::update_zoom()
{
    _statusbar->update_zoom();
}

// ---------------------- Rotation ------------------------

void
SPDesktopWidget::update_rotation()
{
    _statusbar->update_rotate();
}

// --------------- Rulers/Scrollbars/Etc. -----------------

void
SPDesktopWidget::toggle_command_palette() {
    // TODO: Turn into action and remove this function.
    _canvas_grid->ToggleCommandPalette();
}

void
SPDesktopWidget::toggle_rulers()
{
    // TODO: Turn into action and remove this function.
    _canvas_grid->ToggleRulers();
}

void
SPDesktopWidget::toggle_scrollbars()
{
    // TODO: Turn into action and remove this function.
    _canvas_grid->ToggleScrollbars();
}

Gio::ActionMap* SPDesktopWidget::get_action_map() {
    return _window;
}

void SPDesktopWidget::remove_from_top_toolbar_or_hbox(Gtk::Widget &widget)
{
    g_assert(_top_toolbars);
    g_assert(_hbox        );

    auto const parent = widget.get_parent();
    if (!parent) return;

    if (parent == _top_toolbars) {
        _top_toolbars->remove(widget);
    } else if (parent == _hbox) {
        _hbox->remove(widget);
    } else {
        g_critical("SPDesktopWidget::remove_from_top_toolbar_or_hbox(): unexpected parent!");
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
