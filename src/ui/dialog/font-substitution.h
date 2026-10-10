// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief FontSubstitution dialog
 */
/* Authors:
 *
 *
 * Copyright (C) 2012 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_DIALOG_FONT_SUBSTITUTION_H
#define INKSCAPE_UI_DIALOG_FONT_SUBSTITUTION_H

class SPDesktop;
class SPDocument;

namespace Inkscape::UI::Dialog {

/// What a document that has just opened in `desktop` needs said about its fonts.
void checkFontSubstitutions(SPDesktop *desktop);

/// PROOF: list the fonts the window's document names that aren't installed, with a font
/// to replace each by across the document. What opening a document shows, on request.
void showMissingFonts(SPDesktop *desktop);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_FONT_SUBSTITUTION_H

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
