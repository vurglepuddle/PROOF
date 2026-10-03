// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief A window for floating dialogs.
 *
 * Authors: see git history
 *   Tavmjong Bah
 *
 * Copyright (c) 2018 Tavmjong Bah, Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "dialog-window.h"

#include <glibmm/i18n.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/shortcutcontroller.h>

#include "document.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "ui/dialog/dialog-base.h"
#include "ui/dialog/dialog-multipaned.h"
#include "ui/pack.h"
#include "ui/shortcuts.h"
#include "ui/themes.h"

// Sizing constants
static constexpr int MINIMUM_WINDOW_WIDTH       = 210;
static constexpr int MINIMUM_WINDOW_HEIGHT      = 320;
static constexpr int INITIAL_WINDOW_WIDTH       = 360;
static constexpr int INITIAL_WINDOW_HEIGHT      = 520;
static constexpr int WINDOW_DROPZONE_SIZE       =  10;
static constexpr int WINDOW_DROPZONE_SIZE_LARGE =  16;
static constexpr int NOTEBOOK_TAB_HEIGHT        =  36;

namespace Inkscape::UI::Dialog {

[[nodiscard]] static auto get_max_margin(Gtk::Widget const &widget)
{
    return std::max({widget.get_margin_top(), widget.get_margin_bottom(),
                     widget.get_margin_start(), widget.get_margin_end()});
}

// Create a dialog window and move page from old notebook.
DialogWindow::DialogWindow(InkscapeWindow *inkscape_window, Gtk::Widget *page)
    : _app(InkscapeApplication::instance())
    , _inkscape_window(inkscape_window)
{
    g_assert(_app != nullptr);

    // ============ Initialization ===============
    set_name("DialogWindow");
    set_transient_for(*inkscape_window);
    _app->gtk_app()->add_window(*this);

    signal_close_request().connect([this]{
        DialogManager::singleton().store_state(*this);
        delete this;
        return true;
    }, false); // before GTKʼs default handler, so it wonʼt try to double-delete

    // ================ Window ==================
    set_title(_("Dialog Window"));
    int window_width = INITIAL_WINDOW_WIDTH;
    int window_height = INITIAL_WINDOW_HEIGHT;

    // ================ Shortcuts ================
    auto& shortcuts_instance = Inkscape::Shortcuts::getInstance();
    auto shortcut_controller = Gtk::ShortcutController::create(shortcuts_instance.get_liststore());
    shortcut_controller->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
    add_controller(shortcut_controller);

    // =============== Outer Box ================
    auto const box_outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    set_child(*box_outer);

    // =============== Container ================
    _container = Gtk::make_managed<DialogContainer>(inkscape_window);
    DialogMultipaned *columns = _container->get_columns();
    auto drop_size = Inkscape::Preferences::get()->getBool("/options/dockingzone/value", true) ? WINDOW_DROPZONE_SIZE / 2 : WINDOW_DROPZONE_SIZE;
    columns->set_dropzone_sizes(drop_size, drop_size);
    UI::pack_end(*box_outer, *_container);

    // If there is no page, create an empty Dialogwindow to be populated later
    if (page) {
        // ============= Initial Column =============
        auto col = _container->create_column();
        auto const column = col.get();
        columns->append(std::move(col));

        // ============== New Notebook ==============
        auto nb = std::make_unique<DialogNotebook>(_container);
        auto const dialog_notebook = nb.get();
        column->append(std::move(nb));
        column->set_dropzone_sizes(drop_size, drop_size);
        dialog_notebook->move_page(*page);

        // Set window title
        DialogBase *dialog = dynamic_cast<DialogBase *>(page);
        if (dialog) {
            set_title(dialog->get_name());
        }

        // Set window size considering what the dialog needs
        Gtk::Requisition minimum_size, natural_size;
        dialog->get_preferred_size(minimum_size, natural_size);
        int overhead = 2 * (drop_size + get_max_margin(*dialog));
        int width = natural_size.get_width() + overhead;
        int height = natural_size.get_height() + overhead + NOTEBOOK_TAB_HEIGHT;
        window_width = std::max(width, window_width);
        window_height = std::max(height, window_height);

        // PROOF: stay within three quarters of the main window, so a long dialog such as
        // Preferences does not open taller than the screen.
        if (inkscape_window) {
            int const max_width = inkscape_window->get_width() * 3 / 4;
            int const max_height = inkscape_window->get_height() * 3 / 4;
            if (max_width >= MINIMUM_WINDOW_WIDTH) window_width = std::min(window_width, max_width);
            if (max_height >= MINIMUM_WINDOW_HEIGHT) window_height = std::min(window_height, max_height);
        }
    }

    // Set window sizing
    set_size_request(MINIMUM_WINDOW_WIDTH, MINIMUM_WINDOW_HEIGHT);
    set_default_size(window_width, window_height);

    if (page) {
        update_dialogs();
    }

    // TODO: Double-check the phase. This needs to be called after default Window handlerʼs CAPTURE
    auto const key = Gtk::EventControllerKey::create();
    key->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    key->signal_key_pressed().connect([this, &key = *key](auto &&...args) { return on_key_pressed(key, args...); }, true);
    add_controller(key);

    // window is created hidden; don't show it now, its size needs to be restored
}

/**
 * Change InkscapeWindow that DialogWindow is linked to.
 */
void DialogWindow::set_inkscape_window(InkscapeWindow* inkscape_window)
{
    _inkscape_window = inkscape_window;
    update_dialogs();
}

/**
 * Update all dialogs that are owned by the DialogWindow's _container.
 */
void DialogWindow::update_dialogs()
{
    g_assert(_app != nullptr);
    g_assert(_container != nullptr);

    _container->set_inkscape_window(_inkscape_window);
    _container->update_dialogs(); // Updating dialogs is not using the _app reference here.

    // Set window title.
    auto const &dialogs = _container->get_dialogs();
    Glib::ustring title;
    if (dialogs.size() > 1) {
        title = "Multiple dialogs";
    } else if (dialogs.size() == 1) {
        title = dialogs.begin()->second->get_name();
    } else {
        // Should not happen... but does on closing a window!
    }

    if (_inkscape_window) {
        if (auto document_name = _inkscape_window->get_document()->getDocumentName()) {
            title += " - ";
            title += document_name;
        }

        // C++ API requires Glib::RefPtr<Gio::ActionGroup>, use C API here.
        gtk_widget_insert_action_group(Gtk::Widget::gobj(), "win", _inkscape_window->Gio::ActionGroup::gobj());

        insert_action_group("doc", _inkscape_window->get_document()->getActionGroup());
    }

    set_title(title);
    set_sensitive(_inkscape_window);
}

/**
 * Update window width and height in order to fit all dialogs inisde its container.
 *
 * The intended use of this function is at initialization.
 */
void DialogWindow::update_window_size_to_fit_children()
{
    // Declare variables
    int width = 0, height = 0;
    int overhead = 0;

    // Read needed data
    auto allocation = get_allocation();
    auto const &dialogs = _container->get_dialogs();

    // Get largest sizes for dialogs
    for (auto const &[name, dialog] : dialogs) {
        Gtk::Requisition minimum_size, natural_size;
        dialog->get_preferred_size(minimum_size, natural_size);
        width = std::max(natural_size.get_width(), width);
        height = std::max(natural_size.get_height(), height);
        overhead = std::max(overhead, get_max_margin(*dialog));
    }

    // Compute sizes including overhead
    overhead = 2 * (WINDOW_DROPZONE_SIZE_LARGE + overhead);
    width += overhead;
    height += overhead + NOTEBOOK_TAB_HEIGHT;

    // If sizes are lower then current, don't change them
    if (allocation.get_width() >= width && allocation.get_height() >= height) {
        return;
    }

    // Compute largest sizes on both axis
    width = std::max(width, allocation.get_width());
    height = std::max(height, allocation.get_height());

    // Resize window
    set_default_size(width, height);

    // Note: This function also used to maintain the center of the window
    // before GTK4 removed the ability to do that.
}

bool DialogWindow::on_key_pressed(Gtk::EventControllerKey &controller,
                                  unsigned keyval, unsigned keycode, Gdk::ModifierType state)
{
    return _inkscape_window && controller.forward(*_inkscape_window);
}

} // namespace Inkscape::UI::Dialog

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
