// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Consolidates version info for Inkscape,
 * its various dependencies and the OS we're running on
 *//*
 * Authors:
 *   Patrick Storz <eduard.braun2@gmx.de>
 *
 * Copyright (C) 2021 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */


#include <iostream>
#include <ostream>
#include <string>

#include <hb.h> // not indirectly included via gtk.h on macOS
#include <glib.h>
#include <glibmm.h>
#include <gtk/gtk.h>
#include <gtkmm.h>
#include <libxml2/libxml/xmlversion.h>
#include <libxslt/xsltconfig.h>

#ifdef WITH_POPPLER
#include <poppler-config.h>
#endif

#include "inkscape-version.h" // Inkscape version


namespace Inkscape {

/**
 * Return Inkscape version string
 *
 * Returns the Inkscape version string including program name.
 *
 * @return version string
 */
std::string inkscape_version() {
    return std::string("Inkscape ") + Inkscape::version_string;
}

/**
 * PROOF: the name the editor goes by, and what it is built on.
 *
 * `--version` keeps Inkscape's line (inkscape_version()), because extensions and scripts read it.
 */
std::string proof_version() {
    return std::string("PROOF, based on Inkscape ") + Inkscape::version_string;
}

std::string proof_build() {
    // The version string is "1.5-dev (revision, date)": the part in brackets is this build's.
    std::string const version = Inkscape::version_string;
    auto const at = version.find('(');
    return at == std::string::npos ? std::string() : version.substr(at);
}

/**
 * Return Inkscape repository revision string
 *
 * @return code revision string
 */
std::string inkscape_revision()
{
    return std::string("revision_" + std::string(Inkscape::revision_string));
}

/**
 * Wrapper around g_spawn_sync which captures STDOUT and strips trailing whitespace.
 *
 * If an error occurs, report it to STDERR and return an empty string.
 */
[[maybe_unused]] static std::string _run(char const *command)
{
    std::string out;
    try {
        Glib::spawn_command_line_sync(command, &out);
    } catch (Glib::Error const &) {
        std::cerr << "Failed to execute " << command << std::endl;
    }
    auto pos = out.find_last_not_of(" \n\r\t");
    out.resize(pos == std::string::npos ? 0 : pos + 1);
    return out;
}

/**
 * Return OS version string
 *
 * Returns the OS version string including OS name.
 *
 * Relies on glib's 'g_get_os_info'.
 * Might be undefined on some OSs. "(unknown)" is returned in this case.
 *
 * @return version string
 */
std::string os_version() {
    std::string os_version_string = "(unknown)";

#ifdef __APPLE__
    os_version_string = _run("sw_vers -productName") + " " +     //
                        _run("sw_vers -productVersion") + " (" + //
                        _run("sw_vers -buildVersion") + ") " +   //
                        _run("uname -m");
#elif GLIB_CHECK_VERSION(2, 64, 0)
    char *os_name = g_get_os_info(G_OS_INFO_KEY_NAME);
    char *os_pretty_name = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
    if (os_pretty_name) {
        os_version_string = os_pretty_name;
    } else if (os_name) {
        os_version_string = os_name;
    }
    g_free(os_name);
    g_free(os_pretty_name);
#endif

    return os_version_string;
}

/**
 * Return full debug info
 *
 * Returns full debug info including:
 *  - Inkscape version
 *  - versions of various dependencies
 *  - OS name and version
 *
 * @return debug info
 */
std::string debug_info() {
    std::stringstream ss;

    ss << proof_version() << std::endl;
    ss << std::endl;

    ss << "                      Compile  (Run)" << std::endl;
    ss << "    GLib version:     " << GLIB_MAJOR_VERSION      << "." << GLIB_MINOR_VERSION      << "." << GLIB_MICRO_VERSION      << std::endl;
    ss << "    GTK version:      " << GTK_MAJOR_VERSION       << "." << GTK_MINOR_VERSION       << "." << GTK_MICRO_VERSION
       << " ("                     << gtk_get_major_version() << "." << gtk_get_minor_version() << "." << gtk_get_micro_version() << ")" << std::endl;
    ss << "    glibmm version:   " << GLIBMM_MAJOR_VERSION    << "." << GLIBMM_MINOR_VERSION    << "." << GLIBMM_MICRO_VERSION    << std::endl;
    ss << "    gtkmm version:    " << GTKMM_MAJOR_VERSION     << "." << GTKMM_MINOR_VERSION     << "." << GTKMM_MICRO_VERSION     << std::endl;
    ss << "    libxml2 version:  " << LIBXML_DOTTED_VERSION << std::endl;
    ss << "    libxslt version:  " << LIBXSLT_DOTTED_VERSION << std::endl;
    ss << "    Cairo version:    " << CAIRO_VERSION_MAJOR     << "." << CAIRO_VERSION_MINOR     << "." << CAIRO_VERSION_MICRO
       << " (" << cairo_version_string() << ")" << std::endl;
    ss << "    Pango version:    " << PANGO_VERSION_MAJOR     << "." << PANGO_VERSION_MINOR     << "." << PANGO_VERSION_MICRO
       << " (" << pango_version_string() << ")" << std::endl;
    ss << "    HarfBuzz version: " << HB_VERSION_MAJOR        << "." << HB_VERSION_MINOR        << "." << HB_VERSION_MICRO
       << " (" << hb_version_string() << ")" << std::endl;
#ifdef WITH_POPPLER
    ss << "    Poppler version:  " << POPPLER_VERSION << std::endl;
#endif
    ss << std::endl;
    ss << "    OS version:       " << os_version();

    return ss.str();
}

/**
 * Return build year as 4 digit
 *
 * @return Inkscape build year
 */
unsigned short int inkscape_build_year()
{
    return Inkscape::build_year;
}

} // namespace Inkscape


/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim:filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99:
