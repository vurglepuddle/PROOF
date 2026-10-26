// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   buliabyak@gmail.com
 *   scislac@users.sf.net
 *
 * Copyright (C) 2005 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_INKSCAPE_UI_SELECTED_STYLE_H
#define SEEN_INKSCAPE_UI_SELECTED_STYLE_H

#include <gtkmm/gesture.h> // Gtk::EventSequenceState
#include <gtkmm/grid.h>

#include "colors/color.h"
#include "rotateable.h"
#include "ui/defocus-target.h"
#include "ui/popup-menu.h"
#include "ui/widget/generic/popover-bin.h"

namespace Gtk {
class Adjustment;
class GestureClick;
class CheckButton;
class Label;
} // namespace Gtk

class SPDesktop;

namespace Inkscape {

namespace Util {
class Unit;
} // namespace Util

namespace UI::Widget {

class InkSpinButton;
class PopoverMenu;
class PopoverMenuItem;

enum PaintType {
    SS_NA,
    SS_NONE,
    SS_UNSET,
    SS_MANY,
    SS_PATTERN,
    SS_HATCH,
    SS_LGRADIENT,
    SS_RGRADIENT,
    SS_MGRADIENT,
    SS_COLOR
};

enum FillOrStroke {
    SS_FILL,
    SS_STROKE
};

class ColorPreview;
class GradientImage;
class SelectedStyle;
class SelectedStyleDropTracker;

class RotateableSwatch : public Rotateable {
  public:
    RotateableSwatch(SelectedStyle *parent, guint mode);
    ~RotateableSwatch() override;

    std::pair<double, double> color_adjust(Colors::Color const &cc, double by, guint state);

    void do_motion (double by, guint state) override;
    void do_release (double by, guint state) override;
    void do_scroll (double by, guint state) override;

private:
    guint fillstroke;

    SelectedStyle *parent;

    std::optional<Colors::Color> startcolor;

    gchar const *undokey = "ssrot1";

    int cursor_state = -1;
};

class RotateableStrokeWidth : public Rotateable {
  public:
    RotateableStrokeWidth(SelectedStyle *parent);
    ~RotateableStrokeWidth() override;

    double value_adjust(double current, double by, guint modifier, bool final);
    void do_motion (double by, guint state) override;
    void do_release (double by, guint state) override;
    void do_scroll (double by, guint state) override;

private:
    SelectedStyle *parent;

    double startvalue;
    bool startvalue_set;

    gchar const *undokey;
};

/**
 * Selected style indicator (fill, stroke, opacity).
 */
class SelectedStyle
    : public UI::Widget::PopoverBin
    , private DefocusTarget
{
public:
    SelectedStyle();
    ~SelectedStyle() override;

    void setDesktop(SPDesktop *desktop);
    SPDesktop *getDesktop() {return _desktop;}
    void update();

    std::optional<Colors::Color> _lastselected[2];
    std::optional<Colors::Color> _thisselected[2];

    guint _mode[2];

    double current_stroke_width = 0.0;
    Inkscape::Util::Unit const *_sw_unit = nullptr; // points to object in UnitTable, do not delete

protected:
    SPDesktop *_desktop = nullptr;

    // Widgets
    Gtk::Grid  *grid;

    Gtk::Label *label[2];    // 'Fill' and 'Stroke'
    Gtk::Label *tag[2];      // 'a', 'm', or empty.

    std::unique_ptr<Gtk::Label> type_label[2]; // 'L', 'R', 'M', or empty.
    std::unique_ptr<GradientImage> gradient_preview[2];
    std::unique_ptr<ColorPreview> color_preview[2];
    RotateableSwatch *swatch[2]; // // Wraps one or two of: "type_label", "gradient_preview", "color_preview"

    Gtk::Label *stroke_width; // Stroke width
    RotateableStrokeWidth *stroke_width_rotateable;

    Glib::RefPtr<Gtk::Adjustment> opacity_adjustment;
    Inkscape::UI::Widget::InkSpinButton *opacity_sb;

    Glib::ustring _paintserver_id[2];

    // Signals
    sigc::scoped_connection selection_changed_connection;
    sigc::scoped_connection selection_modified_connection;
    sigc::scoped_connection stop_selected_connection, stop_refresh_connection;

    Gtk::EventSequenceState on_fill_click   (Gtk::GestureClick const &click,
                                             int n_press, double x, double y);
    Gtk::EventSequenceState on_stroke_click (Gtk::GestureClick const &click,
                                             int n_press, double x, double y);
    Gtk::EventSequenceState on_opacity_click(Gtk::GestureClick const &click,
                                             int n_press, double x, double y);
    Gtk::EventSequenceState on_sw_click     (Gtk::GestureClick const &click,
                                             int n_press, double x, double y);

    bool _opacity_blocked = false;

    std::unique_ptr<UI::Widget::PopoverMenu> _popup_opacity;
    void make_popup_opacity();
    void on_opacity_changed(double value);
    bool on_opacity_popup(PopupMenuOptionalClick);
    void opacity_0();
    void opacity_025();
    void opacity_05();
    void opacity_075();
    void opacity_1();

    void on_fill_remove();
    void on_stroke_remove();
    void on_fill_lastused();
    void on_stroke_lastused();
    void on_fill_lastselected();
    void on_stroke_lastselected();
    void on_fill_unset();
    void on_stroke_unset();
    void on_fill_edit();
    void on_stroke_edit();
    void on_fillstroke_swap();
    void on_fill_invert();
    void on_stroke_invert();
    void on_fill_white();
    void on_stroke_white();
    void on_fill_black();
    void on_stroke_black();
    void on_fill_copy();
    void on_stroke_copy();
    void on_fill_paste();
    void on_stroke_paste();
    void on_fill_opaque();
    void on_stroke_opaque();
    void _on_paste_callback(Glib::RefPtr<Gio::AsyncResult> &result, Glib::ustring typepaste);

    std::unique_ptr<UI::Widget::PopoverMenu> _popup[2];
    UI::Widget::PopoverMenuItem *_popup_copy[2]{};
    void make_popup(FillOrStroke i);

    std::unique_ptr<UI::Widget::PopoverMenu> _popup_sw;
    std::vector<Gtk::CheckButton *> _unit_mis;
    void make_popup_units();
    void on_popup_units(Inkscape::Util::Unit const *u);
    void on_popup_preset(int i);

    std::unique_ptr<SelectedStyleDropTracker> drop[2];
    bool dropEnabled[2] = {false, false};

private:
    void onDefocus() override;
};

} // namespace UI::Widget

} // namespace Inkscape

#endif // SEEN_INKSCAPE_UI_SELECTED_STYLE_H

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
