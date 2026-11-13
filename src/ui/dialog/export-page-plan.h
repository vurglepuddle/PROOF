// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_EXPORT_PAGE_PLAN_H
#define INKSCAPE_UI_DIALOG_EXPORT_PAGE_PLAN_H

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <glib.h>
#include <glibmm/miscutils.h>
#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog {

struct PageExportLabel { std::string label, fallback; };
struct PageExportPlan { std::vector<std::string> files, conflicts; };

inline std::string safe_page_filename(std::string const &label)
{
    Glib::ustring cleaned;
    for (auto c : Glib::ustring(label)) {
        cleaned += g_unichar_iscntrl(c) || (c < 128 && std::string_view("\\/:*?\"<>|").find(char(c)) != std::string_view::npos)
            ? gunichar('_') : c;
    }
    auto name = cleaned.raw();
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    auto const device = Glib::ustring(name.substr(0, name.find('.'))).uppercase();
    bool reserved = device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" ||
                    device == "CONIN$" || device == "CONOUT$";
    for (int i = 1; i <= 9; ++i) {
        reserved |= device == "COM" + std::to_string(i) || device == "LPT" + std::to_string(i);
    }
    for (auto const *digit : {"\u00b9", "\u00b2", "\u00b3"}) {
        reserved |= device == std::string("COM") + digit || device == std::string("LPT") + digit;
    }
    return reserved ? "_" + name : name;
}

// UTF-8 filenames. One page preserves the exact filename. Existing files are reported,
// never renamed; suffixes resolve collisions only among the selected pages themselves.
inline PageExportPlan plan_page_exports(std::string const &filename, std::string const &extension,
                                       std::vector<PageExportLabel> const &pages,
                                       std::function<bool(std::string const &)> const &exists)
{
    PageExportPlan plan;
    auto const folder = Glib::path_get_dirname(filename);
    auto stem = Glib::path_get_basename(filename);
    if (auto dot = stem.find_last_of('.'); dot != std::string::npos && dot != 0) stem.resize(dot);
    std::set<std::string> used;
    for (auto const &page : pages) {
        std::string file = filename;
        if (pages.size() > 1) {
            auto label = safe_page_filename(page.label);
            if (label.empty()) label = safe_page_filename(page.fallback);
            if (label.empty()) label = "_";
            auto const base = stem + "-" + label;
            auto name = base + extension;
            for (unsigned n = 2; !used.insert(Glib::ustring(name).casefold().raw()).second; ++n) {
                name = base + "-" + std::to_string(n) + extension;
            }
            file = Glib::build_filename(folder, name);
        }
        plan.files.push_back(file);
        if (exists(file)) plan.conflicts.push_back(file);
    }
    return plan;
}

} // namespace Inkscape::UI::Dialog
#endif
