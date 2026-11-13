// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Anshudhar Kumar Singh <anshudhar2001@gmail.com>
 *
 * Copyright (C) 1999-2007, 2021 Authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SP_EXPORT_BATCH_H
#define SP_EXPORT_BATCH_H

#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/togglebutton.h>

#include "ui/widget/export-preview.h"

namespace Gtk {
class Builder;
class Button;
class FlowBox;
class ProgressBar;
class Widget;
} // namespace Gtk

class InkscapeApplication;
class SPDesktop;
class SPDocument;
class SPItem;
class SPPage;

namespace Inkscape {

class Preferences;
class Selection;

namespace UI {

namespace Widget {
class ColorPicker;
} // namespace Widget

namespace Dialog {

class ExportList;
class BatchItem;

typedef std::map<std::string, std::unique_ptr<BatchItem>> BatchItems;

class BatchItem final : public Gtk::FlowBoxChild
{
public:
    BatchItem(SPItem *item, bool isolate_item, std::shared_ptr<PreviewDrawing> drawing);
    BatchItem(SPPage *page, std::shared_ptr<PreviewDrawing> drawing);
    ~BatchItem() final;

    Glib::ustring getLabel() const { return _label_str; }
    SPItem *getItem() const { return _item; }
    SPPage *getPage() const { return _page; }
    void refresh(bool hide, guint32 bg_color);
    void setDrawing(std::shared_ptr<PreviewDrawing> drawing);

    auto get_radio_group() { return &_option; }
    void on_parent_changed();
    void on_mode_changed(Gtk::SelectionMode mode);
    void set_selected(bool selected);
    void update_selected();

    /// @brief Get "Export selected only" setting
    /// @returns true if only selected items are shown in export
    bool isolateItem() const { return _isolate_item; }
    void setIsolateItem(bool isolate);

    static void syncItems(BatchItems &items, std::map<std::string, SPObject*> const &objects, Gtk::FlowBox &container, std::shared_ptr<PreviewDrawing> preview, bool isolate_items);
private:
    void init(std::shared_ptr<PreviewDrawing> drawing);
    void update_label();

    Glib::ustring _label_str;
    Gtk::Grid _grid;
    Gtk::Label _label;
    Gtk::CheckButton _selector;
    Gtk::CheckButton _option;
    ExportPreview _preview;
    SPItem *_item = nullptr;
    SPPage *_page = nullptr;
    bool _isolate_item = false;
    bool is_hide = false;

    sigc::scoped_connection _selection_widget_changed_conn;
    sigc::scoped_connection _object_modified_conn;
};

} // namespace Dialog
} // namespace UI
} // namespace Inkscape

#endif // SP_EXPORT_BATCH_H

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
