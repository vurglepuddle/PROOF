// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 *
 * Actions Related to Text
 *
 * Authors:
 *   Sushant A A <sushant.co19@gmail.com>
 *
 * Copyright (C) 2021 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <giomm.h>
#include <glibmm/i18n.h>

#include "object/sp-text.h"
#include "selection.h"
#include "actions/actions-svg-processing.h"
#include "actions-text.h"
#include "actions/actions-extra-data.h"
#include "inkscape-application.h"
#include "text-chemistry.h"
#include "ui/dialog/font-substitution.h"

void
selection_text_put_on_path()
{
    text_put_on_path();
}

void
selection_text_remove_from_path()
{
    text_remove_from_path();
}

void
text_flow_into_frame()
{
    text_flow_into_shape();
}

void
text_flow_subtract_frame()
{
    text_flow_shape_subtract();
}

void
select_text_unflow()
{
    text_unflow();
}

void
select_text_unflow_and_keep_shape(InkscapeApplication* app)
{
    auto selection = app->get_active_selection();
    auto text = cast<SPText>(selection->singleItem());
    if (!text)
        return;
    insert_text_fallback(text->getRepr(),selection->document());
    text->hide_shape_inside();
}

void
text_convert_to_regular()
{
    flowtext_to_text();
}

void
text_convert_to_glyphs()
{
    text_to_glyphs();
}

void
text_unkern()
{
    text_remove_all_kerns();
}

// PROOF: the fonts the document names that aren't installed, and a font to replace each by.
void
text_missing_fonts(InkscapeApplication* app)
{
    Inkscape::UI::Dialog::showMissingFonts(app->get_active_desktop());
}

const Glib::ustring SECTION = NC_("Action Section", "Text");

std::vector<std::vector<Glib::ustring>> raw_data_text =
{
    // clang-format off
    {"app.text-put-on-path",          N_("Put on Path"),            SECTION, N_("Put text on path")},
    {"app.text-remove-from-path",     N_("Remove from Path"),       SECTION, N_("Remove text from path")},
    {"app.text-flow-into-frame",      N_("Flow into Frame"),        SECTION, N_("Put text into a frame (path or shape), creating a flowed text linked to the frame object")},
    {"app.text-flow-subtract-frame",  N_("Set Subtraction Frames"), SECTION, N_("Flow text around a frame (path or shape), only available for SVG 2.0 Flow text.")},
    {"app.text-unflow",               N_("Unflow"),                 SECTION, N_("Remove text from frame (creates a single-line text object)")},
    {"app.text-unflow-and-keep-shape",N_("Unflow and Keep Shape"),  SECTION, N_("Remove text from shape")},
    {"app.text-convert-to-regular",   N_("Convert to Text"),        SECTION, N_("Convert flowed text to regular text object (preserves appearance)")},
    {"app.text-convert-to-glyphs",    N_("Convert to Glyphs"),      SECTION, N_("Convert text into individual glyphs")},
    {"app.text-unkern",               N_("Remove Manual Kerns"),    SECTION, N_("Remove all manual kerns and glyph rotations from a text object")},
    {"app.text-missing-fonts",        N_("Missing Fonts"),          SECTION, N_("List the fonts the document uses that aren't installed, and replace them")}
    // clang-format on
};

void
add_actions_text(InkscapeApplication* app)
{
    auto gapp = app->gio_app();

    // clang-format off
    gapp->add_action( "text-put-on-path",            sigc::ptr_fun(selection_text_put_on_path));
    gapp->add_action( "text-remove-from-path",       sigc::ptr_fun(selection_text_remove_from_path));
    gapp->add_action( "text-flow-into-frame",        sigc::ptr_fun(text_flow_into_frame));
    gapp->add_action( "text-flow-subtract-frame",    sigc::ptr_fun(text_flow_subtract_frame));
    gapp->add_action( "text-unflow",                 sigc::ptr_fun(select_text_unflow));
    gapp->add_action( "text-unflow-and-keep-shape",  sigc::bind(sigc::ptr_fun(&select_text_unflow_and_keep_shape), app));
    gapp->add_action( "text-convert-to-regular",     sigc::ptr_fun(text_convert_to_regular));
    gapp->add_action( "text-convert-to-glyphs",      sigc::ptr_fun(text_convert_to_glyphs));
    gapp->add_action( "text-unkern",                 sigc::ptr_fun(text_unkern));
    gapp->add_action( "text-missing-fonts",          sigc::bind(sigc::ptr_fun(&text_missing_fonts), app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_text);
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
