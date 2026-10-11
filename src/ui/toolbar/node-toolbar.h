// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file Node toolbar
 */
/* Authors:
 *   MenTaLguY <mental@rydia.net>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Frank Felfe <innerspace@iname.com>
 *   John Cliff <simarilius@yahoo.com>
 *   David Turner <novalis@gnu.org>
 *   Josh Andler <scislac@scislac.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Maximilian Albert <maximilian.albert@gmail.com>
 *   Tavmjong Bah <tavmjong@free.fr>
 *   Abhishek Sharma
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *   Vaibhav Malik <vaibhavmalik2018@gmail.com>
 *
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2003 MenTaLguY
 * Copyright (C) 1999-2011 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLBAR_NODE_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_NODE_TOOLBAR_H

#include "toolbar.h"
#include "ui/operation-blocker.h"

namespace Gtk {
class Builder;
class Button;
class ToggleButton;
} // namespace Gtk

namespace Inkscape {
class Selection;
namespace UI {
class ControlPointSelection;
class SimplePrefPusher;
namespace Tools { class NodeTool; }
namespace Widget {
class SpinButton;
class UnitTracker;
} // namespace Widget
} // namespace UI
} // namespace Inkscape

namespace Inkscape::UI::Toolbar {

class NodeToolbar : public Toolbar
{
public:
    NodeToolbar();
    ~NodeToolbar() override;

    void setDesktop(SPDesktop *desktop) override;
    void setActiveUnit(Util::Unit const *unit) override;

private:
    NodeToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    std::unique_ptr<UI::Widget::UnitTracker> _tracker;

    std::unique_ptr<UI::SimplePrefPusher> _pusher_show_handles;
    std::unique_ptr<UI::SimplePrefPusher> _pusher_show_outline;
    std::unique_ptr<UI::SimplePrefPusher> _pusher_edit_clipping_paths;
    std::unique_ptr<UI::SimplePrefPusher> _pusher_edit_masks;

    Gtk::Button &_nodes_lpeedit_btn;

    Gtk::ToggleButton *_show_helper_path_btn;
    Gtk::ToggleButton *_show_handles_btn;
    Gtk::ToggleButton *_object_edit_mask_path_btn;
    Gtk::ToggleButton *_object_edit_clip_path_btn;
    Gtk::Button &_add_corners_btn;

    UI::Widget::SpinButton &_nodes_x_item;
    UI::Widget::SpinButton &_nodes_y_item;
    UI::Widget::SpinButton &_nodes_d_item;
    Gtk::Box &_nodes_d_box;

    OperationBlocker _blocker;

    sigc::connection c_selection_changed;
    sigc::connection c_selection_modified;
    sigc::connection c_subselection_changed;

    void value_changed(Glib::ustring const &name, Glib::RefPtr<Gtk::Adjustment> const &adj);
    void sel_changed(Selection *selection);
    void sel_modified(Selection *selection, unsigned flags);
    void coord_changed(ControlPointSelection* selected_nodes);
    void on_pref_toggled(Gtk::ToggleButton *item, Glib::ustring const &path);

    void setup_derived_spin_button(UI::Widget::SpinButton &btn, Glib::ustring const &name);

    Tools::NodeTool *get_node_tool() const;
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_NODE_TOOLBAR_H

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
