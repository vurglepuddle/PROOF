// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 *
 *  Actions for opening, saving, etc. files which (mostly) open a dialog or an Inkscape window.
 *  Used by menu items under the "File" submenu.
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

#include "actions-file-window.h"
#include "actions-helper.h"

#include "inkscape-application.h"
#include "inkscape-window.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "file.h"
#include "print.h"
#include "preferences.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog/link-missing.h"
#include "ui/dialog/save-template-dialog.h"
#include "ui/dialog/new-from-template.h"
#include "ui/icon-names.h"

void
document_new(InkscapeWindow* win)
{
    sp_file_new_default();
}

void
document_dialog_templates(InkscapeWindow* win)
{
    if (win) {
        Inkscape::UI::NewFromTemplate::load_new_from_template(*win);
    }
}

void
document_open(InkscapeWindow* win)
{
    // Open File Dialog
    auto files = Inkscape::choose_file_open_images(_("Select file(s) to open"), win, "/dialog/open/path", _("Open"));
    auto *app = InkscapeApplication::instance();
    for (auto& file : files) {
        app->create_window(file);
    }
}

void
document_revert(InkscapeWindow* win)
{
    sp_file_revert_dialog();
}

void
document_save(InkscapeWindow* win)
{
    // Save File
    sp_file_save(*win, nullptr, nullptr);
}

void
document_save_as(InkscapeWindow* win)
{
    // Save File As
    sp_file_save_as(*win, nullptr, nullptr);
}

void
document_save_copy(InkscapeWindow* win)
{
    // Save A copy
    sp_file_save_a_copy(*win, nullptr, nullptr);
}

void
document_save_template(InkscapeWindow* win)
{
    // Save As Template
    Inkscape::UI::Dialog::SaveTemplate::save_document_as_template(*win);
}

void
document_import(InkscapeWindow* win)
{
    // Import File Dialog
    auto files = Inkscape::choose_file_open_images(_("Select file(s) to import"), win, "/dialog/import/path", _("Import"));
    auto document = win->get_document(); // Target document.
    for (auto file : files) {
        file_import(document, file->get_path(), nullptr);
    }
}

void
document_print(InkscapeWindow* win)
{
    // Print File
    if (auto doc = win->get_document()) {
        sp_print_document(*win, doc);
    }
}

// PROOF: the linked pictures whose files can't be found, and a file to link each to.
void
document_missing_links(InkscapeWindow* win)
{
    Inkscape::UI::Dialog::showMissingLinks(win->get_desktop());
}

void
document_cleanup(InkscapeWindow* win)
{
    // Cleanup Up Document
    auto doc = win->get_document();
    unsigned int diff = doc->vacuumDocument();

    Inkscape::DocumentUndo::done(doc, RC_("Undo", "Clean up document"), INKSCAPE_ICON("document-cleanup"));

    // Show status messages when in GUI mode
    if (diff > 0) {
        win->get_desktop()->messageStack()->flashF(Inkscape::NORMAL_MESSAGE,
                ngettext("Removed <b>%i</b> unused definition in &lt;defs&gt;.",
                        "Removed <b>%i</b> unused definitions in &lt;defs&gt;.",
                        diff),
                diff);
    } else {
        win->get_desktop()->messageStack()->flash(Inkscape::NORMAL_MESSAGE,  _("No unused definitions in &lt;defs&gt;."));
    }
}

// Close tab, checking for data loss. If it's the last tab, keep open with new document.
void document_close(InkscapeWindow *win)
{
    auto app = InkscapeApplication::instance();
    app->destroyDesktop(win->get_desktop(), true); // true == keep alive with new new document
}

const Glib::ustring SECTION = NC_("Action Section", "Window-File");

std::vector<std::vector<Glib::ustring>> raw_data_dialog_window =
{
    // clang-format off
    {"win.document-new",              N_("New"),               SECTION,   N_("Create new document from the default template")},
    {"win.document-dialog-templates", N_("New from Template"), SECTION,   N_("Create new project from template")},
    {"win.document-open",             N_("Open File Dialog"),  SECTION,   N_("Open an existing document")},
    {"win.document-revert",           N_("Revert"),            SECTION,   N_("Revert to the last saved version of document (changes will be lost)")},
    {"win.document-save",             N_("Save"),              SECTION,   N_("Save document")},
    {"win.document-save-as",          N_("Save As"),           SECTION,   N_("Save document under a new name")},
    {"win.document-save-copy",        N_("Save a Copy"),       SECTION,   N_("Save a copy of the document under a new name")},
    {"win.document-save-template",    N_("Save Template"),     SECTION,   N_("Save a copy of the document as template")},
    {"win.document-import",           N_("Import"),            SECTION,   N_("Import a bitmap or SVG image into this document")},
    {"win.document-print",            N_("Print"),             SECTION,   N_("Print document")},
    {"win.document-cleanup",          N_("Clean Up Document"), SECTION,   N_("Remove unused definitions (such as gradients or clipping paths) from the document")},
    {"win.document-missing-links",    N_("Missing Links"),     SECTION,   N_("List the linked pictures whose files can't be found, and link them again")},
    {"win.document-close",            N_("Close"),             SECTION,   N_("Close document (unless last document)")},
    // clang-format on
};

void
add_actions_file_window(InkscapeWindow* win)
{
    // clang-format off
    win->add_action( "document-new",                sigc::bind(sigc::ptr_fun(&document_new),               win));
    win->add_action( "document-dialog-templates",   sigc::bind(sigc::ptr_fun(&document_dialog_templates),  win));
    win->add_action( "document-open",               sigc::bind(sigc::ptr_fun(&document_open),              win));
    win->add_action( "document-revert",             sigc::bind(sigc::ptr_fun(&document_revert),            win));
    win->add_action( "document-save",               sigc::bind(sigc::ptr_fun(&document_save),              win));
    win->add_action( "document-save-as",            sigc::bind(sigc::ptr_fun(&document_save_as),           win));
    win->add_action( "document-save-copy",          sigc::bind(sigc::ptr_fun(&document_save_copy),         win));
    win->add_action( "document-save-template",      sigc::bind(sigc::ptr_fun(&document_save_template),     win));
    win->add_action( "document-import",             sigc::bind(sigc::ptr_fun(&document_import),            win));
    win->add_action( "document-print",              sigc::bind(sigc::ptr_fun(&document_print),             win));
    win->add_action( "document-cleanup",            sigc::bind(sigc::ptr_fun(&document_cleanup),           win));
    win->add_action( "document-missing-links",      sigc::bind(sigc::ptr_fun(&document_missing_links),     win));
    win->add_action( "document-close",              sigc::bind(sigc::ptr_fun(&document_close),             win));
    // clang-format on

    auto app = InkscapeApplication::instance();
    if (!app) {
        show_output("add_actions_file_window: no app!");
        return;
    }
    app->get_action_extra_data().add_data(raw_data_dialog_window);
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
