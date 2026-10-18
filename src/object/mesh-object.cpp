// SPDX-License-Identifier: GPL-2.0-or-later
#include "mesh-object.h"

#include <array>
#include <cstring>
#include <unordered_set>
#include <vector>

#include <2geom/bezier-curve.h>

#include "document.h"
#include "helper/geom.h"
#include "path-chemistry.h"
#include "sp-ellipse.h"
#include "sp-mesh-gradient.h"
#include "sp-path.h"
#include "sp-rect.h"
#include "sp-star.h"
#include "style.h"
#include "svg/svg.h"
#include "xml/repr.h"

namespace Inkscape::MeshObject {
namespace {

bool is_outer_ring(SPMeshNodeArray const &array)
{
    auto const &nodes = array.nodes;
    // Stars/conical meshes form a closed outer ring with a collapsed inner
    // ring. The two radial seams coincide: they are not part of the outline
    // and must not become an extra stroked line through the object's middle.
    auto const center = nodes.back().front()->p;
    bool ring = Geom::are_near(nodes.front().front()->p, nodes.front().back()->p);
    for (auto node : nodes.back()) {
        ring = ring && Geom::are_near(node->p, center);
    }
    for (auto const &row : nodes) {
        ring = ring && Geom::are_near(row.front()->p, row.back()->p);
    }
    return ring;
}

Geom::PathVector boundary(SPMeshNodeArray const &array, bool outer_ring)
{
    auto const &nodes = array.nodes;
    if (outer_ring) {
        Geom::Path outline(nodes.front().front()->p);
        for (unsigned col = 1; col < nodes.front().size(); col += 3) {
            outline.appendNew<Geom::CubicBezier>(nodes.front()[col]->p, nodes.front()[col + 1]->p,
                                                 nodes.front()[col + 2]->p);
        }
        outline.close();
        return outline;
    }
    return array.outline_path();
}

bool fit_outline(SPShape *shape, SPMeshGradient *mesh)
{
    auto curve = shape->curve(); // The visible contour, including live path effects.
    if (!curve || curve->size() != 1 || curve->front().empty() || !curve->front().closed()) {
        return false; // Compound/open paths retain their ordinary mesh fill.
    }
    if (auto rect = cast<SPRect>(shape); rect && !rect->hasPathEffect() &&
        rect->rx.computed == 0 && rect->ry.computed == 0) {
        return true;
    }
    if (auto ellipse = cast<SPGenericEllipse>(shape); ellipse && !ellipse->hasPathEffect() && ellipse->is_whole()) {
        return true;
    }
    if (auto star = cast<SPStar>(shape); star && !star->hasPathEffect() &&
        star->rounded == 0 && star->randomized == 0) {
        return true;
    }

    // Inkscape's generic creation uses the bounding box, which would turn a
    // path/rounded shape into a rectangle when its mesh becomes its outline.
    // Preserve the actual single closed contour instead. Four-sided contours
    // form one patch; longer contours use the same radial topology as stars.
    auto curves = pathv_to_linear_and_cubic_beziers(*curve);
    if (curves.empty()) {
        return false;
    }
    std::vector<std::array<Geom::Point, 4>> sides;
    for (auto const &segment : curves.front()) {
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&segment)) {
            sides.push_back({(*cubic)[0], (*cubic)[1], (*cubic)[2], (*cubic)[3]});
        } else {
            auto a = segment.initialPoint(), b = segment.finalPoint();
            if (!Geom::are_near(a, b)) {
                sides.push_back({a, a + (b - a) / 3, a + (b - a) * (2.0 / 3), b});
            }
        }
    }
    if (sides.empty()) {
        return false;
    }
    auto &array = mesh->array;
    auto rows = array.patch_rows();
    auto columns = array.patch_columns();
    auto color = array.nodes.front().front()->color;
    auto alternate = array.nodes.front()[3]->color;
    auto bounds = curve->boundsExact();
    if (!bounds) {
        return false;
    }
    auto center = bounds->midpoint();
    array.clear();
    if (sides.size() == 4) {
        SPMeshPatchI patch(&array.nodes, 0, 0);
        for (unsigned side = 0; side < 4; ++side) {
            for (unsigned point = 0; point < 4; ++point) {
                patch.setPoint(side, point, sides[side][point]);
            }
            patch.setPathType(side, 'c');
            patch.setColor(side, *(side % 2 ? alternate : color));
        }
        patch.updateNodes();
        array.split_row(0, rows);
        array.split_column(0, columns);
    } else {
        for (unsigned col = 0; col < sides.size(); ++col) {
            SPMeshPatchI patch(&array.nodes, 0, col);
            for (unsigned point = 0; point < 4; ++point) {
                patch.setPoint(0, point, sides[col][point]);
            }
            patch.setPoint(2, 0, center);
            patch.setPoint(3, 0, center);
            for (unsigned side = 0; side < 4; ++side) {
                patch.setPathType(side, side == 0 ? 'c' : 'l');
                patch.setColor(side, *((col + side) % 2 ? alternate : color));
            }
            patch.updateNodes();
        }
        array.split_row(0, rows);
    }
    array.update_node_vectors();
    array.write(mesh);
    return true;
}

} // namespace

bool is_mesh_object(SPObject const *object)
{
    auto path = cast<SPPath>(object);
    auto marker = path ? path->getRepr()->attribute(ATTRIBUTE) : nullptr;
    return marker && std::strcmp(marker, "true") == 0;
}

bool sync(SPItem *item, SPMeshGradient *mesh, bool write_repr)
{
    auto path = cast<SPPath>(item);
    if (!path || path->cloned || !mesh || !path->style ||
        path->style->getFillPaintServer() != mesh ||
        mesh->getUnits() != SP_GRADIENT_UNITS_USERSPACEONUSE) {
        return false;
    }
    if (!is_mesh_object(path) || path->hasPathEffect()) {
        return false;
    }
    auto source = cast<SPMeshGradient>(mesh->getArray());
    auto array = source ? &source->array : &mesh->array;
    if (array->nodes.empty()) {
        mesh->ensureArray();
        array = &mesh->array;
    }
    if (array->nodes.empty()) {
        return false;
    }

    auto kind = path->getRepr()->attribute(BOUNDARY_ATTRIBUTE);
    bool outer_ring = kind && std::strcmp(kind, "outer-ring") == 0;
    auto outline = boundary(*array, outer_ring) * mesh->gradientTransform;
    auto d = sp_svg_write_path(outline);
    auto previous = path->getRepr()->attribute("d");
    bool repr_changed = !previous || d != previous;
    auto current = path->curve();
    bool curve_changed = !current || sp_svg_write_path(*current) != d;
    if (write_repr && repr_changed) {
        path->setAttribute("d", d);
        // Rounding or anchor types recorded for an outline that is no longer
        // the mesh's (files saved before Direct Selection left mesh objects alone).
        path->removeAttribute("inkscape:live-corners");
        path->removeAttribute("sodipodi:nodetypes");
    } else if (curve_changed) {
        path->setCurve(std::move(outline));
    }
    return curve_changed || (write_repr && repr_changed);
}

SPPath *create(SPItem *item, SPMeshGradient *mesh)
{
    auto shape = cast<SPShape>(item);
    if (!shape || shape->cloned || !mesh || mesh->array.nodes.empty() ||
        mesh->getUnits() != SP_GRADIENT_UNITS_USERSPACEONUSE || !fit_outline(shape, mesh)) {
        return nullptr;
    }
    // The normal conversion also bakes live shape/path effects and preserves
    // identity, stacking position, transforms, style, clipping and masks.
    auto document = item->document;
    std::string id = item->getId();
    std::vector<SPItem *> selected{item};
    std::vector<XML::Node *> converted;
    sp_item_list_to_curves({item}, selected, converted);
    auto path = cast<SPPath>(document->getObjectById(id));
    if (!path) {
        return nullptr;
    }
    // The visible contour was baked into the mesh. Old node/corner metadata
    // must not later reconstruct a different pre-mesh outline.
    path->removeAttribute("inkscape:live-corners");
    path->removeAttribute("sodipodi:nodetypes");
    path->getRepr()->setAttribute(ATTRIBUTE, "true");
    // Topology belongs to the object. Do not infer it again from coincident
    // points after an inner handle or centre point has been moved.
    path->getRepr()->setAttribute(BOUNDARY_ATTRIBUTE,
                                is_outer_ring(mesh->array) ? "outer-ring" : nullptr);
    sync(path, mesh);
    return path;
}

void sync_users(SPMeshGradient *mesh)
{
    std::vector<SPMeshGradient *> pending{mesh};
    std::unordered_set<SPMeshGradient *> visited;
    while (!pending.empty()) {
        auto gradient = pending.back();
        pending.pop_back();
        if (!visited.insert(gradient).second) {
            continue;
        }
        // Updating a path can notify style users. Do not walk the live list.
        auto users = gradient->hrefList;
        for (auto user : users) {
            if (auto reference = cast<SPMeshGradient>(user)) {
                pending.push_back(reference);
            } else if (auto item = cast<SPItem>(user)) {
                sync(item, gradient);
            }
        }
    }
}

} // namespace Inkscape::MeshObject
