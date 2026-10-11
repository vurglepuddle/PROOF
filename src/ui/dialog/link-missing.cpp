// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the Missing Links window. See link-missing.h.
 */
#include "link-missing.h"

#include <string>
#include <vector>

#include <giomm/file.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "object/sp-image.h"
#include "preferences.h"
#include "selection.h"
#include "ui/dialog-run.h"
#include "ui/dialog/choose-file.h"
#include "ui/icon-names.h"
#include "ui/pack.h"

namespace Inkscape::UI::Dialog {
namespace {

constexpr char const *AT_OPEN = "/options/links/missingdlg";

/// A file's name and the folder it was in, whichever kind of machine wrote them.
std::pair<std::string, std::string> name_and_folder(std::string const &file)
{
    auto const cut = file.find_last_of("/\\");
    if (cut == std::string::npos) return {file, {}};
    return {file.substr(cut + 1), file.substr(0, cut)};
}

class MissingLinks
{
public:
    MissingLinks(SPDesktop *desktop, bool at_open);

    void run();

private:
    void fill();
    void update();
    void relink();

    SPDesktop *_desktop;
    SPDocument *_document;
    bool _at_open;

    Gtk::Dialog _dialog;
    Gtk::Label _message, _found;
    Gtk::ScrolledWindow _scroll;
    Gtk::ListBox _list;
    Gtk::Box _relink_row{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Button _relink;
    Gtk::CheckButton _select, _quiet;

    std::vector<SPImage *> _pictures;
};

MissingLinks::MissingLinks(SPDesktop *desktop, bool at_open)
    : _desktop(desktop)
    , _document(desktop->getDocument())
    , _at_open(at_open)
{
    _dialog.set_title(_("Missing Links"));
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

    _relink.set_label(_("_Relink..."));
    _relink.set_use_underline(true);
    _relink.set_tooltip_text(_("Choose the file for the picture picked in the list. The others that are missing are "
                               "looked for in the same folder"));
    _relink.signal_clicked().connect([this] { relink(); });
    _found.set_xalign(0);
    _found.set_hexpand(true);
    _found.set_ellipsize(Pango::EllipsizeMode::END);
    _found.add_css_class("dim-label");
    _relink_row.append(_relink);
    _relink_row.append(_found);
    UI::pack_start(*box, _relink_row, false, false);

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
void MissingLinks::fill()
{
    while (auto *row = _list.get_row_at_index(0)) _list.remove(*row);
    _pictures = sp_images_missing(_document);
    for (auto const *picture : _pictures) {
        auto const [file, folder] = name_and_folder(picture->linkedFile());
        auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->set_margin(4);
        auto *name = Gtk::make_managed<Gtk::Label>(file);
        name->set_xalign(0);
        name->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        name->set_max_width_chars(36);
        name->set_tooltip_text(picture->linkedFile());
        auto *where = Gtk::make_managed<Gtk::Label>(folder);
        where->set_xalign(0);
        where->set_hexpand(true);
        where->set_ellipsize(Pango::EllipsizeMode::START);
        where->add_css_class("dim-label");
        row->append(*name);
        row->append(*where);
        _list.append(*row);
    }
    if (auto *first = _list.get_row_at_index(0)) _list.select_row(*first);
    _scroll.set_visible(!_pictures.empty());
    _relink_row.set_visible(!_pictures.empty() || !_found.get_text().empty());
    _relink.set_visible(!_pictures.empty());
    if (!_pictures.empty()) {
        _message.set_text(_("These linked pictures can't be found. Each shows as the small picture the document keeps "
                            "of it, or as a marked box, until it is linked again."));
    } else {
        _message.set_text(_("Every picture this document links to was found."));
    }
    _select.set_visible(!_pictures.empty());
    update();
}

void MissingLinks::update()
{
    _relink.set_sensitive(_list.get_selected_row() != nullptr);
}

void MissingLinks::relink()
{
    auto const *row = _list.get_selected_row();
    if (!row || row->get_index() < 0 || std::size_t(row->get_index()) >= _pictures.size()) return;
    auto *picture = _pictures[row->get_index()];

    static std::string current_folder;
    std::vector<Glib::ustring> const mime_types = {"image/png", "image/jpeg", "image/gif", "image/bmp", "image/tiff",
                                                   "image/svg+xml"};
    auto const [name, folder] = name_and_folder(picture->linkedFile());
    auto const file = choose_file_open(Glib::ustring::compose(_("Relink %1"), name), &_dialog, mime_types, current_folder);
    if (!file) return;

    auto const before = _pictures.size();
    auto const linked = sp_images_relink(picture, file->get_path());
    if (!linked) {
        _found.set_text(_("That file isn't a picture that can be read."));
        return;
    }
    DocumentUndo::done(_document, RC_("Undo", "Relink pictures that can't be found"), INKSCAPE_ICON("shape-image"));
    _found.set_text(linked > 1 ? Glib::ustring::compose(_("Linked, and %1 more found in that folder."), linked - 1)
                               : before > 1 ? Glib::ustring(_("Linked. None of the others is in that folder."))
                                            : Glib::ustring(_("Linked.")));
    fill();
}

void MissingLinks::run()
{
    Inkscape::UI::dialog_run(_dialog);

    if (_at_open && _quiet.get_active()) Inkscape::Preferences::get()->setBool(AT_OPEN, false);
    if (_select.get_visible() && _select.get_active()) {
        std::vector<SPItem *> list;
        for (auto *picture : sp_images_missing(_document)) list.push_back(picture);
        auto *selection = _desktop->getSelection();
        selection->clear();
        selection->setList(list);
    }
}

} // namespace

bool missingLinksToShow(SPDocument *document)
{
    return document && Inkscape::Preferences::get()->getBool(AT_OPEN, true) && !sp_images_missing(document).empty();
}

void showMissingLinks(SPDesktop *desktop, bool at_open)
{
    if (!desktop || !desktop->getDocument()) return;
    MissingLinks(desktop, at_open).run();
}

} // namespace Inkscape::UI::Dialog
