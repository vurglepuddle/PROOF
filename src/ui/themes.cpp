// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Gtk <themes> helper code.
 */
/*
 * Authors:
 *   Jabiertxof
 *   Martin Owens
 *
 * Copyright (C) 2017-2021 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "themes.h"

#include <regex>
#include <glibmm/miscutils.h>
#include <glibmm/regex.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/settings.h>
#include <gtkmm/version.h>
#include <pangomm/fontdescription.h>

#include "config.h"
#include "desktop.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "io/resource.h"
#include "object/sp-item-group.h" // set_default_highlight_colors
#include "svg/css-ostringstream.h"
#include "ui/dialog/dialog-manager.h"
#include "ui/dialog/dialog-window.h"
#include "ui/util.h"
#include "util-string/ustring-format.h"

#if WITH_GSOURCEVIEW
#   include <gtksourceview/gtksource.h>
#endif

#ifdef __APPLE__
#include <gdk/macos/gdkmacos.h>
#endif

namespace Inkscape::UI {

ThemeContext::ThemeContext()
{ }

ThemeContext::~ThemeContext() = default;

/**
 * Inkscape fill gtk, taken from glib/gtk code with our own checks.
 */
void ThemeContext::inkscape_fill_gtk(const gchar *path, gtkThemeList &themes)
{
    const gchar *dir_entry;
    GDir *dir = g_dir_open(path, 0, nullptr);
    if (!dir)
        return;
    while ((dir_entry = g_dir_read_name(dir))) {
        gchar *filename = g_build_filename(path, dir_entry, "gtk-4.0", "gtk.css", nullptr);
        bool has_prefer_dark = false;
  
        Glib::ustring theme = dir_entry;
        gchar *filenamedark = g_build_filename(path, dir_entry, "gtk-4.0", "gtk-dark.css", nullptr);
        if (g_file_test(filenamedark, G_FILE_TEST_IS_REGULAR))
            has_prefer_dark = true;
        if (themes.find(theme) != themes.end() && !has_prefer_dark) {
            continue;
        }
        if (g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
            themes[theme] = has_prefer_dark;
        }
        g_free(filename);
        g_free(filenamedark);
    }
  
    g_dir_close(dir);
}

/**
 * Get available themes based on locations of gtk directories.
 */
std::map<Glib::ustring, bool> 
ThemeContext::get_available_themes()
{
    // NOTE: This function tries to mimic what _gtk_css_find_theme in gtk4 does to locate a theme;
    // that is we traverse resources and then certain directories looking for themes.
    // We only gather theme names as this is what's saved in gtk settings to select a UI theme.
    // gtk4 will load a theme based solely on its name searching for it in a list of folders (that we cannot change).

    gtkThemeList themes;
    Glib::ustring theme = "";
    std::string path;
    gchar **builtin_themes;
    guint i, j;
    const gchar *const *dirs;
  
    /* Builtin themes */
    builtin_themes = g_resources_enumerate_children("/org/gtk/libgtk/theme", G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    for (i = 0; builtin_themes[i] != NULL; i++) {
        if (g_str_has_suffix(builtin_themes[i], "/")) {
            theme = builtin_themes[i];
            theme.resize(theme.size() - 1);
            Glib::ustring theme_path = "/org/gtk/libgtk/theme";
            theme_path += "/" + theme;
            gchar **builtin_themes_files =
                g_resources_enumerate_children(theme_path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
            bool has_prefer_dark = false;
            if (builtin_themes_files != NULL) {
                for (j = 0; builtin_themes_files[j] != NULL; j++) {
                    Glib::ustring file = builtin_themes_files[j];
                    if (file == "gtk-dark.css") {
                        has_prefer_dark = true;
                    }
                }
            }
            g_strfreev(builtin_themes_files);
            themes[theme] = has_prefer_dark;
        }
    }

    g_strfreev(builtin_themes);

    path = Glib::build_filename(g_get_user_data_dir(), "themes", nullptr);
    inkscape_fill_gtk(path.c_str(), themes);
  
    path = Glib::build_filename(g_get_home_dir(), ".themes", nullptr);
    inkscape_fill_gtk(path.c_str(), themes);
  
    dirs = g_get_system_data_dirs();
    for (i = 0; dirs[i]; i++) {
        path = Glib::build_filename(dirs[i], "themes", nullptr);
        inkscape_fill_gtk(path.c_str(), themes);
    }
    return themes;
}

Glib::ustring 
ThemeContext::get_symbolic_colors()
{
    Glib::ustring css_str;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Glib::ustring themeiconname = prefs->getString("/theme/iconTheme", prefs->getString("/theme/defaultIconTheme", ""));
    auto colorsetbase = prefs->getColor("/theme/" + themeiconname + "/symbolicBaseColor", "#2e3436");
    auto colorsetsuccess = prefs->getColor("/theme/" + themeiconname + "/symbolicSuccessColor", "#4ad589");
    auto colorsetwarning = prefs->getColor("/theme/" + themeiconname + "/symbolicWarningColor", "#f57900");
    auto colorseterror = prefs->getColor("/theme/" + themeiconname + "/symbolicErrorColor", "#cc0000");
    css_str += "@define-color warning_color " + colorsetwarning.toString() + ";\n";
    css_str += "@define-color error_color " + colorseterror.toString() + ";\n";
    css_str += "@define-color success_color " + colorsetsuccess.toString() + ";\n";
    /* ":not(.rawstyle) > image" works only on images in first level of widget container
    if in the future we use a complex widget with more levels and we dont want to tweak the color
    here, retaining default we can add more lines like ":not(.rawstyle) > > image" 
    if we not override the color we use defautt theme colors*/
    bool overridebasecolor = !prefs->getBool("/theme/symbolicDefaultBaseColors", true);
    if (overridebasecolor) {
        css_str += "#InkRuler:not(.shadow):not(.page):not(.selection),";
        css_str += ":not(.rawstyle) > image:not(.arrow),";
        css_str += ":not(.rawstyle) treeview.image";
        css_str += "{color:";
        css_str += colorsetbase.toString();
        css_str += ";}";
    }
    css_str += ".dark .forcebright :not(.rawstyle) > image,";
    css_str += ".dark .forcebright image:not(.rawstyle),";
    css_str += ".bright .forcedark :not(.rawstyle) > image,";
    css_str += ".bright .forcedark image:not(.rawstyle),";
    css_str += ".dark :not(.rawstyle) > image.forcebright,";
    css_str += ".dark image.forcebright:not(.rawstyle),";
    css_str += ".bright :not(.rawstyle) > image.forcedark,";
    css_str += ".bright image.forcedark:not(.rawstyle),";
    css_str += ".inverse :not(.rawstyle) > image,";
    css_str += ".inverse image:not(.rawstyle)";
    css_str += "{color:";
    if (overridebasecolor) {
        auto colorsetbase_inverse = colorsetbase;
        colorsetbase_inverse.invert();
        css_str += colorsetbase_inverse.toString();
    } else {
        // we override base color in this special cases using inverse color
        css_str += "@theme_bg_color";
    }
    css_str += ";}";
    return css_str;
}

std::string sp_tweak_background_colors(std::string cssstring, double crossfade, double contrast, bool dark)
{
    static std::regex re_no_affect("(inherit|unset|initial|none|url)");
    static std::regex re_color("background-color( ){0,3}:(.*?);");
    static std::regex re_image("background-image( ){0,3}:(.*?\\)) *?;");
    std::string sub = "";
    std::smatch m;
    std::regex_search(cssstring, m, re_no_affect);
    if (m.size() == 0) {
        if (cssstring.find("background-color") != std::string::npos) {
            sub = "background-color:shade($2," + Inkscape::ustring::format_classic(crossfade) + ");";
            cssstring = std::regex_replace(cssstring, re_color, sub);
        } else if (cssstring.find("background-image") != std::string::npos) {
            if (dark) {
                contrast = std::clamp((int)((contrast) * 27), 0, 100);
                sub = "background-image:cross-fade(" + Inkscape::ustring::format_classic(contrast) + "% image(rgb(255,255,255)), $2);";
            } else {
                contrast = std::clamp((int)((contrast) * 90), 0 , 100);
                sub = "background-image:cross-fade(" + Inkscape::ustring::format_classic(contrast) + "% image(rgb(0,0,0)), $2);";
            }
            cssstring = std::regex_replace(cssstring, re_image, sub);
        }
    } else {
        cssstring = "";
    }
    return cssstring;
}

static void
show_parsing_error(const Glib::RefPtr<const Gtk::CssSection>& section, const Glib::Error& error)
{
#ifndef NDEBUG
  g_warning("There is a warning parsing theme CSS:: %s", error.what());
#endif
}

// callback for a "narrow spinbutton" preference change
struct NarrowSpinbuttonObserver : Preferences::Observer {
    NarrowSpinbuttonObserver(const char* path, Glib::RefPtr<Gtk::CssProvider> provider):
        Preferences::Observer(path), _provider(std::move(provider)) {}

    void notify(Preferences::Entry const& new_val) override {
        auto const display = Gdk::Display::get_default();
        if (new_val.getBool()) {
            Gtk::StyleProvider::add_provider_for_display(display, _provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        else {
            Gtk::StyleProvider::remove_provider_for_display(display, _provider);
        }
    }

    Glib::RefPtr<Gtk::CssProvider> _provider;
};

/**
 * \brief Add our CSS style sheets
 * @param only_providers: Apply only the providers part, from inkscape preferences::theme change, no need to reaply
 */
void ThemeContext::add_gtk_css(bool only_providers, bool cached)
{
    using namespace Inkscape::IO::Resource;
    // Add style sheet (GTK3)
    auto const display = Gdk::Display::get_default();
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Glib::ustring themeiconname;
    GtkSettings *settings = gtk_settings_get_default();
    if (settings && !only_providers) {
        gchar *gtkThemeName = nullptr;
        gchar *gtkIconThemeName = nullptr;
        gboolean gtkApplicationPreferDarkTheme = false;

        g_object_get(settings, "gtk-icon-theme-name", &gtkIconThemeName, nullptr);
        g_object_get(settings, "gtk-theme-name", &gtkThemeName, nullptr);
        g_object_get(settings, "gtk-application-prefer-dark-theme", &gtkApplicationPreferDarkTheme, nullptr);
        prefs->setBool("/theme/defaultPreferDarkTheme", gtkApplicationPreferDarkTheme);
        prefs->setString("/theme/defaultGtkTheme", Glib::ustring(gtkThemeName ? gtkThemeName : ""));
        prefs->setString("/theme/defaultIconTheme", Glib::ustring(gtkIconThemeName ? gtkIconThemeName : ""));
        Glib::ustring gtkthemename = prefs->getString("/theme/gtkTheme");
        if (gtkthemename != "") {
            g_object_set(settings, "gtk-theme-name", gtkthemename.c_str(), nullptr);
        }
        bool preferdarktheme = prefs->getBool("/theme/preferDarkTheme", false);
        g_object_set(settings, "gtk-application-prefer-dark-theme", preferdarktheme, nullptr);
        themeiconname = prefs->getString("/theme/iconTheme");
        if (themeiconname != "") {
            g_object_set(settings, "gtk-icon-theme-name", themeiconname.c_str(), nullptr);
        }

        g_free(gtkThemeName);
        g_free(gtkIconThemeName);
    }


    int themecontrast = prefs->getInt("/theme/contrast", 10);
    if (!_contrastthemeprovider) {
        _contrastthemeprovider = Gtk::CssProvider::create();
        // We can uncomment this line to remove warnings and errors on the theme
        _contrastthemeprovider->signal_parsing_error().connect(sigc::ptr_fun(show_parsing_error));
    }
    static std::string cssstringcached = "";
    // we use contrast only if is setup (!= 10)
    if (themecontrast < 10) {
        Glib::ustring css_contrast = "";
        double contrast = (10 - themecontrast) / 30.0;
        double shade = 1 - contrast;
        bool dark = prefs->getBool("/theme/darkTheme", false);
        const gchar *variant = dark ? "dark" : "";
        if (dark) {
            contrast *= 2.5;
            shade = 1 + contrast;
        }
        Glib::ustring current_theme = prefs->getString("/theme/gtkTheme", prefs->getString("/theme/defaultGtkTheme", ""));
        
        std::string cssstring = "";
        if (cached && !cssstringcached.empty()) {
            cssstring = cssstringcached;    
        } else {
            auto current_themeprovider = Gtk::CssProvider::create();
            current_themeprovider->load_named(current_theme, variant);
            cssstring = current_themeprovider->to_string();
        }
        if (contrast) {
            std::string cssdefined = ""; 
            // we do this way to fix issue Inkscape#2345
            // windows seem crash if text length > 2000;
            
            std::istringstream f(cssstring);
            std::string line;    
            while (std::getline(f, line)) {
                // here we ignore most of class to parse because is in additive mode
                // so stiles not applied are set on previous context style
                if (line.find(";") != std::string::npos &&
                    line.find("background-image") == std::string::npos &&
                    line.find("background-color") == std::string::npos)
                {
                    continue;
                }
                cssdefined += sp_tweak_background_colors(line, shade, contrast, dark);
                cssdefined += "\n";
                if (!cached) {
                    cssstringcached += line;
                    cssstringcached += "\n";
                }
            }
            if (!cached) {
                // Split on curly brackets. Even tokens are selectors, odd are values.
                std::vector<Glib::ustring> tokens = Glib::Regex::split_simple("[}{]", cssstringcached.c_str()); // Must use c_str() as a std::string
                                                                                                                // cannot be converted converted directly
                                                                                                                // to a Glib::UStringView in Gtk4.

                cssstringcached = "";
                for (unsigned i = 0; i < tokens.size() - 1; i += 2) {
                    Glib::ustring selector = tokens[i];
                    Glib::ustring properties = "";
                    if ((i + 1) < tokens.size()) {
                        properties = tokens[i + 1];
                    }
                    if (properties.find(";") != Glib::ustring::npos) {
                        cssstringcached += selector;
                        cssstringcached += "{\n";
                        cssstringcached += properties;
                        cssstringcached += "}\n";
                    }
                }
            }
            cssstring = cssdefined;
        }
        if (!cssstring.empty()) {
            _contrastthemeprovider->load_from_data(cssstring);
            Gtk::StyleProvider::add_provider_for_display(display, _contrastthemeprovider, GTK_STYLE_PROVIDER_PRIORITY_SETTINGS);
        }
    } else {
        cssstringcached = "";
        if (_contrastthemeprovider) {
            Gtk::StyleProvider::remove_provider_for_display(display, _contrastthemeprovider);
        }
    } 
    Glib::ustring style = get_filename(UIS, "style.css");
    if (!style.empty()) {
        if (_styleprovider) {
            Gtk::StyleProvider::remove_provider_for_display(display, _styleprovider);
        }
        if (!_styleprovider) {
            _styleprovider = Gtk::CssProvider::create();
        }
        try {
            _styleprovider->load_from_path(style);
        } catch (const Gtk::CssParserError &ex) {
            g_critical("CSSProviderError::load_from_path(): failed to load '%s'\n(%s)", style.c_str(),
                       ex.what());
        }
        // note: priority higher than that of the theme, so we can override styles that not even higher specificity can patch
        Gtk::StyleProvider::add_provider_for_display(display, _styleprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    // load small CSS snippet to style spinbuttons by removing excessive padding
    if (!_spinbuttonprovider) {
        _spinbuttonprovider = Gtk::CssProvider::create();
        Glib::ustring style = get_filename(UIS, "spinbutton.css");
        if (!style.empty()) {
            try {
                _spinbuttonprovider->load_from_path(style);
            } catch (const Gtk::CssParserError &ex) {
                g_critical("CSSProviderError::load_from_path(): failed to load '%s'\n(%s)", style.c_str(), ex.what());
            }
        }
    }
    _spinbutton_observer = std::make_unique<NarrowSpinbuttonObserver>("/theme/narrowSpinButton", _spinbuttonprovider);
    // note: ideally we should remove the callback during destruction, but ThemeContext is never deleted
    prefs->addObserver(*_spinbutton_observer);
    // establish default value, so both this setting here and checkbox in preferences are in sync
    if (!prefs->getEntry(_spinbutton_observer->observed_path).isValidBool()) {
        prefs->setBool(_spinbutton_observer->observed_path, true);
    }
    _spinbutton_observer->notify(prefs->getEntry(_spinbutton_observer->observed_path));

    Glib::ustring gtkthemename = prefs->getString("/theme/gtkTheme", prefs->getString("/theme/defaultGtkTheme", ""));
    gtkthemename += ".css";
    style = get_filename(UIS, gtkthemename.c_str(), false, true);
    if (!style.empty()) {
        if (_themeprovider) {
            Gtk::StyleProvider::remove_provider_for_display(display, _themeprovider);
        }
        if (!_themeprovider) {
            _themeprovider = Gtk::CssProvider::create();
        }
        try {
            _themeprovider->load_from_path(style);
        } catch (const Gtk::CssParserError &ex) {
            g_critical("CSSProviderError::load_from_path(): failed to load '%s'\n(%s)", style.c_str(),
                       ex.what());
        }
        Gtk::StyleProvider::add_provider_for_display(display, _themeprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    if (!_colorizeprovider) {
        _colorizeprovider = Gtk::CssProvider::create();
    }
    Glib::ustring css_str = "";
    if (prefs->getBool("/theme/symbolicIcons", false)) {
        css_str = get_symbolic_colors();
    }
    try {
        _colorizeprovider->load_from_data(css_str);
    } catch (const Gtk::CssParserError &ex) {
        g_critical("CSSProviderError::load_from_data(): failed to load '%s'\n(%s)", css_str.c_str(), ex.what());
    }
    Gtk::StyleProvider::add_provider_for_display(display, _colorizeprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

#if __APPLE__
    Glib::ustring macstyle = get_filename(UIS, "mac.css");
    if (!macstyle.empty()) {
        if (_macstyleprovider) {
            Gtk::StyleProvider::remove_provider_for_display(display, _macstyleprovider);
        }
        if (!_macstyleprovider) {
            _macstyleprovider = Gtk::CssProvider::create();
        }
        try {
            _macstyleprovider->load_from_path(macstyle);
        } catch (const Gtk::CssParserError &ex) {
            g_critical("CSSProviderError::load_from_path(): failed to load '%s'\n(%s)", macstyle.c_str(), ex.what());
        }
        // increased priority to be able to override Inkscape style.css
        Gtk::StyleProvider::add_provider_for_display(display, _macstyleprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 20);
    }
#endif

    // PROOF: square corners everywhere, above the theme and style.css but below user.css.
    style = get_filename(UIS, "proof-square.css");
    if (!style.empty()) {
        static Glib::RefPtr<Gtk::CssProvider> _squareprovider;
        if (_squareprovider) {
            Gtk::StyleProvider::remove_provider_for_display(display, _squareprovider);
        }
        if (!_squareprovider) {
            _squareprovider = Gtk::CssProvider::create();
        }
        try {
            _squareprovider->load_from_path(style);
        } catch (const Gtk::CssParserError &ex) {
            g_critical("CSSProviderError::load_from_path(): failed to load '%s'\n(%s)", style.c_str(), ex.what());
        }
        Gtk::StyleProvider::add_provider_for_display(display, _squareprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    }

    style = get_filename(UIS, "user.css");
    if (!style.empty()) {
        if (_userprovider) {
            Gtk::StyleProvider::remove_provider_for_display(display, _userprovider);
        }
        if (!_userprovider) {
            _userprovider = Gtk::CssProvider::create();
        }
        try {
            _userprovider->load_from_path(style);
        } catch (const Gtk::CssParserError &ex) {
            g_critical("CSSProviderError::load_from_path(): failed to load '%s'\n(%s)", style.c_str(),
                       ex.what());
        }
        // increased priority to be able to override Inkscape style.css
        Gtk::StyleProvider::add_provider_for_display(display, _userprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 20);
    }

}

/**
 * Check if current applied theme is dark or not by looking at style context.
 * This is important to check system default theme is dark or not
 * It only return True for dark and False for Bright. It does not apply any
 * property other than preferDarkTheme, so theme should be set before calling
 * this function as it may otherwise return outdated result.
 */
bool ThemeContext::isCurrentThemeDark(Gtk::Window * const window)
{
    if (!window) return false;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Glib::ustring current_theme =
        prefs->getString("/theme/gtkTheme", prefs->getString("/theme/defaultGtkTheme", ""));

    if (auto const settings = Gtk::Settings::get_default()) {
        auto preferDarkTheme = prefs->getBool("/theme/preferDarkTheme", false);
#if GTKMM_CHECK_VERSION(4, 20, 0)
        settings->property_gtk_interface_color_scheme() =
            preferDarkTheme ? Gtk::InterfaceColorScheme::DARK : Gtk::InterfaceColorScheme::LIGHT;
#else
        settings->property_gtk_application_prefer_dark_theme() = preferDarkTheme;
#endif
    }

    auto dark = current_theme.find(":dark") != std::string::npos;

    // if theme is dark or we use contrast slider feature and have set preferDarkTheme we force the theme dark
    // and avoid color check, this fix a issue with low contrast themes bad switch of dark theme toggle
    dark = dark || (prefs->getInt("/theme/contrast", 10) != 10 && prefs->getBool("/theme/preferDarkTheme", false));
    if (dark) return true;

    // Otherwise, check the foreground color, and if that has luminance >= 50%, we conclude the theme is dark.
    // Note: Use @theme_fg_color, since currentColor might not be set or correct
    auto const rgba = get_color_with_class(*window, "theme_fg_color");
    dark = get_luminance(rgba) >= 0.5;
    return dark;
}

void 
ThemeContext::themechangecallback() {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    // sync "dark" class between app window and floating dialog windows to ensure that
    // CSS providers relying on it apply in dialog windows too
    auto dark = prefs->getBool("/theme/darkTheme", false);
    std::vector<Gtk::Window *> winds;
    for (auto wnd : Inkscape::UI::Dialog::DialogManager::singleton().get_all_floating_dialog_windows()) {
        winds.push_back(wnd);
    }
    for (auto desktop : INKSCAPE.get_desktops()) {
        if (desktop == SP_ACTIVE_DESKTOP) {
            winds.emplace_back(desktop->getInkscapeWindow());
        } else {
            winds.insert(winds.begin(), desktop->getInkscapeWindow());
        }
    }
    for (auto wnd : winds) {
        if (auto w = wnd->get_surface()) {
            set_dark_titlebar(w, dark);
        }
        if (dark) {
            wnd->add_css_class("dark");
            wnd->remove_css_class("bright");
        } else {
            wnd->add_css_class("bright");
            wnd->remove_css_class("dark");
        }
        if (prefs->getBool("/theme/symbolicIcons", false)) {
            wnd->add_css_class("symbolic");
            wnd->remove_css_class("regular");
        } else {
            wnd->add_css_class("regular");
            wnd->remove_css_class("symbolic");
        }
#if (defined (_WIN32) || defined (_WIN64))
        wnd->present();
#endif
    }

    // set default highlight colors (dark/light theme-specific)
    if (!winds.empty()) {
        set_default_highlight_colors(getHighlightColors(winds.front()));
    }

    // select default syntax coloring theme, if needed
    if (auto desktop = INKSCAPE.active_desktop()) {
        select_default_syntax_style(isCurrentThemeDark(desktop->getInkscapeWindow()));
    }
}

/**
 * Load the highlight colours from the current theme. If the theme changes
 * you can call this function again to refresh the list.
 */
std::vector<guint32> ThemeContext::getHighlightColors(Gtk::Window *window)
{
    std::vector<guint32> colors;
    if (!window) return colors;

    auto const child = window->get_child();
    if (!child) return colors;

    Glib::ustring name = "highlight-color-";

    for (int i = 1; i <= 8; ++i) {
        // The highlight colors will be attached to a GtkWidget
        // but it isn't neccessary to use this in the .css file.
        // N.B. We must use Window:child; Window itself gives a constant color.

        auto const css_class = name + std::to_string(i);
        child->add_css_class(css_class);

        auto const rgba = child->get_color();
        colors.push_back( to_guint32(rgba) );

        child->remove_css_class(css_class);
    }

    return colors;
}

// That's what Gtk Inspector does to determine 100% scale, i.e. "normal" dpi
static double get_dpi_ratio(Gdk::Display* display) {
#ifdef __APPLE__
    if (GDK_IS_MACOS_DISPLAY(display ? display->gobj() : nullptr)) {
        return 72.0 * 1024.0;
    }
#endif
    return 96.0 * 1024.0;
}

void ThemeContext::adjustGlobalFontScale(double factor) {
    if (factor < 0.1 || factor > 10) {
        g_warning("Invalid font scaling factor %f in ThemeContext::adjust_global_font_scale", factor);
        return;
    }

    if (auto settings = Gtk::Settings::get_default()) {
        auto display = Gdk::Display::get_default();
        auto normal_dpi = get_dpi_ratio(display.get());
        settings->property_gtk_xft_dpi().set_value(int(factor * normal_dpi));
    }
    else {
        g_warning("Cannot obtain application settings to change text scale.\n");
    }
}

void ThemeContext::initialize_source_syntax_styles() {
#if WITH_GSOURCEVIEW
    auto manager = gtk_source_style_scheme_manager_get_default();
    // to reset path: gtk_source_style_scheme_manager_set_search_path(manager, nullptr);
    auto themes = IO::Resource::get_path_string(IO::Resource::SYSTEM, IO::Resource::UIS, "syntax-themes");
    gtk_source_style_scheme_manager_prepend_search_path(manager, themes.c_str());
#endif
}

void ThemeContext::select_default_syntax_style(bool dark_theme)
{
#if WITH_GSOURCEVIEW
    auto prefs = Inkscape::Preferences::get();
    auto default_theme = prefs->getString("/theme/syntax-color-theme");
    auto light = "inkscape-light";
    auto dark = "inkscape-dark";
    if (default_theme.empty() || default_theme == light || default_theme == dark) {
        prefs->setString("/theme/syntax-color-theme", dark_theme ? dark : light);
    }
#endif
}

void ThemeContext::saveMonospacedFont(Pango::FontDescription desc)
{
    Preferences::get()->setString(get_monospaced_font_pref_path(), desc.to_string());
}

void ThemeContext::applyMonospacedFont(Pango::FontDescription desc) {
    auto display = Gdk::Display::get_default();
    // add monospace font selector
    if (_monofont_styleprovider) {
        Gtk::StyleProvider::remove_provider_for_display(display, _monofont_styleprovider);
    }
    else {
        _monofont_styleprovider = Gtk::CssProvider::create();
    }
    CSSOStringStream os;
    os.precision(3);
    os << ".mono-font {";
    os << "font-family: '" << desc.get_family() << "';";
    switch (desc.get_style()) {
    case Pango::Style::ITALIC:
        os << "font-style: italic;";
        break;
    case Pango::Style::OBLIQUE:
        os << "font-style: oblique;";
        break;
    }
    os << "font-weight: " << static_cast<int>(desc.get_weight()) << ";";
    double size = desc.get_size();
    // font size in root em units, so it reacts to the global font scaling
    os << "font-size: " << (desc.get_size_is_absolute() ? size : size / Pango::SCALE) / 13.0 << "rem;";
    os << "}";
    _monofont_styleprovider->load_from_data(os.str());

    Gtk::StyleProvider::add_provider_for_display(display, _monofont_styleprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

Pango::FontDescription ThemeContext::getMonospacedFont() const
{
    auto font = Preferences::get()->getString(get_monospaced_font_pref_path(), "Monospace 13");
    return Pango::FontDescription(font);
}

double ThemeContext::getFontScale() const
{
    return Preferences::get()->getDoubleLimited(get_font_scale_pref_path(), 100.0, 10.0, 500.0);
}

void ThemeContext::saveFontScale(double scale)
{
    Preferences::get()->setDouble(get_font_scale_pref_path(), scale);
}

} // namespace Inkscape::UI

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
