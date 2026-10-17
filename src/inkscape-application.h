// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The main Inkscape application.
 *
 * Copyright (C) 2018 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */
#ifndef INKSCAPE_APPLICATION_H
#define INKSCAPE_APPLICATION_H

#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/application.h>

#include "actions/actions-effect-data.h"
#include "actions/actions-extra-data.h"
#include "actions/actions-hint-data.h"
#include "io/file-export-cmd.h"   // File export (non-verb)
#include "extension/internal/pdfinput/enums.h"
#include "util/smart_ptr_keys.h"

namespace Gio {
class File;
} // namespace Gio

using action_vector_t = std::vector<std::pair<std::string, Glib::VariantBase>>;

class InkscapeWindow;
class SPDocument;
class SPDesktop;

namespace Inkscape {
class Selection;
namespace UI {
class StartupSplash;
}
namespace UI::Dialog {
class StartScreen;
}
} // namespace Inkscape

class InkscapeApplication
{
public:
    /// Singleton instance.
    static InkscapeApplication *instance();

    /// Exclusively for the creation of the singleton instance inside main().
    InkscapeApplication();
    ~InkscapeApplication();

    /// The Gtk application instance, or NULL if running headless without display
    Gtk::Application *gtk_app() { return dynamic_cast<Gtk::Application *>(_gio_application.get()); }
    /// The Gio application instance, never NULL
    Gio::Application *gio_app() { return _gio_application.get(); }

    SPDesktop *createDesktop(SPDocument *document, bool replace, bool new_window = false);
    void create_window(Glib::RefPtr<Gio::File> const &file = {});
    bool destroyDesktop(SPDesktop *desktop, bool keep_alive = false);
    void detachDesktopToNewWindow(SPDesktop *desktop);
    bool destroy_all();
    void print_action_list();
    void print_input_type_list() const;

    InkFileExportCmd *file_export() { return &_file_export; }
    int on_handle_local_options(const Glib::RefPtr<Glib::VariantDict> &options);
    void on_new();
    void on_quit(); // Check for data loss.
    void on_quit_immediate(); // Don't check for data loss.

    // Gio::Actions need to know what document, selection, desktop to work on.
    // In headless mode, these are set for each file processed.
    // With GUI, these are set everytime the cursor enters an InkscapeWindow.
    SPDocument*           get_active_document() { return _active_document; };
    void                  set_active_document(SPDocument* document) { _active_document = document; };

    Inkscape::Selection*  get_active_selection() { return _active_selection; }
    void                  set_active_selection(Inkscape::Selection* selection)
                                                               {_active_selection = selection;};

    // A desktop should track selection and canvas to document transform matrix. This is partially
    // redundant with the selection functions above.
    // Canvas to document transform matrix should be stored in the canvas, itself.
    SPDesktop*            get_active_desktop() { return _active_desktop; }
    void                  set_active_desktop(SPDesktop *desktop);

    // The currently focused window (nominally corresponding to _active_document).
    // A window must have a document but a document may have zero, one, or more windows.
    // This will replace _active_desktop.
    InkscapeWindow*       get_active_window() { return _active_window; }
    void                  set_active_window(InkscapeWindow* window) { _active_window = window; }

    /****** Document ******/
    /* These should not require a GUI! */
    SPDocument *document_add(std::unique_ptr<SPDocument> document);

    SPDocument *document_new(std::string const &template_filename = {});
    std::pair<SPDocument *, bool /*cancelled*/> document_open(Glib::RefPtr<Gio::File> const &file);
    SPDocument *document_open(std::span<char const> buffer);
    bool                  document_swap(SPDesktop *desktop, SPDocument *document);
    bool                  document_revert(SPDocument* document);
    void                  document_close(SPDocument* document);

    /* These require a GUI! */
    void                  document_fix(SPDesktop *desktop);

    std::vector<SPDocument *> get_documents();

    /******* Window *******/
    void startup_close();
    void windowClose(InkscapeWindow *window);

    /******* Desktop *******/
    SPDesktop *desktopOpen(SPDocument *document, bool new_window = false);
    void desktopClose(SPDesktop *desktop);
    void desktopCloseActive();

    /****** Actions *******/
    InkActionExtraData&     get_action_extra_data()     { return _action_extra_data;  }
    InkActionEffectData&    get_action_effect_data()    { return _action_effect_data; }
    InkActionHintData&      get_action_hint_data()      { return _action_hint_data;   }
    std::map<std::string, Glib::ustring>& get_menu_label_to_tooltip_map() { return _menu_label_to_tooltip_map; };

    /******* Debug ********/
    void                  dump();

    int get_number_of_windows() const;

protected:
    Glib::RefPtr<Gio::Application> _gio_application;

    bool _with_gui    = true;
    bool _batch_process = false; // Temp
    bool _use_shell   = false;
    bool _use_pipe    = false;
    bool _auto_export = false;
    int _pdf_poppler  = false;
    FontStrategy _pdf_font_strategy = FontStrategy::RENDER_MISSING;
    bool _pdf_convert_colors = false;
    bool _use_command_line_argument = false;
    Glib::ustring _pages;
    std::string _pdf_group_by = "by-xobject";

    // Documents are owned by the application which is responsible for opening/saving/exporting.
    std::unordered_map<std::unique_ptr<SPDocument>,
                       std::vector<std::unique_ptr<SPDesktop>>,
                       TransparentPtrHash<SPDocument>,
                       TransparentPtrEqual<SPDocument>> _documents;

    std::vector<std::unique_ptr<InkscapeWindow>> _windows;

    // We keep track of these things so we don't need a window to find them (for headless operation).
    SPDocument*               _active_document   = nullptr;
    Inkscape::Selection*      _active_selection  = nullptr;
    SPDesktop*                _active_desktop       = nullptr;
    InkscapeWindow*           _active_window     = nullptr;

    InkFileExportCmd _file_export;

    // Actions from the command line or file.
    // Must read in on_handle_local_options() but parse in on_startup(). This is done as we must
    // have a valid app before initializing extensions which must be done before parsing.
    Glib::ustring _command_line_actions_input;
    action_vector_t _command_line_actions;

    // Extra data associated with actions (Label, Section, Tooltip/Help).
    InkActionExtraData  _action_extra_data;
    InkActionEffectData  _action_effect_data;
    InkActionHintData   _action_hint_data;
    // Needed due to the inabilitiy to get the corresponding Gio::Action from a Gtk::MenuItem.
    // std::string is used as key type because Glib::ustring has slow comparison and equality
    // operators.
    std::map<std::string, Glib::ustring> _menu_label_to_tooltip_map;
    void on_startup();
    void on_activate();
    void on_open(const Gio::Application::type_vec_files &files, const Glib::ustring &hint);
    void process_document(SPDocument* document, std::string output_path, bool new_window = false);
    void parse_actions(const Glib::ustring& input, action_vector_t& action_vector);

    void redirect_output();
    void shell(bool active_window = false);

    void _start_main_option_section(const Glib::ustring& section_name = "");
    
private:
    std::unique_ptr<Inkscape::UI::StartupSplash> _startup_splash;
    sigc::connection _splash_windows;
    bool _no_splash = false;
    void _finishStartupSplash();
    void _watchStartupSplash();

    void init_extension_action_data();
    std::vector<Glib::RefPtr<Gio::SimpleAction>> _effect_actions;
    bool _no_extensions = false;

    void _openStartScreen();
    void _closeStartScreen();
};

#endif // INKSCAPE_APPLICATION_H

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
