// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief A dialog for the about screen
 *
 * Copyright (C) Martin Owens 2019 <doctormo@gmail.com>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "about.h"

#include <fstream>
#include <random>
#include <regex>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/aspectframe.h>
#include <gtkmm/binlayout.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/picture.h>
#include <gtkmm/textview.h>

#include "desktop.h"
#include "display/cairo-utils.h"
#include "inkscape-version-info.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "io/resource.h"
#include "ui/builder-utils.h"
#include "ui/svg-renderer.h"
#include "ui/themes.h"
#include "ui/util.h"

// how long to show each about screen in seconds
constexpr int SLIDESHOW_DELAY_sec = 10;

using namespace Inkscape::IO;

namespace Inkscape::UI::Dialog {
namespace {

class AboutWindow : public Gtk::Window {
public:
    AboutWindow(BaseObjectType* cobject, const Glib::RefPtr<Gtk::Builder>& builder): Gtk::Window(cobject) {
        find_about_screens();
        if (_about_screens.empty()) {
            g_error("AboutWindow: Missing about screens.");
            return;
        }

        _viewer1 = &get_widget<Gtk::Picture>(builder, "viewer1");
        _viewer1->set_layout_manager(Gtk::BinLayout::create());
        _viewer2 = &get_widget<Gtk::Picture>(builder, "viewer2");
        _viewer2->set_layout_manager(Gtk::BinLayout::create());
        _frame = &get_widget<Gtk::AspectFrame>(builder, "aspect-frame");
        _footer = &get_widget<Gtk::Box>(builder, "dialog-footer");
    }

    void show_window() {
        // PROOF: one screen of its own has nothing to change to.
        if (_about_screens.size() > 1) {
            _refresh = Glib::signal_timeout().connect_seconds([this] {
                transition();
                return true;
            }, SLIDESHOW_DELAY_sec);
        }

        // reset the stage
        _viewer1->set_paintable({});
        _viewer2->set_paintable({});
        _about_index = 0;
        _tick = false;
        auto ctx = _viewer2->get_style_context();
        ctx->remove_class("fade-out");
        ctx->remove_class("fade-in");

        present();
        transition();
    }

private:
    std::vector<std::string> _about_screens;
    size_t _about_index = 0;
    bool _tick = false;
    Gtk::Box* _footer;
    Glib::RefPtr<Gtk::CssProvider> _footer_style;
    Glib::RefPtr<Glib::TimeoutSource> _timer;
    Gtk::Picture *_viewer1;
    Gtk::Picture *_viewer2;
    sigc::scoped_connection _refresh;
    Gtk::AspectFrame* _frame = nullptr;

    void find_about_screens() {
        // PROOF: its own screen. Inkscape's release artwork is shown only where that one is missing.
        if (auto own = Resource::get_filename(Resource::SCREENS, "proof-about.svg", false, true); !own.empty()) {
            _about_screens.push_back(std::move(own));
            return;
        }
        auto path = Glib::build_filename(get_path_string(Resource::SYSTEM, Resource::SCREENS), "about");
        Resource::get_filenames_from_path(_about_screens, path, {".svgz"}, {});
        if (_about_screens.empty()) {
            g_warning("Error loading about screens SVGZs: no such documents in share/screen/about folder.");
            // fall back
            _about_screens.push_back(Resource::get_filename(Resource::SCREENS, "about.svg", true, false));
        }
        std::sort(_about_screens.begin(), _about_screens.end());
    }

    Cairo::RefPtr<Cairo::ImageSurface> load_next(Gtk::Picture *viewer, const Glib::ustring& fname, int device_scale) {
        svg_renderer renderer(fname.c_str());
        auto surface = renderer.render_surface(device_scale);
        if (surface) {
            auto width = renderer.get_width_px();
            auto height = renderer.get_height_px();
            _frame->property_ratio() = width / height;
            viewer->set_size_request(width, height);
        }
        viewer->set_paintable(to_texture(surface));
        return surface;
    }

    void set_footer_matching_color(Cairo::RefPtr<Cairo::ImageSurface> const &image)
    {
        if (!image) return;

        auto scale = get_scale_factor();

        // extract color from a strip at the bottom of the rendered about image
        int width = image->get_width();
        int height = 5 * scale;
        int y = (image->get_height() - height) / scale;
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, width, height);
        cairo_surface_set_device_scale(surface->cobj(), scale, scale);
        auto ctx = Cairo::Context::create(surface);
        ctx->set_source(image, 0, -y);
        ctx->paint();

        // calculate footer color: light/dark depending on a theme
        bool dark = INKSCAPE.themecontext->isCurrentThemeDark(this);
        auto foot = Colors::make_theme_color(ink_cairo_surface_average_color(surface->cobj()), dark);

        auto style_context = _footer->get_style_context();
        _footer_style = Gtk::CssProvider::create();
        _footer_style->load_from_data("box {background-color:" + foot.toString() + ";}");
        if (_footer_style) style_context->remove_provider(_footer_style);
        style_context->add_provider(_footer_style, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    // load next about screen
    void transition() {
        _tick = !_tick;
        auto nv = _tick ? _viewer1 : _viewer2;
        auto image = load_next(nv, _about_screens[_about_index++ % _about_screens.size()], get_scale_factor());

        auto ctx = _viewer2->get_style_context();
        if (_tick) {
            ctx->add_class("fade-out");
            ctx->remove_class("fade-in");
        }
        else {
            ctx->remove_class("fade-out");
            ctx->add_class("fade-in");
        }

        set_footer_matching_color(image);
    }
};

void copy(Gtk::Button *button, Gtk::Label *label, Glib::ustring const &text)
{
    auto clipboard = Gdk::Display::get_default()->get_clipboard();
    clipboard->set_text(text);
    reveal_widget(button, false);
    reveal_widget(label, true);
    Glib::signal_timeout().connect_seconds(
        sigc::track_object([=] { // disconnects on destruction
            reveal_widget(button, true);
            reveal_widget(label, false);
            return false;
        },
        *button),
    2);
}

} // namespace

template <class Random>
[[nodiscard]] static auto get_shuffled_lines(std::string const &filename, Random &&random)
{
    std::ifstream fn{Resource::get_filename(Resource::DOCS, filename.c_str())};
    std::vector<std::string> lines;
    std::size_t capacity = 0;
    for (std::string line; getline(fn, line);) {
        capacity += line.size() + 1;
        lines.push_back(std::move(line));
    }
    std::shuffle(lines.begin(), lines.end(), random);
    return std::pair{std::move(lines), capacity};
}

void show_about()
{
    // Load builder file here
    auto builder = create_builder("inkscape-about.glade");
    auto window       = &get_derived_widget<AboutWindow>(builder, "about-screen-window");
    auto tabs         = &get_widget<Gtk::Notebook>(builder, "tabs");
    auto version      = &get_widget<Gtk::Button>  (builder, "version");
    auto version_lbl  = &get_widget<Gtk::Label>   (builder, "version-label");
    auto label        = &get_widget<Gtk::Label>   (builder, "version-copied");
    auto debug_info   = &get_widget<Gtk::Button>  (builder, "debug-info");
    auto label2       = &get_widget<Gtk::Label>   (builder, "debug-info-copied");
    auto copyright    = &get_widget<Gtk::Label>   (builder, "copyright");
    auto authors      = &get_widget<Gtk::TextView>(builder, "credits-authors");
    auto translators  = &get_widget<Gtk::TextView>(builder, "credits-translators");
    auto license      = &get_widget<Gtk::Label>   (builder, "license-text");

    auto text = Inkscape::proof_version();
    version_lbl->set_label(text);
    version->signal_clicked().connect(
        sigc::bind(&copy, version, label, std::move(text)));

    debug_info->signal_clicked().connect(
        sigc::bind(&copy, version, label2, Inkscape::debug_info()));

    copyright->set_label(
        Glib::ustring::compose(copyright->get_label(), std::to_string(Inkscape::inkscape_build_year())));

    std::random_device rd;
    std::mt19937 g(rd());
    auto const [authors_data, capacity] = get_shuffled_lines("AUTHORS", g);
    std::string str_authors;
    str_authors.reserve(capacity);
    for (auto const &author : authors_data) {
        str_authors.append(author).append(1, '\n');
    }
    authors->get_buffer()->set_text(str_authors.c_str());

    auto const [translators_data, capacity2] = get_shuffled_lines("TRANSLATORS", g);
    std::string str_translators;
    str_translators.reserve(capacity2);
    std::regex e("(.*?)(<.*|)");
    for (auto const &translator : translators_data) {
        str_translators.append(std::regex_replace(translator, e, "$1")).append(1, '\n');
    }
    translators->get_buffer()->set_text(str_translators.c_str());

    std::ifstream fn(Resource::get_filename(Resource::DOCS, "LICENSE"));
    std::string str((std::istreambuf_iterator<char>(fn)),
                        std::istreambuf_iterator<char>());
    license->set_markup(str.c_str());

    // Handle Esc to close the window
    auto const controller = Gtk::EventControllerKey::create();
    controller->signal_key_pressed().connect(
        sigc::track_object([window] (unsigned keyval, unsigned, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) {
                window->close();
                return true;
            }
            return false;
        }, *window),
    false);
    window->add_controller(controller);

    if (auto top = SP_ACTIVE_DESKTOP ? SP_ACTIVE_DESKTOP->getInkscapeWindow() : nullptr) {
        window->set_transient_for(*top);
    }
    tabs->set_current_page(0);
    window->show_window();

    Gtk::manage(window); // will self-destruct
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
