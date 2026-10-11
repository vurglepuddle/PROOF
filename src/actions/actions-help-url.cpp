// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 *
 *  Actions for Help Url
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

#include "actions-help-url.h"
#include "actions-helper.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "desktop.h"
#include "inkscape-version.h"

namespace {

/** Open an URL in the the default application
 *
 * See documentation of gtk_show_uri_on_window() for details
 *
 * @param url URL to be opened
 */
void help_open_url(Glib::ustring const &url)
{
    try {
        Gio::AppInfo::launch_default_for_uri(url);
    } catch (const Glib::Error &e) {
        g_warning("Unable to show '%s': %s", url.c_str(), e.what());
    }
}

void help_url_ask_question(InkscapeWindow *win, const char *lang)
{
    Glib::ustring url = Glib::ustring::compose("https://inkscape.org/%1/community/", lang);
    help_open_url(url);
}

void help_url_man(InkscapeWindow *win, const char *lang, const Glib::ustring branch)
{
    Glib::ustring url = Glib::ustring::compose("https://inkscape.org/%1/doc/inkscape-man-%2.html", lang, branch);
    help_open_url(url);
}

void help_url_faq(InkscapeWindow *win, const char *lang)
{
    Glib::ustring url = Glib::ustring::compose("https://inkscape.org/%1/learn/faq/", lang);
    help_open_url(url);
}

void help_url_keys(InkscapeWindow *win, const char *lang, const Glib::ustring branch)
{
    Glib::ustring url = Glib::ustring::compose("https://inkscape.org/%1/doc/keys-%2.html", lang, branch);
    help_open_url(url);
}

void help_url_release_notes(InkscapeWindow *win, const char *lang, const char *version, const bool development_version)
{
    Glib::ustring url = Glib::ustring::compose("https://inkscape.org/%1/release/inkscape-%2", lang, development_version ? "master" : version);
    help_open_url(url);
}

void help_url_report_bug(InkscapeWindow *win, const char *lang)
{
    // PROOF: a fault of this editor is PROOF's to hear of, not Inkscape's. The page has no translations.
    help_open_url("https://github.com/vurglepuddle/PROOF/issues");
}

void help_url_manual(InkscapeWindow *win)
{
    Glib::ustring url = "https://inkscape.org/manual";
    help_open_url(url);
}

void help_url_beginners_guide(InkscapeWindow *win)
{
    Glib::ustring url = "https://inkscape.org/manual/beginners_guide";
    help_open_url(url);
}

void help_url_inkex(InkscapeWindow *win)
{
    Glib::ustring url = "https://inkscape.org/manual/inkex";
    help_open_url(url);
}

void help_url_donate(InkscapeWindow *win, const char *lang, const char *version)
{
    Glib::ustring url = Glib::ustring::compose("https://inkscape.org/%1/donate#lang=%1&version=%2", lang, version);
    help_open_url(url);
}

void help_url_svg11_spec(InkscapeWindow *win)
{
    Glib::ustring url = "http://www.w3.org/TR/SVG11/";
    help_open_url(url);
}

void help_url_svg2_spec(InkscapeWindow *win)
{
    Glib::ustring url = "http://www.w3.org/TR/SVG2/";
    help_open_url(url);
}

const Glib::ustring SECTION = NC_("Action Section", "Help Url");

std::vector<std::vector<Glib::ustring>> raw_data_help_url = {
    // clang-format off
    { "win.help-url-ask-question",    N_("Ask Us a Question"),           SECTION, N_("Ask Us a Question") },
    { "win.help-url-man",             N_("Command Line Options"),        SECTION, N_("Command Line Options")},
    { "win.help-url-faq",             N_("FAQ"),                         SECTION, N_("FAQ")},
    { "win.help-url-keys",            N_("Keys and Mouse Reference"),    SECTION, N_("Keys and Mouse Reference")},
    { "win.help-url-release-notes",   N_("New in This Version"),         SECTION, N_("New in This Version")},
    { "win.help-url-report-bug",      N_("Report a Bug"),                SECTION, N_("Report a Bug")},
    { "win.help-url-manual",          N_("Inkscape Manual"),             SECTION, N_("Inkscape Manual")},
    { "win.help-url-beginners-guide", N_("Beginners' Guide"),            SECTION, N_("Beginners' Guide")},
    { "win.help-url-inkex",           N_("Extension Development Guide"), SECTION, N_("Extension Development Guide")},
    { "win.help-url-donate",          N_("Donate"),                      SECTION, N_("Donate to Inkscape")},
    { "win.help-url-svg11-spec",      N_("SVG 1.1 Specification"),       SECTION, N_("SVG 1.1 Specification")},
    { "win.help-url-svg2-spec",       N_("SVG 2 Specification"),         SECTION, N_("SVG 2 Specification")}
    // clang-format on
};

} // namespace

void add_actions_help_url(InkscapeWindow *win)
{
    const char *lang = _("en"); // TODO: strip /en/ for English version?
    const char *version = Inkscape::version_string_without_revision;
    const bool development_version = g_str_has_suffix(version, "-dev"); // this detection is not perfect but should be close enough
    const Glib::ustring branch = development_version ? "master" : Glib::ustring::compose("%1.%2.x", Inkscape::version_major,  Inkscape::version_minor);

    // clang-format off
    win->add_action( "help-url-ask-question",    sigc::bind(sigc::ptr_fun(&help_url_ask_question), win, lang));
    win->add_action( "help-url-man",             sigc::bind(sigc::ptr_fun(&help_url_man), win, lang, branch));
    win->add_action( "help-url-faq",             sigc::bind(sigc::ptr_fun(&help_url_faq), win, lang));
    win->add_action( "help-url-keys",            sigc::bind(sigc::ptr_fun(&help_url_keys), win, lang, branch));
    win->add_action( "help-url-release-notes",   sigc::bind(sigc::ptr_fun(&help_url_release_notes), win, lang, version, development_version));
    win->add_action( "help-url-report-bug",      sigc::bind(sigc::ptr_fun(&help_url_report_bug), win, lang));
    win->add_action( "help-url-manual",          sigc::bind(sigc::ptr_fun(&help_url_manual), win));
    win->add_action( "help-url-beginners-guide", sigc::bind(sigc::ptr_fun(&help_url_beginners_guide), win));
    win->add_action( "help-url-inkex",           sigc::bind(sigc::ptr_fun(& help_url_inkex), win));
    win->add_action( "help-url-donate",          sigc::bind(sigc::ptr_fun(&help_url_donate), win, lang, version));
    win->add_action( "help-url-svg11-spec",      sigc::bind(sigc::ptr_fun(&help_url_svg11_spec), win));
    win->add_action( "help-url-svg2-spec",       sigc::bind(sigc::ptr_fun(&help_url_svg2_spec), win));
    // clang-format on

    auto app = InkscapeApplication::instance();
    if (!app) {
        show_output("add_actions_help_url: no app!");
        return;
    }
    app->get_action_extra_data().add_data(raw_data_help_url);
}
