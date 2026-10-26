// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Fill and Stroke dialog
 */
/* Authors:
 *   Bryce W. Harrington <bryce@bryceharrington.org>
 *   Gustav Broberg <broberg@kth.se>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2004--2007 Authors
 * Copyright (C) 2010 Jon A. Cruz
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_DIALOG_FILL_AND_STROKE_H
#define INKSCAPE_UI_DIALOG_FILL_AND_STROKE_H

#include <gtkmm/menubutton.h>
#include <gtkmm/notebook.h>

#include "ui/dialog/dialog-base.h"
#include "ui/widget/advanced-tab.h"
#include "ui/widget/object-composite-settings.h"
#include "ui/widget/paint-switch.h"
#include "ui/widget/style-subject.h"

namespace Gtk {
class Box;
} // namespace Gtk

namespace Inkscape::UI {

namespace Widget {
class NotebookPage;
class StrokeStyle;
} // namespace Widget

namespace Dialog {

class FillAndStroke final : public DialogBase
{
public:
    FillAndStroke();
    ~FillAndStroke() final;

    void desktopReplaced() final;
    void documentReplaced() final;

    void showPageFill();
    void showPageStrokePaint();
    void showPageStrokeStyle();

protected:
    Gtk::Notebook   _notebook;

    UI::Widget::NotebookPage    *_page_fill         = nullptr;
    UI::Widget::NotebookPage    *_page_stroke_paint = nullptr;
    UI::Widget::NotebookPage    *_page_stroke_style = nullptr;
    UI::Widget::NotebookPage    * _page_advanced    = nullptr;

    UI::Widget::StyleSubject::Selection _subject;
    UI::Widget::ObjectCompositeSettings _composite_settings;
    Gtk::Box &_createPageTabLabel(const Glib::ustring &label,
                                  const char *label_image);

    void _setupRecolorBtn();
    void _layoutPageFill();
    void _layoutPageStrokePaint();
    void _layoutPageStrokeStyle();
    void _savePagePref(guint page_num);
    void _onSwitchPage(Gtk::Widget *page, guint pagenum);

private:
    void selectionChanged (Selection *selection                ) final;
    void selectionModified(Selection *selection, unsigned flags) final;
    void _ConnectPaintSignals(UI::Widget::PaintSwitch *paint_switch, bool is_fill);
    void _updateFromSelection();
    void _layoutPageAdvanced();
    int npage = 0;
    bool page_changed = false;
    bool changed_fill = true;
    bool changed_stroke = true;
    bool changed_stroke_style = true;
    bool _ignore_updates = false;
    std::unique_ptr<UI::Widget::PaintSwitch> _fill_switch;
    std::unique_ptr<UI::Widget::PaintSwitch> _stroke_switch;
    UI::Widget::StrokeStyle *strokeStyleWdgt = nullptr;
    std::unique_ptr<UI::Widget::AdvancedTab> _advanced_tab;
    std::vector<Gtk::Label *> _tab_labels;

    sigc::scoped_connection _switch_page_conn;
    sigc::scoped_connection _stop_selected, _stop_refresh;
    std::vector<sigc::connection> _paint_connections;
};

} // namespace Dialog

} // namespace Inkscape::UI


#endif // INKSCAPE_UI_DIALOG_FILL_AND_STROKE_H

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
