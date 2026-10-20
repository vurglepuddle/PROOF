// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Illustrator's own records for a PROOF document. See ai-native-writer.h.
 *
 * The record forms are the ones Illustrator 29.2.1 opened as editable art in PROOF's spikes
 * (tools/native-ai-spike.py, 2026-10-02; the writer spike, 2026-10-17), in the coordinates
 * of the writer recipe (doc/PROOF-ai-interchange.md): the first artboard's top-left corner
 * is 0,0, y negative downward, in points.
 * The Illustrator 7 variant uses Adobe's 1998 file-format specification, sections 5 and 9,
 * and the standalone writer spike: bottom-left origin, ten-operand layers and x/X spot tints.
 */

#include "ai-native-writer.h"
#include "ai-native-writer-gradient.h"
#include "ai-native-writer-picture.h"
#include "ai-native-writer-string.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

#include <glib.h>
#include <2geom/bezier-curve.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>

#include "colors/color.h"
#include "colors/document-colors.h"
#include "colors/spaces/base.h"
#include "document.h"
#include "helper/geom.h"
#include "object/sp-clippath.h"
#include "object/sp-gradient.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-page.h"
#include "object/sp-pattern.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "spot-ink.h"
#include "style.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;

constexpr double PX_TO_PT = 0.75;

/// The node tool's best fit when no authored type is given: opposite handles, or a
/// handle tangent to the neighbouring line. Two straight sides stay a corner.
bool smooth_join(Geom::Path const &path, std::size_t node)
{
    auto const count = path.size();
    if (!count || (!path.closed() && (node == 0 || node == count))) return false;
    node %= count;
    auto prev = (node + count - 1) % count, next = node;
    // Ellipse construction can leave a microscopic closing line. It rounds to
    // zero in the records; infer both coincident anchors from the actual sides.
    auto tiny_line = [&](std::size_t at) {
        auto const &curve = path[at];
        return curve.isLineSegment() && Geom::distance(curve.initialPoint(), curve.finalPoint()) < 1e-9;
    };
    if (path.closed()) {
        for (std::size_t i = 0; i < count && tiny_line(prev); ++i) prev = (prev + count - 1) % count;
        for (std::size_t i = 0; i < count && tiny_line(next); ++i) next = (next + 1) % count;
    }
    auto const &before = path[prev];
    auto const &after = path[next];
    auto const anchor = after.initialPoint();
    auto const incoming = dynamic_cast<Geom::CubicBezier const *>(&before);
    auto const outgoing = dynamic_cast<Geom::CubicBezier const *>(&after);
    auto back = incoming ? (*incoming)[2] - anchor : Geom::Point();
    auto front = outgoing ? (*outgoing)[1] - anchor : Geom::Point();
    bool const back_handle = back.length() > 1e-9, front_handle = front.length() > 1e-9;
    if (!back_handle && !front_handle) return false;
    if (!back_handle) {
        if (!before.isLineSegment()) return false;
        back = before.initialPoint() - anchor;
    }
    if (!front_handle) {
        if (!after.isLineSegment()) return false;
        front = after.finalPoint() - anchor;
    }
    double const scale = back.length() * front.length();
    return scale > 0 && Geom::dot(back, front) < 0 && std::abs(Geom::cross(back, front)) <= 1e-6 * scale;
}

/// A number as Illustrator writes it: up to four decimals, no trailing zeros.
std::string num(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

/// A PostScript string: parentheses and backslashes escaped, other bytes outside ASCII in octal.
std::string ps_string(std::string const &s)
{
    std::string out = "(";
    for (unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            out += std::format("\\{:03o}", c);
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + ")";
}

double scale24(unsigned value)
{
    return value / double(SP_SCALE24_MAX);
}

class Writer
{
public:
    Writer(SPDocument *doc, RecordsVersion version)
        : _doc(doc), _legacy(version == RecordsVersion::Illustrator7)
    {}

    WrittenRecords write();

private:
    void layer(SPGroup const *group);
    void item(SPItem const *item);
    void contents(SPObject const *parent);
    void shape(SPShape const *shape);
    void check_effects(SPItem const *item);
    bool open_clip(SPItem const *item);
    std::vector<std::string> path_types(SPShape const *shape, Geom::PathVector const &paths, Geom::Affine const &affine);
    void path_data(Geom::Path const &path, std::string_view types);
    void state(std::string &cache, std::string const &line);
    std::optional<std::string> paint(SPIPaint const &p, SPStyle const *style, bool fill);
    std::optional<std::string> colour(Colors::Color const &c, bool fill);
    bool declare_ink(std::string const &name, std::array<double, 4> const &cmyk);
    std::string encoded_name(std::string const &text, std::string_view what);
    void ink_comments(std::ostream &head);
    void refuse(std::string what) { _refused.insert(std::move(what)); }

    SPDocument *_doc;
    bool const _legacy;
    Geom::Affine _to_art;
    /// The art origin on the y-down canvas where Illustrator applies a gradient's Bg matrix.
    Geom::Point _canvas;
    std::ostringstream _body;
    std::set<std::string> _refused;
    std::map<std::string, std::array<double, 4>> _spots;
    std::vector<std::string> _gradients;
    std::size_t _layers = 0;
    // What has been set already, so each operator is written only when it changes.
    std::string _fill_over, _fill, _stroke_over, _stroke, _stroke_style, _rule;
};

std::string Writer::encoded_name(std::string const &text, std::string_view what)
{
    if (!_legacy) return ps_string(text);
    auto const bytes = legacy_name_bytes(text);
    if (!bytes) {
        refuse(std::string(what) + " outside Windows-1252 (Illustrator 7)");
        return {};
    }
    return ps_string(*bytes);
}

void Writer::ink_comments(std::ostream &head)
{
    bool first = true;
    for (auto const &[name, c] : _spots) {
        head << (first ? "%%DocumentCustomColors: " : "%%+ ") << encoded_name(name, "ink names") << '\n';
        first = false;
    }
    first = true;
    for (auto const &[name, c] : _spots) {
        head << (first ? "%%CMYKCustomColor: " : "%%+ ")
             << std::format("{} {} {} {} {}\n", num(c[0]), num(c[1]), num(c[2]), num(c[3]), encoded_name(name, "ink names"));
        first = false;
    }
}

std::optional<std::string> Writer::colour(Colors::Color const &c, bool fill)
{
    if (c.getOpacity() < 1.0 - 1e-6) {
        refuse("transparency");
        return {};
    }
    if (c.getSpace() && c.getSpace()->getComponentType() == Type::CMYK) {
        return std::format("{} {} {} {} {}", num(c[0]), num(c[1]), num(c[2]), num(c[3]), fill ? "k" : "K");
    }
    if (auto rgb = c.converted(Type::RGB)) {
        return std::format("{} {} {} {}", num((*rgb)[0]), num((*rgb)[1]), num((*rgb)[2]), fill ? "Xa" : "XA");
    }
    refuse("colours that are neither CMYK nor RGB");
    return {};
}

/// The operator that sets this paint, or nothing for none (and for what is refused).
std::optional<std::string> Writer::paint(SPIPaint const &p, SPStyle const *style, bool fill)
{
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL || p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
        refuse("context paint");
        return {};
    }
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CURRENT_COLOR) return colour(style->color.getColor(), fill);
    if (p.isNone()) return {};
    if (p.isColor()) return colour(p.getColor(), fill);
    auto server = p.isPaintserver() && p.href ? p.href->getObject() : nullptr;
    auto swatch = cast<SPGradient>(server);
    if (swatch && swatch->isSolid()) {
        auto ink = SpotInk::read(swatch->getRepr());
        if (!ink && swatch->getVector()) ink = SpotInk::read(swatch->getVector()->getRepr());
        if (!ink) return colour(const_cast<SPGradient *>(swatch)->getPreviewAverageColor(), fill);
        auto const &a = ink->alternate;
        if (!a.getSpace() || a.getSpace()->getComponentType() != Type::CMYK) {
            refuse("spot inks defined in RGB or Lab");
            return {};
        }
        std::array<double, 4> cmyk{a[0], a[1], a[2], a[3]};
        if (!declare_ink(ink->name, cmyk)) return {};
        auto const head = std::format("{} {} {} {} {}", num(cmyk[0]), num(cmyk[1]), num(cmyk[2]), num(cmyk[3]),
                                      encoded_name(ink->name, "ink names"));
        // The operand is the tint taken from 1: 0 is the ink at full strength.
        // Illustrator 7 uses x/X for CMYK spots at every tint. Its Xx/XX form is RGB only.
        if (_legacy) return head + " " + num(1.0 - ink->tint) + " " + (fill ? "x" : "X");
        if (ink->tint >= 1.0 - 1e-6) return head + " 0 " + (fill ? "x" : "X");
        return head + " " + num(1.0 - ink->tint) + " 0 " + (fill ? "Xx" : "XX");
    }
    refuse(is<SPMeshGradient>(server) ? "mesh gradients" : is<SPPattern>(server) ? "patterns" : "gradients");
    return {};
}

/// Adds a spot ink to those the file declares; false, refused, when the name has another definition.
bool Writer::declare_ink(std::string const &name, std::array<double, 4> const &cmyk)
{
    if (_legacy && !legacy_name_bytes(name)) {
        refuse("ink names outside Windows-1252 (Illustrator 7)");
        return false;
    }
    auto [known, added] = _spots.emplace(name, cmyk);
    if (!added && known->second != cmyk) {
        refuse("one ink name with two definitions");
        return false;
    }
    return true;
}

void Writer::state(std::string &cache, std::string const &line)
{
    if (cache == line) return;
    cache = line;
    _body << line << '\n';
}

std::vector<std::string> Writer::path_types(SPShape const *shape, Geom::PathVector const &paths, Geom::Affine const &affine)
{
    std::vector<std::string> types(paths.size());
    auto const attr = shape->getAttribute("sodipodi:nodetypes");
    if (!attr) return types;
    // The node tool indexes types after converting the local geometry to lines and
    // cubics. Keep the writer's transformed geometry and its approximation order:
    // when an arc gains subdivisions, carry the original segment's endpoint type
    // to its new final piece, and infer the newly inserted joins separately.
    auto const local = pathv_to_linear_and_cubic_beziers(*shape->curve());
    std::string_view remaining(attr);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        // The node tool ignores naked movetos. Closed paths duplicate the seam
        // type, and use the final duplicate when it is present.
        auto const count = local[i].empty() ? 0 : local[i].size() + 1;
        auto const take = std::min(count, remaining.size());
        std::string const authored(remaining.substr(0, take));
        remaining.remove_prefix(take);
        if (local[i].size() == paths[i].size()) {
            types[i] = authored;
            continue;
        }
        types[i].assign(paths[i].empty() ? 0 : paths[i].size() + 1, 'b');
        if (!authored.empty() && !types[i].empty()) types[i][0] = authored[0];
        std::size_t from = 1, to = 1;
        for (auto const &curve : (*shape->curve())[i]) {
            Geom::Path piece(curve.initialPoint());
            piece.append(curve);
            Geom::PathVector const one{piece};
            auto const old_count = pathv_to_linear_and_cubic_beziers(one)[0].size();
            auto const new_count = pathv_to_linear_and_cubic_beziers(one * affine)[0].size();
            if (old_count == new_count) {
                for (std::size_t j = 0; j < new_count && from + j < authored.size() && to + j < types[i].size(); ++j) {
                    types[i][to + j] = authored[from + j];
                }
            } else if (old_count && new_count && from + old_count <= authored.size() && to + new_count <= types[i].size()) {
                types[i][to + new_count - 1] = authored[from + old_count - 1];
            }
            from += old_count;
            to += new_count;
        }
        if (paths[i].closed() && !types[i].empty() && authored.size() < count) types[i].back() = types[i].front();
    }
    return types;
}

void Writer::path_data(Geom::Path const &path, std::string_view types)
{
    auto const start = path.initialPoint();
    _body << num(start.x()) << ' ' << num(start.y()) << " m\n";
    auto segment = [&](Geom::Curve const &c, std::size_t node) {
        char type = node < types.size() ? types[node] : '\0';
        if (path.closed() && node == path.size() && node >= types.size() && !types.empty()) type = types.front();
        bool const smooth = type == 's' || type == 'z' || type == 'a' || (type != 'c' && smooth_join(path, node));
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&c)) {
            _body << std::format("{} {} {} {} {} {} {}\n", num((*cubic)[1].x()), num((*cubic)[1].y()),
                                 num((*cubic)[2].x()), num((*cubic)[2].y()), num((*cubic)[3].x()),
                                 num((*cubic)[3].y()), smooth ? 'c' : 'C');
        } else {
            _body << num(c.finalPoint().x()) << ' ' << num(c.finalPoint().y()) << (smooth ? " l\n" : " L\n");
        }
    };
    std::size_t node = 0;
    // Includes a non-degenerate closing line. Its destination type, or the final
    // cubic's when it ends at the start, carries the closed seam's semantics.
    for (auto const &c : path) segment(c, ++node);
}

/// Opens a clipping group for the item's clipping path: `q`, the path, `W n`. Whether it did.
bool Writer::open_clip(SPItem const *item)
{
    auto clip = item->getClipObject();
    if (!clip) return false;
    std::vector<SPShape const *> shapes;
    for (auto const &child : clip->children) {
        if (auto s = cast<SPShape>(&child)) {
            shapes.push_back(s);
        } else if (is<SPItem>(&child)) {
            shapes.clear();
            break;
        }
    }
    if (clip->clippath_units() || shapes.size() != 1 || !shapes[0]->curve()) {
        refuse("clipping paths other than one path");
        return false;
    }
    auto const affine = shapes[0]->transform * item->i2doc_affine() * _to_art;
    auto const pv = pathv_to_linear_and_cubic_beziers(*shapes[0]->curve() * affine);
    if (pv.size() != 1) {
        refuse("clipping paths other than one path");
        return false;
    }
    auto const types = path_types(shapes[0], pv, affine);
    _body << "q\n";
    path_data(pv[0], types[0]);
    _body << "h\nW\nn\n";
    return true;
}

void Writer::shape(SPShape const *s)
{
    auto const *style = s->style;
    if (!s->curve() || s->curve()->empty()) return;
    if (s->hasMarkers()) refuse("markers");
    auto const affine = s->i2doc_affine() * _to_art;
    auto const is_gradient = [](SPIPaint const &p) {
        auto const g = cast<SPGradient>(p.isPaintserver() && p.href ? p.href->getObject() : nullptr);
        return g && !g->isSolid();
    };
    bool const fill_gradient = is_gradient(style->fill);
    bool const stroke_gradient = style->stroke_width.computed > 0 && is_gradient(style->stroke);
    // The fill and stroke have independent instances. Illustrator's filled-stroke control
    // closes the fill instance before opening the stroke's, with one final b/B operator on
    // the same path (doc/PROOF-ai-interchange.md, "Filled and dual-gradient strokes").
    std::optional<WrittenGradient> fill_instance, stroke_instance;
    std::optional<std::string> fill, stroke;
    auto prepare_gradient = [&](bool on_stroke) {
        auto gradient = write_gradient(s, affine, "PROOF_G" + std::to_string(_gradients.size() + 1), _legacy, _canvas,
                                       on_stroke, on_stroke && fill_gradient);
        for (auto const &why : gradient.refused) refuse(why);
        for (auto const &[name, cmyk] : gradient.inks) declare_ink(name, cmyk);
        if (!gradient.definition.empty()) _gradients.push_back(gradient.definition);
        return gradient;
    };
    if (fill_gradient) fill_instance = prepare_gradient(false);
    if (stroke_gradient) stroke_instance = prepare_gradient(true);
    // A compound path's early contours need a plain paint before its final gradient.
    fill = fill_gradient ? std::optional<std::string>("0 g") : paint(style->fill, style, true);
    if (stroke_gradient) {
        stroke = "0 G";
    } else if (style->stroke_width.computed > 0) {
        stroke = paint(style->stroke, style, false);
    }
    if (scale24(style->fill_opacity.value) < 1.0 - 1e-6 && fill) refuse("transparency");
    if (scale24(style->stroke_opacity.value) < 1.0 - 1e-6 && stroke) refuse("transparency");

    if (stroke) {
        if (style->vector_effect.stroke) refuse("non-scaling strokes");
        if (!affine.withoutTranslation().preservesAngles(1e-6)) refuse("strokes on stretched or skewed objects");
        for (auto const layer : style->paint_order.layer) {
            if (layer == SP_CSS_PAINT_ORDER_FILL) break;
            if (layer == SP_CSS_PAINT_ORDER_STROKE && fill) {
                refuse("strokes painted under their fill");
                break;
            }
        }
    }
    if (!_refused.empty()) return;

    auto const overprint = s->getAttribute("proof:overprint") ? std::string(s->getAttribute("proof:overprint")) : "";
    state(_fill_over, overprint.find("fill") != std::string::npos ? "1 O" : "0 O");
    if (fill) state(_fill, *fill);
    state(_stroke_over, overprint.find("stroke") != std::string::npos ? "1 R" : "0 R");
    if (stroke) {
        state(_stroke, *stroke);
        double const scale = std::sqrt(std::abs(affine.det()));
        std::string dash = "[";
        for (auto const &d : style->stroke_dasharray.values) {
            dash += (dash.size() > 1 ? " " : "") + num(d.computed * scale);
        }
        dash += "]" + num(style->stroke_dashoffset.computed * scale) + " d";
        int const cap = style->stroke_linecap.computed == SP_STROKE_LINECAP_ROUND    ? 1
                        : style->stroke_linecap.computed == SP_STROKE_LINECAP_SQUARE ? 2 : 0;
        int const join = style->stroke_linejoin.computed == SP_STROKE_LINEJOIN_ROUND   ? 1
                         : style->stroke_linejoin.computed == SP_STROKE_LINEJOIN_BEVEL ? 2 : 0;
        state(_stroke_style, std::format("{} w {} J {} j {} M {}", num(style->stroke_width.computed * scale), cap,
                                         join, num(style->stroke_miterlimit.value), dash));
    }
    state(_rule, style->fill_rule.computed == SP_WIND_RULE_EVENODD ? "1 XR" : "0 XR");

    auto const pv = pathv_to_linear_and_cubic_beziers(*s->curve() * affine);
    auto const types = path_types(s, pv, affine);
    auto op = [&](Geom::Path const &p) {
        char c = fill && stroke ? 'b' : fill ? 'f' : stroke ? 's' : 'n';
        return p.closed() ? c : static_cast<char>(g_ascii_toupper(c));
    };
    // The instance brackets the final contour's paint operator; its end says whether that
    // operator strokes the path, and closes it first.
    auto contour = [&](std::size_t i) {
        path_data(pv[i], types[i]);
        bool const last = i + 1 == pv.size();
        auto const c = op(pv[i]);
        if (fill_instance && last) {
            _body << fill_instance->begin;
            if (stroke_instance) _body << gradient_end(c);
        }
        if (stroke_instance && last) _body << stroke_instance->begin;
        _body << c << '\n';
        if ((fill_instance || stroke_instance) && last) _body << gradient_end(c);
    };
    if (pv.size() == 1) {
        contour(0);
        return;
    }
    _body << "*u\n";
    for (std::size_t i = 0; i < pv.size(); ++i) contour(i);
    _body << "*U\n";
}

void Writer::check_effects(SPItem const *it)
{
    auto const *style = it->style;
    if (scale24(style->opacity.value) < 1.0 - 1e-6) refuse("transparency");
    if (style->mix_blend_mode.set && style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) refuse("blend modes");
    if (it->getMaskObject()) refuse("opacity masks");
    if (const_cast<SPStyle *>(style)->getFilter()) refuse("filters");
}

void Writer::item(SPItem const *it)
{
    check_effects(it);
    // Only the object's own: a locked layer's objects aren't each locked.
    bool const hidden = it->isHidden();
    bool const locked = it->getAttribute("sodipodi:insensitive") != nullptr;
    if (hidden) {
        if (_legacy) refuse("individually hidden objects (Illustrator 7)");
        else _body << "1 Xw\n";
    }
    if (locked) _body << "1 A\n";
    bool const clipped = open_clip(it);

    if (auto use = cast<SPUse>(it)) {
        // A clone is written as a copy of what it shows.
        if (use->child) item(use->child);
    } else if (auto group = cast<SPGroup>(it)) {
        if (!clipped) _body << "u\n";
        contents(group);
        if (!clipped) _body << "U\n";
    } else if (auto s = cast<SPShape>(it)) {
        shape(s);
    } else if (auto image = cast<SPImage>(it)) {
        auto const written = write_picture(image, image->i2doc_affine() * _to_art, _legacy);
        for (auto const &why : written.refused) refuse(why);
        _body << written.records;
    } else {
        refuse("type");
    }

    if (clipped) _body << "Q\n";
    if (locked) _body << "0 A\n";
    if (hidden && !_legacy) _body << "0 Xw\n";
}

void Writer::contents(SPObject const *parent)
{
    for (auto const &child : parent->children) {
        auto it = cast<SPItem>(&child);
        if (!it) continue;
        auto group = cast<SPGroup>(it);
        if (group && group->isLayer()) {
            layer(group);
        } else {
            item(it);
        }
    }
}

void Writer::layer(SPGroup const *group)
{
    ++_layers;
    auto attr = [&](char const *name) {
        auto v = group->getAttribute(name);
        return v ? std::string(v) : std::string();
    };
    int r = 79, g = 128, b = 255;
    auto const color = attr("inkscape:highlight-color");
    if (color.size() >= 7 && color[0] == '#') {
        auto hex = [&](std::size_t at) { return static_cast<int>(std::strtol(color.substr(at, 2).c_str(), nullptr, 16)); };
        r = hex(1), g = hex(3), b = hex(5);
    }
    // Illustrator shows a layer in its preset colour by index, whatever RGB follows it. The
    // presets as the owner's files pair them (census, 2026-10-17; 4 to 6 vary between files and
    // are left out): the nearest one.
    static constexpr std::array<std::array<int, 4>, 18> presets{{
        {0, 79, 128, 255}, {1, 255, 79, 79},   {2, 79, 255, 79},   {3, 79, 79, 255},  {7, 128, 128, 128},
        {8, 0, 0, 0},      {9, 255, 102, 0},   {10, 0, 85, 0},     {11, 0, 153, 153}, {12, 204, 153, 102},
        {13, 153, 51, 0},  {14, 153, 51, 255}, {15, 255, 153, 0},  {16, 0, 0, 136},   {17, 255, 153, 204},
        {19, 153, 0, 0},   {21, 255, 153, 153}, {0, 79, 128, 255}}};
    auto const nearest = *std::min_element(presets.begin(), presets.end(), [&](auto const &p, auto const &q) {
        auto d = [&](auto const &c) { return (c[1] - r) * (c[1] - r) + (c[2] - g) * (c[2] - g) + (c[3] - b) * (c[3] - b); };
        return d(p) < d(q);
    });
    auto const dim = attr("proof:layer-dim");
    auto const label = group->label() ? group->label() : group->defaultLabel();
    if (_legacy) {
        if (!dim.empty() && dim != "50") refuse("layer image dimming other than 50% (Illustrator 7)");
        for (auto const *parent = group->parent; parent; parent = parent->parent) {
            if (auto ancestor = cast<SPGroup>(parent); ancestor && ancestor->isLayer()) {
                refuse("nested layers (Illustrator 7)");
                break;
            }
        }
    }
    // visible, preview, unlocked, printing, dimmed, 0, 1, colour index, its RGB, 0, dim %, 0: as
    // Illustrator 29 writes it (`1 1 1 1 0 0 1 0 79 128 255 0 50 0 Lb`).
    _body << "%AI5_BeginLayer\n"
          << std::format("{} {} {} {} {} 0 ", group->isHidden() ? 0 : 1,
                         attr("proof:layer-preview") == "false" ? 0 : 1, attr("sodipodi:insensitive").empty() ? 1 : 0,
                         attr("proof:layer-print") == "false" ? 0 : 1, dim.empty() ? 0 : 1);
    if (!_legacy) _body << "1 ";
    _body << std::format("{} {} {} {}", nearest[0], nearest[1], nearest[2], nearest[3]);
    if (!_legacy) _body << " 0 " << (dim.empty() ? "50" : dim) << " 0";
    _body << " Lb\n" << encoded_name(label, "layer names") << " Ln\n0 A\n";
    if (!_legacy) _body << "0 Xw\n";
    check_effects(group);
    bool const clipped = open_clip(group);
    contents(group);
    if (clipped) _body << "Q\n";
    _body << "LB\n%AI5_EndLayer--\n";
}

WrittenRecords Writer::write()
{
    // Shapes such as rectangles have their path only once the document is up to date.
    _doc->ensureUpToDate();
    auto const title = encoded_name(_doc->getDocumentName() ? _doc->getDocumentName() : "Untitled", "document titles");
    // Artboards: the document's pages, or its own size when it has none.
    std::vector<std::pair<std::string, Geom::Rect>> boards;
    for (auto const *page : _doc->getPageManager().getPages()) {
        boards.emplace_back(page->getLabel(), page->getDocumentRect());
    }
    if (boards.empty()) boards.emplace_back("Artboard 1", Geom::Rect(Geom::Point(0, 0), _doc->getDimensions()));
    if (_legacy && boards.size() != 1) refuse("more than one artboard (Illustrator 7)");
    auto const origin = boards.front().second.corner(0);
    _to_art = Geom::Translate(-origin) * Geom::Scale(PX_TO_PT, -PX_TO_PT);
    // The standalone format's origin is the single artboard's bottom-left, y upward.
    if (_legacy) _to_art *= Geom::Translate(0, boards.front().second.height() * PX_TO_PT);
    // The document's extent from the first artboard's top-left, and Illustrator's canvas around
    // it: the template box at its centre. Illustrator reads a file another application wrote on
    // the 8640-point canvas of its versions 3 to 8, its centre at 4320 4320 and y down, and
    // applies a gradient's Bg matrix there (doc/PROOF-ai-interchange.md, "Radial gradients").
    Geom::Rect extent = boards.front().second;
    for (auto const &[name, rect] : boards) extent.unionWith(rect);
    double const extent_w = (extent.right() - origin.x()) * PX_TO_PT, extent_h = (extent.bottom() - origin.y()) * PX_TO_PT;
    double const template_x = extent_w / 2 + 0.5, template_y = -extent_h / 2 - 0.5;
    _canvas = {4320 - template_x, 4320 + template_y};

    // Items outside any layer go to a layer of their own, as Illustrator keeps every object in one.
    auto const *root = _doc->getRoot();
    check_effects(root);
    if (root->getClipObject()) refuse("clipping on the SVG root");
    bool loose = false;
    for (auto const &child : root->children) {
        auto group = cast<SPGroup>(&child);
        if (is<SPItem>(&child) && !(group && group->isLayer())) loose = true;
    }
    for (auto const &child : root->children) {
        auto group = cast<SPGroup>(&child);
        if (group && group->isLayer()) layer(group);
    }
    if (loose) {
        ++_layers;
        _body << (_legacy ? "%AI5_BeginLayer\n1 1 1 1 0 0 0 79 128 255 Lb\n(Layer) Ln\n0 A\n"
                         : "%AI5_BeginLayer\n1 1 1 1 0 0 1 0 79 128 255 0 50 0 Lb\n(Layer) Ln\n0 A\n0 Xw\n");
        for (auto const &child : root->children) {
            auto group = cast<SPGroup>(&child);
            if (auto it = cast<SPItem>(&child); it && !(group && group->isLayer())) item(it);
        }
        _body << "LB\n%AI5_EndLayer--\n";
    }

    WrittenRecords out;
    if (!_refused.empty()) {
        out.refused.assign(_refused.begin(), _refused.end());
        return out;
    }

    bool const cmyk = Colors::DocumentColors::mode(_doc) == Type::CMYK;
    if (_legacy) {
        double const w = boards.front().second.width() * PX_TO_PT;
        double const h = boards.front().second.height() * PX_TO_PT;
        std::ostringstream head;
        head << "%!PS-Adobe-3.0\n%%Creator: PROOF\n%%Title: " << title << '\n'
             << std::format("%%BoundingBox: 0 0 {} {}\n%%HiResBoundingBox: 0 0 {} {}\n",
                            num(std::ceil(w)), num(std::ceil(h)), num(w), num(h))
             << "%%DocumentProcessColors: Cyan Magenta Yellow Black\n";
        ink_comments(head);
        // Newer Illustrator converts legacy rasters into the document's colour model on
        // open. Keep the source mode as an advisory comment; version 7 ignores this later
        // comment (1998 specification, section 1.5), while the artwork remains version 7.
        head << "%AI5_FileFormat 3\n%AI3_ColorUsage: Color\n%AI9_ColorModel: " << (cmyk ? 2 : 1) << '\n'
             << std::format("%AI3_Cropmarks: 0 0 {} {}\n%AI3_TileBox: 0 0 {} {}\n", num(w), num(h), num(w), num(h))
             // Legacy conversion places the artboard ceil(w/2) left and ceil(h/2) up
             // from this origin. Match the crop rectangle, including fractional sizes,
             // or Illustrator creates a second, displaced artboard.
             << std::format("%AI3_TemplateBox: {} {} {} {}\n", num(std::ceil(w / 2)), num(h - std::ceil(h / 2)),
                            num(std::ceil(w / 2)), num(h - std::ceil(h / 2)))
             << std::format("%AI5_ArtSize: {} {}\n", num(w), num(h))
             << "%AI5_RulerUnits: 2\n%AI3_DocumentPreview: None\n%AI5_NumLayers: " << _layers << '\n'
             << "%%EndComments\n%%BeginProlog\n%%EndProlog\n%%BeginSetup\n";
        if (!_gradients.empty()) {
            head << _gradients.size() << " Bn\n";
            for (auto const &definition : _gradients) head << definition;
        }
        head << "%%EndSetup\n";
        out.records = head.str() + _body.str() + "%%PageTrailer\n%%Trailer\n%%EOF\n";
        return out;
    }

    double const w = extent_w, h = extent_h, cx = template_x, cy = template_y;
    std::ostringstream head;
    head << "%!PS-Adobe-3.0 \n%%Creator: PROOF\n%%AI8_CreatorVersion: 29.2.1\n%%For: () ()\n"
         << "%%Title: " << title << '\n'
         << "%%Canvassize: 16383\n"
         << std::format("%%BoundingBox: 0 {} {} 0\n%%HiResBoundingBox: 0 {} {} 0\n", num(std::floor(-h)),
                        num(std::ceil(w)), num(-h), num(w))
         << "%%DocumentProcessColors: Cyan Magenta Yellow Black\n";
    ink_comments(head);
    head << "%AI5_FileFormat 14.0\n%AI12_BuildNumber: 116\n%AI3_ColorUsage: Color\n"
         << std::format("%AI3_Cropmarks: 0 {} {} 0\n", num(-h), num(w))
         << std::format("%AI3_TemplateBox: {} {} {} {}\n", num(cx), num(cy), num(cx), num(cy))
         << std::format("%AI3_TileBox: 0 {} {} 0\n", num(-h), num(w))
         << "%AI3_DocumentPreview: None\n%AI5_ArtSize: 14400 14400\n%AI5_RulerUnits: 2\n"
         << "%AI24_LargeCanvasScale: 1\n"
         << "%AI9_ColorModel: " << (cmyk ? 2 : 1) << '\n'
         << "%AI5_ArtFlags: 0 0 0 1 0 0 1 0 0\n%AI5_TargetResolution: 800\n"
         << "%AI5_NumLayers: " << _layers << '\n';
    // Where Illustrator's window looks when the file opens: the first artboard, fitted. Without
    // it the window opened on empty canvas (the owner, 2026-10-17). Its top-left corner in art
    // space, the zoom, then a 1910 by 914 window and constants as Illustrator 29 writes them.
    auto const &first_board = boards.front().second;
    double const bw = first_board.width() * PX_TO_PT, bh = first_board.height() * PX_TO_PT;
    double const zoom = std::clamp(0.9 * std::min(1910 / std::max(bw, 1.0), 914 / std::max(bh, 1.0)), 0.03125, 64.0);
    auto const view = std::format("{} {} {}", num(bw / 2 - 1910 / (2 * zoom)), num(-bh / 2 + 914 / (2 * zoom)), num(zoom));
    head << "%AI17_Begin_Content_if_version_gt:24 4\n"
         << "%AI10_OpenToVie: " << view << " 0 8191 8191 1910 914 18 0 0 5 121 0 0 0 1 1 0 1 1 0 1\n"
         << "%AI17_Alternate_Content\n"
         << "%AI9_OpenToView: " << view << " 1910 914 18 0 0 5 121 0 0 0 1 1 0 1 1 0 1\n"
         // No %AI5_OpenViewLayers: it holds view states per layer that override `Lb`'s, and a
         // copied "7" moved the hidden and locked flags onto other layers.
         << "%AI17_End_Versioned_Content\n"
         << std::format("%%PageOrigin:0 {}\n", num(-h)) << "%%EndComments\n"
         << "%%BeginProlog\n%%EndProlog\n%%BeginSetup\n";

    // The artboards, as the writer recipe's document data.
    head << "%AI9_BeginDocumentData\n%_/Document :\n%_/Dictionary :\n%_0 /Int (CropAreaActive) ,\n%_/Array :\n";
    for (auto const &[name, rect] : boards) {
        double const l = (rect.left() - origin.x()) * PX_TO_PT, t = -(rect.top() - origin.y()) * PX_TO_PT;
        double const r = (rect.right() - origin.x()) * PX_TO_PT, bot = -(rect.bottom() - origin.y()) * PX_TO_PT;
        auto uuid = g_uuid_string_random();
        head << "%_/Dictionary :\n"
             << std::format("%_{} {} /RealPointRelToROrigin\n%_ (PositionPoint1) ,\n", num(l), num(t))
             << std::format("%_{} {} /RealPointRelToROrigin\n%_ (PositionPoint2) ,\n", num(r), num(bot))
             << "%_0 /Bool (IsArtboardDefaultName) ,\n%_0 /Bool (IsArtboardSelected) ,\n%_0 /Int (DisplayMark) ,\n"
             << std::format("%_{} {} /RealPoint\n%_ (RulerOrigin) ,\n", num(std::floor(8191.5 - cx + l)),
                            num(std::floor(8191.5 + cy - t)))
             << "%_(" << uuid << ") /String (ArtboardUUID) ,\n%_1 /Real (PAR) ,\n"
             << "%_" << ps_string(name) << " /UnicodeString (Name) ,\n%_; ,\n";
        g_free(uuid);
    }
    head << "%_; (ArtboardArray) ,\n%_; /Recorded ,\n%_;\n%AI9_EndDocumentData\n";
    if (!_gradients.empty()) {
        head << _gradients.size() << " Bn\n";
        for (auto const &definition : _gradients) head << definition;
    }
    head << "%%EndSetup\n";

    out.records = head.str() + _body.str() +
                  "%%PageTrailer\ngsave annotatepage grestore showpage\n%%Trailer\n%%EOF\n";
    return out;
}

} // namespace

WrittenRecords write_records(SPDocument *document, RecordsVersion version)
{
    return Writer(document, version).write();
}

} // namespace Inkscape::Extension::Internal::AiNative
