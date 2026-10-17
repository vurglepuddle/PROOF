// SPDX-License-Identifier: GPL-2.0-or-later
/** Opaque process linear gradients shared by modern and Illustrator 7 records.
 *
 * Adobe's 1998 format, section 3.5, provides editable Bg geometry independently of
 * the optional imaging matrices. Collapse the SVG affine scalar field to an absolute
 * art-space vector and use identity Bg: no legacy canvas-relative matrix convention
 * or modern Xm interpretation is needed. See doc/PROOF-ai-interchange.md. Actual
 * Illustrator and native/PDF parity require qualification beyond synthetic readback.
 */
#include "ai-native-writer-gradient.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <set>
#include <sstream>

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
    constexpr std::array<char const *, 5> placement{"x1", "y1", "x2", "y2", "gradientTransform"};
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

} // namespace

WrittenGradient write_gradient(SPShape const *shape, Geom::Affine const &item_to_art,
                               std::string const &unique_name, bool legacy)
{
    if (!shape || !shape->style) return refused("gradients without a shape");
    auto const *style = shape->style;
    if (!style->stroke.isNone()) return refused("strokes on gradient-filled objects");
    if (style->opacity.as_double() < 1.0 - 1e-6 || style->fill_opacity.as_double() < 1.0 - 1e-6) {
        return refused("gradient transparency");
    }
    auto const *server = style->fill.isPaintserver() && style->fill.href ? style->fill.href->getObject() : nullptr;
    if (is<SPMeshGradient>(server)) return refused("mesh gradients");
    if (is<SPRadialGradient>(server)) return refused("radial gradients");
    auto const *gradient = cast<SPLinearGradient>(server);
    if (!gradient || gradient->isSolid()) return refused("fills other than linear gradients");
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
    std::array<SVGLength const *, 4> const lengths{&gradient->x1, &gradient->y1, &gradient->x2, &gradient->y2};
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

    auto const name = ps_string(unique_name);
    std::ostringstream definition;
    definition << "%AI5_BeginGradient: " << name << '\n' << name << " 0 " << stops.size() << " Bd\n[\n";
    for (auto it = stops.rbegin(); it != stops.rend(); ++it) definition << *it;
    definition << "BD\n%AI5_EndGradient\n";
    WrittenGradient out;
    out.definition = definition.str();
    out.begin = std::format("Bb\n1 {} {} {} {} {} 1 0 0 1 0 0 Bg\n", name, num(origin.x()), num(origin.y()),
                            num(angle), num(length));
    out.end = "0 BB\n";
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative
