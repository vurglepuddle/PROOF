// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for selection tied to the application and without GUI.
 *
 * Copyright (C) 2018 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include "actions-transform.h"

#include <giomm.h> // Not <gtkmm.h>! To eventually allow a headless version!
#include <glibmm/i18n.h>

#include "actions-helper.h"
#include "box-frame.h"
#include "desktop.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "preferences.h"
#include "selection.h" // Selection
#include "page-manager.h"
#include "ui/icon-names.h"

void
transform_translate(const Glib::VariantBase& value, InkscapeApplication *app)
{
    Glib::Variant<Glib::ustring> s = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring> >(value);

    std::vector<Glib::ustring> tokens = Glib::Regex::split_simple(",", s.get());
    if (tokens.size() != 2) {
        show_output("action:transform_translate: requires two comma separated numbers");
        return;
    }
    double dx = 0;
    double dy = 0;

    try {
        dx = std::stod(tokens[0]);
        dy = std::stod(tokens[1]);
    } catch (...) {
        show_output("action:transform-move: invalid arguments");
        return;
    }

    auto selection = app->get_active_selection();
    selection->move(dx, dy);

    // Needed to update repr (is this the best way?).
    Inkscape::DocumentUndo::done(app->get_active_document(), Inkscape::Util::Internal::ContextString("ActionTransformTranslate"), "");
}

void
transform_scale(const Glib::VariantBase& value, InkscapeApplication *app)
{
    auto scale = (Glib::VariantBase::cast_dynamic<Glib::Variant<double>>(value)).get();
    app->get_active_selection()->scaleAnchored(scale, false);
}

void
transform_grow(const Glib::VariantBase& value, InkscapeApplication *app)
{
    auto scale = (Glib::VariantBase::cast_dynamic<Glib::Variant<double>>(value)).get();
    app->get_active_selection()->scaleAnchored(scale);
}

void
transform_grow_step(const Glib::VariantBase& value, InkscapeApplication *app)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    auto scale = (Glib::VariantBase::cast_dynamic<Glib::Variant<double>>(value)).get();
    app->get_active_selection()->scaleAnchored(scale * prefs->getDoubleLimited("/options/defaultscale/value", 2, 0, 1000));
}

void
transform_grow_screen(const Glib::VariantBase& value, InkscapeWindow *win)
{
    auto scale = (Glib::VariantBase::cast_dynamic<Glib::Variant<double>>(value)).get();
    auto desktop = win->get_desktop();
    desktop->getSelection()->scaleAnchored(scale / desktop->current_zoom());
}

void
transform_rotate(const Glib::VariantBase& value, InkscapeApplication *app)
{
    auto angle = (Glib::VariantBase::cast_dynamic<Glib::Variant<double>>(value)).get();

    if (auto doc = app->get_active_document()) {
        angle *= doc->yaxisdir();
    }
    app->get_active_selection()->rotateAnchored(angle);
}

void
transform_rotate_step(const Glib::VariantBase& value, InkscapeApplication *app)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    auto angle = (Glib::VariantBase::cast_dynamic<Glib::Variant<double>>(value)).get();
    double snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);

    if (auto doc = app->get_active_document()) {
        angle *= doc->yaxisdir();
    }
    app->get_active_selection()->rotateAnchored(angle / snaps);
}

void
transform_rotate_screen(const Glib::VariantBase& value, InkscapeWindow *win)
{
    auto angle = (Glib::VariantBase::cast_dynamic<Glib::Variant<double>>(value)).get();
    if (auto desktop = win->get_desktop()) {
        angle *= desktop->yaxisdir();
        desktop->getSelection()->rotateAnchored(angle, desktop->current_zoom());
    }
}


void
transform_remove(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    selection->removeTransform();

    // Needed to update repr (is this the best way?).
    Inkscape::DocumentUndo::done(app->get_active_document(), Inkscape::Util::Internal::ContextString("ActionTransformRemoveTransform"), "");
}

void transform_reapply(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    selection->reapplyAffine();
    Inkscape::DocumentUndo::maybeDone(app->get_active_document(), "reapply-transform", RC_("Undo", "Reapply Transforms"),
                                      INKSCAPE_ICON("tool-pointer"));
}

/// PROOF: Object > Transform > Reset Bounding Box, as in Illustrator: rotated objects keep a
/// rotated box, and this makes it upright again without moving anything.
void transform_reset_box(InkscapeApplication *app)
{
    auto selection = app->get_active_selection();
    bool changed = false;
    for (auto item : selection->items_vector()) {
        changed = Inkscape::reset_box_angle(*item) || changed;
    }
    if (changed) {
        Inkscape::DocumentUndo::done(app->get_active_document(), RC_("Undo", "Reset Bounding Box"),
                                     INKSCAPE_ICON("tool-pointer"));
    }
}

void page_rotate(const Glib::VariantBase& value, InkscapeApplication *app)
{
    auto document = app->get_active_document();
    Glib::Variant<int> i = Glib::VariantBase::cast_dynamic<Glib::Variant<int> >(value);
    document->getPageManager().rotatePage(i.get());
    Inkscape::DocumentUndo::done(document, RC_("Undo", "Rotate Page"), INKSCAPE_ICON("tool-pages"));
}

const Glib::ustring SECTION = NC_("Action Section", "Transform");

// SHOULD REALLY BE SELECTION LEVEL ACTIONS
std::vector<std::vector<Glib::ustring>> raw_data_transform = {
    // clang-format off
    {"app.transform-translate",     N_("Translate"),          SECTION, N_("Translate selected objects (dx,dy)")},
    {"app.transform-rotate",        N_("Rotate"),             SECTION, N_("Rotate selected objects by degrees")},
    {"app.transform-scale",         N_("Scale"),              SECTION, N_("Scale selected objects by scale factor")},
    {"app.transform-grow",          N_("Grow/Shrink"),        SECTION, N_("Grow/shrink selected objects")},
    {"app.transform-grow-step",     N_("Grow/Shrink Step"),   SECTION, N_("Grow/shrink selected objects by multiple of step value")},
    {"win.transform-grow-screen",   N_("Grow/Shrink Screen"), SECTION, N_("Grow/shrink selected objects relative to zoom level")},
    {"app.transform-rotate",        N_("Rotate"),             SECTION, N_("Rotate selected objects")},
    {"app.transform-rotate-step",   N_("Rotate Step"),        SECTION, N_("Rotate selected objects by multiple of step value")},
    {"win.transform-rotate-screen", N_("Rotate Screen"),      SECTION, N_("Rotate selected objects relative to zoom level")},
    {"app.transform-rotate(90.0)",  N_("Rotate Object 90°"),   SECTION, N_("Rotate selected objects 90° clockwise")},
    {"app.transform-rotate(-90.0)", N_("Rotate Object 90° CCW"), SECTION, N_("Rotate selected objects 90° counter-clockwise")},

    {"app.transform-remove",        N_("Remove Transforms"),  SECTION, N_("Remove any transforms from selected objects")},
    {"app.transform-reapply",       N_("Reapply Transforms"), SECTION, N_("Reapply the last transformation to the selection")},
    {"app.transform-reset-box",     N_("Reset Bounding Box"), SECTION, N_("Make the bounding box of rotated objects upright again")},
    {"app.page-rotate",             N_("Rotate Page 90°"),    SECTION, N_("Rotate page by 90-degree rotation steps")},
    // clang-format on
};

std::vector<std::vector<Glib::ustring>> hint_data_transform =
{
    // clang-format off
    {"app.transform-translate",     N_("Enter two comma-separated numbers, e.g. 50,-2.5")},
    {"app.transform-rotate",        N_("Enter angle (in degrees) for clockwise rotation")},
    {"app.transform-scale",         N_("Enter scaling factor, e.g. 1.5")},
    {"app.transform-grow",          N_("Enter positive or negative number to grow/shrink selection")},
    {"app.transform-grow-step",     N_("Enter positive or negative number to grow or shrink selection relative to preference step value")},
    {"app.transform-grow-screen",   N_("Enter positive or negative number to grow or shrink selection relative to zoom level")},
    {"app.page-rotate",             N_("Enter number of 90-degree rotation steps")},
    // clang-format on
};

void
add_actions_transform(InkscapeApplication* app)
{
    // If these ever get moved to the Inkscape::Selection object, the screen and app based ones can be combined again.
    Glib::VariantType Bool(  Glib::VARIANT_TYPE_BOOL);
    Glib::VariantType Int(   Glib::VARIANT_TYPE_INT32);
    Glib::VariantType Double(Glib::VARIANT_TYPE_DOUBLE);
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    // clang-format off
    gapp->add_action_with_parameter( "transform-translate",      String, sigc::bind(sigc::ptr_fun(&transform_translate),       app));
    gapp->add_action_with_parameter( "transform-rotate",         Double, sigc::bind(sigc::ptr_fun(&transform_rotate),          app));
    gapp->add_action_with_parameter( "transform-scale",          Double, sigc::bind(sigc::ptr_fun(&transform_scale),           app));
    gapp->add_action_with_parameter( "transform-grow",           Double, sigc::bind(sigc::ptr_fun(&transform_grow),            app));
    gapp->add_action_with_parameter( "transform-grow-step",      Double, sigc::bind(sigc::ptr_fun(&transform_grow_step),       app));
    gapp->add_action_with_parameter( "transform-rotate",         Double, sigc::bind(sigc::ptr_fun(&transform_rotate),          app));
    gapp->add_action_with_parameter( "transform-rotate-step",    Double, sigc::bind(sigc::ptr_fun(&transform_rotate_step),     app));
    gapp->add_action(                "transform-remove",                 sigc::bind(sigc::ptr_fun(&transform_remove),          app));
    gapp->add_action(                "transform-reapply",                sigc::bind(sigc::ptr_fun(&transform_reapply),         app));
    gapp->add_action(                "transform-reset-box",              sigc::bind(sigc::ptr_fun(&transform_reset_box),       app));
    gapp->add_action_with_parameter( "page-rotate",              Int,    sigc::bind(sigc::ptr_fun(&page_rotate),               app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_transform);
    app->get_action_hint_data().add_data(hint_data_transform);
}

void
add_actions_transform(InkscapeWindow* win)
{
    Glib::VariantType Double(Glib::VARIANT_TYPE_DOUBLE);

    win->add_action_with_parameter( "transform-grow-screen", Double, sigc::bind(sigc::ptr_fun(&transform_grow_screen), win));
    win->add_action_with_parameter( "transform-rotate-screen", Double, sigc::bind(sigc::ptr_fun(&transform_rotate_screen), win));

    // action data already added above by app actions.
}

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
