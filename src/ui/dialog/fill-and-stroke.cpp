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

void remove_opacity(SPObject *item, bool is_fill)
{
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_unset_property(css, is_fill ? "fill-opacity" : "stroke-opacity");
    sp_repr_css_change_recursive(item->getRepr(), css, "style");
    sp_repr_css_attr_unref(css);
}

// Connects signals from the PaintSwitch widget to the document/desktop.
void FillAndStroke::_ConnectPaintSignals(UI::Widget::PaintSwitch *paint_switch, bool is_fill) {
    paint_switch->get_pattern_changed().connect([this, is_fill](auto pattern, auto color, auto label, auto transform, auto offset, auto uniform, auto gap) {
        if (_ignore_updates || !getDesktop()) return;
        auto doc = getDesktop()->getDocument();
        auto items = getDesktop()->getSelection()->items_vector();
        if (items.empty() || !pattern) return;

        auto kind = is_fill ? FILL : STROKE;
        for (auto item : items) {
            sp_item_apply_pattern(item, pattern, kind, color, label, transform, offset, uniform, gap);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc, is_fill ? RC_("Undo", "Set pattern on fill") : RC_("Undo", "Set pattern on stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    });
    
    paint_switch->get_hatch_changed().connect([this, is_fill](auto hatch, auto color, auto label, auto transform, auto offset, auto pitch, auto rotation, auto stroke) {
        if (_ignore_updates || !getDesktop()) return;
        auto doc = getDesktop()->getDocument();
        auto items = getDesktop()->getSelection()->items_vector();
        if (items.empty() || !hatch) return;

        auto kind = is_fill ? FILL : STROKE;
        for (auto item : items) {
            sp_item_apply_hatch(item, hatch, kind, color, label, transform, offset, pitch, rotation, stroke);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc, is_fill ? RC_("Undo", "Set hatch on fill") : RC_("Undo", "Set hatch on stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    });
    
    paint_switch->get_gradient_changed().connect([this, is_fill, paint_switch](auto vector, auto gradient_type) {
        if (_ignore_updates || !getDesktop()) return;

        auto doc = getDesktop()->getDocument();
        auto items = getDesktop()->getSelection()->items_vector();
        auto kind = is_fill ? FILL : STROKE;

        for (auto item : items) {
            sp_item_apply_gradient(item, vector, getDesktop(), gradient_type, false, kind);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc, 
            is_fill ? RC_("Undo", "Set gradient on fill") : RC_("Undo", "Set gradient on stroke"), 
            INKSCAPE_ICON("dialog-fill-and-stroke"));
    });

    paint_switch->get_mesh_changed().connect([this, is_fill](auto mesh) {
        if (_ignore_updates || !getDesktop()) return;
        auto doc = getDesktop()->getDocument();
        auto items = getDesktop()->getSelection()->items_vector();
        if (items.empty()) return;
        FillOrStroke kind = is_fill ? FILL : STROKE;

        for (auto item : items) {
            sp_item_apply_mesh(item, mesh, doc, kind);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc, 
            (is_fill) ? RC_("Undo", "Set mesh on fill") : RC_("Undo", "Set mesh on stroke"), 
            INKSCAPE_ICON("dialog-fill-and-stroke"));
    });

    paint_switch->get_swatch_changed().connect([this, is_fill](auto vector, auto operation, auto replacement, std::optional<Color> color, auto label) {
        if (_ignore_updates || !getDesktop()) return;
        auto doc = getDesktop()->getDocument();
        auto selection = getDesktop()->getSelection();
        auto items = selection->items_vector();

        if (items.empty() && operation == EditOperation::New){
            return;
        }

        auto kind = is_fill ? FILL : STROKE;
        switch (operation) {
            case EditOperation::New: {
                for (auto item : items) {
                    if (!item || !item->style) continue;

                    auto paint = item->style->getFillOrStroke(is_fill);
                    auto clr = paint && paint->isColor() ? std::optional(paint->getColor()) : std::nullopt;
                    vector = clr ? sp_find_matching_swatch(doc, *clr) : nullptr;

                    sp_item_apply_gradient(item, vector, getDesktop(), SP_GRADIENT_TYPE_LINEAR, true, kind);
                    remove_opacity(item, is_fill);
                }
                DocumentUndo::done(doc, is_fill ? RC_("Undo", "Set swatch on fill") : RC_("Undo", "Set swatch on stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
                break;
            }
            case EditOperation::Change: {
                if (vector && color) {
                    sp_change_swatch_color(vector, color.value());
                    DocumentUndo::maybeDone(doc, "swatch-color", RC_("Undo", "Change swatch color"), INKSCAPE_ICON("dialog-fill-and-stroke"));
                } else if (vector) {
                    for (auto item : items) {
                        sp_item_apply_gradient(item, vector, getDesktop(), SP_GRADIENT_TYPE_LINEAR, true, kind);
                        remove_opacity(item, is_fill);
                    }
                    DocumentUndo::maybeDone(doc, "swatch-assign", 
                        is_fill ? RC_("Undo", "Set swatch on fill") : RC_("Undo", "Set swatch on stroke"), 
                        INKSCAPE_ICON("dialog-fill-and-stroke"));
                }
                break;
            }
            case EditOperation::Delete: {
                if (!vector || !replacement) return;
    
                auto kind = is_fill ? FILL : STROKE;
                for (auto item : items) {
                    sp_delete_item_swatch(item, kind, vector, replacement);
                }

                DocumentUndo::done(doc, RC_("Undo", "Delete swatch"),  INKSCAPE_ICON("dialog-fill-and-stroke"));
                break;
            }
            case EditOperation::Rename: {
                if (vector && !label.empty()) {
                    sp_rename_swatch(vector, label);
                    DocumentUndo::maybeDone(doc, "swatch-rename", RC_("Undo", "Rename swatch"), INKSCAPE_ICON("dialog-fill-and-stroke"));
                }
                break;
            }
            default:
                break;
        }
    });

    paint_switch->get_flat_color_changed().connect([this, is_fill](auto& color) {
        if (_ignore_updates || !getDesktop()) return;

        sp_desktop_set_color(getDesktop(), color, false, is_fill);

        DocumentUndo::maybeDone(getDesktop()->getDocument(), 
                                is_fill ? "fill:flatcolor" : "stroke:flatcolor", 
                                is_fill ? RC_("Undo", "Set fill color") : RC_("Undo", "Set stroke color"),
                                INKSCAPE_ICON("dialog-fill-and-stroke"));
    });

    paint_switch->get_fill_rule_changed().connect([this, is_fill](auto fill_rule) {
        if (_ignore_updates || !getDesktop()) return;

        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "fill-rule", 
            (fill_rule == UI::Widget::FillRule::EvenOdd) ? "evenodd" : "nonzero");
        sp_desktop_set_style(getDesktop(), css, true);  
        sp_repr_css_attr_unref(css);

        DocumentUndo::maybeDone(getDesktop()->getDocument(), 
            "change-fill-rule", 
            RC_("Undo", "Change fill rule"), 
            INKSCAPE_ICON("dialog-fill-and-stroke"));
    });

    paint_switch->get_inherit_mode_changed().connect([this, is_fill](auto mode) {
        if (_ignore_updates || !getDesktop()) return;
        auto doc = getDesktop()->getDocument();
        auto items = getDesktop()->getSelection()->items_vector();
        if (items.empty()) return;

        auto property = is_fill ? "fill" : "stroke";
        SPCSSAttr *css = sp_repr_css_attr_new();

        switch (mode) {
        case UI::Widget::PaintDerivedMode::Unset:
            sp_repr_css_unset_property(css, property);
            break;
        case UI::Widget::PaintDerivedMode::Inherit:
            sp_repr_css_set_property(css, property, "inherit");
            break;
        case UI::Widget::PaintDerivedMode::ContextFill:
            sp_repr_css_set_property(css, property, "context-fill");
            break;
        case UI::Widget::PaintDerivedMode::ContextStroke:
            sp_repr_css_set_property(css, property, "context-stroke");
            break;
        case UI::Widget::PaintDerivedMode::CurrentColor:
            sp_repr_css_set_property(css, property, "currentColor");
            break;
        default:
            g_warning("file %s: line %d: Unknown inherited paint mode", __FILE__, __LINE__);
            sp_repr_css_attr_unref(css);
            return;
        }

        sp_desktop_set_style(getDesktop(), css, true);
        sp_repr_css_attr_unref(css);

        DocumentUndo::done(doc,
            (is_fill) ? RC_("Undo", "Inherit fill") : RC_("Undo", "Inherit stroke"),
            INKSCAPE_ICON("dialog-fill-and-stroke"));
    });

    paint_switch->get_signal_mode_changed().connect([this, is_fill](auto mode) {
        if (_ignore_updates || !getDesktop()) return;
        auto doc = getDesktop()->getDocument();
        auto items = getDesktop()->getSelection()->items_vector();
        if (items.empty()) return;

        if (mode == UI::Widget::PaintMode::None) {
            SPCSSAttr *css = sp_repr_css_attr_new();

            sp_repr_css_set_property(css, (is_fill) ? "fill" : "stroke", "none");
            sp_desktop_set_style(getDesktop(), css, true);
            sp_repr_css_attr_unref(css);

            DocumentUndo::done(doc, 
                (is_fill) ? RC_("Undo", "Remove fill") : RC_("Undo", "Remove stroke"), 
                INKSCAPE_ICON("dialog-fill-and-stroke"));
        } else if (mode == UI::Widget::PaintMode::Derived) {
            
            SPCSSAttr *css = sp_repr_css_attr_new();
            sp_repr_css_unset_property(css, is_fill ? "fill" : "stroke");
            sp_desktop_set_style(getDesktop(), css, true); 
            sp_repr_css_attr_unref(css);

            DocumentUndo::done(doc, 
                (is_fill) ? RC_("Undo", "Unset fill") : RC_("Undo", "Unset stroke"), 
                INKSCAPE_ICON("dialog-fill-and-stroke"));
        }
    });
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
