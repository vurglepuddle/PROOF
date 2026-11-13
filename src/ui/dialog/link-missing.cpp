// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the Missing Links window. See link-missing.h.
 */
#include "link-missing.h"

#include <algorithm>
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
#include <gtkmm/sizegroup.h>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "extension/missing-link-save.h"
#include "object/sp-image.h"
#include "preferences.h"
#include "selection.h"
#include "ui/dialog-run.h"
#include "ui/dialog/choose-file.h"
#include "ui/icon-names.h"
#include "ui/interface.h"
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
    MissingLinks(SPDesktop *desktop, bool at_open, Extension::MissingLinkSave *staged = nullptr);

    int run();

private:
    void fill();
    void update();
    void relink();
    void discard(SPImage *picture = nullptr);
    bool can_discard(SPImage const *picture) const;

    SPDesktop *_desktop;
    SPDocument *_document;
    bool _at_open;
    bool _saving;
    Extension::MissingLinkSave *_staged;

    Gtk::Dialog _dialog;
    Gtk::Label _message, _found;
    Gtk::ScrolledWindow _scroll;
    Gtk::ListBox _list;
    Gtk::Box _content{Gtk::Orientation::VERTICAL, 8}, _buttons{Gtk::Orientation::HORIZONTAL, 12};
    Gtk::Box _spacer;
    Gtk::Button _relink, _discard_all, _cancel, _confirm;
    Glib::RefPtr<Gtk::SizeGroup> _button_sizes = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL);
    Gtk::CheckButton _select, _quiet;

    std::vector<SPImage *> _pictures;
};

MissingLinks::MissingLinks(SPDesktop *desktop, bool at_open, Extension::MissingLinkSave *staged)
    : _desktop(desktop)
    , _document(staged ? staged->document() : desktop->getDocument())
    , _at_open(at_open)
    , _saving(staged != nullptr)
    , _staged(staged)
{
    _dialog.set_title(_("Missing Links"));
    _dialog.set_modal(true);
    _dialog.set_resizable(true);
    _dialog.set_default_size(680, 360);
    _dialog.set_size_request(640, 300);
    _desktop->setWindowTransient(_dialog);

    auto *area = _dialog.get_content_area();
    area->set_margin(12);
    area->set_spacing(12);
    area->set_vexpand(true);
    _content.set_vexpand(true); // Still expands when the now-empty list is hidden.
    area->append(_content);
    area->append(_buttons);
    auto *box = &_content;

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
    box->append(_found);
    _discard_all.set_label(_("_Discard Missing"));
    _discard_all.set_use_underline(true);
    _discard_all.signal_clicked().connect([this] { discard(); });
    _cancel.set_label(_("_Cancel"));
    _cancel.set_use_underline(true);
    _cancel.signal_clicked().connect([this] { _dialog.response(Gtk::ResponseType::CANCEL); });
    _confirm.set_use_underline(true);
    _confirm.signal_clicked().connect([this] {
        _dialog.response(_saving ? Gtk::ResponseType::ACCEPT : Gtk::ResponseType::CLOSE);
    });
    _spacer.set_hexpand(true);
    _buttons.append(_relink);
    _buttons.append(_discard_all);
    _buttons.append(_spacer);
    _buttons.append(_cancel);
    _buttons.append(_confirm);
    for (auto *button : {&_relink, &_discard_all, &_cancel, &_confirm}) {
        button->set_size_request(112, 36);
        _button_sizes->add_widget(*button);
    }

    _select.set_label(_("Select all the affected items"));
    _select.set_active(_at_open);
    if (!_saving) UI::pack_start(*box, _select, false, false);
    _quiet.set_label(_("Don't show this warning again"));
    if (_at_open) UI::pack_start(*box, _quiet, false, false);

    if (_saving) {
        _dialog.set_default_widget(_cancel);
        _dialog.set_focus(_cancel);
    } else {
        _cancel.set_visible(false);
        _confirm.set_label(_("_Close"));
        _dialog.set_default_widget(_confirm);
    }

    fill();
}

/// The list as the document now stands.
void MissingLinks::fill()
{
    while (auto *row = _list.get_row_at_index(0)) _list.remove(*row);
    _pictures = sp_images_missing(_document);
    for (auto *picture : _pictures) {
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
        auto *remove = Gtk::make_managed<Gtk::Button>(_("Discard"));
        remove->set_size_request(112, 36);
        remove->set_sensitive(can_discard(picture));
        remove->set_tooltip_text(can_discard(picture) ? _("Remove this picture. Undo brings it back.") :
            _("Discard is unavailable inside a clipping path or mask, or without a matching object ID."));
        remove->signal_clicked().connect([this, picture] { discard(picture); });
        row->append(*remove);
        _list.append(*row);
    }
    if (auto *first = _list.get_row_at_index(0)) _list.select_row(*first);
    _scroll.set_visible(!_pictures.empty());
    _found.set_visible(!_found.get_text().empty());
    _relink.set_visible(!_pictures.empty());
    _discard_all.set_visible(!_pictures.empty());
    _discard_all.set_sensitive(std::any_of(_pictures.begin(), _pictures.end(), [this](auto *p) { return can_discard(p); }));
    if (!_pictures.empty()) {
        _message.set_text(_("These linked pictures can't be found. Each shows as the small picture the document keeps "
                            "of it, or as a marked box, until it is linked again."));
    } else {
        _message.set_text(_("No linked pictures are missing."));
    }
    if (_saving) {
        if (!_pictures.empty()) _message.set_text(_("Linked pictures are missing. Relink them, discard them, save anyway with a warning, "
            "or cancel. PDF output may leave missing pictures blank. Native Illustrator files keep their links and previews; "
            "saving is refused if a usable preview is unavailable. Discard removes a picture; Undo brings it back after saving."));
        _confirm.set_label(_pictures.empty() ? _("_Continue") : _("_Save Anyway"));
    }
    if (std::any_of(_pictures.begin(), _pictures.end(), [this](auto *p) { return !can_discard(p); })) {
        _message.set_text(_message.get_text() + " " + _("Discard is unavailable inside clipping paths or masks, or without a matching object ID."));
    }
    _select.set_visible(!_saving && !_pictures.empty());
    update();
}

void MissingLinks::update()
{
    _relink.set_sensitive(_list.get_selected_row() != nullptr);
}

bool MissingLinks::can_discard(SPImage const *picture) const
{
    return _staged ? _staged->can_discard(picture) : Extension::can_discard_missing_picture(picture);
}

void MissingLinks::discard(SPImage *picture)
{
    auto const pictures = picture ? std::vector<SPImage *>{picture} : _pictures;
    unsigned removed = 0;
    for (auto *image : pictures) {
        if (_staged ? _staged->discard(image) : Extension::discard_missing_picture(image)) ++removed;
    }
    if (removed && !_saving) DocumentUndo::done(_document, RC_("Undo", "Discard missing pictures"), INKSCAPE_ICON("shape-image"));
    _found.set_text(_saving ? _("Discarded. Undo brings the pictures back after the changes are accepted.") :
                             _("Discarded. Undo brings the pictures back."));
    fill();
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
    if (!_saving) DocumentUndo::done(_document, RC_("Undo", "Relink pictures that can't be found"), INKSCAPE_ICON("shape-image"));
    _found.set_text(linked > 1 ? Glib::ustring::compose(_("Linked, and %1 more found in that folder."), linked - 1)
                               : before > 1 ? Glib::ustring(_("Linked. None of the others is in that folder."))
                                            : Glib::ustring(_("Linked.")));
    fill();
}

int MissingLinks::run()
{
    auto const response = Inkscape::UI::dialog_run(_dialog);

    if (_at_open && _quiet.get_active()) Inkscape::Preferences::get()->setBool(AT_OPEN, false);
    if (_select.get_visible() && _select.get_active()) {
        std::vector<SPItem *> list;
        for (auto *picture : sp_images_missing(_document)) list.push_back(picture);
        auto *selection = _desktop->getSelection();
        selection->clear();
        selection->setList(list);
    }
    return response;
}

} // namespace

Extension::MissingLinkAction resolveMissingLinksForSave(SPDesktop *desktop, SPDocument *document)
{
    using Extension::MissingLinkAction;
    if (sp_images_missing(document).empty()) return MissingLinkAction::RequirePresent;
    if (!desktop) return MissingLinkAction::Cancel;
    Extension::MissingLinkSave staged(document);
    if (MissingLinks(desktop, false, &staged).run() != Gtk::ResponseType::ACCEPT) {
        return MissingLinkAction::Cancel;
    }
    auto const action = sp_images_missing(staged.document()).empty() ? MissingLinkAction::RequirePresent
                                                                   : MissingLinkAction::Ignore;
    if (!staged.accept(action, desktop->getDocument())) {
        sp_ui_error_dialog(_("Missing picture changes could not be matched to this document. Nothing was applied. "
                             "Reopen the document before trying again."));
        return MissingLinkAction::Cancel;
    }
    if (staged.changed) DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Relink or discard missing pictures"),
                                         INKSCAPE_ICON("shape-image"));
    return action;
}

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
