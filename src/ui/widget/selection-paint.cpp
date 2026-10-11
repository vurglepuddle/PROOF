// SPDX-License-Identifier: GPL-2.0-or-later
// Paint operations shared with the Fill and Stroke dialog.
#include "selection-paint.h"
#include <glibmm/i18n.h>
#include "desktop.h"
#include "desktop-style.h"
#include "document-undo.h"
#include "gradient-chemistry.h"
#include "pattern-manipulation.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "ui/icon-names.h"
#include "ui/widget/paint-switch.h"

namespace Inkscape::UI::Widget {
namespace {
void remove_opacity(SPObject *item, bool is_fill)
{
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_unset_property(css, is_fill ? "fill-opacity" : "stroke-opacity");
    sp_repr_css_change_recursive(item->getRepr(), css, "style");
    sp_repr_css_attr_unref(css);
}

} // namespace

void remember_default_paint(SPDesktop *desktop, bool is_fill)
{
    if (!desktop || !desktop->getSelection()->isEmpty()) return;
    auto css = sp_repr_css_attr_new();
    for (auto property : {is_fill ? "fill" : "stroke", is_fill ? "fill-opacity" : "stroke-opacity",
                          is_fill ? "fill-rule" : "stroke-width"}) {
        if (auto value = sp_repr_css_property(desktop->current, property, nullptr)) {
            sp_repr_css_set_property(css, property, value);
        } else {
            sp_repr_css_unset_property(css, property);
        }
    }
    // Match the creation tools' per-type current-style caches. Explicit per-tool
    // fixed styles remain governed by their existing usecurrent preference.
    sp_css_attr_unset_uris(css);
    auto prefs = Preferences::get();
    for (auto type : {"rect", "arc", "star", "spiral", "path", "text", "3dbox"}) {
        prefs->mergeStyle(Glib::ustring("/desktop/") + type + "/style", css);
    }
    sp_repr_css_attr_unref(css);
}

std::vector<sigc::connection> connect_selection_paint(
    PaintSwitch &paint_switch, bool is_fill, std::function<SPDesktop *()> desktop,
    std::function<bool()> blocked, unsigned int undo_tag, std::function<void()> changed)
{
    std::vector<sigc::connection> connections;

    connections.push_back(paint_switch.get_pattern_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto pattern, auto color, auto label, auto transform, auto offset, auto uniform, auto gap) {
        if (blocked() || !desktop()) return;
        auto doc = desktop()->getDocument();
        auto items = desktop()->getSelection()->items_vector();
        if (items.empty() || !pattern) return;

        auto kind = is_fill ? FILL : STROKE;
        for (auto item : items) {
            sp_item_apply_pattern(item, pattern, kind, color, label, transform, offset, uniform, gap);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc, is_fill ? RC_("Undo", "Set pattern on fill") : RC_("Undo", "Set pattern on stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
    }));

    connections.push_back(paint_switch.get_hatch_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto hatch, auto color, auto label, auto transform, auto offset, auto pitch, auto rotation, auto stroke) {
        if (blocked() || !desktop()) return;
        auto doc = desktop()->getDocument();
        auto items = desktop()->getSelection()->items_vector();
        if (items.empty() || !hatch) return;

        auto kind = is_fill ? FILL : STROKE;
        for (auto item : items) {
            sp_item_apply_hatch(item, hatch, kind, color, label, transform, offset, pitch, rotation, stroke);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc, is_fill ? RC_("Undo", "Set hatch on fill") : RC_("Undo", "Set hatch on stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
    }));

    connections.push_back(paint_switch.get_gradient_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto vector, auto gradient_type) {
        if (blocked() || !desktop()) return;

        auto doc = desktop()->getDocument();
        auto items = desktop()->getSelection()->items_vector();
        auto kind = is_fill ? FILL : STROKE;

        for (auto item : items) {
            sp_item_apply_gradient(item, vector, desktop(), gradient_type, false, kind);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc,
            is_fill ? RC_("Undo", "Set gradient on fill") : RC_("Undo", "Set gradient on stroke"),
            INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
    }));

    connections.push_back(paint_switch.get_mesh_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto mesh) {
        if (blocked() || !desktop()) return;
        auto doc = desktop()->getDocument();
        auto items = desktop()->getSelection()->items_vector();
        if (items.empty()) return;
        FillOrStroke kind = is_fill ? FILL : STROKE;

        for (auto item : items) {
            sp_item_apply_mesh(item, mesh, doc, kind);
            remove_opacity(item, is_fill);
        }

        DocumentUndo::done(doc,
            (is_fill) ? RC_("Undo", "Set mesh on fill") : RC_("Undo", "Set mesh on stroke"),
            INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
    }));

    connections.push_back(paint_switch.get_swatch_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto vector, auto operation, auto replacement, std::optional<Color> color, auto label) {
        if (blocked() || !desktop()) return;
        auto doc = desktop()->getDocument();
        auto selection = desktop()->getSelection();
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

                    sp_item_apply_gradient(item, vector, desktop(), SP_GRADIENT_TYPE_LINEAR, true, kind);
                    remove_opacity(item, is_fill);
                }
                DocumentUndo::done(doc, is_fill ? RC_("Undo", "Set swatch on fill") : RC_("Undo", "Set swatch on stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
                break;
            }
            case EditOperation::Change: {
                if (vector && color) {
                    sp_change_swatch_color(vector, color.value());
                    DocumentUndo::maybeDone(doc, "swatch-color", RC_("Undo", "Change swatch color"), INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
                } else if (vector) {
                    for (auto item : items) {
                        sp_item_apply_gradient(item, vector, desktop(), SP_GRADIENT_TYPE_LINEAR, true, kind);
                        remove_opacity(item, is_fill);
                    }
                    DocumentUndo::maybeDone(doc, "swatch-assign",
                        is_fill ? RC_("Undo", "Set swatch on fill") : RC_("Undo", "Set swatch on stroke"),
                        INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
                }
                break;
            }
            case EditOperation::Delete: {
                if (!vector || !replacement) return;

                auto kind = is_fill ? FILL : STROKE;
                for (auto item : items) {
                    sp_delete_item_swatch(item, kind, vector, replacement);
                }

                DocumentUndo::done(doc, RC_("Undo", "Delete swatch"),  INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
                break;
            }
            case EditOperation::Rename: {
                if (vector && !label.empty()) {
                    sp_rename_swatch(vector, label);
                    DocumentUndo::maybeDone(doc, "swatch-rename", RC_("Undo", "Rename swatch"), INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
                }
                break;
            }
            default:
                break;
        }
    }));

    connections.push_back(paint_switch.get_flat_color_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto& color) {
        if (blocked() || !desktop()) return;

        sp_desktop_set_color(desktop(), color, false, is_fill);
        remember_default_paint(desktop(), is_fill);
        if (changed) changed();

        DocumentUndo::maybeDone(desktop()->getDocument(),
                                is_fill ? "fill:flatcolor" : "stroke:flatcolor",
                                is_fill ? RC_("Undo", "Set fill color") : RC_("Undo", "Set stroke color"),
                                INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
    }));

    connections.push_back(paint_switch.get_fill_rule_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto fill_rule) {
        if (blocked() || !desktop()) return;

        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "fill-rule",
            (fill_rule == UI::Widget::FillRule::EvenOdd) ? "evenodd" : "nonzero");
        sp_desktop_set_style(desktop(), css, true);
        sp_repr_css_attr_unref(css);
        remember_default_paint(desktop(), is_fill);
        if (changed) changed();

        DocumentUndo::maybeDone(desktop()->getDocument(),
            "change-fill-rule",
            RC_("Undo", "Change fill rule"),
            INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
    }));

    connections.push_back(paint_switch.get_inherit_mode_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto mode) {
        if (blocked() || !desktop()) return;
        auto doc = desktop()->getDocument();

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

        sp_desktop_set_style(desktop(), css, true);
        sp_repr_css_attr_unref(css);
        remember_default_paint(desktop(), is_fill);
        if (changed) changed();

        DocumentUndo::done(doc,
            (is_fill) ? RC_("Undo", "Inherit fill") : RC_("Undo", "Inherit stroke"),
            INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
    }));

    connections.push_back(paint_switch.get_signal_mode_changed().connect([desktop, blocked, is_fill, undo_tag, changed](auto mode) {
        if (blocked() || !desktop()) return;
        auto doc = desktop()->getDocument();

        if (mode == UI::Widget::PaintMode::None) {
            SPCSSAttr *css = sp_repr_css_attr_new();

            sp_repr_css_set_property(css, (is_fill) ? "fill" : "stroke", "none");
            sp_desktop_set_style(desktop(), css, true);
            sp_repr_css_attr_unref(css);
            remember_default_paint(desktop(), is_fill);
            if (changed) changed();

            DocumentUndo::done(doc,
                (is_fill) ? RC_("Undo", "Remove fill") : RC_("Undo", "Remove stroke"),
                INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
        } else if (mode == UI::Widget::PaintMode::Derived) {

            SPCSSAttr *css = sp_repr_css_attr_new();
            sp_repr_css_unset_property(css, is_fill ? "fill" : "stroke");
            sp_desktop_set_style(desktop(), css, true);
            sp_repr_css_attr_unref(css);
            remember_default_paint(desktop(), is_fill);
            if (changed) changed();

            DocumentUndo::done(doc,
                (is_fill) ? RC_("Undo", "Unset fill") : RC_("Undo", "Unset stroke"),
                INKSCAPE_ICON("dialog-fill-and-stroke"), undo_tag);
        }
    }));

    return connections;
}
} // namespace Inkscape::UI::Widget
