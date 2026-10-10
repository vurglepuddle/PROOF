// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *
 * Copyright (C) 2012 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <set>
#include <tuple>

#include <glibmm/i18n.h>
#include <glibmm/regex.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/textview.h>
#include <libnrtype/font-factory.h>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "extension/internal/ai/ai-native-fonts.h"
#include "inkscape.h"
#include "object/sp-flowdiv.h"
#include "object/sp-root.h"
#include "object/sp-textpath.h"
#include "object/sp-tspan.h"
#include "preferences.h"
#include "selection-chemistry.h"
#include "selection.h"
#include "text-editing.h"
#include "ui/dialog-events.h"
#include "ui/dialog-run.h"
#include "ui/pack.h"

namespace Inkscape::UI::Dialog {
namespace {

/// `missing`: the list has fonts that aren't installed, which has a preference of its own.
void show(std::vector<SPItem*> const &list, Glib::ustring const &out, bool substituted, bool missing)
{
   Gtk::MessageDialog warning(_("Some fonts are not available and have been substituted."),
                              false, Gtk::MessageType::INFO, Gtk::ButtonsType::OK, true);
   warning.set_resizable(true);
   warning.set_title(_("Font substitution"));
   
   sp_transientize(warning);

   Gtk::TextView textview;
   textview.set_editable(false);
   textview.set_wrap_mode(Gtk::WrapMode::WORD);
   textview.set_visible(true);
   textview.get_buffer()->set_text(_(out.c_str()));

   Gtk::ScrolledWindow scrollwindow;
   scrollwindow.set_child(textview);
   scrollwindow.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
   scrollwindow.set_has_frame(true);
   scrollwindow.set_size_request(0, 100);
   scrollwindow.set_visible(true);

   Gtk::CheckButton cbSelect;
   cbSelect.set_label(_("Select all the affected items"));
   cbSelect.set_active(true);
   cbSelect.set_visible(true);

   Gtk::CheckButton cbWarning;
   cbWarning.set_label(_("Don't show this warning again"));
   cbWarning.set_visible(true);

   auto box = warning.get_content_area();
   box->set_margin(5);
   box->set_spacing(2);
   UI::pack_start(*box, scrollwindow, true, true, 4);
   UI::pack_start(*box, cbSelect, false, false);
   UI::pack_start(*box, cbWarning, false, false);

   Inkscape::UI::dialog_run(warning);

   if (cbWarning.get_active()) {
       if (substituted) Inkscape::Preferences::get()->setBool("/options/font/substitutedlg", false);
       if (missing) Inkscape::Preferences::get()->setBool("/options/font/missingdlg", false);
   }

   if (cbSelect.get_active()) {
       auto desktop = SP_ACTIVE_DESKTOP;
       auto selection = desktop->getSelection();
       selection->clear();
       selection->setList(list);
   }
}

/*
 * Find all the fonts that are in the document but not available on the user's system
 * and have been substituted for other fonts.
 *
 * Return a list of SPItems where fonts have been substituted.
 *
 * Walk through all the objects ...
 * a. Build up a list of the objects with fonts defined in the style attribute
 * b. Build up a list of the objects rendered fonts - taken for the objects layout/spans
 * If there are fonts in a. that are not in b. then those fonts have been substituted.
 */
std::pair<std::vector<SPItem*>, Glib::ustring> getFontReplacedItems(SPDocument *doc)
{
    std::vector<SPItem*> outList;
    std::set<Glib::ustring> setErrors;
    std::set<Glib::ustring> setFontSpans;
    std::map<SPItem*, Glib::ustring> mapFontStyles;
    Glib::ustring out;

    auto const allList = get_all_items(doc->getRoot(), SP_ACTIVE_DESKTOP, false, false, true);
    for (auto item : allList) {
        auto style = item->style;
        Glib::ustring family = "";

        if (is_top_level_text_object(item)) {
            // Should only need to check the first span, since the others should be covered by TSPAN's etc
            family = te_get_layout(item)->getFontFamily(0);
            setFontSpans.insert(family);
        }
        else if (auto textpath = cast<SPTextPath>(item)) {
            if (textpath->originalPath) {
                family = cast<SPText>(item->parent)->layout.getFontFamily(0);
                setFontSpans.insert(family);
            }
        }
        else if (is<SPTSpan>(item) || is<SPFlowtspan>(item)) {
            // is_part_of_text_subtree (item)
             // TSPAN layout comes from the parent->layout->_spans
             SPObject *parent_text = item;
             while (parent_text && !is<SPText>(parent_text)) {
                 parent_text = parent_text->parent;
             }
             if (parent_text) {
                 family = cast<SPText>(parent_text)->layout.getFontFamily(0);
                 // Add all the spans fonts to the set
                 for (unsigned int f = 0; f < parent_text->children.size(); f++) {
                     family = cast<SPText>(parent_text)->layout.getFontFamily(f);
                     setFontSpans.insert(family);
                 }
             }
        }

        if (style) {
            char const *style_font = nullptr;
            if (style->font_family.set) {
                style_font = style->font_family.value();
            } else if (style->font_specification.set) {
                style_font = style->font_specification.value();
            } else {
                style_font = style->font_family.value();
            }

            if (style_font) {
                if (has_visible_text(item)) {
                    mapFontStyles.insert(std::make_pair(item, style_font));
                }
            }
        }
    }

    // Check if any document styles are not in the actual layout
    for (auto mapIter = mapFontStyles.rbegin(); mapIter != mapFontStyles.rend(); ++mapIter) {
        SPItem *item = mapIter->first;
        Glib::ustring fonts = mapIter->second;

        // CSS font fallbacks can have more that one font listed, split the font list
        std::vector<Glib::ustring> vFonts = Glib::Regex::split_simple("," , fonts);
        bool fontFound = false;
        for (auto const &font : vFonts) {
            // trim whitespace
            size_t startpos = font.find_first_not_of(" \n\r\t");
            size_t endpos = font.find_last_not_of(" \n\r\t");
            if (startpos == std::string::npos || endpos == std::string::npos) {
                continue; // empty font name
            }
            auto const trimmed = font.substr(startpos, endpos - startpos + 1);
            if (setFontSpans.find(trimmed) != setFontSpans.end() ||
                trimmed == Glib::ustring("sans-serif") ||
                trimmed == Glib::ustring("Sans") ||
                trimmed == Glib::ustring("serif") ||
                trimmed == Glib::ustring("Serif") ||
                trimmed == Glib::ustring("monospace") ||
                trimmed == Glib::ustring("Monospace"))
            {
                fontFound = true;
                break;
            }
        }
        if (!fontFound) {
            Glib::ustring subName = getSubstituteFontName(fonts);
            Glib::ustring err = Glib::ustring::compose(_("Font '%1' substituted with '%2'"), fonts.c_str(), subName.c_str());
            setErrors.insert(err);
            outList.emplace_back(item);
        }
    }

    for (auto const &err : setErrors) {
        out.append(err + "\n");
        g_warning("%s", err.c_str());
    }

    return std::make_pair(std::move(outList), std::move(out));
}

} // namespace

void checkFontSubstitutions(SPDocument *doc)
{
    namespace AiNative = Inkscape::Extension::Internal::AiNative;
    // PROOF: text from an Illustrator document whose font wasn't installed gets it back once
    // it is. Like the document's colour mode, that isn't an edit.
    {
        Inkscape::DocumentUndo::ScopedInsensitive insensitive(doc);
        AiNative::restore_fonts(doc);
    }

    // A document is looked at once: opening one comes through here more than once.
    static std::set<unsigned long> seen;
    if (!seen.insert(doc->serial()).second) {
        return;
    }

    // PROOF: the fonts it names that still aren't installed are listed, as Illustrator lists
    // them on opening, unless that was switched off; other fonts that were substituted only
    // when asked for, as before.
    auto *prefs = Inkscape::Preferences::get();
    bool const show_substituted = prefs->getBool("/options/font/substitutedlg");
    auto const missing = prefs->getBool("/options/font/missingdlg", true) ? AiNative::missing_fonts(doc)
                                                                         : std::vector<AiNative::MissingFont>();
    if (!show_substituted && missing.empty()) {
        return;
    }

    std::vector<SPItem *> list;
    Glib::ustring msg;
    if (show_substituted) std::tie(list, msg) = getFontReplacedItems(doc);
    bool const substituted = !msg.empty();
    for (auto const &font : missing) {
        msg += Glib::ustring::compose(
            ngettext("Font '%1' isn't installed: %2 text object is set in another font until it is",
                     "Font '%1' isn't installed: %2 text objects are set in another font until it is", font.texts.size()),
            font.name, font.texts.size()) + "\n";
        for (auto *text : font.texts) {
            if (std::find(list.begin(), list.end(), text) == list.end()) list.push_back(text);
        }
    }
    if (!msg.empty()) {
        show(list, msg, substituted, !missing.empty());
    }
}

} // namespace Inkscape::UI::Dialog

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
