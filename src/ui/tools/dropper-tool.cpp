// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tool for picking colors from drawing
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2005 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/i18n.h>
#include <2geom/circle.h>

#include "desktop-style.h"
#include "document-undo.h"
#include "message-context.h"
#include "selection.h"
#include "style.h"
#include "page-manager.h"

#include "display/drawing.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-drawing.h"
#include "display/translucency-group.h"

#include "ui/cursor-utils.h"
#include "ui/icon-names.h"
#include "ui/tools/dropper-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::DocumentUndo;

namespace Inkscape::UI::Tools {

enum PickMode
{
    PICK_VISIBLE,
    PICK_ACTUAL
};

DropperTool::DropperTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/dropper", "dropper-pick-fill.svg")
    , mod_dropper_dropping(Modifiers::Modifier::get(Modifiers::Type::DROPPER_DROPPING))
    , mod_dropper_invert(Modifiers::Modifier::get(Modifiers::Type::DROPPER_INVERT))
    , mod_dropper_stroke(Modifiers::Modifier::get(Modifiers::Type::DROPPER_STROKE))
    , mod_select_force_drag(Modifiers::Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
{
    area = make_canvasitem<CanvasItemBpath>(desktop->getCanvasControls());
    area->set_stroke(0x0000007f);
    area->set_fill(0x0, SP_WIND_RULE_EVENODD);
    area->set_visible(false);

    auto prefs = Preferences::get();

    if (prefs->getBool("/tools/dropper/selcue")) {
        enableSelectionCue();
    }

    if (prefs->getBool("/tools/dropper/gradientdrag")) {
        enableGrDrag();
    }

    // Disable other tool's translucencies
    _translucency_key = _desktop->getTranslucencyGroups().createGroupKey(1.0, false);
}

DropperTool::~DropperTool()
{
    _desktop->getTranslucencyGroups().removeGroupKey(_translucency_key);
    enableGrDrag(false);
    ungrabCanvasEvents();
}

/**
 * Returns the current dropper context color.
 *
 * - If in dropping mode, returns color from selected objects.
 *   Ignored if non_dropping set to true.
 * - If in dragging mode, returns average color on canvas, depending on radius.
 * - If in pick mode, alpha is not premultiplied. Alpha is only set if in pick mode
 *   and setalpha is true. Both values are taken from preferences.
 *
 * @param invert If true, invert the rgb value
 * @param non_dropping If true, use color from canvas, even in dropping mode.
 */
std::optional<Colors::Color> DropperTool::get_color(bool invert, bool non_dropping) const
{
    auto prefs = Preferences::get();

    int pick = prefs->getInt("/tools/dropper/pick", PICK_VISIBLE);
    bool setalpha = prefs->getBool("/tools/dropper/setalpha", true);

    // non_dropping ignores dropping mode and always uses color from canvas.
    // Used by the clipboard
    auto color = non_dropping ? non_dropping_color : stored_color;

    if (color && invert)
        color->invert();

    if (color && (pick != PICK_ACTUAL || !setalpha))
        color->enableOpacity(false);

    return color;
}

bool DropperTool::root_handler(CanvasEvent const &event)
{
    auto prefs = Preferences::get();
    int pick = prefs->getInt("/tools/dropper/pick", PICK_VISIBLE);

    // Decide first what kind of 'mode' we're in.
    auto modifiers = event.modifiersAfter();
    stroke   = mod_dropper_stroke->active(modifiers);
    dropping = mod_dropper_dropping->active(modifiers);
    invert   = mod_dropper_invert->active(modifiers);

    // Get color from selected object
    // Only if dropping mode enabled and object's color is set.
    // Otherwise dropping mode disabled.
    if (dropping) {
        auto selection = _desktop->getSelection();
        g_assert(selection);

        std::optional<Inkscape::Colors::Color> apply_color;
        for (auto const &obj: selection->objects()) {
            if (obj->style) {
                if (!stroke && obj->style->fill.set) {
                    apply_color = obj->style->fill.getColor();
                    apply_color->addOpacity(obj->style->fill_opacity.as_double());
                } else if (stroke && obj->style->stroke.set) {
                    apply_color = obj->style->stroke.getColor();
                    apply_color->addOpacity(obj->style->stroke_opacity.as_double());
                }
            }
        }

        if (apply_color) {
            stored_color = apply_color;
        } else {
            // This means that having no selection or some other error
            // we will default back to normal dropper mode.
            dropping = false;
        }
    }

    bool ret = false;
    bool self_destroyed = false;

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (event.num_press != 1) {
                return;
            }

            if (event.button == 1) {
                centre = event.pos;
                dragging = true;
                ret = true;
            }

            grabCanvasEvents(EventType::KEY_PRESS      |
                             EventType::KEY_RELEASE    |
                             EventType::BUTTON_RELEASE |
                             EventType::MOTION         |
                             EventType::BUTTON_PRESS);
        },

        [&] (MotionEvent const &event) {
            if (event.modifiers & (GDK_BUTTON2_MASK | GDK_BUTTON3_MASK)) {
                // pass on middle and right drag
                return;
            }

            // otherwise, constantly calculate color no matter if any button pressed or not
            Geom::IntRect pick_area;
            if (dragging) {
                // Calculate average

                // Radius
                double rw = std::min((event.pos - centre).length(), 400.0);
                if (rw == 0) { // happens sometimes, little idea why...
                    return;
                }
                radius = rw;

                auto const cd = _desktop->w2d(centre);
                auto const w2dt = _desktop->w2d();
                auto const scale = rw * w2dt.descrim();
                auto const sm = Geom::Scale(scale) * Geom::Translate(cd);

                // Show circle on canvas.
                auto path = Geom::Path(Geom::Circle(0, 0, 1)); // Unit circle centered at origin.
                path *= sm;
                area->set_bpath(std::move(path));
                area->set_visible(true);

                // Get buffer
                auto r = Geom::Rect(centre, centre);
                r.expandBy(rw);
                if (!r.hasZeroArea()) {
                    pick_area = r.roundOutwards();
                }
            } else {
                // Pick single pixel
                pick_area = Geom::IntRect::from_xywh(0, 0, 1, 1) + event.pos.floor();
            }

            auto canvas_item_drawing = _desktop->getCanvasDrawing();
            auto drawing = canvas_item_drawing->get_drawing();

            // Get average color of on screen pixels (sRGB)
            auto avg = drawing->averageColor(pick_area);

            if (pick == PICK_VISIBLE || avg.getOpacity() == 0.0) {
                // Compose with page color
                auto const &bg = _desktop->getDocument()->getPageManager().getDefaultBackgroundColor();
                avg = bg.composed(avg);
            }

            // Remember color
            if (!dropping) {
                stored_color = avg;
            }

            // Remember color from canvas, even in dropping mode.
            // These values are used by the clipboard.
            non_dropping_color = avg;
            ret = true;
        },

        [&] (ButtonReleaseEvent const &event) {
            if (event.button != 1) {
                return;
            }

            area->set_visible(false);
            dragging = false;

            ungrabCanvasEvents();

            auto selection = _desktop->getSelection();
            g_assert(selection);

            auto old_selection = selection->items_vector();

            if (dropping) {
                auto const button_w = event.pos;
                // Remember clicked item, disregarding groups, honoring Alt.
                auto force_drag = mod_select_force_drag->active(event.modifiers);
                item_to_select = sp_event_context_find_item(_desktop, button_w, force_drag, true);

                // Change selected object to object under cursor.
                if (item_to_select) {
                    selection->set(item_to_select.get());
                }
            }

            auto picked_color = get_color(invert);
            if (!picked_color) {
                ret = true;
                return;
            }

            // One time pick has active signal, call them all and clear.
            if (!onetimepick_signal.empty()) {
                onetimepick_signal.emit(*picked_color);
                onetimepick_signal.clear();
                // Do this last as it destroys the picker tool.
                sp_toggle_dropper(_desktop);
                self_destroyed = true;
                return;
            }

            // do the actual color setting
            sp_desktop_set_color(_desktop, *picked_color, false, !stroke);

            // REJON: set aux. toolbar input to hex color!
            if (!_desktop->getSelection()->isEmpty()) {
                DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Set picked color"), INKSCAPE_ICON("color-picker"));
            }

            if (dropping) {
                selection->setList(old_selection);
            }

            ret = true;
        },

        [&] (KeyPressEvent const &event) {
            switch (get_latin_keyval(event)) {
                case GDK_KEY_Up:
                case GDK_KEY_Down:
                case GDK_KEY_KP_Up:
                case GDK_KEY_KP_Down:
                    // Prevent the zoom field from activating.
                    if (!mod_ctrl_only(event)) {
                        ret = true;
                    }
                    break;
                case GDK_KEY_Escape:
                    _desktop->getSelection()->clear();
                    break;
                default:
                    break;
            }
        },

        [&] (CanvasEvent const &event) {}
    );

    if (self_destroyed) {
        return true;
    }

    // set the status message to the right text.
    auto color = get_color(invert);

    if (color) {
        // alpha of color under cursor, to show in the statusbar
        // locale-sensitive printf is OK, since this goes to the UI, not into SVG
        auto alphastr = g_strdup_printf(_(" alpha %.3g"), color->getOpacity());
        // where the color is picked, to show in the statusbar
        auto where = dragging ? g_strdup_printf(_(", averaged with radius %d"), (int)radius) : g_strdup_printf("%s", _(" under cursor"));
        // message, to show in the statusbar
        auto message = dragging ? _("<b>Release mouse</b> to set color.") :
                                  g_strdup_printf(_("<b>Click</b> to set fill, <b>%s+Click</b> to set stroke; <b>drag</b> to average color in area; with <b>%s</b> to pick inverse color; <b>Ctrl+C</b> to copy the color under mouse to clipboard"),
                                                  mod_dropper_stroke->get_label().c_str(),
                                                  mod_dropper_invert->get_label().c_str());

        defaultMessageContext()->setF(
            Inkscape::NORMAL_MESSAGE,
            "<b>%s%s</b>%s. %s", color->toString(false).c_str(),
            pick == PICK_VISIBLE ? "" : alphastr, where, message);

        g_free(where);
        g_free(alphastr);
    }

    // Set the right cursor for the mode and apply the special Fill color
    _cursor_filename = dropping ? (stroke ? "dropper-drop-stroke.svg" : "dropper-drop-fill.svg")
                                : (stroke ? "dropper-pick-stroke.svg" : "dropper-pick-fill.svg");

    // We do this ourselves to get color correct.
    set_svg_cursor(*_desktop->getCanvas(), _cursor_filename, color);

    if (!ret) {
        ret = ToolBase::root_handler(event);
    }

    return ret;
}

} // namespace Inkscape::UI::Tools

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
