// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * A mesh gradient as Illustrator's gradient mesh. See ai-native-writer-mesh.h.
 *
 * The record forms are the ones the reader takes (Reader::mesh_entry) and Illustrator 29.2.1
 * writes: its mesh controls (2026-10-17) and the owner's and collaborator's files, where `[a b]
 * /Size` goes with `[a b] /P` (a 3 by 1 mesh's patches are [0 0], [1 0], [2 0]) and neighbours
 * share their corners, colours and sides.
 */

#include "ai-native-writer-mesh.h"
#include "ai-native-writer-gradient.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <sstream>

#include <2geom/bezier-curve.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>

#include "colors/color.h"
#include "colors/spaces/base.h"
#include "helper/geom.h"
#include "object/sp-mesh-array.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-shape.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;
/// The reader's default limit of patches in one mesh.
constexpr std::size_t MAX_PATCHES = 4096;

std::string num(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

/// Compare the complete control polygons, allowing a different start, direction, or subdivision
/// of straight sides. Sampling alone can miss a changed boundary. If this cannot prove the
/// outlines equal, the writer keeps the shape as a clip instead.
bool same_outline(Geom::Path const &a, Geom::Path const &b)
{
    constexpr double tolerance = 1e-4; // art points, independent of the object's translation
    using Curves = std::vector<std::unique_ptr<Geom::Curve>>;
    auto simplify = [&](Geom::Path const &path) {
        Curves curves;
        for (auto const &curve : path) {
            auto const *bezier = dynamic_cast<Geom::BezierCurve const *>(&curve);
            if (!bezier) return Curves{};
            auto const start = curve.initialPoint(), end = curve.finalPoint();
            Geom::LineSegment line(start, end);
            bool straight = true;
            double previous = 0;
            for (unsigned k = 0; k <= bezier->order(); ++k) {
                auto const p = bezier->controlPoint(k);
                auto const t = line.nearestTime(p);
                straight = straight && Geom::are_near(p, line.pointAt(t), tolerance) && t >= previous;
                previous = t;
            }
            if (straight && Geom::are_near(start, end, tolerance)) continue;
            if (straight && !curves.empty() && dynamic_cast<Geom::LineSegment *>(curves.back().get())) {
                Geom::LineSegment joined(curves.back()->initialPoint(), end);
                if (Geom::are_near(start, joined.pointAt(joined.nearestTime(start)), tolerance)) {
                    curves.back().reset(joined.duplicate());
                    continue;
                }
            }
            curves.emplace_back(straight ? line.duplicate() : curve.duplicate());
        }
        return curves;
    };
    auto const x = simplify(a), y = simplify(b);
    if (x.empty() || x.size() != y.size()) return false;
    for (std::size_t offset = 0; offset < y.size(); ++offset) {
        bool forward = true, backward = true;
        for (std::size_t i = 0; i < x.size(); ++i) {
            forward = forward && x[i]->isNear(*y[(offset + i) % y.size()], tolerance);
            std::unique_ptr<Geom::Curve> reversed(y[(offset + y.size() - i) % y.size()]->reverse());
            backward = backward && x[i]->isNear(*reversed, tolerance);
        }
        if (forward || backward) return true;
    }
    return false;
}

} // namespace

WrittenMesh write_mesh(SPShape const *shape, SPMeshGradient const *mesh, Geom::Affine const &item_to_art)
{
    WrittenMesh out;
    auto refused = [&](std::string why) {
        out.refused.push_back(std::move(why));
        return out;
    };
    if (!shape || !mesh || !shape->curve()) return refused("mesh gradients without a shape");
    // What the drawing takes (DrawingMeshGradient): the mesh's own units, transform and smoothing,
    // the patches of the mesh along its references that has them.
    if ((mesh->type_set && mesh->type == SP_MESH_TYPE_BICUBIC)) return refused("smoothed mesh gradients");
    mesh->ensureArray();
    auto &array = mesh->array;
    auto const rows = array.patch_rows(), columns = array.patch_columns();
    if (!rows || !columns) return refused("empty mesh gradients");
    if (std::size_t(rows) * columns > MAX_PATCHES) return refused("mesh gradients of more than 4096 patches");
    auto matrix = mesh->getGradientTransform();
    if (mesh->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {
        auto const box = shape->geometricBounds();
        if (!box || box->width() <= 0 || box->height() <= 0) return refused("empty gradient object bounds");
        matrix *= Geom::Affine(box->width(), 0, 0, box->height(), box->left(), box->top());
    }
    matrix *= item_to_art;
    bool regular = std::abs(matrix.det()) > 1e-12;
    for (int i = 0; i < 6; ++i) regular = regular && std::isfinite(matrix[i]);
    if (!regular) return refused("singular or nonfinite mesh transforms");

    // Each patch's sides in art space, its corners' colours, by row and column.
    struct Patch
    {
        std::array<std::array<Geom::Point, 4>, 4> sides;
        std::array<Colors::Color, 4> colors;
    };
    std::vector<Patch> patches;
    patches.reserve(rows * columns);
    std::optional<Type> model;
    std::map<Colors::CMS::Profile const *, bool> rgb_profiles;
    Geom::Affine const linear(matrix[0], matrix[1], matrix[2], matrix[3], 0, 0);
    for (unsigned r = 0; r < rows; ++r) {
        for (unsigned c = 0; c < columns; ++c) {
            SPMeshPatchI p(&array.nodes, r, c);
            Patch patch{{}, {Colors::Color(0x000000ff), Colors::Color(0x000000ff), Colors::Color(0x000000ff),
                             Colors::Color(0x000000ff)}};
            for (unsigned k = 0; k < 4; ++k) {
                for (unsigned i = 0; i < 4; ++i) {
                    auto const q = p.getPoint(k, i) * matrix;
                    if (!std::isfinite(q.x()) || !std::isfinite(q.y())) return refused("nonfinite mesh geometry");
                    patch.sides[k][i] = q;
                }
                // Illustrator's patches are Coons patches: a tensor point moved from there
                // draws a different surface.
                if (p.tensorIsSet(k) &&
                    Geom::L2((p.getTensorPoint(k) - p.coonsTensorPoint(k)) * linear) > 1e-6) {
                    return refused("mesh gradients with tensor points");
                }
                auto const color = p.getColor(k);
                if (!color || !std::isfinite(color->getOpacity())) return refused("mesh corners without a colour");
                auto const space = color->getSpace();
                std::optional<Type> corner;
                if (space && space->getComponentType() == Type::CMYK) {
                    // CMYK in a profile, as the import gives a CMYK file's paints, is written as its
                    // numbers, as solid paint and gradient stops are.
                    corner = Type::CMYK;
                } else if (space && (space->getType() == Type::RGB || space->getType() == Type::CSSNAME)) {
                    corner = Type::RGB;
                } else if (space && space->getType() == Type::CMS && space->getComponentType() == Type::RGB &&
                           space->hasValidCmsProfile()) {
                    if (auto const profile = space->getProfile()) {
                        auto [known, added] = rgb_profiles.try_emplace(profile.get(), false);
                        if (added) known->second = standard_rgb_profile(*profile);
                        if (known->second) corner = Type::RGB;
                    }
                }
                if (!corner) return refused("mesh colours other than standard RGB or CMYK");
                // Illustrator can change a mesh's space from point to point; how it blends two is
                // unqualified.
                if (model && *model != *corner) return refused("mesh gradients of more than one colour model");
                model = corner;
                for (unsigned i = 0; i < (*corner == Type::CMYK ? 4u : 3u); ++i) {
                    auto const v = (*color)[i];
                    if (!std::isfinite(v) || v < 0 || v > 1) return refused("out-of-range mesh colours");
                }
                patch.colors[k] = *color;
            }
            patches.push_back(std::move(patch));
        }
    }
    auto at = [&](std::size_t a, std::size_t b) -> Patch const & { return patches[b * columns + a]; };

    // The outline: the first row's side 0, the last column's side 1, the last row's side 2 and the
    // first column's side 3, around from the first patch's corner 0 (Builder::whole_mesh).
    Geom::Path outline(at(0, 0).sides[0][0]);
    auto add = [&](Patch const &p, std::size_t k) {
        auto const &s = p.sides[k];
        outline.appendNew<Geom::CubicBezier>(s[1], s[2], s[3]);
    };
    for (std::size_t a = 0; a < columns; ++a) add(at(a, 0), 0);
    for (std::size_t b = 0; b < rows; ++b) add(at(columns - 1, b), 1);
    for (std::size_t a = columns; a-- > 0;) add(at(a, rows - 1), 2);
    for (std::size_t b = rows; b-- > 0;) add(at(0, b), 3);
    outline.close(true);
    // Omit the clip only when the complete boundaries agree.
    auto const pv = pathv_to_linear_and_cubic_beziers(*shape->curve() * item_to_art);
    out.outlined = pv.size() == 1 && same_outline(pv[0], outline);

    // Illustrator smooths colour across the lines of a multi-patch grid; SVG/Cairo blends
    // each patch's corners independently. Keep those surfaces as individual editable meshes,
    // grouped by the caller so opacity and a mask apply to the complete object exactly once.
    out.patches = patches.size();
    std::ostringstream records;
    for (std::size_t a = 0; a < columns; ++a) {
        for (std::size_t b = 0; b < rows; ++b) {
            auto const &p = at(a, b);
            records << "/Mesh X!\n%_2 /Version X#\n%_/Cartesian /Type X#\n"
                       "%_[1 1] /Size X#\n%_/Data X#\n%_[0 0] /P X#\n%_[0] /R X#\n";
            records << (*model == Type::CMYK ? "%_/DeviceCMYK /CS X#\n" : "%_/DeviceRGB /CS X#\n");
            for (std::size_t k = 0; k < 4; ++k) {
                auto const &c = p.colors[k];
                auto const &point = p.sides[k][0], &next = p.sides[k][1], &previous = p.sides[(k + 3) % 4][2];
                records << "%_[";
                for (unsigned i = 0; i < (*model == Type::CMYK ? 4u : 3u); ++i) records << num(c[i]) << ' ';
                records << std::format("{} {} {} {} {} 1 {} {} 1] /N X#\n", num(std::clamp(c.getOpacity(), 0.0, 1.0)),
                                       num(point.x()), num(point.y()), num(next.x()), num(next.y()),
                                       num(previous.x()), num(previous.y()));
            }
            records << "%_/E X#\n/End X!\n\n";
            if (out.patches > 1) records << "0 0 Xd\n6 () XW\n";
        }
    }
    out.records = records.str();
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative
