// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_EXPORT_PANEL_PLAN_H
#define INKSCAPE_UI_DIALOG_EXPORT_PANEL_PLAN_H
#include "export-page-plan.h"
#include <2geom/rect.h>

namespace Inkscape::UI::Dialog::ExportPlanning {
enum class Mode { Document, Pages, Selection, Layers, Custom };
struct Candidate {
    std::string id, label;
    Geom::Rect area;
    std::vector<std::string> selected_children;
};
struct Input {
    Mode mode = Mode::Document;
    Candidate document, custom;
    std::vector<Candidate> pages, layers, selection;
    bool each = false, hide = false;
};
struct Item {
    std::string label;
    Geom::Rect area;
    std::vector<std::string> objects, pages;
    std::string hint_id;
};
struct Format { std::string extension, suffix; double dpi = 96; bool raster = true, one_pdf = true; };
struct File { Item item; std::size_t format; std::string filename; bool conflict; };

inline std::vector<Item> items(Input const &in)
{
    std::vector<Item> out;
    std::vector<std::string> selected;
    std::vector<Candidate> selection;
    for (auto const &c : in.selection) {
        selected.push_back(c.id);
        if (c.area.width() > 0 && c.area.height() > 0) selection.push_back(c);
    }
    if (in.hide && selected.empty()) return out;
    auto add = [&](Candidate const &c, std::vector<std::string> objects, std::vector<std::string> pages = {}) {
        if (c.area.width() > 0 && c.area.height() > 0) out.push_back({c.label, c.area, objects, pages, c.id});
    };
    switch (in.mode) {
        case Mode::Document: add(in.document, in.hide ? selected : std::vector<std::string>{}); break;
        case Mode::Custom: add(in.custom, in.hide ? selected : std::vector<std::string>{}); break;
        case Mode::Pages:
            for (auto const &c : in.pages) add(c, in.hide ? selected : std::vector<std::string>{}, {c.id});
            break;
        case Mode::Layers:
            for (auto const &c : in.layers) {
                if (!in.hide || !c.selected_children.empty()) add(c, in.hide ? c.selected_children : std::vector<std::string>{c.id});
            }
            break;
        case Mode::Selection:
            if (in.each) {
                for (auto const &c : selection) add(c, in.hide ? std::vector<std::string>{c.id} : std::vector<std::string>{});
            } else if (!selection.empty()) {
                auto combined = selection.front();
                for (auto const &c : selection) combined.area.unionWith(c.area);
                if (selection.size() > 1) { combined.label = "Selection"; combined.id.clear(); }
                add(combined, in.hide ? selected : std::vector<std::string>{});
            }
            break;
    }
    return out;
}

inline std::vector<File> files(std::vector<Item> const &items, std::vector<Format> const &formats,
                               Mode mode, std::string const &folder, std::string const &name,
                               std::function<bool(std::string const &)> const &exists)
{
    std::vector<File> out;
    std::set<std::string> used;
    for (std::size_t r = 0; r < formats.size() && !items.empty(); ++r) {
        auto const &format = formats[r];
        bool const combined = mode == Mode::Pages && format.extension == ".pdf" && format.one_pdf;
        auto row_items = items;
        if (combined) {
            Item item = items.front();
            if (items.size() > 1) { item.label = "Pages"; item.hint_id.clear(); }
            for (std::size_t i = 1; i < items.size(); ++i) {
                item.area.unionWith(items[i].area);
                item.pages.insert(item.pages.end(), items[i].pages.begin(), items[i].pages.end());
            }
            row_items = {item};
        }
        for (auto const &item : row_items) {
            auto label = safe_page_filename(item.label);
            if (label.empty()) label = "_";
            auto base = safe_page_filename(name);
            if (base.empty()) base = label;
            else if (row_items.size() > 1) base += "-" + label;
            auto suffix = format.suffix;
            for (std::size_t pos = 0; format.raster && (pos = suffix.find("{dpi}", pos)) != std::string::npos;) {
                auto dpi = std::to_string(static_cast<int>(format.dpi)); suffix.replace(pos, 5, dpi); pos += dpi.size();
            }
            suffix = safe_page_filename(suffix);
            if (!suffix.empty()) base += "-" + suffix;
            auto leaf = base + format.extension;
            for (unsigned n = 2; !used.insert(Glib::ustring(leaf).casefold().raw()).second; ++n)
                leaf = base + "-" + std::to_string(n) + format.extension;
            auto path = Glib::build_filename(folder, leaf);
            out.push_back({item, r, path, exists(path)});
        }
    }
    return out;
}
} // namespace Inkscape::UI::Dialog::ExportPlanning
#endif
