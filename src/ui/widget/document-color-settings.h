// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_DOCUMENT_COLOR_SETTINGS_H
#define INKSCAPE_DOCUMENT_COLOR_SETTINGS_H
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include "colors/document-colors.h"
#include "xml/helper-observer.h"

namespace Inkscape::UI::Widget {
class DocumentColorSettings : public Gtk::Box {
public:
    DocumentColorSettings();
    void set_document(SPDocument *document);
private:
    void populate();
    void update();
    void update_profiles();
    void apply();
    Colors::Space::Type selected_mode() const;
    SPDocument *_document = nullptr;
    bool _updating = false;
    bool _populated = false;
    Gtk::Grid _grid;
    Gtk::ComboBoxText _mode, _assignment, _profile, _new_mode, _rgb, _cmyk, _intent;
    Gtk::CheckButton _bpc{"Black point compensation"};
    Gtk::Button _apply{"Assign Profile"};
    Gtk::Label _status, _help;
    std::vector<std::shared_ptr<Colors::CMS::Profile>> _rgb_profiles, _cmyk_profiles, _document_profiles;
    XML::SignalObserver _observer;
};
}
#endif
