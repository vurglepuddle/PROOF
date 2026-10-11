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

#ifndef SEEN_INKSCAPE_VERSION_INFO_H
#define SEEN_INKSCAPE_VERSION_INFO_H

#include <string>

namespace Inkscape {

    /// PROOF: what follows a document's name in the title of its window.
    inline constexpr char const *PROOF_TITLE_SUFFIX = " - PROOF";

    /// PROOF: the editor's name with the Inkscape version it is based on, for About and for reports.
    std::string proof_version();
    /// PROOF: the revision and date of the build, in brackets. PROOF has no version number of its own.
    std::string proof_build();

    std::string inkscape_version();
    std::string inkscape_revision();
    std::string os_version();
    std::string debug_info();

    unsigned short int inkscape_build_year();
} // namespace Inkscape

#endif // SEEN_INKSCAPE_VERSION_INFO_H

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
