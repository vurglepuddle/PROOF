// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SEEN_CHOOSE_FILE_FILTER_H
#define SEEN_CHOOSE_FILE_FILTER_H

#include <glibmm/refptr.h>
#include <vector>

namespace Inkscape::Extension { class Output; }

namespace Gio {
template <typename> class ListStore;
} // namespace Gio

namespace Glib {
class ustring;
}

namespace Gtk {
class FileFilter;
} // namespace Gtk

namespace Inkscape::UI::Dialog {

/// Find the start directory for a file dialog.
void get_start_directory(std::string &start_path, Glib::ustring const &prefs_path, bool try_document_dir = false);

/// Create a Gtk::FileFilter for all image file types.
[[nodiscard]] Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> create_open_filters();

/// Create a Gtk::FileFilter for all export file types.
/// With filter_outputs, retain separate modules sharing a suffix and return their
/// pointers in filter order (the All Files entry is null).
[[nodiscard]] Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> create_export_filters(
    bool for_save = false, bool allow_save_copy = false,
    std::vector<Inkscape::Extension::Output *> *filter_outputs = nullptr);


} // namespace Inkscape::UI::Dialog

#endif // SEEN_CHOOSE_FILE_FILTER_H

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
