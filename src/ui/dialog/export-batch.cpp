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

#include "ui/dialog/export-batch.h"

#include <algorithm>
#include <regex>
#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/error.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/progressbar.h>

#include "desktop.h"
#include "document-undo.h"
#include "extension/output.h"
#include "inkscape-window.h"
#include "io/fix-broken-links.h"
#include "io/sandbox.h"
#include "io/sys.h"
#include "layer-manager.h"
#include "object/sp-page.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/dialog-run.h"
#include "ui/dialog/export.h"
#include "ui/dialog/link-missing.h"
#include "ui/icon-names.h"
#include "ui/widget/color-picker.h"
#include "ui/widget/export-lists.h"
#include "util/units.h"
#include "util/scope_exit.h"

namespace Inkscape::UI::Dialog {

BatchItem::BatchItem(SPItem *item, bool isolate_item, std::shared_ptr<PreviewDrawing> drawing)
    : _item{item}
    , _isolate_item{isolate_item}
{
    init(std::move(drawing));
    _object_modified_conn = _item->connectModified([=, this](SPObject *obj, unsigned int flags) {
        update_label();
    });
    update_label();
}

BatchItem::BatchItem(SPPage *page, std::shared_ptr<PreviewDrawing> drawing)
    : _page{page}
{
    init(std::move(drawing));
    _object_modified_conn = _page->connectModified([=, this](SPObject *obj, unsigned int flags) {
        update_label();
    });
    update_label();
}

BatchItem::~BatchItem() = default;

void BatchItem::update_label()
{
    Glib::ustring label = "no-name";
    if (_page) {
        label = _page->getDefaultLabel();
        if (auto id = _page->label()) {
            label = id;
        }
    } else if (_item) {
        label = _item->defaultLabel();
        if (label.empty()) {
            if (auto _id = _item->getId()) {
                label = _id;
            } else {
                label = "no-id";
            }
        }
    }
    gtk_accessible_update_property(GTK_ACCESSIBLE(_selector.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
    _label_str = label;
    _label.set_text(label);
    set_tooltip_text(label);
}

/// @brief Set "Export selected only"
/// @param isolate true if only selected items should be shown in export
void BatchItem::setIsolateItem(bool isolate)
{
    if (_isolate_item != isolate) {
        _isolate_item = isolate;
        _preview.setItem(_item, _isolate_item);
    }
}

void BatchItem::init(std::shared_ptr<PreviewDrawing> drawing) {
    _grid.set_row_spacing(5);
    _grid.set_column_spacing(5);
    _grid.set_valign(Gtk::Align::CENTER);

    _selector.set_active(true);
    _selector.set_tooltip_text(_("Export this item"));
    _selector.set_focusable(true);
    _selector.set_margin_start(2);
    _selector.set_margin_bottom(2);
    _selector.set_valign(Gtk::Align::END);

    _option.set_active(false);
    _option.set_focusable(true);
    _option.set_margin_start(2);
    _option.set_margin_bottom(2);
    _option.set_valign(Gtk::Align::END);

    _preview.set_name("export_preview_batch");
    _preview.setItem(_item, _isolate_item);
    _preview.setDrawing(std::move(drawing));
    _preview.setSize(64);
    _preview.set_halign(Gtk::Align::CENTER);
    _preview.set_valign(Gtk::Align::CENTER);

    _label.set_width_chars(10);
    _label.set_ellipsize(Pango::EllipsizeMode::END);
    _label.set_halign(Gtk::Align::CENTER);

    set_valign(Gtk::Align::START);
    set_halign(Gtk::Align::START);
    set_child(_grid);
    set_focusable(true);

    _selector.signal_toggled().connect([this]() {
        set_selected(_selector.get_active());
    });
    _option.signal_toggled().connect([this]() {
        set_selected(_option.get_active());
    });

    // This initially packs the widgets with a hidden preview.
    refresh(!is_hide, 0);

    property_parent().signal_changed().connect([this] { on_parent_changed(); });
}

/**
 * Syncronise the FlowBox selection to the active widget activity.
 */
void BatchItem::set_selected(bool selected)
{
    auto box = dynamic_cast<Gtk::FlowBox *>(get_parent());
    if (box && selected != is_selected()) {
        if (selected) {
            box->select_child(*this);
        } else {
            box->unselect_child(*this);
        }
    }
}

/**
 * Syncronise the FlowBox selection to the existing active widget state.
 */
void BatchItem::update_selected()
{
    if (auto parent = dynamic_cast<Gtk::FlowBox *>(get_parent()))
        on_mode_changed(parent->get_selection_mode());
    if (_selector.get_visible()) {
        set_selected(_selector.get_active());
    } else if (_option.get_visible()) {
        set_selected(_option.get_active());
    }
}

/**
 * A change in the selection mode for the flow box.
 */
void BatchItem::on_mode_changed(Gtk::SelectionMode mode)
{
    _selector.set_visible(mode == Gtk::SelectionMode::MULTIPLE);
    _option.set_visible(mode == Gtk::SelectionMode::SINGLE);
}

/**
 * Update the connection to the parent FlowBox
 */
void BatchItem::on_parent_changed()
{
    auto parent = dynamic_cast<Gtk::FlowBox *>(get_parent());
    if (!parent) {
        return;
    }

    _selection_widget_changed_conn = parent->signal_selected_children_changed().connect([this]() {
        // Synchronise the active widget state to the Flowbox selection.
        if (_selector.get_visible()) {
            _selector.set_active(is_selected());
        } else if (_option.get_visible()) {
            _option.set_active(is_selected());
        }
    });
    update_selected();

    for (auto child = parent->get_first_child(); child; child = child->get_next_sibling()) {
        if (child != this) {
            if (auto item = dynamic_cast<BatchItem *>(child)) {
                auto group = item->get_radio_group();
                _option.set_group(*group);
                break;
            }
        }
    }
}

void BatchItem::refresh(bool hide, uint32_t bg_color)
{
    if (_page) {
        _preview.setBox(_page->getDocumentRect());
    }

    _preview.setBackgroundColor(bg_color);

    // When hiding the preview, we show the items as a checklist
    // So all items must be packed differently on refresh.
    if (hide != is_hide) {
        is_hide = hide;

        auto remove_grid_child = [&] (Gtk::Widget &widget) {
            if (widget.get_parent() == &_grid) {
                _grid.remove(widget);
            }
        };
        remove_grid_child(_selector);
        remove_grid_child(_option);
        remove_grid_child(_label);
        remove_grid_child(_preview);

        if (hide) {
            _selector.set_valign(Gtk::Align::BASELINE);
            _label.set_xalign(0.0);
            _label.set_max_width_chars(-1);
            _grid.attach(_selector, 0, 1, 1, 1);
            _grid.attach(_option, 0, 1, 1, 1);
            _grid.attach(_label, 1, 1, 1, 1);
        } else {
            _selector.set_valign(Gtk::Align::END);
            _label.set_xalign(0.5);
            _label.set_max_width_chars(18);
            _grid.attach(_preview, 0, 0, 2, 2);
            _grid.attach(_selector, 0, 1, 1, 1);
            _grid.attach(_option, 0, 1, 1, 1);
            _grid.attach(_label, 0, 2, 2, 1);
        }
        update_selected();
    }

    if (!hide) {
        _preview.queueRefresh();
    }
}

void BatchItem::setDrawing(std::shared_ptr<PreviewDrawing> drawing)
{
    _preview.setDrawing(std::move(drawing));
}

/**
 * Add and remove batch items and their previews carefully and insert new ones into the container FlowBox
 *
 * @param items List of batch-items (e.g. layers) in the batch dialog. Is updated by this function.
 * @param objects New list of objects (e.g. layers). Taken as input.
 * @param container GUI widget containing the selection of batch items. Is updated by this function.
 * @param isolate_items true if only the given (selected) items should be shown in export
 */
void BatchItem::syncItems(BatchItems &items, std::map<std::string, SPObject*> const &objects, Gtk::FlowBox &container, std::shared_ptr<PreviewDrawing> preview, bool isolate_items)
{
    // Pre- and post-condition of this function is that
    // `items` and `container.children` have the same content.
    // (They have different types and contain slightly different information,
    // but there is still a 1:1 correspondence.)
    //
    // We update `items` so that it matches `objects`.
    // Any necessary change to `items` is also applied to `container.children`.

    // a) Remove any items not in objects
    for (auto it = items.begin(); it != items.end();) {
        if (!objects.contains(it->first)) {
            container.remove(*it->second);
            it = items.erase(it);
        } else {
            it->second->setIsolateItem(isolate_items);
            ++it;
        }
    }

    // b) Add any objects not in items

    // A special container for pages allows them to be sorted correctly
    std::set<SPPage *, SPPage::PageIndexOrder> pages;

    for (auto &[id, obj] : objects) {
        if (auto page = cast<SPPage>(obj)) {
            if (!items[id] || items[id]->getPage() != page)
                pages.insert(page);
            continue;
        }

        auto item = cast<SPItem>(obj);

        // If an Item or Page with same Id is already present, Skip
        if (items[id] && items[id]->getItem() == item)
            continue;

        if (items[id]) {
            // Remove existing item with same id
            // (can occur when switching between document tabs)
            container.remove(*items[id]);
        }
        // Add new item to the end of list
        items[id] = std::make_unique<BatchItem>(item, isolate_items, preview);
        container.insert(*items[id], -1);
        items[id]->set_selected(true);
    }

    for (auto &page : pages) {
        if (auto id = page->getId()) {
            if (items[id]) {
                container.remove(*items[id]);
            }
            items[id] = std::make_unique<BatchItem>(page, preview);
            container.insert(*items[id], -1);
            items[id]->set_selected(true);
        }
    }

    // Check postconditions
    g_assert(items.size() == objects.size());
    g_assert(container.get_children().size() == items.size());
}

} // namespace Inkscape::UI::Dialog
