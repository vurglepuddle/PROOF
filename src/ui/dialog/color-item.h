// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Color item used in palettes and swatches UI.
 */
/* Authors: PBS <pbs3141@gmail.com>
 * Copyright (C) 2022 PBS
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_DIALOG_COLOR_ITEM_H
#define INKSCAPE_UI_DIALOG_COLOR_ITEM_H

#include <variant>
#include <gtkmm/drawingarea.h>
#include <gtkmm/gesture.h>

#include "colors/color.h"

namespace Gtk {
class DragSource;
class GestureClick;
class Popover;
} // namespace Gtk

class SPGradient;

namespace Inkscape::UI::Dialog {

class DialogBase;

/**
 * The color item you see on-screen as a clickable box.
 *
 * Note: This widget must be outlived by its parent dialog, passed in the constructor.
 */
class ColorItem : public Gtk::Widget
{
public:
    /// No fill option
    explicit ColorItem(DialogBase *);

    /// Create a static color. A spot color (PROOF) is applied as a linked spot-ink swatch,
    /// at spot_tint (below 1 for library tint swatches).
    ColorItem(Colors::Color, DialogBase *, bool spot = false, double spot_tint = 1.0);

    /**
     * Create a dynamically-updating color from a gradient, to which it remains linked.
     * If the gradient is destroyed, the widget will go into an inactive state.
     */
    ColorItem(SPGradient *, DialogBase *);

    /// Add new group or filler element.
    explicit ColorItem(Glib::ustring name);

    ~ColorItem() override;

    // Returns true if this is group heading rather than a color
    bool is_group() const;
    // Returns true if this is alignment filler item, not a color
    bool is_filler() const;
    // Is paint "None"?
    bool is_paint_none() const;
    // PROOF: is this a spot ink (palette spot entry or spot-ink document swatch)?
    bool is_spot() const;

    /// Update the fill indicator, showing this widget is the fill of the current selection.
    void set_fill(bool);

    /// Update the stroke indicator, showing this widget is the stroke of the current selection.
    void set_stroke(bool);

    /// Update whether this item is pinned.
    bool is_pinned() const;
    void set_pinned_pref(const std::string &path);

    const Glib::ustring &get_description() const { return description; }

    sigc::signal<void ()>& signal_modified() { return _signal_modified; };
    sigc::signal<void ()>& signal_pinned() { return _signal_pinned; };

private:
    void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const &snapshot) override;

    Glib::RefPtr<Gdk::ContentProvider> on_drag_prepare();
    void on_drag_begin(Gtk::DragSource &source);

    // Common post-construction setup.
    void common_setup();

    void on_motion_enter();
    void on_motion_leave();

    Gtk::EventSequenceState on_click_pressed (Gtk::GestureClick const &click);
    Gtk::EventSequenceState on_click_released(Gtk::GestureClick const &click);

    // Perform the on-click action of setting the fill or stroke.
    void on_click(bool stroke);

    // Perform the right-click action of showing the context menu.
    void on_rightclick();

    // Actions
    void action_set_fill();
    void action_set_stroke();
    void action_delete();
    void action_edit();
    void action_toggle_pin();
    void action_convert(Glib::ustring const &name);

    // Draw the color only (i.e. no indicators). Used for drawing both the widget and the drag/drop icon.
    void draw_color(Glib::RefPtr<Gtk::Snapshot> const &snapshot, int w, int h) const;

    // Return the color (or average if a gradient), for choosing the color of the fill/stroke indicators.
    Colors::Color getColor() const;

    // Description of the color, shown in help text.
    Glib::ustring description;
    Glib::ustring color_id;
    Glib::ustring tooltip;

    /// The pinned preference path
    Glib::ustring pinned_pref;
    bool pinned_default = false;

    // The color.
    struct Undefined {};
    struct PaintNone {};
    struct GradientData { SPGradient *gradient; };
    std::variant<Undefined, PaintNone, Colors::Color, GradientData> data;

    // The dialog this widget belongs to. Used for determining what desktop to take action on.
    DialogBase *dialog = nullptr;

    // PROOF: palette entry is a named spot ink, applied at this tint.
    bool spot = false;
    double spot_tint = 1.0;

    // Whether this color is in use as the fill or stroke of the current selection.
    bool is_fill = false;
    bool is_stroke = false;

    bool was_grad_pinned = false;

    // For ensuring that clicks that release outside the widget don't count.
    bool mouse_inside = false;

    sigc::signal<void ()> _signal_modified;
    sigc::signal<void ()> _signal_pinned;

    std::unique_ptr<Gtk::Popover> _popover;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_COLOR_ITEM_H

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
