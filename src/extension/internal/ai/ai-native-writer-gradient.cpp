// SPDX-License-Identifier: GPL-2.0-or-later
/** Opaque process linear and radial gradients shared by modern and Illustrator 7 records.
 *
 * Adobe's 1998 format, section 3.5, provides editable Bg geometry independently of
 * the optional imaging matrices. Collapse the SVG affine scalar field to an absolute
 * art-space vector and use identity Bg: no legacy canvas-relative matrix convention
 * or modern Xm interpretation is needed. See doc/PROOF-ai-interchange.md. Actual
 * Illustrator and native/PDF parity require qualification beyond synthetic readback.
 *
 * A radial instance is written as Illustrator writes every one: Bb, the highlight Bh,
 * Bg, and in the modern form the whole placement Bm (the unit circle onto the art).
 * Illustrator itself reads Bg, an ellipse there by its aspect ratio and, turned, by Bg's
 * matrix on its canvas (see radial_instance). Illustrator 7 records get circles only.
 */
#include "ai-native-writer-gradient.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <set>
#include <sstream>
#include <variant>
#include <vector>

#include "colors/color.h"
#include "colors/spaces/base.h"
#include "object/sp-gradient.h"
#include "object/sp-gradient-reference.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-shape.h"
#include "object/sp-stop.h"
#include "style.h"
#include "xml/node.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;
constexpr std::size_t MAX_STOPS = 256; // The native reader's default limit, including endpoints.
constexpr std::size_t MAX_REFS = 32;

std::string num(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

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

WrittenGradient refused(std::string why)
{
    WrittenGradient out;
    out.refused.push_back(std::move(why));
    return out;
}

bool finite(Geom::Affine const &m)
{
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

bool regular(Geom::Affine const &m)
{
    if (!finite(m)) return false;
    double const scale = std::max({std::abs(m[0]), std::abs(m[1]), std::abs(m[2]), std::abs(m[3])});
    return scale > 0 && std::isfinite(m.det()) && std::abs(m.det()) > 1e-12 * scale * scale;
}

bool ink_metadata(SPObject const *object)
{
    return object->getRepr()->attribute("proof:ink") || object->getRepr()->attribute("proof:ink-alternate") ||
           object->getRepr()->attribute("proof:ink-tint");
}

// Check the entire chain before composed-vector access: malformed hrefs must not become
// synthetic transparent stops or a silently defaulted placement.
std::optional<std::string> check_references(SPGradient const *gradient)
{
    std::set<SPGradient const *> visited;
    constexpr std::array<char const *, 5> linear{"x1", "y1", "x2", "y2", "gradientTransform"};
    constexpr std::array<char const *, 7> radial{"cx", "cy", "r", "fx", "fy", "fr", "gradientTransform"};
    std::vector<char const *> placement(linear.begin(), linear.end());
    if (is<SPRadialGradient>(gradient)) placement.assign(radial.begin(), radial.end());
    for (auto const *g = gradient; g;) {
        if (!visited.insert(g).second || visited.size() > MAX_REFS) return "cyclic or excessive gradient references";
        if (ink_metadata(g)) return "spot inks in gradients";
        if (g != gradient) {
            for (auto const *key : placement) {
                if (!gradient->getRepr()->attribute(key) && g->getRepr()->attribute(key)) {
                    return "inherited gradient geometry or transforms";
                }
            }
        }
        for (auto const &child : g->children) {
            if (is<SPStop>(&child) && ink_metadata(&child)) return "spot inks in gradients";
        }
        auto const *next = g->ref ? g->ref->getObject() : nullptr;
        if (!next && (g->getRepr()->attribute("xlink:href") || g->getRepr()->attribute("href"))) {
            return "unresolved gradient references";
        }
        g = next;
    }
    if (!gradient->getVector()->hasStops()) return "gradients without stops";
    return {};
}

/// A linear gradient's instance: Bb, then Bg with the absolute art-space vector of the scalar
/// field `matrix` carries the gradient's own onto.
std::variant<std::string, WrittenGradient> linear_instance(SPLinearGradient const *gradient, Geom::Affine const &matrix,
                                                           std::string const &name)
{
    Geom::Point const p0(gradient->x1.computed, gradient->y1.computed);
    Geom::Point const d(gradient->x2.computed - p0.x(), gradient->y2.computed - p0.y());
    double const dd = d.x() * d.x() + d.y() * d.y();
    if (!std::isfinite(dd) || dd <= 0) return refused("zero-length or nonfinite linear gradients");
    auto const inverse = matrix.inverse();
    Geom::Point const n((inverse[0] * d.x() + inverse[1] * d.y()) / dd,
                        (inverse[2] * d.x() + inverse[3] * d.y()) / dd);
    double const nn = n.x() * n.x() + n.y() * n.y();
    if (!std::isfinite(nn) || nn <= 0) return refused("zero-length or nonfinite linear gradients");
    auto const vector = n / nn;
    auto const origin = p0 * matrix;
    double const length = std::hypot(vector.x(), vector.y());
    double const angle = std::atan2(vector.y(), vector.x()) * 180.0 / std::acos(-1.0);
    if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) || !std::isfinite(length) || num(length) == "0") {
        return refused("zero-length or nonfinite linear gradients");
    }
    return std::format("Bb\n1 {} {} {} {} {} 1 0 0 1 0 0 Bg\n", name, num(origin.x()), num(origin.y()), num(angle),
                       num(length));
}

/** A radial gradient's instance, as Illustrator writes every radial one: Bb, the highlight
 * `x y angle share Bh`, Bg, and in the modern form Bm, the whole placement.
 *
 * Illustrator reads Bg and not Bm, and reads Bg two ways (Illustrator 29, 2026-10-20; see
 * doc/PROOF-ai-interchange.md, "Radial gradients"). In a file whose creator is Illustrator,
 * its origin and length are relative to the object's bounds; in any other, as here, they are
 * art space, and its angle turns nothing. So the ellipse is written lying along x: Bg's
 * length is its x radius and the aspect ratio (modern form) y's over x's, either side of 1.
 * One that is turned also gets Bg's matrix, which Illustrator applies on its y-down canvas,
 * where the art origin is `canvas`: there it turns the ellipse about its centre.
 *
 * Bm, which PostScript and PROOF's reader take, carries the unit circle onto the art, x along
 * the first axis and y along the other, mirrored as most of Illustrator's own are. Bh's angle
 * and share are taken in it (`share·(cos a, −sin a)` through Bm is the offset `x y`);
 * Illustrator takes them in the ellipse before its turn, which is the same.
 */
std::variant<std::string, WrittenGradient> radial_instance(SPRadialGradient const *gradient, Geom::Affine const &matrix,
                                                           std::string const &name, bool legacy, Geom::Point const &canvas)
{
    double const pi = std::acos(-1.0);
    double const r = gradient->r.computed;
    if (!std::isfinite(r) || r <= 0) return refused("zero-radius or nonfinite radial gradients");
    // An absent fr reads as 0; SVG 2's focal circle has no counterpart in Illustrator.
    if (!std::isfinite(gradient->fr.computed) || std::abs(gradient->fr.computed) > 1e-9 * r) {
        return refused("radial gradients with a focal radius");
    }
    Geom::Point const centre(gradient->cx.computed, gradient->cy.computed);
    auto const unit = Geom::Affine(r, 0, 0, r, centre.x(), centre.y()) * matrix;
    if (!regular(unit)) return refused("singular or nonfinite gradient transforms");

    // The circle becomes an ellipse whose axes are the eigenvectors of A·Aᵀ, where A's
    // columns are the images of the unit axes.
    double const p = unit[0] * unit[0] + unit[2] * unit[2];
    double const q = unit[0] * unit[1] + unit[2] * unit[3];
    double const s = unit[1] * unit[1] + unit[3] * unit[3];
    double const mean = (p + s) / 2, half = std::hypot((p - s) / 2, q);
    double rx = std::sqrt(mean + half), ry = std::sqrt(std::max(mean - half, 0.0));
    double turn = 0.5 * std::atan2(2 * q, p - s); // Of the first axis, in (−90°, 90°].
    if (rx - ry <= 1e-6 * rx) {
        ry = rx;
        turn = 0;
    } else {
        if (legacy) return refused("elliptical radial gradients (Illustrator 7)");
        // Lying along y needs no turn: y's radius is then the larger.
        if (std::abs(std::abs(turn) - pi / 2) < 1e-6) {
            std::swap(rx, ry);
            turn = 0;
        } else if (std::abs(turn) < 1e-6) {
            turn = 0;
        }
    }
    double const cosine = std::cos(turn), sine = std::sin(turn);
    Geom::Affine const bm(rx * cosine, rx * sine, ry * sine, -ry * cosine, unit[4], unit[5]);
    if (!regular(bm) || !std::isfinite(rx) || num(rx) == "0" || num(ry / rx) == "0") {
        return refused("singular or nonfinite gradient transforms");
    }

    // The focal point in the unit circle. The canvas draws one outside the circle on it.
    Geom::Point focus((gradient->fx.computed - centre.x()) / r, (gradient->fy.computed - centre.y()) / r);
    if (!std::isfinite(focus.x()) || !std::isfinite(focus.y())) return refused("nonfinite radial gradient focal points");
    if (Geom::L2(focus) > 1) focus /= Geom::L2(focus);
    auto const offset = focus * unit.withoutTranslation();
    auto const in_bm = offset * bm.withoutTranslation().inverse();
    double const share = std::min(Geom::L2(in_bm), 1.0);
    std::string out = "Bb\n";
    if (num(share) == "0") {
        out += "0 0 0 0 Bh\n";
    } else {
        out += std::format("{} {} {} {} Bh\n", num(offset.x()), num(offset.y()),
                           num(std::atan2(-in_bm.y(), in_bm.x()) * 180 / pi), num(share));
    }

    // The turn about the centre, on the canvas: y runs down there, so it turns the other way.
    Geom::Affine turned;
    if (turn != 0) {
        Geom::Point const there(unit[4] + canvas.x(), canvas.y() - unit[5]);
        Geom::Affine const back(cosine, -sine, sine, cosine, 0, 0);
        turned = back * Geom::Translate(there - there * back);
        if (!regular(turned)) return refused("singular or nonfinite gradient transforms");
    }
    // To ten places: the turn is about a point some 4000 points out on the canvas, where four
    // would put the ellipse a fifth of a point off. Illustrator writes these to fifteen.
    auto const fine = [](double v) {
        auto t = std::format("{:.10f}", v);
        t.erase(t.find_last_not_of('0') + 1);
        if (t.back() == '.') t.pop_back();
        return t == "-0" ? std::string("0") : t;
    };
    out += std::format("1 {} {} {} 0 {} {} {} {} {} {} {}", name, num(unit[4]), num(unit[5]), num(rx), fine(turned[0]),
                       fine(turned[1]), fine(turned[2]), fine(turned[3]), fine(turned[4]), fine(turned[5]));
    if (legacy) return out + " Bg\n";
    return out + std::format(" {} Bg\n{} {} {} {} {} {} Bm\n", num(ry / rx), num(bm[0]), num(bm[1]), num(bm[2]),
                             num(bm[3]), num(bm[4]), num(bm[5]));
}

} // namespace

WrittenGradient write_gradient(SPShape const *shape, Geom::Affine const &item_to_art,
                               std::string const &unique_name, bool legacy, Geom::Point const &canvas)
{
    if (!shape || !shape->style) return refused("gradients without a shape");
    auto const *style = shape->style;
    if (!style->stroke.isNone()) return refused("strokes on gradient-filled objects");
    if (style->opacity.as_double() < 1.0 - 1e-6 || style->fill_opacity.as_double() < 1.0 - 1e-6) {
        return refused("gradient transparency");
    }
    auto const *server = style->fill.isPaintserver() && style->fill.href ? style->fill.href->getObject() : nullptr;
    if (is<SPMeshGradient>(server)) return refused("mesh gradients");
    auto const *linear = cast<SPLinearGradient>(server);
    auto const *radial = cast<SPRadialGradient>(server);
    SPGradient const *gradient = linear;
    if (radial) gradient = radial;
    if (!gradient || gradient->isSolid()) return refused("fills other than linear or radial gradients");
    if (auto why = check_references(gradient)) return refused(*why);
    if (gradient->fetchSpread() != SP_GRADIENT_SPREAD_PAD) return refused("repeating or reflected gradients");
    auto const interpolation = gradient->style->color_interpolation.getInterpolationSpace();
    if (interpolation && interpolation->getType() != Type::RGB && interpolation->getType() != Type::CMYK) {
        return refused("gradient interpolation other than RGB or CMYK");
    }
    if (unique_name.empty()) return refused("gradients without a generated name");
    if (legacy && !std::all_of(unique_name.begin(), unique_name.end(), [](unsigned char c) { return c < 128; })) {
        return refused("non-ASCII gradient names (Illustrator 7)");
    }

    bool const bbox_units = gradient->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX;
    std::vector<SVGLength const *> lengths;
    if (linear) {
        lengths = {&linear->x1, &linear->y1, &linear->x2, &linear->y2};
    } else {
        lengths = {&radial->cx, &radial->cy, &radial->r, &radial->fx, &radial->fy};
    }
    for (auto const *length : lengths) {
        if (bbox_units && length->unit != SVGLength::NONE && length->unit != SVGLength::PERCENT) {
            return refused("non-normalized object-bounding-box gradient lengths");
        }
        if (!bbox_units && (length->unit == SVGLength::PERCENT || length->unit == SVGLength::EM ||
                            length->unit == SVGLength::EX)) {
            return refused("viewport-relative gradient lengths");
        }
        if (!std::isfinite(length->computed)) return refused("nonfinite gradient geometry");
    }
    auto matrix = gradient->gradientTransform;
    if (bbox_units) {
        auto const box = shape->geometricBounds();
        if (!box || box->width() <= 0 || box->height() <= 0) return refused("empty gradient object bounds");
        matrix *= Geom::Affine(box->width(), 0, 0, box->height(), box->left(), box->top());
    }
    matrix *= item_to_art;
    if (!regular(matrix)) return refused("singular or nonfinite gradient transforms");
    auto const name = ps_string(unique_name);
    auto instance = linear ? linear_instance(linear, matrix, name) : radial_instance(radial, matrix, name, legacy, canvas);
    if (auto *why = std::get_if<WrittenGradient>(&instance)) return std::move(*why);

    auto const *effective = gradient->getGradientVector();
    if (effective->stops.size() < 2 || effective->stops.size() > MAX_STOPS) return refused("excessive or missing gradient stops");
    std::optional<Type> model;
    std::vector<std::string> stops;
    double previous = -1;
    for (auto const &stop : effective->stops) {
        if (!stop.color || !std::isfinite(stop.offset)) return refused("invalid gradient stops");
        auto const &color = *stop.color;
        if (!std::isfinite(color.getOpacity()) || color.getOpacity() < 1.0 - 1e-6) return refused("gradient transparency");
        auto const space = color.getSpace();
        if (!space || (space->getType() != Type::RGB && space->getType() != Type::CSSNAME &&
                       space->getType() != Type::CMYK)) {
            return refused("gradient colours other than standard RGB or DeviceCMYK");
        }
        // A CSS colour name is standard sRGB with different spelling, not a named ink.
        auto const stop_model = space->getType() == Type::CSSNAME ? Type::RGB : space->getType();
        if (model && *model != stop_model) return refused("mixed gradient colour spaces");
        model = stop_model;
        if (interpolation && interpolation->getType() != stop_model) {
            return refused("gradient interpolation different from its stop colour space");
        }
        auto const offset = std::round(stop.offset * 1000000.0) / 1000000.0;
        if (offset <= previous) return refused("gradient hard stops or coincident offsets");
        previous = offset;
        auto const count = *model == Type::CMYK ? 4u : 3u;
        for (unsigned i = 0; i < count; ++i) {
            if (!std::isfinite(color[i]) || color[i] < 0 || color[i] > 1) return refused("out-of-range gradient colours");
        }
        std::ostringstream line;
        if (*model == Type::RGB) {
            auto const alternate = color.converted(Type::CMYK);
            if (!alternate) return refused("RGB gradient colours without a CMYK alternate");
            for (unsigned i = 0; i < 4; ++i) line << num((*alternate)[i]) << ' ';
        }
        for (unsigned i = 0; i < count; ++i) line << num(color[i]) << ' ';
        line << (*model == Type::CMYK ? 1 : 2) << " 50 " << num(offset * 100) << " %_Bs\n";
        stops.push_back(line.str());
    }

    // Illustrator lists a linear gradient's stops last first and a radial one's first first,
    // and takes them in that order whatever their ramp points say: a radial's written last
    // first opened inside out in Illustrator 29 (2026-10-20). Its own files, without
    // exception: 6,906 linear definitions descending, 4,482 radial ones ascending.
    std::ostringstream definition;
    definition << "%AI5_BeginGradient: " << name << '\n' << name << (radial ? " 1 " : " 0 ") << stops.size() << " Bd\n[\n";
    if (radial) {
        for (auto const &stop : stops) definition << stop;
    } else {
        for (auto it = stops.rbegin(); it != stops.rend(); ++it) definition << *it;
    }
    definition << "BD\n%AI5_EndGradient\n";
    WrittenGradient out;
    out.definition = definition.str();
    out.begin = std::get<std::string>(instance);
    out.end = "0 BB\n";
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative
