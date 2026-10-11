// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the linked pictures of a document whose files can't be found, and a file to link
 * each to instead.
 *
 * Such a picture keeps its link and shows as the small picture its document keeps of it
 * (SPImage::LINK_PREVIEW_ATTRIBUTE), or as a marked box. This is the list Illustrator shows
 * on opening such a document, and its Relink.
 */
#ifndef INKSCAPE_UI_DIALOG_LINK_MISSING_H
#define INKSCAPE_UI_DIALOG_LINK_MISSING_H

class SPDesktop;
class SPDocument;

namespace Inkscape::UI::Dialog {

/// Whether a document that has just opened has such pictures to list: it has some, and
/// listing them at opening isn't switched off (`/options/links/missingdlg`).
bool missingLinksToShow(SPDocument *document);

/// List them, with Relink.
/// @param at_open shown because a document opened: it can then be told not to show again.
void showMissingLinks(SPDesktop *desktop, bool at_open = false);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_LINK_MISSING_H
