// SPDX-License-Identifier: GPL-2.0-or-later

#include "choose-file.h"

#include <giomm/liststore.h>
#include <glib/gi18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/error.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/dialog.h>

#include "choose-file-utils.h"
#include "io/sys.h"
#include "preferences.h"

namespace Inkscape {

Glib::RefPtr<Gtk::FileDialog> create_file_dialog(Glib::ustring const &title,
                                                 Glib::ustring const &accept_label)
{
    auto const file_dialog = Gtk::FileDialog::create();
    file_dialog->set_title(title);
    file_dialog->set_accept_label(accept_label);
    return file_dialog;
}

void set_filters(Gtk::FileDialog &file_dialog,
                 Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters)
{
    file_dialog.set_filters(filters);
    if (filters->get_n_items() > 0) {
        file_dialog.set_default_filter(filters->get_item(0));
    }
}

void set_filter(Gtk::FileDialog &file_dialog, Glib::RefPtr<Gtk::FileFilter> const &filter)
{
    auto const filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    set_filters(file_dialog, filters);
}

using StartMethod = void (Gtk::FileDialog::*)
                    (Gtk::Window &, Gio::SlotAsyncReady const &,
                     Glib::RefPtr<Gio::Cancellable> const &);

using FinishMethod = Glib::RefPtr<Gio::File> (Gtk::FileDialog::*)
                     (Glib::RefPtr<Gio::AsyncResult> const &);

[[nodiscard]] static auto run(Gtk::FileDialog &file_dialog, Gtk::Window &parent,
                              std::string &current_folder,
                              StartMethod const start, FinishMethod const finish)
{
    file_dialog.set_initial_folder(Gio::File::create_for_path(current_folder));

    bool responded = false;
    Glib::RefPtr<Gio::File> file;

    (file_dialog.*start)(parent, [&](Glib::RefPtr<Gio::AsyncResult> const &result)
    {
        try {
            responded = true;

            file = (file_dialog.*finish)(result);
            if (!file) {
                return;
            }

            current_folder = file->get_parent()->get_path();
        } catch (Gtk::DialogError const &error) {
            if (error.code() == Gtk::DialogError::Code::DISMISSED) {
                responded = true;
            } else {
                throw;
            }
        }
    }, Glib::RefPtr<Gio::Cancellable>{});

    auto const main_context = Glib::MainContext::get_default();
    while (!responded) {
         main_context->iteration(true);
    }

    return file;
}

Glib::RefPtr<Gio::File> choose_file_save(Glib::ustring const &title, Gtk::Window *parent,
                                         Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters_model,
                                         std::string const &file_name,
                                         std::string &current_folder,
                                         Glib::RefPtr<Gtk::FileFilter> *selected_filter)
{
    if (!parent) return {};

    if (current_folder.empty()) {
        current_folder = Glib::get_home_dir();
    }

    if (selected_filter) {
        // FileDialog returns only a path. Saving needs the chosen output module too:
        // native AI, legacy AI and PDF interchange all use the same filename suffix.
        *selected_filter = {};
        // The Win32 FileChooserNative getter also returns its fallback dialog's
        // filter, discarding the native selection. Use the chooser that owns it.
        Gtk::FileChooserDialog chooser(*parent, title, Gtk::FileChooser::Action::SAVE, true);
        chooser.set_modal(true);
        chooser.add_button(_("Cancel"), Gtk::ResponseType::CANCEL);
        chooser.add_button(_("Save"), Gtk::ResponseType::ACCEPT);
        chooser.set_default_response(Gtk::ResponseType::ACCEPT);
        chooser.set_current_folder(Gio::File::create_for_path(current_folder));
        chooser.set_current_name(file_name);
        if (filters_model) {
            for (unsigned i = 0; i < filters_model->get_n_items(); ++i) {
                chooser.add_filter(filters_model->get_item(i));
            }
            if (filters_model->get_n_items()) chooser.set_filter(filters_model->get_item(0));
        }
        auto filename_connection = chooser.property_filter().signal_changed().connect([&] {
            auto filter = chooser.get_filter();
            auto suffix = filter ? static_cast<char const *>(
                g_object_get_data(G_OBJECT(filter->gobj()), "inkscape-save-suffix")) : nullptr;
            std::string name = chooser.get_current_name();
            if (suffix && !name.empty()) {
                IO::swap_file_extension(name, suffix);
                chooser.set_current_name(name);
            }
        });
        bool responded = false;
        Glib::RefPtr<Gio::File> file;
        auto connection = chooser.signal_response().connect([&](int response) {
            if (response == static_cast<int>(Gtk::ResponseType::ACCEPT)) {
                file = chooser.get_file();
                *selected_filter = chooser.get_filter();
            }
            responded = true;
        });
        chooser.present();
        auto context = Glib::MainContext::get_default();
        while (!responded) context->iteration(true);
        connection.disconnect();
        filename_connection.disconnect();
        if (file && file->get_parent()) current_folder = file->get_parent()->get_path();
        return file;
    }

    auto const file_dialog = create_file_dialog(title, _("Save"));

    if (filters_model) {
        set_filters(*file_dialog, filters_model);
        // for (int i = 0; i < filters_model->get_n_items(); ++i) {
        //     std::cout << filters_model->get_item(i)->get_name() << std::endl;
        // }
    }

    file_dialog->set_initial_name(file_name);

    return run(*file_dialog, *parent, current_folder,
               &Gtk::FileDialog::save, &Gtk::FileDialog::save_finish);
}

Glib::RefPtr<Gio::File> choose_file_save(Glib::ustring const &title, Gtk::Window *parent,
                                         Glib::ustring const &mime_type,
                                         std::string const &file_name,
                                         std::string &current_folder)
{
    if (!parent) return {};

    auto filters_model = Gio::ListStore<Gtk::FileFilter>::create();
    auto filter = Gtk::FileFilter::create();
    if (!mime_type.empty()) {
        auto filter = Gtk::FileFilter::create();
        filter->add_mime_type(mime_type);
    }

    return choose_file_save(title, parent, filters_model, file_name, current_folder);
}

Glib::RefPtr<Gio::File> choose_file_open(Glib::ustring const &title, Gtk::Window *parent,
                                         Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters_model,
                                         std::string &current_folder,
                                         Glib::ustring const &accept)
{
    if (!parent) return Glib::RefPtr<Gio::File>();

    if (current_folder.empty()) {
        current_folder = Glib::get_home_dir();
    }

    auto const file_dialog = create_file_dialog(title, accept.empty() ? _("Open") : accept);

    if (filters_model) {
        set_filters(*file_dialog, filters_model);
    }

    return run(*file_dialog, *parent, current_folder,
               &Gtk::FileDialog::open, &Gtk::FileDialog::open_finish);
}

Glib::RefPtr<Gio::File> choose_file_open(Glib::ustring const &title, Gtk::Window *parent,
                                         std::vector<Glib::ustring> const &mime_types,
                                         std::string &current_folder,
                                         Glib::ustring const &accept)
{
    auto filters_model = Gio::ListStore<Gtk::FileFilter>::create();
    auto filter = Gtk::FileFilter::create();
    for (auto const &t : mime_types) {
        filter->add_mime_type(t);
    }
    filters_model->append(filter);

    return choose_file_open(title, parent, filters_model, current_folder, accept);
}

Glib::RefPtr<Gio::File> choose_file_open(Glib::ustring const &title, Gtk::Window *parent,
                                         std::vector<std::pair<Glib::ustring, Glib::ustring>> const &filters,
                                         std::string &current_folder,
                                         Glib::ustring const &accept)
{
    auto filters_model = Gio::ListStore<Gtk::FileFilter>::create();

    auto all_supported = Gtk::FileFilter::create();
    if (filters.size() > 1) {
        all_supported->set_name(_("All Supported Formats"));
        filters_model->append(all_supported);
    }

    for (auto const &f : filters) {
        auto filter = Gtk::FileFilter::create();
        filter->set_name(f.first);
        filter->add_pattern(f.second);
        filters_model->append(filter);
        all_supported->add_pattern(f.second);
    }

    return choose_file_open(title, parent, filters_model, current_folder, accept);
}

// Open one or more image files.
std::vector<Glib::RefPtr<Gio::File>> choose_file_open_images(Glib::ustring const &title,
                                                             Gtk::Window* parent,
                                                             std::string const &pref_path,
                                                             Glib::ustring const &accept)
{
    auto const file_dialog = create_file_dialog(title, accept);
    auto filter_model = Inkscape::UI::Dialog::create_open_filters();
    set_filters(*file_dialog, filter_model);

    std::string current_folder;
    Inkscape::UI::Dialog::get_start_directory(current_folder, pref_path, true);
    if (current_folder.empty()) {
        current_folder = Glib::get_home_dir();
    }
    file_dialog->set_initial_folder(Gio::File::create_for_path(current_folder));

    bool responded = false;
    std::vector<Glib::RefPtr<Gio::File>> files;

    file_dialog->open_multiple(*parent, [&](Glib::RefPtr<Gio::AsyncResult> const &result)
    {
        try {
            responded = true;

            files = file_dialog->open_multiple_finish(result);
            if (files.size() == 0) {
                return;
            }
            if (files.size() == 1) {
                // Save current_folder.
                current_folder = files[0]->get_parent()->get_path();
                Inkscape::Preferences *prefs = Inkscape::Preferences::get();
                prefs->setString(pref_path, current_folder);
            }
        } catch (Gtk::DialogError const &error) {
            if (error.code() == Gtk::DialogError::Code::DISMISSED) {
                responded = true;
            } else {
                throw;
            }
        }
    }, Glib::RefPtr<Gio::Cancellable>{});

    auto const main_context = Glib::MainContext::get_default();
    while (!responded) {
        main_context->iteration(true);
    }

    return files;
}} // namespace Inkscape

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
