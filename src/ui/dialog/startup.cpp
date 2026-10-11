// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief A dialog for the about screen
 */
/*
 * Copyright (C) Martin Owens 2019 <doctormo@gmail.com>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "startup.h"

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/combobox.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/infobar.h>
#include <gtkmm/liststore.h>
#include <gtkmm/overlay.h>
#include <gtkmm/picture.h>
#include <gtkmm/settings.h>
#include <gtkmm/switch.h>
#include <gtkmm/version.h>
#include <gtkmm/windowhandle.h>

#include "inkscape-application.h"
#include "inkscape-version-info.h"
#include "inkscape-version.h"
#include "inkscape.h"
#include "io/recent-files.h"
#include "io/resource.h"
#include "ui/builder-utils.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/shortcuts.h"
#include "ui/themes.h"
#include "ui/util.h"

using namespace Inkscape::IO;
using namespace Inkscape::UI::View;

namespace Inkscape::UI::Dialog {

class NameIdCols: public Gtk::TreeModel::ColumnRecord {
    public:
        // These types must match those for the model in the .glade file
        NameIdCols() {
            this->add(this->col_name);
            this->add(this->col_id);
        }
        Gtk::TreeModelColumn<Glib::ustring> col_name;
        Gtk::TreeModelColumn<Glib::ustring> col_id;
};

class RecentCols: public Gtk::TreeModel::ColumnRecord {
    public:
        // These types must match those for the model in the .glade file
        RecentCols() {
            this->add(this->col_name);
            this->add(this->col_id);
            this->add(this->col_dt);
            this->add(this->col_crash);
        }
        Gtk::TreeModelColumn<Glib::ustring> col_name;
        Gtk::TreeModelColumn<Glib::ustring> col_id;
        Gtk::TreeModelColumn<gint64> col_dt;
        Gtk::TreeModelColumn<bool> col_crash;
};

class CanvasCols: public Gtk::TreeModel::ColumnRecord {
    public:
        // These types must match those for the model in the .glade file
        CanvasCols() {
            this->add(this->id);
            this->add(this->name);
            this->add(this->icon_filename);
            this->add(this->pagecolor);
            this->add(this->checkered);
            this->add(this->bordercolor);
            this->add(this->shadow);
            this->add(this->deskcolor);
        }
        Gtk::TreeModelColumn<Glib::ustring> id;
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> icon_filename;
        Gtk::TreeModelColumn<Glib::ustring> pagecolor;
        Gtk::TreeModelColumn<bool> checkered;
        Gtk::TreeModelColumn<Glib::ustring> bordercolor;
        Gtk::TreeModelColumn<bool> shadow;
        Gtk::TreeModelColumn<Glib::ustring> deskcolor;
};

class ThemeCols: public Gtk::TreeModel::ColumnRecord {
    public:
        // These types must match those for the model in the .glade file
        ThemeCols() {
            this->add(this->id);
            this->add(this->name);
            this->add(this->theme);
            this->add(this->icons);
            this->add(this->base);
            this->add(this->base_dark);
            this->add(this->success);
            this->add(this->warn);
            this->add(this->error);
            this->add(this->symbolic);
            this->add(this->smallicons);
            this->add(this->enabled);
        }
        Gtk::TreeModelColumn<Glib::ustring> id;
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> theme;
        Gtk::TreeModelColumn<Glib::ustring> icons;
        Gtk::TreeModelColumn<Glib::ustring> base;
        Gtk::TreeModelColumn<Glib::ustring> base_dark;
        Gtk::TreeModelColumn<Glib::ustring> success;
        Gtk::TreeModelColumn<Glib::ustring> warn;
        Gtk::TreeModelColumn<Glib::ustring> error;
        Gtk::TreeModelColumn<bool> symbolic;
        Gtk::TreeModelColumn<bool> smallicons;
        Gtk::TreeModelColumn<bool> enabled;
};

using Inkscape::UI::Widget::TemplateList;

StartScreen::StartScreen()
    : opt_shown(std::string("/options/boot/shown/ver") + Inkscape::version_string_without_revision)
    , build_splash(create_builder("inkscape-splash.glade"))
    // Global widgets
    , banners{get_widget<Gtk::WindowHandle>(build_splash, "banner")}
    , close_btn{get_widget<Gtk::Button>(build_splash, "close_window")}
    , messages{get_widget<Gtk::Label>(build_splash, "messages")}
{
    set_name("start-screen-window");
    set_title("PROOF");
    set_focusable(true);
    set_receives_default(true);
    set_default_widget(*this);
    set_modal(true);

    // Move banner to dialog window
    set_titlebar(banners);

    set_child(_box);
    _box.append(messages);

    set_default_size(700, 360);
    messages.hide();

    build_welcome = create_builder("inkscape-welcome.glade");

    // Populate with template extensions
    templates.init(Inkscape::Extension::TEMPLATE_NEW_WELCOME, TemplateList::All, true);

    recentfiles = &get_widget<Gtk::TreeView>(build_welcome, "recent_treeview");

    auto tabs = &get_widget<Gtk::Notebook>(build_welcome, "tabs");
    _box.append(*tabs);

    // Get references to various widget used locally. (In order of appearance.)
    auto canvas      = &get_widget<Gtk::ComboBox>    (build_welcome, "canvas");
    auto themes      = &get_widget<Gtk::ComboBox>    (build_welcome, "themes");
    auto keys        = &get_widget<Gtk::ComboBox>    (build_welcome, "keys");
    auto kinds       = &get_widget<Gtk::Notebook>    (build_welcome, "kinds");
    auto save        = &get_widget<Gtk::Button>      (build_welcome, "save");
    auto thanks      = &get_widget<Gtk::Button>      (build_welcome, "thanks");
    auto load_btn    = &get_widget<Gtk::Button>      (build_welcome, "load");
    auto new_btn     = &get_widget<Gtk::Button>      (build_welcome, "new");
    auto show_toggle = &get_widget<Gtk::CheckButton> (build_welcome, "show_toggle");
    auto dark_toggle = &get_widget<Gtk::Switch>      (build_welcome, "dark_toggle");

    // Add signals and setup things.
    auto prefs = Inkscape::Preferences::get();

    auto const key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(sigc::mem_fun(*this, &StartScreen::on_key_pressed), true);
    add_controller(key);

    _tabs_switch_page_conn = tabs->signal_switch_page().connect([this](Gtk::Widget *tab, unsigned page_num) {
        banner_switch(page_num);
    });

    // Setup the lists of items
    enlist_recent_files();
    enlist_keys();
    filter_themes(themes);
    set_active_combo("themes", prefs->getString("/options/boot/theme"));
    set_active_combo("canvas", prefs->getString("/options/boot/canvas"));

    // initialise dark depending on prefs and background
    refresh_dark_switch();

    // Load pictures. Gtk::Picture doesn't appear to be able to load image files from builder files.
    // PROOF: one banner of its own while loading and above the setup and the files tabs. It has no
    // words, so it needs no translated text over it. The tab that asks for support of the Inkscape
    // project keeps Inkscape's picture.
    auto const start_welcome_file  = Resource::get_filename(Resource::SCREENS, "proof-banner.png");
    auto const start_support_file  = Resource::get_filename(Resource::SCREENS, "start-support.png");
    auto const start_splash_file   = start_welcome_file;
    auto const start_support_time  = Resource::get_filename(Resource::SCREENS, "start-support-time.png");
    auto const start_support_money = Resource::get_filename(Resource::SCREENS, "start-support-money.png");

    get_widget<Gtk::Picture>(build_splash, "start-welcome" ).set_filename(start_welcome_file);
    get_widget<Gtk::Picture>(build_splash, "start-support" ).set_filename(start_support_file);
    get_widget<Gtk::Picture>(build_splash, "start-splash"  ).set_filename(start_splash_file);
    get_widget<Gtk::Picture>(build_welcome, "start-support-time" ).set_filename(start_support_time);
    get_widget<Gtk::Picture>(build_welcome, "start-support-money").set_filename(start_support_money);

    // Welcome! tab
    canvas->signal_changed().connect(sigc::mem_fun(*this, &StartScreen::canvas_changed));
    keys->signal_changed().connect(sigc::mem_fun(*this, &StartScreen::keyboard_changed));
    themes->signal_changed().connect(sigc::mem_fun(*this, &StartScreen::theme_changed));
    dark_toggle->property_active().signal_changed().connect(sigc::mem_fun(*this, &StartScreen::theme_changed));
    save->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &StartScreen::notebook_next), save));

    // "Supported by You" tab
    thanks->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &StartScreen::notebook_next), thanks));

    // "Time to Draw" tab
    recentfiles->signal_row_activated().connect(sigc::hide(sigc::hide((sigc::mem_fun(*this, &StartScreen::load_document)))));
    recentfiles->get_selection()->signal_changed().connect(sigc::mem_fun(*this, &StartScreen::on_recent_changed));
    _templates_switch_page_conn = templates.signal_switch_page().connect(sigc::mem_fun(*this, &StartScreen::on_kind_changed));
    load_btn->set_sensitive(true);

    show_toggle->signal_toggled().connect(sigc::mem_fun(*this, &StartScreen::show_toggle));
    load_btn->signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::load_document));
    templates.connectItemSelected([this](int){ new_document(); });
    new_btn->signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::new_document));
    close_btn.signal_clicked().connect([this] { close(); });

    // move pages from stack to our notebook
    for (auto cat : templates.get_categories()) {
        if (auto page = templates.get_child_by_name(cat)) {
            page->reference();
            templates.remove(*page);
            kinds->append_page(*page, templates.get_category_label(cat));
            page->unreference();
        }
    }
    kinds->signal_switch_page().connect([this](Gtk::Widget* page, auto) {
        templates.reset_selection(page);
    });

    // Show the first tab ONLY on the first run for this version
    std::string opt_shown = "/options/boot/shown/ver";
    opt_shown += Inkscape::version_string_without_revision;
    if (!prefs->getBool(opt_shown, false)) {
        theme_changed();
        tabs->set_current_page(0);
        prefs->setBool(opt_shown, true);
    } else {
        tabs->set_current_page(2);
    }
    // Refresh keyboard warning message
    refresh_keys_warning();

    show();
}

/**
 * Return the active row of the named combo box.
 *
 * @param widget_name - The name of the widget in the glade file
 * @return Gtk Row object ready for use.
 * @throws Two errors depending on where it failed.
 */
Gtk::TreeModel::Row
StartScreen::active_combo(std::string widget_name)
{
    auto &combo = get_widget<Gtk::ComboBox>(build_welcome, widget_name.c_str());
    Gtk::TreeModel::iterator iter = combo.get_active();
    if (!iter) throw 2;
    Gtk::TreeModel::Row row = *iter;
    if (!row) throw 3;
    return row;
}

/**
 * Set the active item in the combo based on the unique_id (column set in glade)
 *
 * @param widget_name - The name of the widget in the glade file
 * @param unique_id - The column id to activate, sets to first item if blank.
 */
void
StartScreen::set_active_combo(std::string widget_name, std::string unique_id)
{
    auto &combo = get_widget<Gtk::ComboBox>(build_welcome, widget_name.c_str());
    if (unique_id.empty()) {
        combo.set_active(0); // Select the first
    } else if (!combo.set_active_id(unique_id)) {
        combo.set_active(-1); // Select nothing
    }
}

/**
 * When a notbook is switched, reveal the right banner image (gtk signal).
 */
void
StartScreen::banner_switch(unsigned page_num)
{
    auto &stack = get_widget<Gtk::Stack>(build_splash, "banner-stack");
    auto page = get_nth_child(stack, page_num);
    stack.set_visible_child(*page);
}

void
StartScreen::enlist_recent_files()
{
    RecentCols cols;

    auto store = &dynamic_cast<Gtk::ListStore &>(*recentfiles->get_model());
    store->clear();
    // Now sort the result by visited time
    store->set_sort_column(cols.col_dt, Gtk::SortType::DESCENDING);

    // Open [other]
    Gtk::TreeModel::Row first_row = *(store->append());
    first_row[cols.col_name] = _("Browse for other files...");
    first_row[cols.col_id] = "";
    first_row[cols.col_dt] = std::numeric_limits<gint64>::max();
    recentfiles->get_selection()->select(store->get_path(first_row.get_iter()));

    auto recent_files = Inkscape::IO::getInkscapeRecentFiles();
    auto shortened_path_map = Inkscape::IO::getShortenedPathMap(recent_files);

    for (auto const &recent_file : recent_files) {
        // This uri is a GVFS uri, so parse it with that or it will fail.
        auto file = Gio::File::create_for_uri(recent_file->get_uri());
        std::string path = file->get_path();
        // Note: Do not check if the file exists, to avoid long delays. See https://gitlab.com/inkscape/inkscape/-/issues/2348 .
        if (!path.empty() && recent_file->get_mime_type() == "image/svg+xml") {
            Gtk::TreeModel::Row row = *(store->append());
            row[cols.col_name] = shortened_path_map[recent_file->get_uri_display()];
            row[cols.col_id] = recent_file->get_uri();
            row[cols.col_dt] = recent_file->get_modified().to_unix();
            row[cols.col_crash] = recent_file->has_group("Crash");
        }
    }
}

/**
 * Called when a new recent document is selected.
 */
void
StartScreen::on_recent_changed()
{
    // TODO: In the future this is where previews and other information can be loaded.
}

/**
 * Called when the left side tabs are changed.
 */
void StartScreen::on_kind_changed(const Glib::ustring& name)
{
    auto load_btn = &get_widget<Gtk::Button>(build_welcome, "load");
    load_btn->set_visible(name == "???");
}

/**
 * Called when new button clicked or template is double clicked, or escape pressed.
 */
void StartScreen::new_document()
{
    // Generate a new document from the selected template.
    if (auto document = get_template_document()) {
        // Quit welcome screen if options not cancelled
        _finish(document);
    }
}

/**
 * Return the template document from the kinds widget.
 */
SPDocument *
StartScreen::get_template_document()
{
    auto kinds = &get_widget<Gtk::Notebook>(build_welcome, "kinds");
    return templates.new_document(kinds->get_nth_page(kinds->get_current_page()));
}

/**
 * Called when load button clicked.
 */
void
StartScreen::load_document()
{
    RecentCols cols;
    auto app = InkscapeApplication::instance();

    auto iter = recentfiles->get_selection()->get_selected();
    if (iter) {
        Gtk::TreeModel::Row row = *iter;
        if (row) {
            Glib::ustring uri = row[cols.col_id];
            Glib::RefPtr<Gio::File> file;

            if (!uri.empty()) {
                file = Gio::File::create_for_uri(uri);
            } else {
                // Browse for file instead
                std::string open_path;
                get_start_directory(open_path, "/dialogs/open/path");

                std::string current_folder;
                get_start_directory(current_folder, "/dialogs/open/path");

                auto filters = create_open_filters();
                file = choose_file_open(_("Open a different file"), this, filters, current_folder);

                if (!file) {
                    return; // Cancel
                }

                auto prefs = Inkscape::Preferences::get();
                prefs->setString("/dialogs/open/path", current_folder);
            }

            // Now we have file, open document.
            app->create_window(file);
            if (auto document = app->get_active_document()) {
                _finish(document);
            }
        }
    }
}

/**
 * When a button needs to go to the next notebook page.
 */
void
StartScreen::notebook_next(Gtk::Widget *button)
{
    auto tabs = &get_widget<Gtk::Notebook>(build_welcome, "tabs");
    int page = tabs->get_current_page();
    if (page == 2) {
        _finish(nullptr); // Only occurs from keypress.
    } else {
        tabs->set_current_page(page + 1);
    }
}

/**
 * When a key is pressed in the main window.
 */
bool StartScreen::on_key_pressed(unsigned keyval, unsigned /*keycode*/, Gdk::ModifierType state)
{
#ifdef __APPLE__
    // On macOS only, if user press Cmd+Q => exit
    if (keyval == 'q' && state == Gdk::ModifierType::META_MASK) {
        close();
        return false;
    }
#endif

    switch (keyval) {
        case GDK_KEY_Escape:
            // Prevent loading any selected items
            _finish(nullptr);
            return true;
        case GDK_KEY_Return:
            notebook_next(nullptr);
            return true;
    }

    return false;
}

void StartScreen::_finish(SPDocument *document)
{
    _signal_open.emit(document);
    close(); // Caution: Typically deletes self.
}

/**
 * Get the preference for the startup mode.
 *
 * @returns
 *    0 - Show nothing
 *    1 = Show the startup screen
 */
int StartScreen::get_start_mode()
{
    auto prefs = Inkscape::Preferences::get();
    auto old_enabled = prefs->getBool("/options/boot/enabled", true);
    return prefs->getInt("/options/boot/mode", old_enabled ? 1 : 0);
}

void
StartScreen::show_toggle()
{
    auto &button = get_widget<Gtk::CheckButton>(build_welcome, "show_toggle");
    auto prefs = Inkscape::Preferences::get();
    prefs->setInt("/options/boot/mode", button.get_active() ? 1 : 0);
}

/**
 * Refresh theme in-place so user can see a semi-preview. This theme selection
 * is not meant to be perfect, but hint to the user that they can set the
 * theme if they want.
 *
 * @param theme_name - The name of the theme to load.
 */
void
StartScreen::refresh_theme(Glib::ustring theme_name)
{
    auto const display = Gdk::Display::get_default();

    if (INKSCAPE.themecontext->getContrastThemeProvider()) {
        Gtk::StyleProvider::remove_provider_for_display(display, INKSCAPE.themecontext->getContrastThemeProvider());
    }

    auto settings = Gtk::Settings::get_default();
    auto prefs = Inkscape::Preferences::get();
    auto preferDarkTheme = prefs->getBool("/theme/preferDarkTheme", true);

    settings->property_gtk_theme_name() = theme_name;
#if GTKMM_CHECK_VERSION(4, 20, 0)
    settings->property_gtk_interface_color_scheme() =
        preferDarkTheme ? Gtk::InterfaceColorScheme::DARK : Gtk::InterfaceColorScheme::LIGHT;
#else
    settings->property_gtk_application_prefer_dark_theme() = preferDarkTheme;
#endif
    settings->property_gtk_icon_theme_name() = prefs->getString("/theme/iconTheme", prefs->getString("/theme/defaultIconTheme", ""));

    if (prefs->getBool("/theme/symbolicIcons", false)) {
        add_css_class("symbolic");
        remove_css_class("regular");
    } else {
        add_css_class("regular");
        remove_css_class("symbolic");
    }

    if (INKSCAPE.themecontext->getColorizeProvider()) {
        Gtk::StyleProvider::remove_provider_for_display(display, INKSCAPE.themecontext->getColorizeProvider());
    }

    if (!prefs->getBool("/theme/symbolicDefaultHighColors", false)) {
        Gtk::CssProvider::create();
        Glib::ustring css_str = INKSCAPE.themecontext->get_symbolic_colors();
        try {
            INKSCAPE.themecontext->getColorizeProvider()->load_from_string(css_str);
        } catch (Gtk::CssParserError const &ex) {
            g_critical("CSSProviderError::load_from_data(): failed to load '%s'\n(%s)", css_str.c_str(), ex.what());
        }
        Gtk::StyleProvider::add_provider_for_display(display, INKSCAPE.themecontext->getColorizeProvider(),
                                                     GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    // set dark switch and disable if there is no prefer option for dark
    refresh_dark_switch();

    INKSCAPE.themecontext->getChangeThemeSignal().emit();
}

/**
 * Set the theme, icon pack and other theme options from a set defined
 * in the glade file. The combo box has a number of columns with the needed
 * data describing how to set up the theme.
 */
void
StartScreen::theme_changed()
{
    auto prefs = Inkscape::Preferences::get();

    ThemeCols cols;
    try {
        auto row = active_combo("themes");
        Glib::ustring theme_id = row[cols.id];
        if (theme_id == "custom") return;
        prefs->setString("/options/boot/theme", row[cols.id]);

        // Update theme from combo.
        Glib::ustring icons = row[cols.icons];
        prefs->setBool("/toolbox/tools/small", row[cols.smallicons]);
        prefs->setString("/theme/gtkTheme", row[cols.theme]);
        prefs->setString("/theme/iconTheme", icons);
        prefs->setBool("/theme/symbolicIcons", row[cols.symbolic]);

        auto &dark_toggle = get_widget<Gtk::Switch>(build_welcome, "dark_toggle");
        bool is_dark = dark_toggle.get_active();
        prefs->setBool("/theme/preferDarkTheme", is_dark);
        prefs->setBool("/theme/darkTheme", is_dark);
        // Symbolic icon colours
        if (!Colors::Color::parse((Glib::ustring)row[cols.base])) {
            prefs->setBool("/theme/symbolicDefaultBaseColors", true);
        } else {
            Glib::ustring prefix = "/theme/" + icons;
            prefs->setBool("/theme/symbolicDefaultBaseColors", false);
            auto base_column = is_dark ? cols.base_dark : cols.base;
            prefs->setColor(prefix + "/symbolicBaseColor", row[base_column]);
            prefs->setColor(prefix + "/symbolicSuccessColor", row[cols.success]);
            prefs->setColor(prefix + "/symbolicWarningColor", row[cols.warn]);
            prefs->setColor(prefix + "/symbolicErrorColor", row[cols.error]);
        }

        refresh_theme(prefs->getString("/theme/gtkTheme", prefs->getString("/theme/defaultGtkTheme", "")));
    } catch(int e) {
        g_warning("Couldn't find theme value.");
    }
}

/**
 * Called when the canvas dropdown changes.
 */
void
StartScreen::canvas_changed()
{
    CanvasCols cols;
    try {
        auto row = active_combo("canvas");

        auto prefs = Inkscape::Preferences::get();
        prefs->setString("/options/boot/canvas", row[cols.id]);

        Gdk::RGBA gdk_color = Gdk::RGBA(row[cols.pagecolor]);
        prefs->setString("/template/base/pagecolor", gdk_to_css_color(gdk_color));
        prefs->setDouble("/template/base/pageopacity", gdk_color.get_alpha());

        Gdk::RGBA gdk_border = Gdk::RGBA(row[cols.bordercolor]);
        prefs->setString("/template/base/bordercolor", gdk_to_css_color(gdk_border));
        prefs->setDouble("/template/base/borderopacity", gdk_border.get_alpha());

        prefs->setBool("/template/base/pagecheckerboard", row[cols.checkered]);
        prefs->setInt("/template/base/pageshadow", row[cols.shadow] ? 2 : 0);

        Gdk::RGBA gdk_desk = Gdk::RGBA(row[cols.deskcolor]);
        prefs->setString("/template/base/deskcolor", gdk_to_css_color(gdk_desk));
    } catch(int e) {
        g_warning("Couldn't find canvas value.");
    }
}

// Enable themes which are available
void StartScreen::filter_themes(Gtk::ComboBox *themes)
{
    ThemeCols cols;
    auto store = &dynamic_cast<Gtk::ListStore &>(*themes->get_model());
    auto available = INKSCAPE.themecontext->get_available_themes();

    for(auto row : store->children()) {
        if (!row[cols.enabled]) {
            // Available themes; We only "enable" them, we don't disable them.
            row[cols.enabled] = available.find(row[cols.theme]) != available.end();
        }
    }
}

void
StartScreen::refresh_keys_warning()
{
    auto prefs = Inkscape::Preferences::get();
    auto current_file = prefs->getString("/options/kbshortcuts/shortcutfile", "default.xml");
    auto &keys_warning = get_widget<Gtk::InfoBar>(build_welcome, "keys_warning");
    if (current_file != "inkscape.xml" && current_file != "default.xml") {
        keys_warning.set_visible(true);
    } else {
        keys_warning.set_message_type(Gtk::MessageType::WARNING);
        keys_warning.set_visible(false);
    }
}

void
StartScreen::enlist_keys()
{
    NameIdCols cols;
    auto &keys = get_widget<Gtk::ComboBox>(build_welcome, "keys");

    auto store = &dynamic_cast<Gtk::ListStore &>(*keys.get_model());
    store->clear();

    for (auto const &item : Inkscape::Shortcuts::get_file_names()) {
        Gtk::TreeModel::Row row = *(store->append());
        row[cols.col_name] = item.first;
        row[cols.col_id] = item.second;
    }

    auto prefs = Inkscape::Preferences::get();
    auto current = prefs->getString("/options/kbshortcuts/shortcutfile");
    if (current.empty()) {
        // PROOF: default.xml is what is read when no file is chosen, and it is PROOF's own keys.
        current = "default.xml";
    }
    keys.set_active_id(current);
}

/**
 * Set the keys file based on the keys set in the enlist above
 */
void
StartScreen::keyboard_changed()
{
    NameIdCols cols;
    auto row = active_combo("keys");
    auto prefs = Inkscape::Preferences::get();
    Glib::ustring set_to = row[cols.col_id];
    prefs->setString("/options/kbshortcuts/shortcutfile", set_to);
    refresh_keys_warning();
}

/**
 * Set current state of Dark Switch based on current selected theme.
 */

void StartScreen::refresh_dark_switch()
{
    auto prefs = Inkscape::Preferences::get();

    auto const window = dynamic_cast<Gtk::Window *>(get_root());
    bool dark = INKSCAPE.themecontext->isCurrentThemeDark(window);
    prefs->setBool("/theme/preferDarkTheme", dark);
    prefs->setBool("/theme/darkTheme", dark);

    auto themes = INKSCAPE.themecontext->get_available_themes();
    Glib::ustring current_theme = prefs->getString("/theme/gtkTheme", prefs->getString("/theme/defaultGtkTheme", ""));

    auto &dark_toggle = get_widget<Gtk::Switch>(build_welcome, "dark_toggle");
    dark_toggle.set_active(dark);
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
