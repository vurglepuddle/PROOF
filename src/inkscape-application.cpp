// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The main Inkscape application.
 *
 * Copyright (C) 2018 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 */

/* Application flow:
 *   main() -> InkscapeApplication::singleton().gio_app()->run(argc, argv);
 *
 *     InkscapeApplication::InkscapeApplication
 *       Initialized: GC, Debug, Gettext, Autosave, Actions, Commandline
 *     InkscapeApplication::on_handle_local_options
 *       InkscapeApplication::parse_actions
 *
 *     -- Switch to main instance if new Inkscape instance is merged with existing instance. --
 *        New instances are merged with existing instance unless app_id is changed, see below.
 *
 *     InkscapeApplication::on_startup                   | Only called for main instance
 *
 *     InkscapeApplication::on_activate (no file specified) OR InkscapeApplication::on_open (file specified)
 *       InkscapeApplication::process_document           | Will use command-line actions from main instance!
 *
 *       InkscapeApplication::create_window (document)
 *         Inkscape::Shortcuts
 *
 *     InkscapeApplication::create_window (file)         |
 *       InkscapeApplication::create_window (document)   | Open/Close document
 *     InkscapeApplication::destroy_window               |
 *
 *     InkscapeApplication::on_quit
 *       InkscapeApplication::destroy_all
 *       InkscapeApplication::destroy_window
 */

#include "inkscape-application.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <cerrno>  // History file
#include <regex>
#include <numeric>
#include <unistd.h>
#include <chrono>
#include <thread>

#include <giomm/file.h>
#include <glibmm/i18n.h>  // Internationalization
#include <gtkmm/application.h>

#include "inkscape-version-info.h"
#include "inkscape-window.h"
#include "auto-save.h"              // Auto-save
#include "desktop.h"                // Access to window
#include "document.h"
#include "document-update.h"
#include "file.h"                   // sp_file_convert_dpi
#include "inkscape.h"               // Inkscape::Application
#include "object/sp-namedview.h"
#include "selection.h"
#include "path-prefix.h"            // Data directory

#include "actions/actions-base.h"
#include "actions/actions-dialogs.h"
#include "actions/actions-edit.h"
#include "actions/actions-effect.h"
#include "actions/actions-element-a.h"
#include "actions/actions-element-image.h"
#include "actions/actions-file.h"
#include "actions/actions-helper.h"
#include "actions/actions-helper-gui.h"
#include "actions/actions-hide-lock.h"
#include "actions/actions-object-align.h"
#include "actions/actions-object.h"
#include "actions/actions-output.h"
#include "actions/actions-paths.h"
#include "actions/actions-selection-object.h"
#include "actions/actions-selection.h"
#include "actions/actions-text.h"
#include "actions/actions-transform.h"
#include "actions/actions-tutorial.h"
#include "actions/actions-window.h"
#include "debug/logger.h"           // INKSCAPE_DEBUG_LOG support
#include "extension/db.h"
#include "extension/effect.h"
#include "extension/init.h"
#include "extension/input.h"
#include "helper/gettext.h"   // gettext init
#include "inkgc/gc-core.h"          // Garbage Collecting init
#include "io/file.h"                // File open (command line).
#include "io/fix-broken-links.h"    // Fix up references.
#include "io/recent-files.h"
#include "io/resource.h"            // TEMPLATE
#include "object/sp-root.h"         // Inkscape version.
#include "ui/desktop/document-check.h"    // Check for data loss on closing document window.
#include "ui/dialog/dialog-manager.h"     // Save state
#include "ui/dialog/dialog-window.h"
#include "ui/dialog/font-substitution.h"  // Warn user about font substitution.
#include "ui/dialog/startup.h"
#include "ui/error-reporter.h"
#include "ui/interface.h"                 // sp_ui_error_dialog
#include "ui/tools/shortcuts.h"
#include "ui/widget/desktop-widget.h"
#include "util/scope_exit.h"

#ifdef WITH_GNU_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

using Inkscape::IO::Resource::UIS;

using EffectDict = std::map<Glib::ustring, Glib::ustring>;

// This is a bit confusing as there are two ways to handle command line arguments and files
// depending on if the Gio::Application::Flags::HANDLES_OPEN and/or Gio::Application::Flags::HANDLES_COMMAND_LINE
// flags are set. If the open flag is set and the command line not, the all the remainng arguments
// after calling on_handle_local_options() are assumed to be filenames.

// Add document to app.
SPDocument *InkscapeApplication::document_add(std::unique_ptr<SPDocument> document)
{
    assert(document);
    auto [it, inserted] = _documents.try_emplace(std::move(document));
    assert(inserted);
    INKSCAPE.add_document(it->first.get());
    return it->first.get();
}

// New document, add it to app. TODO: This should really be open_document with option to strip template data.
SPDocument *InkscapeApplication::document_new(std::string const &template_filename)
{
    if (template_filename.empty()) {
        auto def = Inkscape::IO::Resource::get_filename(Inkscape::IO::Resource::TEMPLATES, "default.svg", true);
        if (!def.empty()) {
            return document_new(def);
        }
    }

    // Open file
    auto doc_uniq = ink_file_new(template_filename);
    if (!doc_uniq) {
        std::cerr << "InkscapeApplication::new_document: failed to open new document!" << std::endl;
        return nullptr;
    }

    auto doc = document_add(std::move(doc_uniq));

    // Set viewBox if it doesn't exist.
    if (!doc->getRoot()->viewBox_set) {
        doc->setViewBox();
    }

    return doc;
}

// Open a document, add it to app.
std::pair<SPDocument *, bool> InkscapeApplication::document_open(Glib::RefPtr<Gio::File> const &file)
{
    // Open file
    auto [document, cancelled] = ink_file_open(file);
    if (cancelled) {
        return {nullptr, true};
    }
    if (!document) {
        std::cerr << "InkscapeApplication::document_open: Failed to open: " << file->get_parse_name().raw() << std::endl;
        return {nullptr, false};
    }

    document->setVirgin(false); // Prevents replacing document in same window during file open.

    // Add/promote recent file; when we call add_item and file is on a recent list already,
    // then apparently only "modified" time changes.
    auto path = file->get_path();
    // Opening crash files or auto-save files, we can link them back using the
    // recent files manager to get the original context for the file.
    if (auto original = Inkscape::IO::openAsInkscapeRecentOriginalFile(path)) {
        document->setModifiedSinceSave(true);
        document->setModifiedSinceAutoSaveFalse(); // don't re-auto-save an unmodified auto-save
        document->setDocumentFilename(original->empty() ? nullptr : original->c_str());
    } else {
        auto name = document->getDocumentName();
        Inkscape::IO::addInkscapeRecentSvg(path, name ? name : "");
    }

    return {document_add(std::move(document)), false};
}

// Open a document, add it to app.
SPDocument *InkscapeApplication::document_open(std::span<char const> buffer)
{
    // Open file
    auto document = ink_file_open(buffer);

    if (!document) {
        std::cerr << "InkscapeApplication::document_open: Failed to open memory document." << std::endl;
        return nullptr;
    }

    document->setVirgin(false); // Prevents replacing document in same window during file open.

    return document_add(std::move(document));
}

/**
 * Swap out one document for another in a tab.
 * Does not delete old document!
 * Fixme: Lots of callers leak old document.
 */
bool InkscapeApplication::document_swap(SPDesktop *desktop, SPDocument *document)
{
    if (!document || !desktop) {
        std::cerr << "InkscapeAppliation::swap_document: Missing desktop or document!" << std::endl;
        return false;
    }

    auto old_document = desktop->getDocument();
    desktop->change_document(document);

    // We need to move window from the old document to the new document.

    // Find old document
    auto doc_it = _documents.find(old_document);
    if (doc_it == _documents.end()) {
        std::cerr << "InkscapeApplication::swap_document: Old document not in map!" << std::endl;
        return false;
    }

    // Remove desktop from document map.
    auto dt_it = std::find_if(doc_it->second.begin(), doc_it->second.end(), [=] (auto &dt) { return dt.get() == desktop; });
    if (dt_it == doc_it->second.end()) {
        std::cerr << "InkscapeApplication::swap_document: Desktop not found!" << std::endl;
        return false;
    }

    auto dt_uniq = std::move(*dt_it);
    doc_it->second.erase(dt_it);

    // Find new document
    doc_it = _documents.find(document);
    if (doc_it == _documents.end()) {
        std::cerr << "InkscapeApplication::swap_document: New document not in map!" << std::endl;
        return false;
    }

    doc_it->second.push_back(std::move(dt_uniq));

    _active_document = document;
    return true;
}

/** Revert document: open saved document and swap it for each window.
 */
bool InkscapeApplication::document_revert(SPDocument *document)
{
    // Find saved document.
    char const *path = document->getDocumentFilename();
    if (!path) {
        std::cerr << "InkscapeApplication::revert_document: Document never saved, cannot revert." << std::endl;
        return false;
    }

    // Open saved document.
    auto file = Gio::File::create_for_path(document->getDocumentFilename());
    auto [new_document, cancelled] = document_open(file);
    if (!new_document) {
        if (!cancelled) {
            std::cerr << "InkscapeApplication::revert_document: Cannot open saved document!" << std::endl;
        }
        return false;
    }

    // Allow overwriting current document.
    document->setVirgin(true);

    auto it = _documents.find(document);
    if (it == _documents.end()) {
        std::cerr << "InkscapeApplication::revert_document: Document not found!" << std::endl;
        return false;
    }

    // Acquire list of desktops attached to old document. (They are about to get moved around.)
    std::vector<SPDesktop *> desktops;
    for (auto const &desktop : it->second) {
        desktops.push_back(desktop.get());
    }

    // Swap reverted document in all windows.
    for (auto const desktop : desktops) {
        // Remember current zoom and view.
        double zoom = desktop->current_zoom();
        Geom::Point c = desktop->current_center();

        bool reverted = document_swap(desktop, new_document);

        if (reverted) {
            desktop->zoom_absolute(c, zoom, false);
            // Update LPE and fix legacy LPE system.
            sp_file_fix_lpe(desktop->getDocument());
        } else {
            std::cerr << "InkscapeApplication::revert_document: Revert failed!" << std::endl;
        }
    }

    document_close(document);

    return true;
}

/**
 * Close a document, remove from app. No checking is done on modified status, etc.
 */
void InkscapeApplication::document_close(SPDocument *document)
{
    if (!document) {
        std::cerr << "InkscapeApplication::close_document: No document!" << std::endl;
        return;
    }

    auto it = _documents.find(document);
    if (it == _documents.end()) {
        std::cerr << "InkscapeApplication::close_document: Document not registered with application." << std::endl;
        return;
    }

    if (!it->second.empty()) {
        std::cerr << "InkscapeApplication::close_document: Window vector not empty!" << std::endl;
    }

    INKSCAPE.remove_document(it->first.get());
    _documents.erase(it);
}

/** Fix up a document if necessary (Only fixes that require GUI). MOVE TO ANOTHER FILE!
 */
void InkscapeApplication::document_fix(SPDesktop *desktop)
{
    // Most fixes are handled when document is opened in SPDocument::createDoc().
    // But some require the GUI to be present. These are handled here.

    if (_with_gui) {

        auto document = desktop->getDocument();

        // Perform a fixup pass for hrefs.
        if (Inkscape::fixBrokenLinks(document)) {
            desktop->showInfoDialog(_("Broken links have been changed to point to existing files."));
        }

        // Fix dpi (pre-92 files).
        if (document->getRoot()->inkscape_version.isInsideRangeExclusive({0, 1}, {0, 92})) {
            sp_file_convert_dpi(document);
        }

        // Update LPE and fix legacy LPE system.
        sp_file_fix_lpe(document);

        // Check for font substitutions, requires text to have been rendered.
        Inkscape::UI::Dialog::checkFontSubstitutions(document);
    }
}

/**
 * Get a list of open documents (from document map).
 */
std::vector<SPDocument *> InkscapeApplication::get_documents()
{
    std::vector<SPDocument *> result;

    for (auto const &[doc, _] : _documents) {
        result.push_back(doc.get());
    }

    return result;
}

// Take an already open document and create a new window, adding window to document map.
SPDesktop *InkscapeApplication::desktopOpen(SPDocument *document, bool new_window)
{
    assert(document);
    // Once we've removed Inkscape::Application (separating GUI from non-GUI stuff)
    // it will be more easy to start up the GUI after-the-fact. Until then, prevent
    // opening a window if GUI not selected at start-up time.
    if (!_with_gui) {
        std::cerr << "InkscapeApplication::window_open: Not in gui mode!" << std::endl;
        return nullptr;
    }

    auto const doc_it = _documents.find(document);
    if (doc_it == _documents.end()) {
        std::cerr << "InkscapeApplication::window_open: Document not in map!" << std::endl;
        return nullptr;
    }

    auto const desktop = doc_it->second.emplace_back(std::make_unique<SPDesktop>(document->getNamedView())).get();
    INKSCAPE.add_desktop(desktop);

    if (_active_window && !new_window) { // Divert all opened documents to new tabs unless asked not to.
        _active_window->get_desktop_widget()->addDesktop(desktop);
    } else {
        auto const win = _windows.emplace_back(std::make_unique<InkscapeWindow>(desktop)).get();

        _active_window = win;
        assert(_active_desktop   == desktop);
        assert(_active_selection == desktop->getSelection());
        assert(_active_document  == document);

        win->present();
    }

    document_fix(desktop); // May need flag to prevent this from being called more than once.

    return desktop;
}

// Close a window. Does not delete document.
void InkscapeApplication::desktopClose(SPDesktop *desktop)
{
    if (!desktop) {
        std::cerr << "InkscapeApplication::close_window: No desktop!" << std::endl;
        return;
    }

    auto document = desktop->getDocument();
    assert(document);

    // Leave active document alone (maybe should find new active window and reset variables).
    _active_selection = nullptr;
    _active_desktop   = nullptr;

    // Remove desktop from document map.
    auto doc_it = _documents.find(document);
    if (doc_it == _documents.end()) {
        std::cerr << "InkscapeApplication::close_window: document not in map!" << std::endl;
        return;
    }

    auto dt_it = std::find_if(doc_it->second.begin(), doc_it->second.end(), [=] (auto &dt) { return dt.get() == desktop; });
    if (dt_it == doc_it->second.end()) {
        std::cerr << "InkscapeApplication::close_window: desktop not found!" << std::endl;
        return;
    }

    if (get_number_of_windows() == 1) {
        // Persist layout of docked and floating dialogs before deleting the last window.
        Inkscape::UI::Dialog::DialogManager::singleton().save_dialogs_state(desktop->getDesktopWidget()->getDialogContainer());
    }

    auto win = desktop->getInkscapeWindow();

    win->get_desktop_widget()->removeDesktop(desktop);

    INKSCAPE.remove_desktop(desktop); // clears selection and event_context
    doc_it->second.erase(dt_it); // Results in call to SPDesktop::destroy()
}

// Closes active window (useful for scripting).
void InkscapeApplication::desktopCloseActive()
{
    if (!_active_desktop) {
        std::cerr << "InkscapeApplication::window_close_active: no active window!" << std::endl;
        return;
    }
    desktopClose(_active_desktop);
}

/// Debug function
void InkscapeApplication::dump()
{
    std::cout << "InkscapeApplication::dump()" << std::endl;
    std::cout << "  Documents: " << _documents.size() << std::endl;
    for (auto const &[doc, desktops] : _documents) {
        std::cout << "    Document: " << (doc->getDocumentName() ? doc->getDocumentName() : "unnamed") << std::endl;
        for (auto const &dt : desktops) {
            std::cout << "      Desktop: " << dt.get() << std::endl;
        }
    }
    std::cout << "  Windows: " << _windows.size() << std::endl;
    for (auto const &win : _windows) {
        std::cout << "    Window: " << win->get_title() << std::endl;
        for (auto dt : win->get_desktop_widget()->get_desktops()) {
            std::cout << "      Desktop: " << dt << std::endl;
        }
    }
}

static InkscapeApplication *_instance = nullptr;

InkscapeApplication *InkscapeApplication::instance()
{
    return _instance;
}

void InkscapeApplication::_start_main_option_section(Glib::ustring const &section_name)
{
#ifndef _WIN32
    // Avoid outputting control characters to non-tty destinations.
    //
    // However, isatty() is not useful on Windows
    //   - it doesn't recognize mintty and similar terminals
    //   - it doesn't work in cmd.exe either, where we have to use the inkscape.com wrapper, connecting stdout to a pipe
    if (!isatty(fileno(stdout))) {
        return;
    }
#endif

    auto *gapp = gio_app();

    if (section_name.empty()) {
        gapp->add_main_option_entry(Gio::Application::OptionType::BOOL, Glib::ustring("\b\b  "), '\0', " ");
    } else {
        gapp->add_main_option_entry(Gio::Application::OptionType::BOOL, Glib::ustring("\b\b  \n") + section_name + ":", '\0', " ");
    }
}

InkscapeApplication::InkscapeApplication()
{
    if (_instance) {
        std::cerr << "Multiple instances of InkscapeApplication" << std::endl;
        std::terminate();
    }
    _instance = this;

    using T = Gio::Application;

    auto app_id = Glib::ustring("org.inkscape.Inkscape");
    auto flags = Gio::Application::Flags::HANDLES_OPEN | // Use default file opening.
                 Gio::Application::Flags::CAN_OVERRIDE_APP_ID;
    auto non_unique = false;

    // Allow an independent instance of Inkscape to run. Will have matching DBus name and paths
    // (e.g org.inkscape.Inkscape.tag, /org/inkscape/Inkscape/tag/window/1).
    // If this flag isn't set, any new instance of Inkscape will be merged with the already running
    // instance of Inkscape before on_open() or on_activate() is called.
    if (auto tag = Glib::getenv("INKSCAPE_APP_ID_TAG"); tag != "") {
        app_id += "." + tag;
        if (!Gio::Application::id_is_valid(app_id)) {
            std::cerr << "InkscapeApplication: invalid application id: " << app_id.raw() << std::endl;
            std::cerr << "  tag must be ASCII and not start with a number." << std::endl;
        }
        non_unique = true;
    } else if (Glib::getenv("SELF_CALL") == "") {
        // Version protection attempts to refuse to merge with inkscape version
        // that have a different build/revision hash. This is important for testing.
        auto test_app = Gio::Application::create(app_id, flags);
        test_app->register_application();
        if (test_app->get_default()->is_remote()) {
            bool enabled;
            Glib::VariantBase hint;
            if (!test_app->query_action(Inkscape::inkscape_revision(), enabled, hint)) {
                app_id += "." + Inkscape::inkscape_revision();
                non_unique = true;
            }
        }
        Gio::Application::unset_default();

        // Silence wrong warning when test_app is destroyed - https://gitlab.gnome.org/GNOME/glib/-/issues/1857.
        // Previous workaround test_app->run(0, nullptr) was not acceptable as it fires spurious activate signals.
        // Fixme: The warning must be removed upstream, or unregister_application() added so we can call it here.
        g_log_set_default_handler([] (auto...) {}, nullptr);
        test_app.reset();
        g_log_set_default_handler(g_log_default_handler, nullptr);
    }

    if (gtk_init_check()) {
        g_set_prgname(app_id.c_str());
        _gio_application = Gtk::Application::create(app_id, flags);
    } else {
        _gio_application = Gio::Application::create(app_id, flags);
        _with_gui = false;
    }

    // Garbage Collector
    Inkscape::GC::init();

    auto *gapp = gio_app();

    // Native Language Support
    Inkscape::initialize_gettext();

    gapp->signal_startup().connect([this]() { this->on_startup(); });
    gapp->signal_activate().connect([this]() { this->on_activate(); });
    gapp->signal_open().connect(sigc::mem_fun(*this, &InkscapeApplication::on_open));

    // ==================== Initializations =====================
#ifndef NDEBUG
    // Use environment variable INKSCAPE_DEBUG_LOG=log.txt for event logging
    Inkscape::Debug::Logger::init();
#endif

    // Don't set application name for now. We don't use it anywhere but
    // it overrides the name used for adding recently opened files and breaks the Gtk::RecentFilter
    // Glib::set_application_name(N_("Inkscape - A Vector Drawing Program"));  // After gettext() init.

    // ======================== Actions =========================
    add_actions_base(this);                 // actions that are GUI independent
    add_actions_edit(this);                 // actions for editing
    add_actions_effect(this);               // actions for Filters and Extensions
    add_actions_element_a(this);            // actions for the SVG a (anchor) element
    add_actions_element_image(this);        // actions for the SVG image element
    add_actions_file(this);                 // actions for file handling
    add_actions_hide_lock(this);            // actions for hiding/locking items.
    add_actions_object(this);               // actions for object manipulation
    add_actions_object_align(this);         // actions for object alignment
    add_actions_output(this);               // actions for file export
    add_actions_selection(this);            // actions for object selection
    add_actions_path(this);                 // actions for Paths
    add_actions_selection_object(this);     // actions for selected objects
    add_actions_text(this);                 // actions for Text
    add_actions_tutorial(this);             // actions for opening tutorials (with GUI only)
    add_actions_transform(this);            // actions for transforming selected objects
    add_actions_window(this);               // actions for windows

    // ====================== Command Line ======================

    // Will automatically handle character conversions.
    // Note: OptionType::FILENAME => std::string, OptionType::STRING => Glib::ustring.

    // Additional informational strings for --help output
    // TODO: Claims to be translated automatically, but seems broken, so pass already translated strings
    gapp->set_option_context_parameter_string(_("file1 [file2 [fileN]]"));
    gapp->set_option_context_summary(_("Process (or open) one or more files."));
    gapp->set_option_context_description(Glib::ustring("\n") + _("Examples:") + '\n'
            + "  " + Glib::ustring::compose(_("Export input SVG (%1) to PDF (%2) format:"), "in.svg", "out.pdf") + '\n'
            + '\t' + "inkscape --export-filename=out.pdf in.svg\n"
            + "  " + Glib::ustring::compose(_("Export input files (%1) to PNG format keeping original name (%2):"), "in1.svg, in2.svg", "in1.png, in2.png") + '\n'
            + '\t' + "inkscape --export-type=png in1.svg in2.svg\n"
            + "  " + Glib::ustring::compose(_("See %1 and %2 for more details."), "'man inkscape'", "http://wiki.inkscape.org/wiki/index.php/Using_the_Command_Line"));

    // clang-format off
    // General
    gapp->add_main_option_entry(T::OptionType::BOOL,     "version",                 'V', N_("Print Inkscape version"),                                                  "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "debug-info",             '\0', N_("Print debugging information"),                                                        "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "system-data-directory",  '\0', N_("Print system data directory"),                                             "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "user-data-directory",    '\0', N_("Print user data directory"),                                               "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "list-input-types",       '\0', N_("List all available input file extensions"),                                               "");
    gapp->add_main_option_entry(T::OptionType::STRING,   "app-id-tag",             '\0', N_("Create a unique instance of Inkscape with the application ID 'org.inkscape.Inkscape.TAG'"), "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "no-extensions",          '\0', N_("Don't load any extensions"), "");

    // Open/Import
    _start_main_option_section(_("File import"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "pipe",                    'p', N_("Read input file from standard input (stdin)"),                             "");
    gapp->add_main_option_entry(T::OptionType::STRING,   "pages",                   'n', N_("Page numbers to import from multi-page document, i.e. PDF"), N_("PAGE[,PAGE]"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "pdf-poppler",            '\0', N_("Use poppler when importing via commandline"),                              "");
    gapp->add_main_option_entry(T::OptionType::STRING,   "pdf-font-strategy",      '\0', N_("How fonts are parsed in the internal PDF importer [draw-missing|draw-all|delete-missing|delete-all|substitute|keep]"), N_("STRATEGY")); // xSP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "pdf-convert-colors",     '\0', N_("Convert all colors to sRGB on import"), "");
    gapp->add_main_option_entry(T::OptionType::STRING,    "pdf-group-by",          '\0', N_("How SVG groups are created from the PDF [xobject|layer]"), "");
    gapp->add_main_option_entry(T::OptionType::STRING,   "convert-dpi-method",     '\0', N_("Method used to convert pre-0.92 document dpi, if needed: [none|scale-viewbox|scale-document]"), N_("METHOD"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "no-convert-text-baseline-spacing", '\0', N_("Do not fix pre-0.92 document's text baseline spacing on opening"), "");

    // Export - File and File Type
    _start_main_option_section(_("File export"));
    gapp->add_main_option_entry(T::OptionType::FILENAME, "export-filename",        'o', N_("Output file name (defaults to input filename; file type is guessed from extension if present; use '-' to write to stdout)"), N_("FILENAME"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-overwrite",      '\0', N_("Overwrite input file (otherwise add '_out' suffix if type doesn't change)"), "");
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-type",           '\0', N_("File type(s) to export: [svg,png,ps,eps,pdf,ai,emf,wmf,xaml]"), N_("TYPE[,TYPE]*"));
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-extension",      '\0', N_("Extension ID to use for exporting"),                         N_("EXTENSION-ID"));

    // Export - Geometry
    _start_main_option_section(_("Export geometry"));                                                                                                                        // B = PNG, S = SVG, P = PS/EPS/PDF
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-area-page",       'C', N_("Area to export is page"),                                                   ""); // BSP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-area-drawing",    'D', N_("Area to export is whole drawing (ignoring page size)"),                     ""); // BSP
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-area",            'a', N_("Area to export in SVG user units"),                          N_("x0:y0:x1:y1")); // BSP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-area-snap",      '\0', N_("Snap the bitmap export area outwards to the nearest integer values"),       ""); // Bxx
    gapp->add_main_option_entry(T::OptionType::DOUBLE,   "export-dpi",             'd', N_("Resolution for bitmaps and rasterized filters; default is 96"),      N_("DPI")); // BxP
    gapp->add_main_option_entry(T::OptionType::INT,      "export-width",           'w', N_("Bitmap width in pixels (overrides --export-dpi)"),                 N_("WIDTH")); // Bxx
    gapp->add_main_option_entry(T::OptionType::INT,      "export-height",          'h', N_("Bitmap height in pixels (overrides --export-dpi)"),               N_("HEIGHT")); // Bxx
    gapp->add_main_option_entry(T::OptionType::INT,      "export-margin",         '\0', N_("Margin around export area: units of page size for SVG, mm for PS/PDF"), N_("MARGIN")); // xSP

    // Export - Options
    _start_main_option_section(_("Export options"));
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-page",           '\0', N_("Page number to export"), N_("all|n[,a-b]"));
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-id",              'i', N_("ID(s) of object(s) to export"),                   N_("OBJECT-ID[;OBJECT-ID]*")); // BSP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-id-only",         'j', N_("Hide all objects except object with ID selected by export-id"),             ""); // BSx
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-plain-svg",       'l', N_("Remove Inkscape-specific SVG attributes/properties"),                       ""); // xSx
    gapp->add_main_option_entry(T::OptionType::INT,      "export-ps-level",       '\0', N_("Postscript level (2 or 3); default is 3"),                         N_("LEVEL")); // xxP
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-pdf-version",    '\0', N_("PDF version (1.4 or 1.5); default is 1.5"),                      N_("VERSION")); // xxP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-text-to-path",    'T', N_("Convert text to paths (PS/EPS/PDF/SVG)"),                                   ""); // xxP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-latex",          '\0', N_("Export text separately to LaTeX file (PS/EPS/PDF)"),                        ""); // xxP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-ignore-filters", '\0', N_("Render objects without filters instead of rasterizing (PS/EPS/PDF)"),       ""); // xxP
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-use-hints",       't', N_("Use stored filename and DPI hints when exporting object selected by --export-id"), ""); // Bxx
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-background",      'b', N_("Background color for exported bitmaps (any SVG color string)"),         N_("COLOR")); // Bxx
    // FIXME: Opacity should really be a DOUBLE, but an upstream bug means 0.0 is detected as NULL
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-background-opacity", 'y', N_("Background opacity for exported bitmaps (0.0 to 1.0, or 1 to 255)"), N_("VALUE")); // Bxx
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-png-color-mode", '\0', N_("Color mode (bit depth and color type) for exported bitmaps (Gray_1/Gray_2/Gray_4/Gray_8/Gray_16/RGB_8/RGB_16/GrayAlpha_8/GrayAlpha_16/RGBA_8/RGBA_16)"), N_("COLOR-MODE")); // Bxx
    gapp->add_main_option_entry(T::OptionType::STRING,      "export-png-use-dithering", '\0', N_("Force dithering or disables it"), "false|true"); // Bxx
    // FIXME: Compression should really be an INT, but an upstream bug means 0 is detected as NULL
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-png-compression", '\0', N_("Compression level for PNG export (0 to 9); default is 6"), N_("LEVEL"));
    // FIXME: Antialias should really be an INT, but an upstream bug means 0 is detected as NULL
    gapp->add_main_option_entry(T::OptionType::STRING,   "export-png-antialias",   '\0', N_("Antialias level for PNG export (0 to 3); default is 2"),   N_("LEVEL"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "export-make-paths",      '\0', N_("Attempt to make the export directory if it doesn't exist."), ""); // Bxx

    // Query - Geometry
    _start_main_option_section(_("Query object/document geometry"));
    gapp->add_main_option_entry(T::OptionType::STRING,   "query-id",               'I', N_("ID(s) of object(s) to be queried"),              N_("OBJECT-ID[,OBJECT-ID]*"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "query-all",              'S', N_("Print bounding boxes of all objects"),                                     "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "query-x",                'X', N_("X coordinate of drawing or object (if specified by --query-id)"),          "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "query-y",                'Y', N_("Y coordinate of drawing or object (if specified by --query-id)"),          "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "query-width",            'W', N_("Width of drawing or object (if specified by --query-id)"),                 "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "query-height",           'H', N_("Height of drawing or object (if specified by --query-id)"),                "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "query-pages",            '\0', N_("Number of pages in the opened file."),                                    "");

    // Processing
    _start_main_option_section(_("Advanced file processing"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "vacuum-defs",           '\0', N_("Remove unused definitions from the <defs> section(s) of document"),        "");
    gapp->add_main_option_entry(T::OptionType::STRING,   "select",                '\0', N_("Select objects: comma-separated list of IDs"),   N_("OBJECT-ID[,OBJECT-ID]*"));

    // Actions
    _start_main_option_section();
    gapp->add_main_option_entry(T::OptionType::STRING,   "actions",                'a', N_("List of actions (with optional arguments) to execute"),     N_("ACTION(:ARG)[;ACTION(:ARG)]*"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "action-list",           '\0', N_("List all available actions"),                                               "");
    gapp->add_main_option_entry(T::OptionType::FILENAME, "actions-file",          '\0', N_("Use a file to input actions list"),                             N_("FILENAME"));

    // Interface
    _start_main_option_section(_("Interface"));
    gapp->add_main_option_entry(T::OptionType::BOOL,     "with-gui",               'g', N_("With graphical user interface (required by some actions)"),                 "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "batch-process",         '\0', N_("Close GUI after executing all actions"),                                    "");
    _start_main_option_section();
    gapp->add_main_option_entry(T::OptionType::BOOL,     "shell",                 '\0', N_("Start Inkscape in interactive shell mode"),                                 "");
    gapp->add_main_option_entry(T::OptionType::BOOL,     "active-window",          'q', N_("Use active window from commandline"),                                       "");
    // clang-format on

    gapp->signal_handle_local_options().connect(sigc::mem_fun(*this, &InkscapeApplication::on_handle_local_options), true);

    if (_with_gui && !non_unique) { // Will fail to register if not unique.
        // On macOS, this enables:
        //   - DnD via dock icon
        //   - system menu "Quit"
        gtk_app()->property_register_session() = true;
    }
}

InkscapeApplication::~InkscapeApplication()
{
    _instance = nullptr;
}

/**
 * Create a desktop given a document. This is used internally in InkscapeApplication.
 */
SPDesktop *InkscapeApplication::createDesktop(SPDocument *document, bool replace, bool new_window)
{
    if (!gtk_app()) {
        g_assert_not_reached();
        return nullptr;
    }

    auto old_document = _active_document;
    auto desktop = _active_desktop;

    if (replace && old_document && desktop) {
        document_swap(desktop, document);

        // Delete old document if no longer attached to any window.
        auto it = _documents.find(old_document);
        if (it != _documents.end()) {
            if (it->second.empty()) {
                document_close(old_document);
            }
        }
    } else {
        desktop = desktopOpen(document, new_window);
    }

    desktop->getNamedView()->set_clip_to_page(desktop, desktop->getNamedView()->clip_to_page);
    return desktop;
}

/** Create a window given a Gio::File. This is what most external functions should call.
  *
  * @param file - The filename to open as a Gio::File object
*/
void InkscapeApplication::create_window(Glib::RefPtr<Gio::File> const &file)
{
    if (!gtk_app()) {
        g_assert_not_reached();
        return;
    }

    SPDocument* document = nullptr;
    SPDesktop *desktop = nullptr;
    bool cancelled = false;

    if (file) {
        std::tie(document, cancelled) = document_open(file);
        if (document) {
            auto old_document = _active_document;
            bool replace = old_document && old_document->getVirgin();

            desktop = createDesktop(document, replace);
            document_fix(desktop);
        } else if (!cancelled) {
            std::cerr << "InkscapeApplication::create_window: Failed to load: "
                      << file->get_parse_name().raw() << std::endl;

            gchar *text = g_strdup_printf(_("Failed to load the requested file %s"), file->get_parse_name().c_str());
            sp_ui_error_dialog(text);
            g_free(text);
        }

    } else {
        document = document_new();
        if (document) {
            desktop = desktopOpen(document);
        } else {
            std::cerr << "InkscapeApplication::create_window: Failed to open default document!" << std::endl;
        }
    }

    _active_document = document;
    _active_window = desktop ? desktop->getInkscapeWindow() : nullptr;
}

/** Destroy a window and close the document it contains. Aborts if document needs saving.
 *  Replaces document and keeps window open if last window and keep_alive is true.
 *  Returns true if window destroyed.
 */
bool InkscapeApplication::destroyDesktop(SPDesktop *desktop, bool keep_alive)
{
    if (!gtk_app()) {
        g_assert_not_reached();
        return false;
    }

    auto document = desktop->getDocument();

    if (!document) {
        std::cerr << "InkscapeApplication::destroy_window: window has no document!" << std::endl;
        return false;
    }

    // Remove document if no desktop with document is left.
    auto it = _documents.find(document);
    if (it != _documents.end()) {
        // If only one desktop for document:
        if (it->second.size() == 1) {
            // Check if document needs saving.
            bool abort = document_check_for_data_loss(desktop);
            if (abort) {
                return false;
            }
        }

        if (get_number_of_windows() == 1 && keep_alive) {
            // Last desktop, replace with new document.
            auto new_document = document_new();
            document_swap(desktop, new_document);
        } else {
            desktopClose(desktop);
            if (get_number_of_windows() == 0) {
                // No Inkscape windows left, remove dialog windows.
                for (auto const &window : gtk_app()->get_windows()) {
                    window->close();
                }
            }
        }

        if (it->second.size() == 0) {
            // No window contains document so let's close it.
            document_close (document);
        }

    } else {
        std::cerr << "InkscapeApplication::destroy_window: Could not find document!" << std::endl;
    }

    return true;
}

void InkscapeApplication::detachDesktopToNewWindow(SPDesktop *desktop)
{
    // Remove from existing window.
    auto old_win = desktop->getInkscapeWindow();
    old_win->get_desktop_widget()->removeDesktop(desktop);

    // Open in a new window.
    auto new_win = _windows.emplace_back(std::make_unique<InkscapeWindow>(desktop)).get();
    new_win->present();
}

bool InkscapeApplication::destroy_all()
{
    if (!gtk_app()) {
        g_assert_not_reached();
        return false;
    }

    while (!_documents.empty()) {
        auto &[doc, desktops] = *_documents.begin();
        if (!desktops.empty()) {
            if (!destroyDesktop(desktops.back().get())) {
                return false; // If destroy aborted, we need to stop exit.
            }
        }
    }

    return true;
}

/** Common processing for documents
 */
void InkscapeApplication::process_document(SPDocument *document, std::string output_path, bool new_window)
{
    // Are we doing one file at a time? In that case, we don't recreate new windows for each file.
    bool replace = _use_pipe || _batch_process;

    // Open window if needed (reuse window if we are doing one file at a time inorder to save overhead).
    _active_document  = document;
    if (_with_gui) {
        _active_desktop = createDesktop(document, replace, new_window);
        _active_window = _active_desktop->getInkscapeWindow();
    } else {
        _active_window = nullptr;
        _active_desktop = nullptr;
        _active_selection = document->getSelection();
    }

    document->ensureUpToDate(); // Or queries don't work!

    // process_file
    activate_any_actions(_command_line_actions, _gio_application, _active_window, _active_document);

    if (_use_shell) {
        shell();
    }
    if (_with_gui && _active_window) {
        document_fix(_active_desktop);
    }
    // Only if --export-filename, --export-type --export-overwrite, or --export-use-hints are used.
    if (_auto_export) {
        // Save... can't use action yet.
        _file_export.do_export(document, output_path);
    }
}

/*
 * Called on first Inkscape instance creation. Not called if a new Inkscape instance is merged
 * with an existing instance.
 */
void InkscapeApplication::on_startup()
{
    // Autosave
    Inkscape::AutoSave::getInstance().init(this);

    // Deprecated...
    Inkscape::Application::create(_with_gui);

    // Extensions
    if (_no_extensions) {
        Inkscape::Extension::shallow_init();
    } else {
        Inkscape::Extension::init();
    }

    // After extensions are loaded query effects to construct action data
    init_extension_action_data();

    // Command line execution. Must be after Extensions are initialized.
    parse_actions(_command_line_actions_input, _command_line_actions);

    if (!_with_gui) {
        return;
    }

    auto *gapp = gio_app();

    // ======================= Actions (GUI) ======================
    gapp->add_action("new",    sigc::mem_fun(*this, &InkscapeApplication::on_new   ));
    gapp->add_action("quit",   sigc::mem_fun(*this, &InkscapeApplication::on_quit  ));

    // ========================= GUI Init =========================
    Gtk::Window::set_default_icon_name("org.inkscape.Inkscape");

    // build_menu(); // Builds and adds menu to app. Used by all Inkscape windows. This can be done
                     // before all actions defined. * For the moment done by each window so we can add
                     // window action info to menu_label_to_tooltip map.

    // Add tool based shortcut meta-data
    init_tool_shortcuts(this);
}

// Open document window with default document or pipe. Either this or on_open() is called.
void InkscapeApplication::on_activate()
{
    std::string output;
    // Create new document, either from pipe or from template.
    SPDocument *document = nullptr;

    if (_use_pipe) {
        // Create document from pipe in.
        std::istreambuf_iterator<char> begin(std::cin), end;
        std::string s(begin, end);
        document = document_open(s);
        output = "-";
    } else if (_with_gui)  {
        if (gtk_app()->get_windows().empty() && Inkscape::UI::Dialog::StartScreen::get_start_mode() > 0) {
            _openStartScreen();
            return;
        }
        _closeStartScreen();
        document = document_new();
    } else if (_use_command_line_argument) {
        document = document_new();
    } else {
        std::cerr << "InkscapeApplication::on_activate: failed to create document!" << std::endl;
        return;
    }

    if (!document) {
        return;
    }

    // Process document (command line actions, shell, create window)
    process_document(document, output, true);

    if (_batch_process) {
        // If with_gui, we've reused a window for each file. We must quit to destroy it.
        gio_app()->quit();
    }
}

void InkscapeApplication::windowClose(InkscapeWindow *window)
{
    if (window == _active_window) {
        _active_window = nullptr;

        // Detach floating dialogs from about-to-be-deleted window.
        for (auto const &win : gtk_app()->get_windows()) {
            if (auto dialog = dynamic_cast<Inkscape::UI::Dialog::DialogWindow *>(win)) {
                dialog->set_inkscape_window(nullptr);
            }
        }
    }

    auto win_it = std::find_if(_windows.begin(), _windows.end(), [=] (auto &w) { return w.get() == window; });
    _windows.erase(win_it);
}

// Open document window for each file. Either this or on_activate() is called.
// type_vec_files == std::vector<Glib::RefPtr<Gio::File> >
void InkscapeApplication::on_open(Gio::Application::type_vec_files const &files, Glib::ustring const &hint)
{
    // on_activate isn't called in this instance
    if(_pdf_poppler)
        INKSCAPE.set_pdf_poppler(_pdf_poppler);
    if(!_pages.empty())
        INKSCAPE.set_pages(_pages);

    INKSCAPE.set_pdf_font_strategy((int)_pdf_font_strategy);
    INKSCAPE.set_pdf_convert_colors(_pdf_convert_colors);
    INKSCAPE.set_pdf_group_by(_pdf_group_by);

    if (files.size() > 1 && !_file_export.export_filename.empty()) {
        for (auto &file : files) {
            std::cerr << " * input-filename: '" << file->get_path().c_str() << "'\n";
        }
        std::cerr << "InkscapeApplication::on_open: "
                     "Can't use '--export-filename' with multiple input files "
                     "(output file would be overwritten for each input file). "
                     "Please use '--export-type' instead and rename manually."
                  << std::endl;
        return;
    }

    _closeStartScreen();

    bool first = true; // for opening all files in one new window
    for (auto file : files) {
        // Open file
        auto [document, cancelled] = document_open(file);
        if (!document) {
            if (!cancelled) {
                std::cerr << "InkscapeApplication::on_open: failed to create document!" << std::endl;
            }
            continue;
        }

        // Process document (command line actions, shell, create window)
        process_document(document, file->get_path(), first);
        first = false;
    }

    if (_batch_process) {
        // If with_gui, we've reused a window for each file. We must quit to destroy it.
        gio_app()->quit();
    }
}

void InkscapeApplication::parse_actions(Glib::ustring const &input, action_vector_t &action_vector)
{
    auto const re_colon = Glib::Regex::create("\\s*:\\s*");

    // Split action list
    std::vector<Glib::ustring> tokens = Glib::Regex::split_simple("\\s*;\\s*", input);
    for (auto token : tokens) {
        // Note: split into 2 tokens max ("param:value"); allows value to contain colon (e.g. abs. paths on Windows)
        std::vector<Glib::ustring> tokens2 = re_colon->split(token, 0,  static_cast<Glib::Regex::MatchFlags>(0), 2);
        Glib::ustring action;
        Glib::ustring value;
        if (tokens2.size() > 0) {
            action = tokens2[0];
        }
        if (action.find_first_not_of(" \f\n\r\t\v") == std::string::npos) {
            continue;
        }
        if (tokens2.size() > 1) {
            value = tokens2[1];
        }

        Glib::RefPtr<Gio::Action> action_ptr = _gio_application->lookup_action(action);
        if (action_ptr) {
            // Doesn't seem to be a way to test this using the C++ binding without Glib-CRITICAL errors.
            const  GVariantType* gtype = g_action_get_parameter_type(action_ptr->gobj());
            if (gtype) {
                // With value.
                Glib::VariantType type = action_ptr->get_parameter_type();
                if (type.get_string() == "b") {
                    bool b = false;
                    if (value == "1" || value == "true" || value.empty()) {
                        b = true;
                    } else if (value == "0" || value == "false") {
                        b = false;
                    } else {
                        std::cerr << "InkscapeApplication::parse_actions: Invalid boolean value: " << action << ":" << value << std::endl;
                    }
                    action_vector.emplace_back(action, Glib::Variant<bool>::create(b));
                } else if (type.get_string() == "i") {
                    action_vector.emplace_back(action, Glib::Variant<int>::create(std::stoi(value)));
                } else if (type.get_string() == "d") {
                    action_vector.emplace_back(action, Glib::Variant<double>::create(std::stod(value)));
                } else if (type.get_string() == "s") {
                    action_vector.emplace_back(action, Glib::Variant<Glib::ustring>::create(value));
                 } else if (type.get_string() == "(dd)") {
                    std::vector<Glib::ustring> tokens3 = Glib::Regex::split_simple(",", value.c_str());
                    if (tokens3.size() != 2) {
                        std::cerr << "InkscapeApplication::parse_actions: " << action << " requires two comma separated numbers" << std::endl;
                        continue;
                    }

                    double d0 = 0;
                    double d1 = 0;
                    try {
                        d0 = std::stod(tokens3[0]);
                        d1 = std::stod(tokens3[1]);
                    } catch (...) {
                        std::cerr << "InkscapeApplication::parse_actions: " << action << " requires two comma separated numbers" << std::endl;
                        continue;
                    }

                    action_vector.emplace_back(action, Glib::Variant<std::tuple<double, double>>::create({d0, d1}));
               } else {
                    std::cerr << "InkscapeApplication::parse_actions: unhandled action value: "
                              << action << ": " << type.get_string() << std::endl;
                }
            } else {
                // Stateless (i.e. no value).
                action_vector.emplace_back(action, Glib::VariantBase());
            }
        } else {
            std::cerr << "InkscapeApplication::parse_actions: could not find action for: " << action << std::endl;
        }
    }
}

#ifdef WITH_GNU_READLINE

// For use in shell mode. Command completion of action names.
char* readline_generator (const char* text, int state)
{
    static std::vector<Glib::ustring> actions;

    // Fill the vector of action names.
    if (actions.size() == 0) {
        auto *app = InkscapeApplication::instance();
        actions = app->gio_app()->list_actions();
        std::sort(actions.begin(), actions.end());
    }

    static int list_index = 0;
    static int len = 0;

    if (!state) {
        list_index = 0;
        len = strlen(text);
    }

    const char* name = nullptr;
    while (list_index < actions.size()) {
        name = actions[list_index].c_str();
        list_index++;
        if (strncmp (name, text, len) == 0) {
            return (strdup(name));
        }
    }

    return ((char*)nullptr);
}

char** readline_completion(const char* text, int start, int end)
{
    char **matches = (char**)nullptr;

    // Match actions names, but only at start of line.
    // It would be nice to also match action names after a ';' but it's not possible as text won't include ';'.
    if (start == 0) {
        matches = rl_completion_matches (text, readline_generator);
    }

    return (matches);
}

void readline_init()
{
    rl_readline_name = "inkscape";
    rl_attempted_completion_function = readline_completion;
}

#endif // WITH_GNU_READLINE

// Once we don't need to create a window just to process verbs!
void InkscapeApplication::shell(bool active_window)
{
    std::cout << "Inkscape interactive shell mode. Type 'action-list' to list all actions. "
              << "Type 'quit' to quit." << std::endl;
    std::cout << " Input of the form:" << std::endl;
    std::cout << " action1:arg1; action2:arg2; ..." << std::endl;
    if (!_with_gui && !active_window) {
        std::cout << "Only actions that don't require a desktop may be used." << std::endl;
    }

#ifdef WITH_GNU_READLINE
    auto history_file = Glib::build_filename(Inkscape::IO::Resource::profile_path(), "shell.history");

#ifdef _WIN32
    gchar *locale_filename = g_win32_locale_filename_from_utf8(history_file.c_str());
    if (locale_filename) {
        history_file = locale_filename;
        g_free(locale_filename);
    }
#endif

    static bool init = false;
    if (!init) {
        readline_init();
        using_history();
        init = true;

        int error = read_history(history_file.c_str());
        if (error && error != ENOENT) {
            std::cerr << "read_history error: " << std::strerror(error) << " " << history_file << std::endl;
        }
    }
#endif

    while (std::cin.good()) {
        bool eof = false;
        std::string input;

#ifdef WITH_GNU_READLINE
        char *readline_input = readline("> ");
        if (readline_input) {
            input = readline_input;
            if (input != "quit" && input != "q") {
                add_history(readline_input);
            }
        } else {
            eof = true;
        }
        free(readline_input);
#else
        std::cout << "> ";
        std::getline(std::cin, input);
#endif

        // Remove trailing space
        input = std::regex_replace(input, std::regex(" +$"), "");

        if (eof || input == "quit" || input == "q") {
            break;
        }

        action_vector_t action_vector;
        if (active_window) {
            input = "active-window-start;" + input + ";active-window-end";
            unlink(get_active_desktop_commands_location().c_str());
        }
        parse_actions(input, action_vector);
        activate_any_actions(action_vector, _gio_application, _active_window, _active_document);
        if (active_window) {
            redirect_output();
        } else {
            // This would allow displaying the results of actions on the fly... but it needs to be well
            // vetted first.
            auto context = Glib::MainContext::get_default();
            while (context->iteration(false)) {};
        }
    }

#ifdef WITH_GNU_READLINE
    stifle_history(200); // ToDo: Make number a preference.
    int error = write_history(history_file.c_str());
    if (error) {
        std::cerr << "write_history error: " << std::strerror(error) << " " << history_file << std::endl;
    }
#endif

    if (_with_gui) {
        _gio_application->quit(); // Force closing windows.
    }
}

// Todo: Code can be improved by using proper IPC rather than temporary file polling.
void InkscapeApplication::redirect_output()
{
    auto const tmpfile = get_active_desktop_commands_location();

    for (int counter = 0; ; counter++) {
        if (Glib::file_test(tmpfile, Glib::FileTest::EXISTS)) {
            break;
        } else if (counter >= 300) { // 30 seconds exit
            std::cerr << "couldn't process response. File not found" << std::endl;
            return;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    auto tmpfile_delete_guard = scope_exit([&] {
        unlink(tmpfile.c_str());
    });

    auto awo = std::ifstream(tmpfile);
    if (!awo) {
        std::cout << "couldn't process response. Couldn't read" << std::endl;
        return;
    }

    auto const content = std::string(std::istreambuf_iterator<char>(awo), std::istreambuf_iterator<char>());
    awo.close();

    auto doc = sp_repr_read_mem(content.c_str(), strlen(content.c_str()), nullptr);
    if (!doc) {
        std::cout << "couldn't process response. Wrong data" << std::endl;
        return;
    }

    auto doc_delete_guard = scope_exit([&] {
        Inkscape::GC::release(doc);
    });

    bool noout = true;
    for (auto child = doc->root()->firstChild(); child; child = child->next()) {
        auto grandchild = child->firstChild();
        auto res = grandchild ? grandchild->content() : nullptr;
        if (res) {
            if (!g_strcmp0(child->name(), "cerr")) {
                std::cerr << res << std::endl;
            } else {
                std::cout << res << std::endl;
            }
            noout = false;
        }
    }

    if (noout) {
        std::cout << "no output" << std::endl;
    }
}

// ========================= Callbacks ==========================

/*
 * Handle command line options.
 *
 * Options are processed in the order they appear in this function.
 * We process in order: Print -> GUI -> Open -> Query -> Process -> Export.
 * For each file without GUI: Open -> Query -> Process -> Export
 * More flexible processing can be done via actions.
 */
int
InkscapeApplication::on_handle_local_options(const Glib::RefPtr<Glib::VariantDict>& options)
{
    auto prefs = Inkscape::Preferences::get();
    if (!options) {
        std::cerr << "InkscapeApplication::on_handle_local_options: options is null!" << std::endl;
        return -1; // Keep going
    }

    // ===================== APP ID ====================
    if (options->contains("app-id-tag")) {
        Glib::ustring id_tag;
        options->lookup_value("app-id-tag", id_tag);
        Glib::ustring app_id = "org.inkscape.Inkscape." + id_tag;
        if (Gio::Application::id_is_valid(app_id)) {
            _gio_application->set_id(app_id);
        } else {
            std::cerr << "InkscapeApplication: invalid application id: " << app_id.raw() << std::endl;
            std::cerr << "  tag must be ASCII and not start with a number." << std::endl;
        }
    }

    // ===================== QUERY =====================
    // These are processed first as they result in immediate program termination.
    // Note: we cannot use actions here as the app has not been registered yet (registering earlier
    // causes problems with changing the app id).
    if (options->contains("version")) {
        std::cout << Inkscape::inkscape_version() << std::endl;
        return EXIT_SUCCESS;
    }

    if (options->contains("debug-info")) {
        std::cout << Inkscape::debug_info() << std::endl;
        return EXIT_SUCCESS;
    }

    if (options->contains("system-data-directory")) {
        std::cout << Glib::build_filename(get_inkscape_datadir(), "inkscape") << std::endl;
        return EXIT_SUCCESS;
    }

    if (options->contains("user-data-directory")) {
        std::cout << Inkscape::IO::Resource::profile_path() << std::endl;
        return EXIT_SUCCESS;
    }

    _no_extensions = options->contains("no-extensions");

    // Can't do this until after app is registered!
    // if (options->contains("action-list")) {
    //     print_action_list();
    //     return EXIT_SUCCESS;
    // }

    // For options without arguments.
    auto base = Glib::VariantBase();

    // ================== GUI and Shell ================

    // Use of most command line options turns off use of gui unless explicitly requested!
    // Listed in order that they appear in constructor.
    if (options->contains("pipe")                  ||

        options->contains("export-filename")       ||
        options->contains("export-overwrite")      ||
        options->contains("export-type")           ||
        options->contains("export-page")           ||

        options->contains("export-area-page")      ||
        options->contains("export-area-drawing")   ||
        options->contains("export-area")           ||
        options->contains("export-area-snap")      ||
        options->contains("export-dpi")            ||
        options->contains("export-width")          ||
        options->contains("export-height")         ||
        options->contains("export-margin")         ||
        options->contains("export-height")         ||

        options->contains("export-id")             ||
        options->contains("export-id-only")        ||
        options->contains("export-plain-svg")      ||
        options->contains("export-ps-level")       ||
        options->contains("export-pdf-version")    ||
        options->contains("export-text-to_path")   ||
        options->contains("export-latex")          ||
        options->contains("export-ignore-filters") ||
        options->contains("export-use-hints")      ||
        options->contains("export-background")     ||
        options->contains("export-background-opacity") ||
        options->contains("export-text-to_path")   ||
        options->contains("export-png-color-mode") ||
        options->contains("export-png-use-dithering") ||
        options->contains("export-png-compression") ||
        options->contains("export-png-antialias") ||
        options->contains("export-make-paths")     ||

        options->contains("query-id")              ||
        options->contains("query-x")               ||
        options->contains("query-all")             ||
        options->contains("query-y")               ||
        options->contains("query-width")           ||
        options->contains("query-height")          ||
        options->contains("query-pages")           ||

        options->contains("vacuum-defs")           ||
        options->contains("select")                ||
        options->contains("list-input-types")      ||
        options->contains("action-list")           ||
        options->contains("actions")               ||
        options->contains("actions-file")          ||
        options->contains("shell")
        ) {
        _with_gui = false;
    }

    if (options->contains("with-gui")        ||
        options->contains("batch-process")
        ) {
        _with_gui = bool(gtk_app()); // Override turning GUI off
        if (!_with_gui)
            std::cerr << "No GUI available, some actions may fail" << std::endl;
    }

    if (options->contains("batch-process"))  _batch_process = true;
    if (options->contains("shell"))          _use_shell = true;
    if (options->contains("pipe"))           _use_pipe  = true;

    // Enable auto-export
    if (options->contains("export-filename")  ||
        options->contains("export-type")      ||
        options->contains("export-overwrite") ||
        options->contains("export-use-hints")
        ) {
        _auto_export = true;
    }

    // 1. If we are running in command-line mode (without gui) and we haven't explicitly changed the app_id,
    // change it here so that this instance of Inkscape is not merged with an existing instance (otherwise
    // unwanted windows will pop up and the command-line arguments will be ignored).
    // 2. Set a new app id if we are calling a new inkscape instance (with a gui) through an extension
    // when the extension author hasn't already done so
    bool use_active_window = options->contains("active-window");
    if (!options->contains("app-id-tag") && (_with_gui ? Glib::getenv("SELF_CALL") != "" : !use_active_window)) {
        Glib::ustring app_id = "org.inkscape.Inkscape.p" + std::to_string(getpid());
        _gio_application->set_id(app_id);
    }

    // ==================== ACTIONS ====================
    // Actions as an argument string: e.g.: --actions="query-id:rect1;query-x".
    // Actions will be processed in order that they are given in argument.
    Glib::ustring actions;
    if (options->contains("actions-file")) {
        std::string fileactions;
        options->lookup_value("actions-file", fileactions);
        if (!fileactions.empty()) {
            std::ifstream awo(fileactions);
            if (awo) {
                std::string content((std::istreambuf_iterator<char>(awo)), (std::istreambuf_iterator<char>()));
                _command_line_actions_input = content + ";";
            }
        }
    } else if (options->contains("actions")) {
        options->lookup_value("actions", _command_line_actions_input);
    }

    // This must be done after the app has been registered!
    if (options->contains("action-list")) {
        _command_line_actions.emplace_back("action-list", base);
    }

    if (options->contains("list-input-types")) {
        _command_line_actions.emplace_back("list-input-types", base);
    }

    // ================= OPEN/IMPORT ===================

    if (options->contains("pages")) {
        options->lookup_value("pages", _pages);
    }

    if (options->contains("pdf-poppler")) {
        _pdf_poppler = true;
    }

    if (options->contains("pdf-font-strategy")) {
        Glib::ustring strategy;
        options->lookup_value("pdf-font-strategy", strategy);
        _pdf_font_strategy = FontStrategy::RENDER_MISSING;
        if (strategy == "delete-all") {
            _pdf_font_strategy = FontStrategy::DELETE_ALL;
        }
        if (strategy == "delete-missing") {
            _pdf_font_strategy = FontStrategy::DELETE_MISSING;
        }
        if (strategy == "draw-all") {
            _pdf_font_strategy = FontStrategy::RENDER_ALL;
        }
        if (strategy == "keep") {
            _pdf_font_strategy = FontStrategy::KEEP_MISSING;
        }
        if (strategy == "substitute") {
            _pdf_font_strategy = FontStrategy::SUBSTITUTE_MISSING;
        }
    }

    if (options->contains("pdf-convert-colors")) {
        _pdf_convert_colors = true;
    }

    if (options->contains("pdf-group-by")) {
        Glib::ustring group_by;
        options->lookup_value("pdf-group-by", group_by);
        _pdf_group_by = "by-" + group_by;
    }

    if (options->contains("convert-dpi-method")) {
        Glib::ustring method;
        options->lookup_value("convert-dpi-method", method);
        if (!method.empty()) {
            _command_line_actions.emplace_back("convert-dpi-method", Glib::Variant<Glib::ustring>::create(method));
        }
    }

    if (options->contains("no-convert-text-baseline-spacing")) {
        _command_line_actions.emplace_back("no-convert-baseline", base);
    }

    // ===================== QUERY =====================

    // 'query-id' should be processed first! Can be a comma-separated list.
    if (options->contains("query-id")) {
        Glib::ustring query_id;
        options->lookup_value("query-id", query_id);
        if (!query_id.empty()) {
            _command_line_actions.emplace_back("select-by-id", Glib::Variant<Glib::ustring>::create(query_id));
        }
    }

    if (options->contains("query-all"))    _command_line_actions.emplace_back("query-all",   base);
    if (options->contains("query-x"))      _command_line_actions.emplace_back("query-x",     base);
    if (options->contains("query-y"))      _command_line_actions.emplace_back("query-y",     base);
    if (options->contains("query-width"))  _command_line_actions.emplace_back("query-width", base);
    if (options->contains("query-height")) _command_line_actions.emplace_back("query-height",base);
    if (options->contains("query-pages"))  _command_line_actions.emplace_back("query-pages", base);

    // =================== PROCESS =====================

    if (options->contains("vacuum-defs"))  _command_line_actions.emplace_back("vacuum-defs", base);

    if (options->contains("select")) {
        Glib::ustring select;
        options->lookup_value("select", select);
        if (!select.empty()) {
            _command_line_actions.emplace_back("select", Glib::Variant<Glib::ustring>::create(select));
        }
    }

    // ==================== EXPORT =====================
    if (options->contains("export-filename")) {
        options->lookup_value("export-filename",  _file_export.export_filename);
    }

    if (options->contains("export-type")) {
        options->lookup_value("export-type",      _file_export.export_type);
    }
    if (options->contains("export-extension")) {
        options->lookup_value("export-extension", _file_export.export_extension);
        _file_export.export_extension = _file_export.export_extension.lowercase();
    }

    if (options->contains("export-overwrite"))    _file_export.export_overwrite    = true;

    if (options->contains("export-page")) {
        options->lookup_value("export-page", _file_export.export_page);
    }

    // Export - Geometry
    if (options->contains("export-area")) {
        Glib::ustring area{};
        options->lookup_value("export-area", area);
        _file_export.set_export_area(area);
    }

    if (options->contains("export-area-drawing")) {
        _file_export.set_export_area_type(ExportAreaType::Drawing);
    }
    if (options->contains("export-area-page")) {
        _file_export.set_export_area_type(ExportAreaType::Page);
    }

    if (options->contains("export-margin")) {
        options->lookup_value("export-margin",    _file_export.export_margin);
    }

    if (options->contains("export-area-snap"))    _file_export.export_area_snap    = true;

    if (options->contains("export-width")) {
        options->lookup_value("export-width",     _file_export.export_width);
    }

    if (options->contains("export-height")) {
        options->lookup_value("export-height",    _file_export.export_height);
    }

    // Export - Options
    if (options->contains("export-id")) {
        options->lookup_value("export-id",        _file_export.export_id);
    }

    if (options->contains("export-id-only"))      _file_export.export_id_only     = true;
    if (options->contains("export-plain-svg"))    _file_export.export_plain_svg      = true;

    if (options->contains("export-dpi")) {
        options->lookup_value("export-dpi",       _file_export.export_dpi);
    }

    if (options->contains("export-ignore-filters")) _file_export.export_ignore_filters = true;
    if (options->contains("export-text-to-path"))   _file_export.export_text_to_path   = true;

    if (options->contains("export-ps-level")) {
        options->lookup_value("export-ps-level",  _file_export.export_ps_level);
    }

    if (options->contains("export-pdf-version")) {
        options->lookup_value("export-pdf-version", _file_export.export_pdf_level);
    }

    if (options->contains("export-latex"))        _file_export.export_latex       = true;
    if (options->contains("export-use-hints"))    _file_export.export_use_hints   = true;
    if (options->contains("export-make-paths"))   _file_export.make_paths = true;

    if (options->contains("export-background")) {
        options->lookup_value("export-background",_file_export.export_background);
    }

    // FIXME: Upstream bug means DOUBLE is ignored if set to 0.0 so doesn't exist in options
    if (options->contains("export-background-opacity")) {
        Glib::ustring opacity;
        options->lookup_value("export-background-opacity", opacity);
        _file_export.export_background_opacity = Glib::Ascii::strtod(opacity);
    }

    if (options->contains("export-png-color-mode")) {
        options->lookup_value("export-png-color-mode", _file_export.export_png_color_mode);
    }

    if (options->contains("export-png-use-dithering")) {
        Glib::ustring val;
        options->lookup_value("export-png-use-dithering", val);
        if (val == "true") {
            _file_export.export_png_use_dithering = true;
#if CAIRO_VERSION < CAIRO_VERSION_ENCODE(1, 18, 0)
            std::cerr << "Your cairo version does not support dithering! Option will be ignored." << std::endl;
#endif
        }
        else if (val == "false") _file_export.export_png_use_dithering = false;
        else std::cerr << "invalid value for export-png-use-dithering. Ignoring." << std::endl;
    } else {
        _file_export.export_png_use_dithering = prefs->getBool("/options/dithering/value", true);
    }

    // FIXME: Upstream bug means INT is ignored if set to 0 so doesn't exist in options
    if (options->contains("export-png-compression")) {
        Glib::ustring compression;
        options->lookup_value("export-png-compression", compression);
        const char *begin = compression.raw().c_str();
        char *end;
        long ival = strtol(begin, &end, 10);
        if (end == begin || *end != '\0' || errno == ERANGE) {
            std::cerr << "Cannot parse integer value "
                      << compression
                      << " for --export-png-compression; the default value "
                      <<  _file_export.export_png_compression
                      << " will be used"
                      << std::endl;
        }
        else {
            _file_export.export_png_compression = ival;
        }
    }

    // FIXME: Upstream bug means INT is ignored if set to 0 so doesn't exist in options
    if (options->contains("export-png-antialias")) {
        Glib::ustring antialias;
        options->lookup_value("export-png-antialias", antialias);
        const char *begin = antialias.raw().c_str();
        char *end;
        long ival = strtol(begin, &end, 10);
        if (end == begin || *end != '\0' || errno == ERANGE) {
            std::cerr << "Cannot parse integer value "
                      << antialias
                      << " for --export-png-antialias; the default value "
                      <<  _file_export.export_png_antialias
                      << " will be used"
                      << std::endl;
        }
        else {
            _file_export.export_png_antialias = ival;
        }
    }

    if (use_active_window) {
        _gio_application->register_application();
        if (!_gio_application->get_default()->is_remote()) {
#ifdef __APPLE__
            std::cerr << "Active window is not available on macOS" << std::endl;
#else
            std::cerr << "No active desktop to run" << std::endl;
#endif
            return EXIT_SUCCESS;
        }
        if (_use_shell) {
            shell(true);
        } else {
            _command_line_actions.emplace(_command_line_actions.begin(), "active-window-start", base);
            _command_line_actions_input = _command_line_actions_input + ";active-window-end";
            unlink(get_active_desktop_commands_location().c_str());
            parse_actions(_command_line_actions_input, _command_line_actions);
            activate_any_actions(_command_line_actions, _gio_application, _active_window, _active_document);
            redirect_output();
        }
        return EXIT_SUCCESS;
    }
    GVariantDict *options_copy = options->gobj_copy();
    GVariant *options_var = g_variant_dict_end(options_copy);
    if (g_variant_get_size(options_var) != 0) {
        _use_command_line_argument = true;
    }
    g_variant_dict_unref(options_copy);
    g_variant_unref(options_var);

    return -1; // Keep going
}

//   ========================  Actions  =========================

void InkscapeApplication::on_new()
{
    create_window();
}

void InkscapeApplication::on_quit()
{
    if (gtk_app()) {
        if (!destroy_all()) return; // Quit aborted.
        // For mac, ensure closing the gtk_app windows
        for (auto window : gtk_app()->get_windows()) {
            window->close();
        }
    }

    gio_app()->quit();
}

/*
 * Quit without checking for data loss.
 */
void
InkscapeApplication::on_quit_immediate()
{
    gio_app()->quit();
}

void InkscapeApplication::set_active_desktop(SPDesktop *desktop)
{
    _active_desktop = desktop;
    if (desktop) {
        INKSCAPE.activate_desktop(desktop);

        // Don't coalesce undo events across leaving then returning to a desktop.
        desktop->getDocument()->resetKey();
    }
}

void
InkscapeApplication::print_action_list()
{
    auto const *gapp = gio_app();

    auto actions = gapp->list_actions();
    std::sort(actions.begin(), actions.end());
    for (auto const &action : actions) {
        Glib::ustring fullname("app." + action);
        std::cout << std::left << std::setw(20) << action
                  << ":  " << _action_extra_data.get_tooltip_for_action(fullname) << std::endl;
    }
}

/**
 * Prints file type extensions (without leading dot) of input formats.
 */
void InkscapeApplication::print_input_type_list() const
{
    Inkscape::Extension::DB::InputList extension_list;
    Inkscape::Extension::db.get_input_list(extension_list);

    for (auto *imod : extension_list) {
        auto suffix = imod->get_extension();
        if (suffix[0] == '.') {
            ++suffix;
        }
        std::cout << suffix << std::endl;
    }
}

/**
 * Return number of open Inkscape Windows (irrespective of number of documents)
.*/
int InkscapeApplication::get_number_of_windows() const {
    if (_with_gui) {
        return std::accumulate(_documents.begin(), _documents.end(), 0,
          [&](int sum, auto& v){ return sum + static_cast<int>(v.second.size()); });
    }
    return 0;
}

/**
 * Adds effect to Gio::Actions
 *
 *  \c effect is Filter or Extension
 *  \c prefs is a set of preferences to set for `effect`
*/
void action_effect(Inkscape::Extension::Effect* effect, EffectDict prefs) {
    auto desktop = InkscapeApplication::instance()->get_active_desktop();
    if (!effect->check()) {
        auto handler = Inkscape::ErrorReporter((bool)desktop);
        handler.handleError(effect->get_name(), effect->getErrorReason());
    } else if (effect->_workingDialog && prefs.empty() && desktop) {
        effect->prefs(desktop);
    } else {
        // Set each of the given preferences
        for (auto const& [key, val] : prefs) {
            effect->set_param_any(key.c_str(), val);
        }
        auto document = InkscapeApplication::instance()->get_active_document();
        effect->effect(desktop, document);
    }
}

// Modifying string to get submenu id
std::string action_menu_name(std::string menu) {
    transform(menu.begin(), menu.end(), menu.begin(), ::tolower);
    for (auto &x:menu) {
        if (x==' ') {
            x = '-';
        }
    }
    return menu;
}

void InkscapeApplication::init_extension_action_data() {
    if (_no_extensions) {
        return;
    }
    for (auto effect : Inkscape::Extension::db.get_effect_list()) {

        std::string aid = effect->get_sanitized_id();
        std::string action_id = "app." + aid;

        auto app = this;
        if (auto gapp = gtk_app()) {
            auto action = gapp->add_action(aid, [effect](){ action_effect(effect, {}); });
            auto action_noprefs = gapp->add_action(aid + ".noprefs", [effect](){ action_effect(effect, {}); });
            auto action_prefs = gapp->add_action_with_parameter(aid + ".prefs", Glib::VariantType("a{ss}"), [effect](const Glib::VariantBase& value){
                auto d = Glib::VariantBase::cast_dynamic<Glib::Variant<EffectDict>>(value);
                action_effect(effect, d.get());
            });
            _effect_actions.emplace_back(action);
            _effect_actions.emplace_back(action_noprefs);
            _effect_actions.emplace_back(action_prefs);
        }

        if (effect->hidden_from_menu()) continue;

        // Submenu retrieval as a list of strings (to handle nested menus).
        auto sub_menu_list = effect->get_menu_list();

        // Setting initial value of description to name of action in case there is no description
        auto description = effect->get_menu_tip();
        if (description.empty()) description = effect->get_name();

        if (effect->is_filter_effect()) {
            std::vector<std::vector<Glib::ustring>>raw_data_filter =
                {{ action_id, effect->get_name(), "Filters", description },
                { action_id + ".noprefs", Glib::ustring(effect->get_name()) + " " + _("(No preferences)"), "Filters (no prefs)", description },
                { action_id + ".prefs",   Glib::ustring(effect->get_name()) + " " + _("(Preferences)"),    "Filters (prefs)",    description }};
            app->get_action_extra_data().add_data(raw_data_filter);
        } else {
            std::vector<std::vector<Glib::ustring>>raw_data_effect =
                {{ action_id, effect->get_name(), "Extensions", description },
                { action_id + ".noprefs", Glib::ustring(effect->get_name()) + " " + _("(No preferences)"), "Extensions (no prefs)", description },
                { action_id + ".prefs",   Glib::ustring(effect->get_name()) + " " + _("(Preferences)"),    "Extensions (prefs)",    description }};
            app->get_action_extra_data().add_data(raw_data_effect);
        }

#if false // enable to see all the loaded effects
        std::cout << " Effect: name:  " << effect->get_name();
        std::cout << "  id: " << aid.c_str();
        std::cout << "  menu: ";
        for (auto sub_menu : sub_menu_list) {
            std::cout << "|" << sub_menu.raw(); // Must use raw() as somebody has messed up encoding.
        }
        std::cout << "|  icon: " << effect->find_icon_file();
        std::cout << std::endl;
#endif

        // Add submenu to effect data
        gchar *ellipsized_name = effect->takes_input() ? g_strdup_printf(_("%s..."), effect->get_name()) : nullptr;
        Glib::ustring menu_name = ellipsized_name ? ellipsized_name : effect->get_name();
        bool is_filter = effect->is_filter_effect();
        app->get_action_effect_data().add_data(aid, is_filter, sub_menu_list, menu_name);
        g_free(ellipsized_name);
    }
}

/// Create and show the start screen. It will self-destruct.
void InkscapeApplication::_openStartScreen()
{
    assert(_with_gui);
    auto win = Gtk::make_managed<Inkscape::UI::Dialog::StartScreen>();
    gtk_app()->add_window(*win);
    win->present();
    win->connectOpen([this] (SPDocument *document) { // this outlives win
        if (!document) {
            document = document_new();
        }
        process_document(document, {});
    });
}

/// Close the start screen, if open.
void InkscapeApplication::_closeStartScreen()
{
    if (!_with_gui) {
        return;
    }
    for (auto win : gtk_app()->get_windows()) {
        if (auto existing_start_screen = dynamic_cast<Inkscape::UI::Dialog::StartScreen *>(win)) {
            existing_start_screen->close();
            break; // Should be unique. Safer to exit, as it prevents dangling win in further iterations.
        }
    }
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
