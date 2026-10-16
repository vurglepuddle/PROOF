// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *
 * Copyright (C) 2012 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <tuple>

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/regex.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/textview.h>
#include <libnrtype/font-factory.h>
#include <libnrtype/font-instance.h>
#include <libnrtype/font-lister.h>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "extension/internal/ai/ai-native-fonts.h"
#include "extension/internal/pdfinput/ai-native-open.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "link-missing.h"
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
#include "ui/icon-names.h"
#include "ui/pack.h"
#include "ui/widget/drop-down-list.h"
#include "xml/repr.h"

namespace Inkscape::UI::Dialog {
namespace {

namespace AiNative = Inkscape::Extension::Internal::AiNative;

/**
 * PROOF: the fonts a document names that aren't installed, each with the number of text
 * objects set in another font meanwhile, and a font to replace one by across the document.
 * Fonts that Inkscape's own check found substituted are listed under them.
 */
class MissingFonts
{
public:
    /// @param at_open shown because a document opened: it can then be told not to show again.
    MissingFonts(SPDesktop *desktop, std::vector<SPItem *> substituted_items, Glib::ustring const &substituted, bool at_open);

    void run();

private:
    void fill();
    void family_changed();
    void update();
    void replace();

    SPDesktop *_desktop;
    SPDocument *_document;
    std::vector<SPItem *> _substituted_items;
    bool _substituted;
    bool _at_open;

    Gtk::Dialog _dialog;
    Gtk::Label _message;
    Gtk::ScrolledWindow _scroll;
    Gtk::ListBox _list;
    Gtk::Box _replace_row{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label _replace_label;
    UI::Widget::DropDownList _family, _style;
    Gtk::Button _replace;
    Gtk::CheckButton _select, _quiet;

    std::vector<AiNative::MissingFont> _fonts;
    std::vector<std::pair<Glib::ustring, PangoFontFamily *>> _families;
    std::vector<StyleNames> _styles;
};

MissingFonts::MissingFonts(SPDesktop *desktop, std::vector<SPItem *> substituted_items, Glib::ustring const &substituted,
                           bool at_open)
    : _desktop(desktop)
    , _document(desktop->getDocument())
    , _substituted_items(std::move(substituted_items))
    , _substituted(!substituted.empty())
    , _at_open(at_open)
{
    _dialog.set_title(_("Missing Fonts"));
    _dialog.set_modal(true);
    _dialog.set_resizable(true);
    _dialog.set_default_size(460, -1);
    _desktop->setWindowTransient(_dialog);

    auto *box = _dialog.get_content_area();
    box->set_margin(10);
    box->set_spacing(8);

    _message.set_wrap(true);
    _message.set_xalign(0);
    _message.set_max_width_chars(60);
    UI::pack_start(*box, _message, false, false);

    _list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    _list.signal_row_selected().connect([this](Gtk::ListBoxRow *) { update(); });
    _scroll.set_child(_list);
    _scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _scroll.set_has_frame(true);
    _scroll.set_min_content_height(96);
    _scroll.set_max_content_height(220);
    _scroll.set_propagate_natural_height(true);
    UI::pack_start(*box, _scroll, true, true);

    // The fonts that are installed, in the order a list of names is read in.
    for (auto const &[name, family] : FontLister::get_instance()->pango_family_map) _families.emplace_back(name, family);
    std::sort(_families.begin(), _families.end(), [](auto const &a, auto const &b) {
        return a.first.casefold_collate_key() < b.first.casefold_collate_key();
    });
    for (auto const &family : _families) _family.append(family.first);
    _family.enable_search();
    _family.set_button_max_chars(24);
    _family.set_hexpand(true);
    _family.set_tooltip_text(_("The font to set the text in instead"));
    _style.set_button_max_chars(16);
    _style.set_tooltip_text(_("Its style"));
    _replace_label.set_text(_("Replace with:"));
    _replace.set_label(_("Replace"));
    _replace.set_tooltip_text(_("Set all the text of the chosen missing font in this font"));
    _replace_row.append(_replace_label);
    _replace_row.append(_family);
    _replace_row.append(_style);
    _replace_row.append(_replace);
    UI::pack_start(*box, _replace_row, false, false);
    _family.signal_changed().connect([this] { family_changed(); });
    _replace.signal_clicked().connect([this] { replace(); });
    // Start from the font that stands in now: replacing with it keeps the text as it looks.
    if (auto stand_in = AiNative::substitute_font(); stand_in && stand_in->face) {
        auto const *family = sp_font_description_get_family(stand_in->face->get_descr());
        Glib::ustring const name = family ? family : "";
        auto const found = std::find_if(_families.begin(), _families.end(), [&](auto const &f) { return f.first == name; });
        if (found != _families.end()) _family.set_selected(found - _families.begin());
    }
    family_changed();

    if (_substituted) {
        auto *text = Gtk::make_managed<Gtk::TextView>();
        text->set_editable(false);
        text->set_cursor_visible(false);
        text->set_wrap_mode(Gtk::WrapMode::WORD);
        text->get_buffer()->set_text(substituted);
        auto *scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        scroll->set_child(*text);
        scroll->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
        scroll->set_has_frame(true);
        scroll->set_size_request(0, 80);
        UI::pack_start(*box, *scroll, true, true);
    }

    _select.set_label(_("Select all the affected items"));
    _select.set_active(_at_open);
    UI::pack_start(*box, _select, false, false);
    _quiet.set_label(_("Don't show this warning again"));
    if (_at_open) UI::pack_start(*box, _quiet, false, false);

    _dialog.add_button(_("_Close"), Gtk::ResponseType::CLOSE);
    _dialog.set_default_response(Gtk::ResponseType::CLOSE);

    fill();
}

/// The list as the document now stands.
void MissingFonts::fill()
{
    while (auto *row = _list.get_row_at_index(0)) _list.remove(*row);
    _fonts = AiNative::missing_fonts(_document);
    for (auto const &font : _fonts) {
        auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->set_margin(4);
        auto *name = Gtk::make_managed<Gtk::Label>(font.name);
        name->set_xalign(0);
        name->set_hexpand(true);
        name->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        name->set_selectable(false);
        auto *count = Gtk::make_managed<Gtk::Label>(Glib::ustring::compose(
            ngettext("%1 text object", "%1 text objects", font.texts.size()), font.texts.size()));
        count->add_css_class("dim-label");
        row->append(*name);
        row->append(*count);
        _list.append(*row);
    }
    if (auto *first = _list.get_row_at_index(0)) _list.select_row(*first);
    _scroll.set_visible(!_fonts.empty());
    _replace_row.set_visible(!_fonts.empty());
    if (!_fonts.empty()) {
        _message.set_text(_("These fonts aren't installed. The text that uses them is set in another font until they "
                            "are installed or replaced."));
    } else if (_substituted) {
        _message.set_text(_("Some fonts are not available and have been substituted."));
    } else {
        _message.set_text(_("Every font this document names is installed or has been replaced."));
    }
    _select.set_visible(!_fonts.empty() || _substituted);
    update();
}

/// The styles of the family chosen.
void MissingFonts::family_changed()
{
    _style.remove_all();
    _styles.clear();
    auto const at = _family.get_selected();
    if (at < _families.size()) _styles = FontFactory::get().GetUIStyles(_families[at].second);
    unsigned regular = 0;
    for (auto const &style : _styles) {
        auto const position = _style.append(style.display_name);
        if (style.css_name == "Normal" || style.css_name == "Regular") regular = position;
    }
    if (!_styles.empty()) _style.set_selected(regular);
    update();
}

void MissingFonts::update()
{
    _replace.set_sensitive(_list.get_selected_row() && _family.get_selected() < _families.size() &&
                           _style.get_selected() < _styles.size());
}

void MissingFonts::replace()
{
    auto const *row = _list.get_selected_row();
    auto const family = _family.get_selected(), style = _style.get_selected();
    if (!row || row->get_index() < 0 || std::size_t(row->get_index()) >= _fonts.size() || family >= _families.size() ||
        style >= _styles.size()) {
        return;
    }
    // The font as the font controls write one that is chosen.
    auto const fontspec = Inkscape::canonize_fontspec(_families[family].first + ", " + _styles[style].css_name);
    auto *css = sp_repr_css_attr_new();
    FontLister::get_instance()->fill_css(css, fontspec);
    Glib::ustring declarations;
    sp_repr_css_write_string(css, declarations);
    sp_repr_css_attr_unref(css);
    if (AiNative::replace_font(_document, _fonts[row->get_index()].name, declarations.raw())) {
        DocumentUndo::done(_document, RC_("Undo", "Replace a font that isn't installed"), INKSCAPE_ICON("draw-text"));
    }
    fill();
}

void MissingFonts::run()
{
    Inkscape::UI::dialog_run(_dialog);

    if (_at_open && _quiet.get_active()) {
        if (_substituted) Inkscape::Preferences::get()->setBool("/options/font/substitutedlg", false);
        Inkscape::Preferences::get()->setBool("/options/font/missingdlg", false);
    }
    if (_select.get_visible() && _select.get_active()) {
        // What is still set in a font other than its own.
        auto list = _substituted_items;
        for (auto const &font : AiNative::missing_fonts(_document)) {
            for (auto *text : font.texts) {
                if (std::find(list.begin(), list.end(), text) == list.end()) list.push_back(text);
            }
        }
        auto *selection = _desktop->getSelection();
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

/// Whether the document is an .ai that opened from its records alone, with no page to check
/// them against (Illustrator 8 and older).
bool openedUnchecked(SPDocument *doc)
{
    auto const *root = doc ? doc->getReprRoot() : nullptr;
    auto const *mark = root ? root->attribute(Inkscape::Extension::Internal::AI_UNCHECKED_ATTRIBUTE) : nullptr;
    auto const *file = doc ? doc->getDocumentFilename() : nullptr;
    if (!mark || !file) return false;
    auto const name = Glib::ustring(file).lowercase();
    return name.size() > 3 && name.substr(name.size() - 3) == ".ai";
}

/// Says that the artwork couldn't be checked against the file's pages, as there are none.
void showUnchecked(Gtk::Window &window)
{
    Gtk::MessageDialog dialog(window, _("This file is in the format of Illustrator 8 or older"), false,
                              Gtk::MessageType::WARNING, Gtk::ButtonsType::OK, true);
    dialog.set_secondary_text(_("It has no PDF copy of its pages, so PROOF couldn't check that its artwork "
                                "came in as it was drawn. Look it over before you use it for print."));
    dialog.set_title(_("Opened Without a Check"));
    dialog_run(dialog);
}

} // namespace

void checkFontSubstitutions(SPDesktop *desktop)
{
    auto *doc = desktop ? desktop->getDocument() : nullptr;
    if (!doc) return;
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
    // PROOF: and so are the linked pictures whose files can't be found, first, as there.
    // Before them, that an Illustrator file of the oldest kind couldn't be checked.
    bool const unchecked = openedUnchecked(doc);
    if (!show_substituted && missing.empty() && !missingLinksToShow(doc) && !unchecked) {
        return;
    }
    bool const show_missing = !missing.empty();

    // A document that opens into a new window comes through here before the window is on
    // screen, and a dialog shown then has nothing to be placed over: it came up in the corner
    // of the screen. It is shown once the window is there.
    auto const serial = doc->serial();
    auto tries = std::make_shared<int>(0);
    Glib::signal_idle().connect([desktop, serial, show_substituted, show_missing, unchecked, tries] {
        // The window and the document it was opened for, while both are still there.
        auto const shown = [desktop, serial]() -> SPDocument * {
            auto const &desktops = INKSCAPE.get_desktops();
            if (std::find(desktops.begin(), desktops.end(), desktop) == desktops.end()) {
                return nullptr; // Closed meanwhile.
            }
            auto *document = desktop->getDocument();
            return document && document->serial() == serial ? document : nullptr;
        };
        auto *document = shown();
        if (!document) {
            return false;
        }
        auto const *window = desktop->getInkscapeWindow();
        if ((!window || !window->get_mapped()) && ++*tries < 500) {
            return true;
        }
        if (unchecked && window) {
            showUnchecked(*desktop->getInkscapeWindow());
            document = shown();
            if (!document) {
                return false;
            }
        }
        if (missingLinksToShow(document)) {
            showMissingLinks(desktop, true);
            document = shown();
            if (!document) {
                return false;
            }
        }
        std::vector<SPItem *> list;
        Glib::ustring msg;
        if (show_substituted) std::tie(list, msg) = getFontReplacedItems(document);
        if (msg.empty() && !(show_missing && !AiNative::missing_fonts(document).empty())) {
            return false;
        }
        MissingFonts(desktop, std::move(list), msg, true).run();
        return false;
    });
}

void showMissingFonts(SPDesktop *desktop)
{
    if (!desktop || !desktop->getDocument()) return;
    MissingFonts(desktop, {}, {}, false).run();
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
