// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: read the swatches of an Illustrator document (.ai swatch libraries).
 *
 * Parses the %AI5_BeginPalette ... %AI5_EndPalette section of Illustrator's
 * native records: Illustrator's Swatches panel in order, with swatch groups.
 * Prototype and Illustrator comparison: tools/ai-palette-prototype.py.
 */
#ifndef SEEN_UI_DIALOG_AI_PALETTE_H
#define SEEN_UI_DIALOG_AI_PALETTE_H

#include <string_view>

namespace Inkscape::UI::Dialog {

struct PaletteFileData;

/**
 * Append the swatches found in Illustrator native records to the palette:
 * process and global colours as plain colours, spot inks (Xx) as spot entries,
 * swatch groups as group headings. Registration, gradient and pattern swatches
 * are skipped. Returns false if the records hold no palette section.
 */
bool parse_ai_palette(std::string_view records, PaletteFileData &palette);

} // namespace Inkscape::UI::Dialog

#endif // SEEN_UI_DIALOG_AI_PALETTE_H
