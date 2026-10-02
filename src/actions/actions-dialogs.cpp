// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for switching tools.
 *
 * Copyright (C) 2020 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include <iostream>
#include <map>

#include <giomm.h>  // Not <gtkmm.h>! To eventually allow a headless version!
#include <glibmm/i18n.h>

#include "config.h" // #ifdef WITH_LIBSPELLING

#include "actions-dialogs.h"
#include "actions-helper.h"

#include "inkscape-application.h"
#include "inkscape-window.h"

#include "ui/dialog/dialog-container.h"
#include "ui/dialog/dialog-data.h"

// Note the "AttrDialog" is now part of the "XMLDialog" and the "Style" dialog is part of the "Selectors" dialog.
// Also note that the "AttrDialog" does not correspond to SP_VERB_DIALOG_ATTR!!!!! (That would be the "ObjectAttributes" dialog.)

const Glib::ustring SECTION = NC_("Action Section", "Dialog");

static const std::vector<std::vector<Glib::ustring>> raw_data_dialogs = {
    // clang-format off
    {"win.dialog-open('AlignDistribute')",    N_("Open Align and Distribute"), SECTION,  N_("Align and distribute objects")                                                           },
    {"win.dialog-open('CloneTiler')",         N_("Open Clone Tiler"),          SECTION,  N_("Create multiple clones of selected object, arranging them into a pattern or scattering") },
    {"win.dialog-open('DebugWindow')",        N_("Open Debugger"),             SECTION,  N_("Open debugger")                                                                      },
    {"win.dialog-open('DocumentProperties')", N_("Open Document Properties"),  SECTION,  N_("Edit properties of this document (to be saved with the document)")                       },
    {"win.dialog-open('DocumentResources')",  N_("Open Document Resources"),   SECTION,  N_("Show document overview and resources") },
    {"win.dialog-open('ExtensionsGallery')",  N_("Open Extension Gallery"),    SECTION,  N_("Show and run available extensions") },
    {"win.dialog-open('Export')",             N_("Open Export"),               SECTION,  N_("Export this document or a selection as a PNG image")                                     },
    {"win.dialog-open('FillStroke')",         N_("Open Fill and Stroke"),      SECTION,  N_("Edit objects' colors, gradients, arrowheads, and other fill and stroke properties...")   },
    {"win.dialog-open('Stroke')",             N_("Open Stroke"),               SECTION,  N_("Edit stroke width, caps, joins, dashes and markers") },
    {"win.dialog-open('FilterEffects')",      N_("Open Filter Effects"),       SECTION,  N_("Manage, edit, and apply SVG filters")                                                    },
    {"win.dialog-open('FilterGallery')",      N_("Open Filter Gallery"),       SECTION,  N_("Show and apply available filters") },
    {"win.dialog-open('Find')",               N_("Open Find"),                 SECTION,  N_("Find objects in document")                                                               },
    {"win.dialog-open('FontCollections')",    N_("Open Font Collections"),     SECTION,  N_("Manage Font Collections")                                                                },
    {"win.dialog-open('IconPreview')",        N_("Open Icon Preview"),         SECTION,  N_("Preview Icon")                                                                           },
    {"win.dialog-open('Input')",              N_("Open Input"),                SECTION,  N_("Configure extended input devices, such as a graphics tablet")                            },
    {"win.dialog-open('LivePathEffect')",     N_("Open Live Path Effect"),     SECTION,  N_("Manage, edit, and apply path effects")                                                   },
    {"win.dialog-open('ObjectProperties')",   N_("Open Object Properties"),    SECTION,  N_("Edit the object attributes (context dependent)...")                                      },
    {"win.dialog-open('Objects')",            N_("Open Objects"),              SECTION,  N_("View Objects")                                                                           },
    {"win.dialog-open('Preferences')",        N_("Open Preferences"),          SECTION,  N_("Edit global Inkscape preferences")                                                       },
    {"win.dialog-open('Selectors')",          N_("Open Selectors"),            SECTION,  N_("View and edit CSS selectors and styles")                                                 },
    {"win.dialog-open('SVGFonts')",           N_("Open SVG Fonts"),            SECTION,  N_("Edit SVG fonts")                                                                         },
    // TRANSLATORS: "Swatches" -> color samples
    {"win.dialog-open('Swatches')",           N_("Open Swatches"),             SECTION,  N_("Select colors from a swatches palette")                                                  },
    {"win.dialog-open('Symbols')",            N_("Open Symbols"),              SECTION,  N_("Select symbol from a symbols palette")                                                   },
    {"win.dialog-open('Text')",               N_("Open Text"),                 SECTION,  N_("View and select font family, font size and other text properties")                       },
    {"win.dialog-open('Trace')",              N_("Open Trace"),                SECTION,  N_("Create one or more paths from a bitmap by tracing it")                                   },
    {"win.dialog-open('Transform')",          N_("Open Transform"),            SECTION,  N_("Precisely control objects' transformations")                                             },
    {"win.dialog-open('UndoHistory')",        N_("Open Undo History"),         SECTION,  N_("Undo History")                                                                           },
    {"win.dialog-open('XMLEditor')",          N_("Open XML Editor"),           SECTION,  N_("View and edit the XML tree of the document")                                             },
    {"app.preferences",                       N_("Open Preferences"),          SECTION,  N_("Edit global Inkscape preferences")                                                       },
#if WITH_LIBSPELLING
    {"win.dialog-open('Spellcheck')",         N_("Open Spellcheck"),           SECTION,  N_("Check spelling of text in document")                                                     },
#endif
#if DEBUG
    {"win.dialog-open('Prototype')",          N_("Open Prototype"),            SECTION,  N_("Prototype Dialog")                                                                       },
#endif

    {"win.dialog-toggle",                     N_("Toggle all dialogs"),        SECTION,  N_("Show or hide all dialogs")                                                               },
    {"win.workspace-reset",                   N_("Reset Workspace"),           SECTION,  N_("Restore Properties and Stroke above Layers for this document window") },
    // clang-format on
};

/**
 * Open dialog.
 */
void
dialog_open(const Glib::VariantBase& value, InkscapeWindow *win)
{
    Glib::Variant<Glib::ustring> s = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring> >(value);
    auto dialog = s.get();

    if (!win) {
        show_output("dialog_toggle: no inkscape window!");
        return;
    }

    auto const &dialog_data = get_dialog_data();
    auto dialog_it = dialog_data.find(dialog);
    if (dialog_it == dialog_data.end()) {
        show_output(Glib::ustring("dialog_open: invalid dialog name: ") + dialog.raw());
        return;
    }

    SPDesktop* dt = win->get_desktop();
    if (!dt) {
        show_output("dialog_toggle: no desktop!");
        return;
    }

    Inkscape::UI::Dialog::DialogContainer *container = dt->getContainer();
    container->new_dialog(dialog);
}

/**
 * Toggle between showing and hiding dialogs.
 */
void
dialog_toggle(InkscapeWindow *win)
{
    SPDesktop* dt = win->get_desktop();
    if (!dt) {
        show_output("dialog_toggle: no desktop!");
        return;
    }

    // Keep track of state?
    // auto action = win->lookup_action("dialog-toggle");
    // if (!action) {
    //     show_output("dialog_toggle: action 'dialog-toggle' missing!");
    //     return;
    // }

    // auto saction = std::dynamic_pointer_cast<Gio::SimpleAction>(action);
    // if (!saction) {
    //     show_output("dialog_toogle: action 'dialog_switch' not SimpleAction!");
    //     return;
    // }

    // saction->get_state();

    Inkscape::UI::Dialog::DialogContainer *container = dt->getContainer();
    container->toggle_dialogs();
}

void add_actions_dialogs(InkscapeApplication *app)
{
    app->get_action_extra_data().add_data(raw_data_dialogs);
}

void add_actions_dialogs(InkscapeWindow *win)
{
    auto String = Glib::VARIANT_TYPE_STRING;

    // clang-format off
    win->add_action_with_parameter( "dialog-open",  String, sigc::bind(sigc::ptr_fun(&dialog_open),   win));
    win->add_action(                "dialog-toggle",        sigc::bind(sigc::ptr_fun(&dialog_toggle), win));
    win->add_action("workspace-reset", [win] {
        if (auto desktop = win->get_desktop()) desktop->getContainer()->reset_workspace();
    });
    // clang-format on

    // macOS automatically uses app.preferences in the application menu
    auto gapp = win->get_application();
    gapp->add_action("preferences", [] { dialog_open(Glib::Variant<Glib::ustring>::create("Preferences"), InkscapeApplication::instance()->get_active_window()); });

    auto app = InkscapeApplication::instance();
    if (!app) {
        show_output("add_actions_dialogs: no app!");
        return;
    }

    app->get_action_extra_data().add_data(raw_data_dialogs);
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
