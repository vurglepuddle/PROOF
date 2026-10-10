// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Drag and drop of drawings onto canvas.
 */

/* Authors:
 *
 * Copyright (C) Tavmjong Bah 2019
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "drag-and-drop.h"

#include <giomm/memoryoutputstream.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <gtkmm/droptarget.h>

#include "colors/dragndrop.h"
#include "desktop-style.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "extension/input.h"
#include "file.h"
#include "gradient-drag.h"
#include "layer-manager.h"
#include "object/sp-flowtext.h"
#include "object/sp-text.h"
#include "path/path-util.h"
#include "selection.h"
#include "style.h"
#include "ui/clipboard.h"
#include "ui/interface.h"
#include "ui/tools/tool-base.h"
#include "ui/widget/canvas.h" // Target, canvas to world transform.
#include "ui/widget/desktop-widget.h"
#include "util/value-utils.h"

using Inkscape::DocumentUndo;
using namespace Inkscape::Util;
namespace Colors = Inkscape::Colors;

namespace {

/*
 * Gtk API wrapping - Todo: Improve gtkmm.
 */

template <typename T, typename F>
void foreach(GSList *list, F &&f)
{
    g_slist_foreach(list, +[] (void *ptr, void *data) {
        auto t = reinterpret_cast<T *>(ptr);
        auto f = reinterpret_cast<F *>(data);
        f->operator()(t);
    }, &f);
}

std::span<char const> get_span(Glib::RefPtr<Glib::Bytes> const &bytes)
{
    gsize size{};
    return {reinterpret_cast<char const *>(bytes->get_data(size)), size};
}

template <typename T>
Glib::RefPtr<Glib::Bytes> make_bytes(T &&t)
{
    using Td = std::decay_t<T>;
    auto const p = new Td(std::forward<T>(t));
    auto const span = std::span<char const>(*p);
    return Glib::wrap(g_bytes_new_with_free_func(span.data(), span.size_bytes(), +[] (void *p) {
        delete reinterpret_cast<Td *>(p);
    }, p));
}

template <typename T>
Glib::ValueBase from_bytes(Glib::RefPtr<Glib::Bytes> &&bytes, char const *mime_type) = delete;

template <typename T>
void deserialize_func(GdkContentDeserializer *deserializer)
{
    auto const in = Glib::wrap(gdk_content_deserializer_get_input_stream(deserializer), true);
    auto const out = Gio::MemoryOutputStream::create();
    out->splice_async(in, [deserializer, out] (Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
            out->splice_finish(result);
            out->close();
            *gdk_content_deserializer_get_value(deserializer) = GlibValue::release(from_bytes<T>(out->steal_as_bytes(), gdk_content_deserializer_get_mime_type(deserializer)));
            gdk_content_deserializer_return_success(deserializer);
        } catch (Glib::Error const &error) {
            gdk_content_deserializer_return_error(deserializer, g_error_copy(error.gobj()));
        }
    }, Gio::OutputStream::SpliceFlags::CLOSE_SOURCE);
};

template <typename T>
void register_deserializer(char const *mime_type)
{
    gdk_content_register_deserializer(mime_type, GlibValue::type<T>(), deserialize_func<T>, nullptr, nullptr);
}

template <typename T>
Glib::RefPtr<Glib::Bytes> to_bytes(T const &t, char const *mime_type) = delete;

template <typename T>
void serialize_func(GdkContentSerializer *serializer)
{
    auto const out = Glib::wrap(gdk_content_serializer_get_output_stream(serializer), true);
    auto const bytes = to_bytes(*GlibValue::get<T>(gdk_content_serializer_get_value(serializer)), gdk_content_serializer_get_mime_type(serializer));
    auto const span = get_span(bytes);
    out->write_all_async(span.data(), span.size_bytes(), [serializer, out, bytes] (Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
            gsize _;
            out->write_all_finish(result, _);
            gdk_content_serializer_return_success(serializer);
        } catch (Glib::Error const &error) {
            gdk_content_serializer_return_error(serializer, g_error_copy(error.gobj()));
        }
    });
};

template <typename T>
void register_serializer(char const *mime_type)
{
    gdk_content_register_serializer(GlibValue::type<T>(), mime_type, serialize_func<T>, nullptr, nullptr);
}

/*
 * Actual code
 */

struct DnDSvg
{
    Glib::RefPtr<Glib::Bytes> bytes;
};

template <>
Glib::ValueBase from_bytes<DnDSvg>(Glib::RefPtr<Glib::Bytes> &&bytes, char const *)
{
    return GlibValue::create<DnDSvg>(DnDSvg{std::move(bytes)});
}

template <>
Glib::ValueBase from_bytes<Colors::Paint>(Glib::RefPtr<Glib::Bytes> &&bytes, char const *mime_type)
{
    try {
        return GlibValue::create<Colors::Paint>(Colors::fromMIMEData(get_span(bytes), mime_type));
    } catch (Colors::ColorError const &c) {
        throw Glib::Error(G_FILE_ERROR, 0, c.what());
    }
}

template <>
Glib::RefPtr<Glib::Bytes> to_bytes<Colors::Paint>(Colors::Paint const &paint, char const *mime_type)
{
    return make_bytes(getMIMEData(paint, mime_type));
}

template <>
Glib::RefPtr<Glib::Bytes> to_bytes<DnDSymbol>(DnDSymbol const &symbol, char const *)
{
    return make_bytes(symbol.id.raw());
}

std::vector<GType> const &get_drop_types()
{
    static auto const instance = [] () -> std::vector<GType> {
        for (auto mime_type : {"image/svg", "image/svg+xml"}) {
            register_deserializer<DnDSvg>(mime_type);
        }

        for (auto mime_type : {Colors::mimeOSWB_COLOR, Colors::mimeX_COLOR}) {
            register_deserializer<Colors::Paint>(mime_type);
        }

        for (auto mime_type : {Colors::mimeOSWB_COLOR, Colors::mimeX_COLOR, Colors::mimeTEXT}) {
            register_serializer<Colors::Paint>(mime_type);
        }

        register_serializer<DnDSymbol>("text/plain;charset=utf-8");

        return {
            GlibValue::type<Colors::Paint>(),
            GlibValue::type<DnDSvg>(),
            GDK_TYPE_FILE_LIST,
            GlibValue::type<DnDSymbol>(),
            GDK_TYPE_TEXTURE
        };
    }();

    return instance;
}

bool on_drop(Glib::ValueBase const &value, double x, double y, SPDesktopWidget *dtw, Glib::RefPtr<Gtk::DropTarget> const &drop_target)
{
    auto const desktop = dtw->get_desktop();
    auto const canvas = dtw->get_canvas();
    auto const doc = desktop->doc();
    auto const prefs = Inkscape::Preferences::get();

    auto const canvas_pos = Geom::Point{std::round(x), std::round(y)};
    auto const world_pos = canvas->canvas_to_world(canvas_pos);
    auto const dt_pos = desktop->w2d(world_pos);

    if (auto const ptr = GlibValue::get<Colors::Paint>(value)) {
        auto paint = *ptr;
        auto const item = desktop->getItemAtPoint(world_pos, true);
        if (!item) {
            return false;
        }

        auto find_gradient = [&] (Colors::Color const &color) -> SPGradient * {
            for (auto obj : doc->getResourceList("gradient")) {
                auto const grad = cast_unsafe<SPGradient>(obj);
                if (grad->hasStops() && grad->getId() == color.getName()) {
                    return grad;
                }
            }
            return nullptr;
        };

        std::string colorspec;
        if (std::holds_alternative<Colors::NoColor>(paint)) {
            colorspec = "none";
        } else {
            auto &color = std::get<Colors::Color>(paint);
            if (auto const grad = find_gradient(color)) {
                colorspec = std::string{"url(#"} + grad->getId() + ")";
            } else {
                colorspec = color.toString();
            }
        }

        if (desktop->getTool() && desktop->getTool()->get_drag()) {
            if (desktop->getTool()->get_drag()->dropColor(item, colorspec.c_str(), dt_pos)) {
                DocumentUndo::done(doc , RC_("Undo", "Drop color on gradient"), "");
                desktop->getTool()->get_drag()->updateDraggers();
                return true;
            }
        }

        //if (tools_active(desktop, TOOLS_TEXT)) {
        //    if (sp_text_context_drop_color(c, button_doc)) {
        //        SPDocumentUndo::done(doc , RC_("Undo", "Drop color on gradient stop"), "");
        //    }
        //}

        bool fillnotstroke = drop_target->get_current_drop()->get_actions() != Gdk::DragAction::MOVE;
        if (fillnotstroke && (is<SPShape>(item) || is<SPText>(item) || is<SPFlowtext>(item))) {
            if (auto const curve = curve_for_item(item)) {
                auto const pathv = *curve * (item->i2dt_affine() * desktop->d2w());

                double dist;
                pathv.nearestTime(world_pos, &dist);

                double const stroke_tolerance =
                    (!item->style->stroke.isNone() ?
                         desktop->current_zoom() *
                             item->style->stroke_width.computed *
                             item->i2dt_affine().descrim() * 0.5
                                                   : 0.0)
                    + prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

                if (dist < stroke_tolerance) {
                    fillnotstroke = false;
                }
            }
        }

        auto const css = sp_repr_css_attr_new();
        sp_repr_css_set_property_string(css, fillnotstroke ? "fill" : "stroke", colorspec);
        sp_desktop_apply_css_recursive(item, css, true);
        sp_repr_css_attr_unref(css);

        item->updateRepr();
        DocumentUndo::done(doc, RC_("Undo", "Drop color"), "");
        return true;
    } else if (auto const dndsvg = GlibValue::get<DnDSvg>(value)) {
        auto const data = get_span(dndsvg->bytes);
        if (data.empty()) {
            return false;
        }

        auto const newdoc = sp_repr_read_mem(data.data(), data.size_bytes(), SP_SVG_NS_URI);
        if (!newdoc) {
            sp_ui_error_dialog(_("Could not parse SVG data"));
            return false;
        }

        auto const root = newdoc->root();
        auto const style = root->attribute("style");

        auto const xml_doc = doc->getReprDoc();
        auto const newgroup = xml_doc->createElement("svg:g");
        newgroup->setAttribute("style", style);
        for (auto child = root->firstChild(); child; child = child->next()) {
            newgroup->appendChild(child->duplicate(xml_doc));
        }

        Inkscape::GC::release(newdoc);

        // Add it to the current layer

        // Greg's edits to add intelligent positioning of svg drops
        auto const new_obj = desktop->layerManager().currentLayer()->appendChildRepr(newgroup);

        auto const selection = desktop->getSelection();
        selection->set(cast<SPItem>(new_obj));

        // move to mouse pointer
        desktop->getDocument()->ensureUpToDate();
        if (auto const sel_bbox = selection->visualBounds()) {
            selection->moveRelative(dt_pos - sel_bbox->midpoint(), false);
        }

        Inkscape::GC::release(newgroup);
        DocumentUndo::done(doc, RC_("Undo", "Drop SVG"), "");
        return true;
    } else if (G_VALUE_HOLDS(value.gobj(), GDK_TYPE_FILE_LIST)) {
        auto const file_list = static_cast<GdkFileList *>(g_value_get_boxed(value.gobj()));
        auto const list = gdk_file_list_get_files(file_list);
        foreach<GFile>(list, [&] (GFile *f) {
            auto const path = g_file_get_path(f);
            auto const uri  = g_file_get_uri(f);
            if (path && std::strlen(path) > 2) {
                file_import(desktop->getDocument(), path, nullptr, dt_pos);
            }
            else if (uri) {
                // gtk4 on macOS provides URIs instead of local paths. Unescape and import.
                auto const unescaped = g_uri_unescape_string(uri, nullptr);
                file_import(desktop->getDocument(), unescaped, nullptr, dt_pos);
                g_free((void*)unescaped);
            }
            g_free(path);
            g_free(uri);
        });

        return true;
    } else if (GlibValue::holds<DnDSymbol>(value)) {
        auto cm = Inkscape::UI::ClipboardManager::get();
        cm->insertSymbol(desktop, dt_pos, false);
        DocumentUndo::done(doc, RC_("Undo", "Drop Symbol"), "");
        return true;
    } else if (G_VALUE_HOLDS(value.gobj(), GDK_TYPE_TEXTURE)) {
        auto const ext = Inkscape::Extension::Input::find_by_mime("image/png");
        bool const save = std::strcmp(ext->get_param_optiongroup("link"), "embed") == 0;
        ext->set_param_optiongroup("link", "embed");
        ext->set_gui(false);

        // Absolutely stupid, and the same for clipboard.cpp.
        auto const filename = Glib::build_filename(Glib::get_user_cache_dir(), "inkscape-dnd-import");
        auto const img = Glib::wrap(GDK_TEXTURE(g_value_get_object(value.gobj())), true);
        img->save_to_png(filename);
        file_import(doc, filename, ext);
        unlink(filename.c_str());

        ext->set_param_optiongroup("link", save ? "embed" : "link");
        ext->set_gui(true);
        DocumentUndo::done(doc, RC_("Undo", "Drop bitmap image"), "");
        return true;
    }

    return false;
}

} // namespace

void ink_drag_setup(SPDesktopWidget *dtw, Gtk::Widget *widget)
{
    auto drop_target = Gtk::DropTarget::create(G_TYPE_INVALID, Gdk::DragAction::COPY | Gdk::DragAction::MOVE);
    drop_target->set_gtypes(get_drop_types());
    drop_target->signal_drop().connect(sigc::bind(&on_drop, dtw, drop_target), false);
    widget->add_controller(drop_target);
}

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
