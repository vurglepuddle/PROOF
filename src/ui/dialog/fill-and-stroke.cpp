// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Fill and Stroke dialog - implementation.
 *
 * Based on the old sp_object_properties_dialog.
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

#include "fill-and-stroke.h"

#include <gtkmm/grid.h>
#include <gtkmm/image.h>

#include "desktop.h"
#include "desktop-style.h"
#include "document-undo.h"
#include "gradient-chemistry.h"
#include "pattern-manipulation.h"
#include "preferences.h"
#include "style.h"
#include "ui/icon-loader.h"
#include "ui/icon-names.h"
#include "ui/pack.h"
#include "ui/widget/notebook-page.h"
#include "ui/widget/recolor-art-manager.h"
#include "ui/widget/stroke-style.h"
#include "ui/widget/selection-paint.h"


namespace Inkscape::UI::Dialog {

FillAndStroke::FillAndStroke()
    : DialogBase("/dialogs/fillstroke", "FillStroke")
    , _page_fill(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1))
    , _page_stroke_paint(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1))
    , _page_stroke_style(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1))
    , _page_advanced(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1))
    , _composite_settings(INKSCAPE_ICON("dialog-fill-and-stroke"),
                          "fillstroke",
                          UI::Widget::SimpleFilterModifier::ISOLATION |
                          UI::Widget::SimpleFilterModifier::BLEND |
                          UI::Widget::SimpleFilterModifier::BLUR |
                          UI::Widget::SimpleFilterModifier::OPACITY)
{
    set_spacing(2);
    UI::pack_start(*this, _notebook, true, true);

    _notebook.append_page(*_page_fill, _createPageTabLabel(_("_Fill"), INKSCAPE_ICON("object-fill")));
    _notebook.append_page(*_page_stroke_paint, _createPageTabLabel(_("Stroke _paint"), INKSCAPE_ICON("object-stroke")));
    _notebook.append_page(*_page_stroke_style, _createPageTabLabel(_("Stroke st_yle"), INKSCAPE_ICON("object-stroke-style")));
    _notebook.append_page(*_page_advanced, _createPageTabLabel(_("Ad_vanced"), INKSCAPE_ICON("gears")));
    _notebook.set_vexpand(true);

    _switch_page_conn = _notebook.signal_switch_page().connect(sigc::mem_fun(*this, &FillAndStroke::_onSwitchPage));

    _layoutPageFill();
    _layoutPageStrokePaint();
    _layoutPageStrokeStyle();
    _layoutPageAdvanced();

    UI::pack_end(*this, _composite_settings, UI::PackOptions::shrink);

    _composite_settings.setSubject(&_subject);
}

FillAndStroke::~FillAndStroke()
{
    for (auto &connection : _paint_connections) connection.disconnect();
    // Disconnect signals from composite settings
    _composite_settings.setSubject(nullptr);

    if (_fill_switch) {
        _fill_switch->set_desktop(nullptr);
    }
    if (_stroke_switch) {
        _stroke_switch->set_desktop(nullptr);
    }
    strokeStyleWdgt->setDesktop(nullptr);
    _subject.setDesktop(nullptr);
}

void FillAndStroke::_setupRecolorBtn() {
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name(INKSCAPE_ICON("object-recolor-art"));
    box->append(*icon);
    
    auto label = Gtk::make_managed<Gtk::Label>(_("Recolor Selection"));
    box->append(*label);
}

void FillAndStroke::_ConnectPaintSignals(UI::Widget::PaintSwitch *paint_switch, bool is_fill)
{
    auto connections = UI::Widget::connect_selection_paint(*paint_switch, is_fill,
        [this] { return getDesktop(); }, [this] { return _ignore_updates; });
    _paint_connections.insert(_paint_connections.end(), connections.begin(), connections.end());
}

// Updates the Dialog UI to reflect the currently selected object(s)
void FillAndStroke::_updateFromSelection()
{
    if (!getDesktop()) return;

    _ignore_updates = true;

    auto selection = getDesktop()->getSelection();
    auto items = selection->items();
    bool is_empty = items.empty();

    if (_fill_switch)       _fill_switch->set_sensitive(!is_empty);
    if (_stroke_switch)     _stroke_switch->set_sensitive(!is_empty);
    if (strokeStyleWdgt)    strokeStyleWdgt->set_sensitive(!is_empty);

    if (_advanced_tab) {
        _advanced_tab->updateFromSelection(selection);
    }

    if (is_empty) {
        if (_fill_switch) {
            _fill_switch->show_placeholder(_("No object selected"), false);
        }
        if (_stroke_switch) {
            _stroke_switch->show_placeholder(_("No object selected"), false);
        }

        _ignore_updates = false;
        return;
    }

    SPItem* anchor = nullptr;
    if (selection && !items.empty()) {
        anchor = items.front(); 
    }

    auto update_channel = [&](UI::Widget::PaintSwitch* pswitch, bool is_fill) {
        if (!pswitch) return;

        SPStyle query(getDesktop()->getDocument());
        int property = is_fill ? QUERY_STYLE_PROPERTY_FILL : QUERY_STYLE_PROPERTY_STROKE;
        int result = sp_desktop_query_style(getDesktop(), &query, property);

        if (result == QUERY_STYLE_MULTIPLE_DIFFERENT) {
            pswitch->show_placeholder(_("Multiple styles"), true);
            return;
        }

        UI::Widget::PaintMode mode = UI::Widget::PaintMode::None;
        Inkscape::Colors::Color color(0x000000ff);
        double opacity = 1.0;
        const SPIPaint* paint_ptr = nullptr;
        bool color_found = false;

        if (anchor && anchor->style) {
            paint_ptr = anchor->style->getFillOrStroke(is_fill);
            opacity = is_fill ? (double)anchor->style->fill_opacity : (double)anchor->style->stroke_opacity;
            
            if (paint_ptr) {
                mode = Inkscape::UI::Widget::get_mode_from_paint(*paint_ptr);

                if (paint_ptr->isColor()) {
                    color = paint_ptr->getColor();
                    color_found = true;
                }
            }
        }

        if (!color_found) {
            if (auto from_desktop = sp_desktop_get_color(getDesktop(), is_fill)) {
                color = *from_desktop;
                if (!paint_ptr || !paint_ptr->set) {
                    mode = UI::Widget::PaintMode::Solid;
                }
            }
        }

        pswitch->set_mode(mode);
        color.setOpacity(opacity);

        if(mode == UI::Widget::PaintMode::Solid) {
            pswitch->set_color(color);
        } else if(mode == UI::Widget::PaintMode::None) {
            pswitch->show_placeholder("No Paint", false);
        }
        if (paint_ptr) {
            pswitch->update_from_paint(*paint_ptr);
        }

        if (is_fill && anchor && anchor->style) {
             using Rule = Inkscape::UI::Widget::FillRule;
             pswitch->set_fill_rule(anchor->style->fill_rule.computed == SP_WIND_RULE_NONZERO ? Rule::NonZero : Rule::EvenOdd);
        }
    };

    update_channel(_fill_switch.get(), true);
    update_channel(_stroke_switch.get(), false);

    _ignore_updates = false;
}

void FillAndStroke::selectionChanged(Selection *selection)
{
    if (!page_changed) {
        changed_fill = true;
        changed_stroke = true;
        changed_stroke_style = true;
    }

    _updateFromSelection();

    if (strokeStyleWdgt && npage == 2) {
        strokeStyleWdgt->selectionChangedCB();
    }
}

void FillAndStroke::selectionModified(Selection *selection, guint flags)
{
    changed_fill = true;
    changed_stroke = true;
    changed_stroke_style = true;

    _updateFromSelection();

    if (strokeStyleWdgt && npage == 2) {
        strokeStyleWdgt->selectionModifiedCB(flags);
    }
}

void FillAndStroke::desktopReplaced()
{
    changed_fill = true;
    changed_stroke = true;
    changed_stroke_style = true;

    if (_fill_switch) {
        _fill_switch->set_desktop(getDesktop());
    }
    if (_stroke_switch) {
        _stroke_switch->set_desktop(getDesktop());
    }
    if (strokeStyleWdgt) {
        strokeStyleWdgt->setDesktop(getDesktop());
    }
    if (_advanced_tab) {
        _advanced_tab->setDesktop(getDesktop());
    }
    _subject.setDesktop(getDesktop());

    documentReplaced();
}

void FillAndStroke::documentReplaced()
{
    if (_fill_switch) {
        _fill_switch->set_document(getDesktop() ? getDesktop()->getDocument() : nullptr);
    }
    if (_stroke_switch) {
        _stroke_switch->set_document(getDesktop() ? getDesktop()->getDocument() : nullptr);
    }
}

void FillAndStroke::_onSwitchPage(Gtk::Widget * page, guint pagenum)
{
    npage = pagenum;
    _updateFromSelection();

    for (size_t i = 0; i < _tab_labels.size(); i++) {
        if (auto label = _tab_labels[i]) {
            if (i == pagenum) {
                label->set_ellipsize(Pango::EllipsizeMode::NONE);
            } else {
                label->set_ellipsize(Pango::EllipsizeMode::END);
            }
        }
    }

    if (page->is_visible()) {
        bool update = false;
        if (npage == 0 && changed_fill) {
            update = true;
            changed_fill = false;
        } else if (npage == 1 && changed_stroke) {
            update = true;
            changed_stroke = false;
        } else if (npage == 2 && changed_stroke_style) {
            update = true;
            changed_stroke_style = false;
        }
        if (update) {
            page_changed = true;
            selectionChanged(getDesktop()->getSelection());
            page_changed = false;
        }
    }
    _savePagePref(pagenum);
}

void
FillAndStroke::_savePagePref(guint page_num)
{
    // remember the current page
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->setInt("/dialogs/fillstroke/page", page_num);
}

void
FillAndStroke::_layoutPageFill()
{
    _fill_switch = UI::Widget::PaintSwitch::create(true, true, true);
    _ConnectPaintSignals(_fill_switch.get(), true);
    _page_fill->table().attach(*_fill_switch, 0, 0, 1, 1);
}

void
FillAndStroke::_layoutPageStrokePaint()
{
    _stroke_switch = UI::Widget::PaintSwitch::create(true, true, true);
    _ConnectPaintSignals(_stroke_switch.get(), false);
    _page_stroke_paint->table().attach(*_stroke_switch, 0, 0, 1, 1);
}

void
FillAndStroke::_layoutPageStrokeStyle()
{
    strokeStyleWdgt = Gtk::make_managed<UI::Widget::StrokeStyle>();
    strokeStyleWdgt->set_hexpand();
    strokeStyleWdgt->set_halign(Gtk::Align::FILL);
    _page_stroke_style->table().attach(*strokeStyleWdgt, 0, 0, 1, 1);
}

void FillAndStroke::_layoutPageAdvanced()
{
    _advanced_tab = std::make_unique<UI::Widget::AdvancedTab>();
    _advanced_tab->set_hexpand();
    _advanced_tab->set_halign(Gtk::Align::FILL);
    _page_advanced->table().attach(*_advanced_tab, 0, 0, 1, 1);

    if (getDesktop()) {
        _advanced_tab->setDesktop(getDesktop());
    }
}

void
FillAndStroke::showPageFill()
{
    blink();
    _notebook.set_current_page(0);
    _savePagePref(0);

}

void
FillAndStroke::showPageStrokePaint()
{
    blink();
    _notebook.set_current_page(1);
    _savePagePref(1);
}

void
FillAndStroke::showPageStrokeStyle()
{
    blink();
    _notebook.set_current_page(2);
    _savePagePref(2);

}

Gtk::Box&
FillAndStroke::_createPageTabLabel(const Glib::ustring& label, const char *label_image)
{
    auto const _tab_label_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);

    auto img = Gtk::manage(sp_get_icon_image(label_image, Gtk::IconSize::NORMAL));
    _tab_label_box->append(*img);

    auto const _tab_label = Gtk::make_managed<Gtk::Label>(label, true);
    _tab_label->set_ellipsize(Pango::EllipsizeMode::END);
    _tab_label->set_lines(1);
    _tab_labels.push_back(_tab_label);
    _tab_label_box->append(*_tab_label);

    _tab_label_box->set_hexpand(true);
    _tab_label_box->set_tooltip_text(_tab_label->get_text());

    return *_tab_label_box;
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
