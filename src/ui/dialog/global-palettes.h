// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Global color palette information.
 */
/* Authors: PBS <pbs3141@gmail.com>
 * Copyright (C) 2022 PBS
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_DIALOG_GLOBAL_PALETTES_H
#define INKSCAPE_UI_DIALOG_GLOBAL_PALETTES_H

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <unordered_map>
#include <variant>

#include "colors/color.h"

namespace Gio {
class File;
} // namespace Gio

namespace Gtk {
class Window;
} // namespace Gtk

namespace Inkscape::UI::Dialog {

/**
 * The data loaded from a palette file.
 */
struct PaletteFileData
{
    /// Name of the palette, either specified in the file or taken from the filename.
    Glib::ustring name;

    /// Unique ID of this palette.
    Glib::ustring id;

    /// The preferred number of columns.
    /// Certain color palettes are organized into blocks, typically 7 or 8 colors long.
    /// This value tells us how big the block are, if any.
    /// We can use this info to organize colors in columns in multiples of this value.
    int columns = 0;

    // dummy item used for aligning color tiles in a palette
    enum SpacerItem {};

    // item delineating start of new group of colors in a palette
    struct GroupStart {
        Glib::ustring name;
    };

    // PROOF: a named spot ink (ASE "spot" entry, ACB spot book, AI spot swatch).
    // The colour holds the ink name and its full-strength alternate appearance;
    // tint is below 1 for tint swatches (e.g. an AI library's "PANTONE 185 C 50%").
    struct SpotColor {
        Colors::Color color;
        double tint = 1.0;
    };

    using ColorItem = std::variant<Colors::Color, SpacerItem, GroupStart, SpotColor>;

    /// The list of colors in the palette.
    std::vector<ColorItem> colors;

    /// Index to a representative color of the color block; starts from 0 for each block.
    unsigned int page_offset = 0;

    /// Empty palette
    PaletteFileData() {}
};

/**
 * Singleton class that manages the static list of global palettes.
 */
class GlobalPalettes
{
    GlobalPalettes();
public:
    static GlobalPalettes const &get();

    const std::vector<PaletteFileData>& palettes() const { return _palettes; }
    const PaletteFileData* find_palette(const Glib::ustring& id) const;

private:
    std::vector<PaletteFileData> _palettes;
    std::unordered_map<std::string, PaletteFileData*> _access;
};

// Try to load color/swatch palette from the file
struct PaletteResult { // todo: replace with std::expected when it becomes available
    std::optional<PaletteFileData> palette;
    Glib::ustring error_message;
};

PaletteResult load_palette(std::string const &path);

// Show file chooser and select color palette file
Glib::RefPtr<Gio::File> choose_palette_file(Gtk::Window* window);

} // namespace Inkscape::UI::Dialog


namespace Inkscape {

// Save list of RGB values with optional names to the GIMP color palette file
// - fname: path with file name
// - colors: list of (RGB, name)
// - palette_name: name recorded inside palette file
void save_gimp_palette(std::string fname, const std::vector<std::pair<int, std::string>>& colors, const char* palette_name);

}

#endif // INKSCAPE_UI_DIALOG_GLOBAL_PALETTES_H

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
