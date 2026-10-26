// SPDX-License-Identifier: GPL-2.0-or-later
#include "tools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glibmm/miscutils.h>

#include "colors/spaces/base.h"
#include "desktop-style.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "helper/pixbuf-ops.h"
#include "object/object-set.h"
#include "object/sp-clippath.h"
#include "object/sp-ellipse.h"
#include "object/sp-flowtext.h"
#include "object/sp-gradient.h"
#include "object/sp-hatch.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-mask.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-page.h"
#include "object/sp-paint-server-reference.h"
#include "object/sp-path.h"
#include "object/sp-pattern.h"
#include "object/sp-root.h"
#include "object/sp-spiral.h"
#include "object/sp-star.h"
#include "object/sp-symbol.h"
#include "object/sp-text.h"
#include "object/sp-textpath.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "protocol.h"
#include "spot-ink.h"
#include "style.h"
#include "text-editing.h"
#include "util/scope_exit.h"
#include "util/units.h"
#include "xml/node.h"

namespace Inkscape::MCP {
namespace {
using Object = Json::Object;
using Array = Json::Array;

char const *const names[] = {"document_info", "list_layers", "list_objects", "get_object", "get_selection", "render_preview"};

Json string_schema() { return Object{{"type", "string"}, {"minLength", 1}}; }
Json integer_schema(int minimum, int maximum)
{
    return Object{{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}
Json enum_schema(Array values) { return Object{{"type", "string"}, {"enum", std::move(values)}}; }
Json definition(std::string name, std::string title, std::string description, Object properties = {}, Array required = {})
{
    Object schema{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) schema.emplace("required", std::move(required));
    return Object{{"name", std::move(name)}, {"title", std::move(title)}, {"description", std::move(description)},
        {"inputSchema", std::move(schema)}, {"annotations", Object{{"readOnlyHint", true}, {"destructiveHint", false},
            {"idempotentHint", true}, {"openWorldHint", false}}}};
}

std::string string_arg(Json const &args, std::string const &key, std::string fallback = {})
{
    if (!args.contains(key)) return fallback;
    auto const &value = args.at(key);
    if (!value.is<std::string>() || value.get<std::string>().empty() || value.get<std::string>().find('\0') != std::string::npos) {
        throw std::runtime_error(key + " must be a non-empty string");
    }
    return value.get<std::string>();
}
double integer_arg(Json const &args, std::string const &key, double fallback, double minimum, double maximum)
{
    if (!args.contains(key)) return fallback;
    auto const &value = args.at(key);
    if (!value.is<double>()) throw std::runtime_error(key + " must be an integer");
    auto number = value.get<double>();
    if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum) {
        throw std::runtime_error(key + " is outside its allowed range");
    }
    return number;
}
void keys(Json const &args, std::initializer_list<std::string_view> allowed)
{
    if (!args.is<Object>()) throw std::runtime_error("Arguments must be an object");
    for (auto const &[key, value] : args.get<Object>()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) throw std::runtime_error("Unknown argument: " + key);
    }
}
Json id(SPObject const *object)
{
    return object && object->getId() ? Json(object->getId()) : Json();
}
bool layer(SPObject const *object)
{
    auto group = cast<SPGroup>(object);
    return group && group->isLayer();
}
bool container(SPObject const *object)
{
    return is<SPGroup>(object) && !is<SPSymbol>(object);
}
std::vector<SPItem *> children(SPObject &object)
{
    std::vector<SPItem *> items;
    for (auto &child : object.children) {
        if (auto item = cast<SPItem>(&child); item && !is<SPSymbol>(item)) items.push_back(item);
    }
    return items;
}
bool drawing_object(SPItem const *item, SPDocument &document)
{
    if (!item || item == document.getRoot() || is<SPSymbol>(item)) return false;
    for (auto parent = item->parent; parent; parent = parent->parent) {
        if (parent == document.getRoot()) return true;
        if (!container(parent)) return false;
    }
    return false;
}
SPItem *find_item(SPDocument &document, std::string const &name)
{
    auto object = document.getObjectById(name);
    if (!object) throw std::runtime_error("Unknown object id: " + name);
    auto item = cast<SPItem>(object);
    if (!drawing_object(item, document)) throw std::runtime_error("Not a drawing object: " + name);
    return item;
}
SPObject *enclosing_layer(SPItem const &item)
{
    for (auto parent = item.parent; parent; parent = parent->parent) if (layer(parent)) return parent;
    return nullptr;
}
double units(SPDocument &document, double pixels)
{
    return Util::Quantity::convert(pixels, "px", document.getDisplayUnit());
}
bool finite(Geom::Rect const &rect)
{
    return std::isfinite(rect.left()) && std::isfinite(rect.top()) && std::isfinite(rect.right()) &&
           std::isfinite(rect.bottom()) && std::isfinite(rect.width()) && std::isfinite(rect.height());
}
Json toolbar_bounds(LookContext const &context, Geom::OptRect const &bounds)
{
    if (!bounds || !finite(*bounds)) return nullptr;
    // SelectToolbar::layout_widget_update: desktop bounds, anchor, selected page, then units.
    auto x = bounds->left() + bounds->width() * context.anchor.x();
    auto y = bounds->top() + bounds->height() * context.anchor.y();
    if (context.document.get_origin_follows_page()) {
        auto page = context.document.getPageManager().getSelectedPageRect();
        x -= page.left();
        y -= page.top();
    }
    return Object{{"x", units(context.document, x)}, {"y", units(context.document, y)},
        {"width", units(context.document, bounds->width())}, {"height", units(context.document, bounds->height())}};
}
Json color(Colors::Color const &value)
{
    auto space = value.getSpace();
    auto type = space->getComponentType(); // an ICC profile's space reports its own model (CMYK, RGB...)
    auto count = std::min<std::size_t>(space->getComponentCount(), value.size());
    double factor = type == Colors::Space::Type::CMYK ? 100 : type == Colors::Space::Type::RGB ? 255 : 1;
    Array values;
    for (std::size_t i = 0; i < count; ++i) values.emplace_back(value[i] * factor);
    auto rgba = value.toRGBA();
    char hex[8];
    g_snprintf(hex, sizeof(hex), "#%06x", unsigned(rgba >> 8));
    return Object{{"type", "color"}, {"space", space->getName()}, {"values", std::move(values)}, {"hex", hex}};
}
// The paint the item's style sets; with \a computed, the one it draws with, inherited or initial.
Json paint(SPItem &item, bool stroke, bool computed = false)
{
    auto style = item.style;
    if (!style) return Object{{"type", "unset"}};
    auto const &value = *style->getFillOrStroke(!stroke);
    Object result;
    if (!value.set && !computed) result = {{"type", "unset"}};
    else if (value.isPaintserver()) {
        auto server = value.href->getObject();
        if (auto ink = SpotInk::read(server->getRepr())) {
            result = {{"type", "spot"}, {"ink", ink->name}, {"tint", ink->tint}, {"alternate", color(ink->alternate)}};
        } else {
            auto type = is<SPMeshGradient>(server) ? "mesh" : is<SPGradient>(server) ? "gradient" :
                        is<SPPattern>(server) ? "pattern" : is<SPHatch>(server) ? "hatch" : "unset";
            result = {{"type", type}, {"id", id(server)}};
        }
    } else if (value.isColor()) result = color(value.getColor()).get<Object>();
    else if (value.isNone()) result = {{"type", "none"}};
    else result = {{"type", "unset"}};
    if (stroke && result.at("type") != Json("none")) {
        result.emplace("width", units(*item.document, style->stroke_width.computed * item.i2doc_affine().descrim()));
    }
    return result;
}
// What paints a text's visible characters, as PROOF's paint panels show it: the <text> element can
// say none while its spans are painted.
Json character_paint(SPItem &text, bool stroke)
{
    Json shared;
    for (auto source : sp_desktop_paint_sources({&text})) {
        auto value = paint(*source, stroke, true);
        if (!shared.is<std::nullptr_t>() && shared != value) return Object{{"type", "mixed"}};
        shared = std::move(value);
    }
    return shared;
}
std::string kind(SPItem const &item)
{
    if (layer(&item)) return "layer";
    if (is<SPSymbol>(&item)) return "symbol";
    if (is<SPStar>(&item)) return "star";
    if (is<SPSpiral>(&item)) return "spiral";
    if (auto ellipse = cast<SPGenericEllipse>(&item)) return ellipse->type == SP_GENERIC_ELLIPSE_CIRCLE ? "circle" : "ellipse";
    auto tag = std::string(item.getRepr()->name());
    if (auto colon = tag.find(':'); colon != std::string::npos) tag.erase(0, colon + 1);
    if (tag == "flowRoot") return "flowtext";
    if (tag == "g") return "group";
    for (auto type : {"path", "rect", "ellipse", "circle", "line", "polyline", "polygon", "text", "image", "use", "switch"}) {
        if (tag == type) return tag;
    }
    return "other";
}
Object summary(LookContext const &context, SPItem &item)
{
    auto visual = item.desktopVisualBounds();
    auto geometric = item.desktopGeometricBounds();
    Object result{{"id", id(&item)}, {"name", item.label() ? Json(item.label()) : Json()},
        {"label", item.defaultLabel()}, {"type", kind(item)}, {"tag", item.getRepr()->name()},
        {"layer", id(enclosing_layer(item))}, {"visible", !item.isHidden()}, {"locked", !item.isSensitive()},
        {"bbox", toolbar_bounds(context, visual)}, {"fill", paint(item, false)}, {"stroke", paint(item, true)},
        {"opacity", item.style ? item.style->opacity.as_double() : 1.0}};
    if (is<SPText>(&item) || is<SPFlowtext>(&item)) {
        result.emplace("characters", Object{{"fill", character_paint(item, false)}, {"stroke", character_paint(item, true)}});
    }
    if (geometric != visual) result.emplace("geometric", toolbar_bounds(context, geometric));
    if (container(&item)) result.emplace("children", double(children(item).size()));
    return result;
}
Json result(LookContext const &context, Object data, std::string text)
{
    data.emplace("document", context.identity);
    return tool_result(std::move(data), std::move(text));
}
Json list_layers(LookContext const &context)
{
    Array layers;
    struct Entry { SPObject *object; SPObject *parent; int depth; };
    std::vector<Entry> stack{{context.document.getRoot(), nullptr, 0}};
    while (!stack.empty()) {
        auto [object, parent, depth] = stack.back();
        stack.pop_back();
        if (layer(object)) {
            auto item = cast<SPItem>(object);
            int count = 0;
            for (auto child : children(*object)) if (!layer(child)) ++count;
            layers.emplace_back(Object{{"id", id(item)}, {"name", item->label() ? Json(item->label()) : Json()},
                {"label", item->defaultLabel()}, {"visible", !item->isHidden()}, {"locked", !item->isSensitive()},
                {"depth", depth}, {"parent", id(parent)}, {"items", count}});
            parent = object;
            ++depth;
        }
        if (!container(object)) continue;
        auto items = children(*object);
        for (auto it = items.rbegin(); it != items.rend(); ++it) stack.push_back({*it, parent, depth});
    }
    auto current = layer(context.current_layer) ? id(context.current_layer) : Json();
    auto count = layers.size();
    return result(context, {{"layers", std::move(layers)}, {"current", current}}, std::to_string(count) + " layers in document order.");
}
Json list_objects(LookContext const &context, Json const &args)
{
    auto &document = context.document;
    SPObject *parent = document.getRoot();
    if (args.contains("parent")) {
        parent = find_item(document, string_arg(args, "parent"));
        if (!container(parent)) throw std::runtime_error("Parent must be a layer or group");
    }
    int depth = integer_arg(args, "depth", 1, 1, 5);
    auto offset = integer_arg(args, "offset", 0, 0, 9007199254740991.0);
    int limit = integer_arg(args, "limit", 100, 1, 500);
    Array objects;
    double total = 0;
    struct Entry { SPItem *item; int depth; };
    std::vector<Entry> stack;
    auto push = [&](SPObject &object, int level) {
        auto items = children(object);
        for (auto it = items.rbegin(); it != items.rend(); ++it) stack.push_back({*it, level});
    };
    push(*parent, 1);
    while (!stack.empty()) {
        auto [item, level] = stack.back();
        stack.pop_back();
        if (total >= offset && objects.size() < limit) {
            auto data = summary(context, *item);
            data.emplace("depth", level);
            objects.emplace_back(std::move(data));
        }
        ++total;
        if (level < depth && container(item)) push(*item, level + 1);
    }
    auto next = offset + objects.size();
    return result(context, {{"parent", parent == document.getRoot() ? Json() : id(parent)}, {"objects", std::move(objects)},
        {"total", total}, {"offset", offset}, {"next_offset", next < total ? Json(next) : Json()}},
        std::to_string(int(total)) + " objects at the requested depth, in bottom-to-top z-order.");
}
Json get_object(LookContext const &context, Json const &args)
{
    auto item = find_item(context.document, string_arg(args, "id"));
    auto data = summary(context, *item);
    if (!item->transform.isIdentity()) {
        Array transform;
        for (unsigned i = 0; i < 6; ++i) transform.emplace_back(item->transform[i]);
        data.emplace("transform", std::move(transform));
    }
    if (item->style && item->style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) {
        data.emplace("blend", item->style->mix_blend_mode.get_value().raw());
    }
    if (auto clip = item->getClipObject()) data.emplace("clip", id(clip));
    if (auto mask = item->getMaskObject()) data.emplace("mask", id(mask));
    if (auto use = cast<SPUse>(item)) data.emplace("href", use->href ? Json(use->href) : Json());
    if (is<SPText>(item) || is<SPFlowtext>(item)) {
        auto text = sp_te_get_string_multiline(item);
        if (text.size() > 2000) { text = text.substr(0, 2000); data.emplace("truncated", true); }
        data.emplace("text", text.raw());
        auto style = item->style;
        data.emplace("font_family", style && style->font_family.value() ? Json(style->font_family.value()) : Json());
        data.emplace("font_size", style ? Util::Quantity::convert(style->font_size.computed * item->i2doc_affine().descrim(), "px", "pt") : 0.0);
        auto svg_text = cast<SPText>(item);
        auto type = is<SPFlowtext>(item) || (svg_text && (svg_text->has_shape_inside() || svg_text->has_inline_size())) ? "area" : "point";
        for (auto &child : item->children) if (is<SPTextPath>(&child)) type = "path";
        data.emplace("kind", type);
    }
    if (auto path = cast<SPPath>(item)) data.emplace("nodes", path->nodesInPath());
    if (auto image = cast<SPImage>(item)) {
        data.emplace("pixels", Object{{"width", image->pixbuf && !image->missing ? image->pixbuf->width() : 0},
                                      {"height", image->pixbuf && !image->missing ? image->pixbuf->height() : 0}});
        std::string link = image->linkedFile();
        if (link.empty()) link = "embedded";
        else {
            auto end = link.find_first_of("?#");
            if (end != std::string::npos) link.erase(end);
            auto start = link.find_last_of("/\\");
            if (start != std::string::npos) link.erase(0, start + 1);
        }
        data.emplace("link", std::move(link));
    }
    if (container(item)) {
        Array ids;
        for (auto child : children(*item)) { if (ids.size() == 200) break; ids.emplace_back(id(child)); }
        data["children"] = std::move(ids);
    }
    return result(context, std::move(data), std::string(item->defaultLabel()) + ": " + kind(*item) + ". Font sizes are in pt.");
}
Json get_selection(LookContext const &context)
{
    Array objects;
    Geom::OptRect bounds;
    int count = 0;
    if (context.selection) for (auto item : context.selection->items()) {
        if (!drawing_object(item, context.document)) continue;
        ++count;
        bounds.unionWith(item->desktopVisualBounds());
        if (objects.size() < 200) objects.emplace_back(summary(context, *item));
    }
    return result(context, {{"count", count}, {"objects", std::move(objects)}, {"bbox", toolbar_bounds(context, bounds)}},
        std::to_string(count) + " objects selected.");
}
Json render_preview(LookContext const &context, Json const &args)
{
    auto &document = context.document;
    auto area_name = string_arg(args, "area", "page");
    auto background = string_arg(args, "background", "page");
    Geom::OptRect area;
    std::vector<SPItem const *> items;
    if (area_name == "page") {
        auto const &pages = document.getPageManager().getPages();
        int page = integer_arg(args, "page", std::max(0, document.getPageManager().getSelectedPageIndex()), 0, std::numeric_limits<int>::max());
        if (page >= int(std::max<std::size_t>(1, pages.size()))) throw std::runtime_error("Unknown page index");
        area = pages.empty() ? Geom::Rect(Geom::Point(0, 0), Geom::Point(document.getWidth().value("px"), document.getHeight().value("px")))
                             : pages[page]->getDocumentRect();
    } else if (area_name == "drawing") area = document.getRoot()->documentVisualBounds();
    else if (area_name == "object") {
        auto item = find_item(document, string_arg(args, "id"));
        items.push_back(item);
        area = item->documentVisualBounds();
    } else if (context.selection) for (auto item : context.selection->items()) {
        if (!drawing_object(item, document)) continue;
        items.push_back(item);
        area.unionWith(item->documentVisualBounds());
    }
    if (!area || !finite(*area) || area->width() <= 0 || area->height() <= 0) throw std::runtime_error("Preview area is empty or degenerate");
    auto size = std::min(2048.0, integer_arg(args, "size", 1024, 64, 9007199254740991.0));
    auto scale = size / std::max(area->width(), area->height());
    if (!std::isfinite(scale) || scale <= 0) throw std::runtime_error("Preview resolution is invalid");
    // Rounding in sp_generate_internal_bitmap uses ceil, so bound both the dimensions and their product.
    auto width = std::max(1.0, std::ceil(area->width() * scale));
    auto height = std::max(1.0, std::ceil(area->height() * scale));
    if (width * height > 4000000) scale *= std::sqrt(4000000 / (width * height));
    while (std::ceil(area->width() * scale) * std::ceil(area->height() * scale) > 4000000 ||
           std::ceil(area->width() * scale) > 2048 || std::ceil(area->height() * scale) > 2048) scale *= 0.9999;
    auto dpi = scale * 96;
    if (!std::isfinite(dpi) || dpi <= 0) throw std::runtime_error("Preview resolution is invalid");
    std::unique_ptr<Pixbuf> bitmap(sp_generate_internal_bitmap(&document, *area, dpi, items));
    if (!bitmap) throw std::runtime_error("Preview could not be rendered");
    if (background != "transparent") {
        auto surface = bitmap->getSurfaceRaw();
        auto cr = cairo_create(surface);
        cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OVER);
        auto rgba = background == "white" ? 0xffffffffu : document.getPageManager().getBackgroundColor().withOpacity(1).toRGBA();
        cairo_set_source_rgba(cr, ((rgba >> 24) & 255) / 255.0, ((rgba >> 16) & 255) / 255.0,
                              ((rgba >> 8) & 255) / 255.0, (rgba & 255) / 255.0);
        cairo_paint(cr);
        cairo_destroy(cr);
    }
    auto pixbuf = bitmap->getPixbufRaw();
    gchar *png = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    auto release = scope_exit([&] { g_free(png); if (error) g_error_free(error); });
    if (!gdk_pixbuf_save_to_buffer(pixbuf, &png, &length, "png", &error, nullptr)) throw std::runtime_error("Preview PNG encoding failed");
    auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(png), length);
    auto release_encoded = scope_exit([&] { g_free(encoded); });
    Object data{{"document", context.identity}, {"area", toolbar_bounds(context, *area * document.doc2dt())},
        {"pixels", Object{{"width", bitmap->width()}, {"height", bitmap->height()}}}, {"dpi", dpi}};
    return Object{{"content", Array{Object{{"type", "image"}, {"data", encoded}, {"mimeType", "image/png"}},
        Object{{"type", "text"}, {"text", "RGB appearance preview of the " + area_name + " (not separations)."}}}},
        {"structuredContent", std::move(data)}, {"isError", false}};
}
} // namespace

Json::Array look_tools()
{
    return {
        definition("document_info", "Document information", "Read the active document's name, pages, object counts and selection count."),
        definition("list_layers", "List layers", "Read layers and sublayers in depth-first document order; depth starts at zero."),
        definition("list_objects", "List objects", "Read drawing objects depth-first in bottom-to-top z-order (the Layers panel shows the reverse). Bounds use selector toolbar coordinates in the display unit.",
            {{"parent", string_schema()}, {"depth", integer_schema(1, 5)}, {"offset", Object{{"type", "integer"}, {"minimum", 0}}}, {"limit", integer_schema(1, 500)}}),
        definition("get_object", "Inspect object", "Read a drawing object's geometry, paint and details. Bounds use selector toolbar coordinates in the display unit; font size is in pt. For a text, characters gives the paint its visible characters are drawn with (mixed when they differ); fill and stroke are the text element's own.",
            {{"id", string_schema()}}, {"id"}),
        definition("get_selection", "Read selection", "Read up to 200 selected objects and their combined visual bounds in selector toolbar coordinates and the display unit."),
        definition("render_preview", "Render preview", "Render an RGB appearance preview, not separations. Page indices start at zero; size is the longest pixel edge, clamped to 2048 and four megapixels. Area uses selector toolbar coordinates in the display unit.",
            {{"area", enum_schema({"page", "drawing", "selection", "object"})}, {"page", Object{{"type", "integer"}, {"minimum", 0}}},
             {"id", string_schema()}, {"size", Object{{"type", "integer"}, {"minimum", 64}}},
             {"background", enum_schema({"page", "white", "transparent"})}})
    };
}
bool known_tool(std::string const &name)
{
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
void validate_arguments(std::string const &name, Json const &args)
{
    if (name == "document_info" || name == "list_layers" || name == "get_selection") keys(args, {});
    else if (name == "list_objects") {
        keys(args, {"parent", "depth", "offset", "limit"});
        if (args.contains("parent")) string_arg(args, "parent");
        integer_arg(args, "depth", 1, 1, 5);
        integer_arg(args, "offset", 0, 0, 9007199254740991.0);
        integer_arg(args, "limit", 100, 1, 500);
    } else if (name == "get_object") {
        keys(args, {"id"});
        if (!args.contains("id")) throw std::runtime_error("id is required");
        string_arg(args, "id");
    } else if (name == "render_preview") {
        keys(args, {"area", "page", "id", "size", "background"});
        auto area = string_arg(args, "area", "page");
        auto background = string_arg(args, "background", "page");
        if (area != "page" && area != "drawing" && area != "selection" && area != "object") throw std::runtime_error("Unknown preview area");
        if (background != "page" && background != "white" && background != "transparent") throw std::runtime_error("Unknown preview background");
        integer_arg(args, "page", 0, 0, std::numeric_limits<int>::max());
        integer_arg(args, "size", 1024, 64, 9007199254740991.0);
        if (area == "object" && !args.contains("id")) throw std::runtime_error("id is required for an object preview");
        if (args.contains("id")) string_arg(args, "id");
        if (args.contains("id") && area != "object") throw std::runtime_error("id is only valid for an object preview");
        if (args.contains("page") && area != "page") throw std::runtime_error("page is only valid for a page preview");
    } else throw std::runtime_error("Unknown tool");
}
Json call_look_tool(LookContext const &context, std::string const &name, Json const &args)
{
    try {
        validate_arguments(name, args);
        context.document.ensureUpToDate();
        if (name == "list_layers") return list_layers(context);
        if (name == "list_objects") return list_objects(context, args);
        if (name == "get_object") return get_object(context, args);
        if (name == "get_selection") return get_selection(context);
        if (name == "render_preview") return render_preview(context, args);
        throw std::runtime_error("Unknown tool");
    } catch (std::exception const &e) { return tool_result(Object{{"document", context.identity}}, e.what(), true); }
    catch (...) { return tool_result(Object{{"document", context.identity}}, "The tool failed", true); }
}
} // namespace Inkscape::MCP
