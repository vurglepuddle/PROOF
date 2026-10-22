// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gradient functions to generate pdf shading funtions.
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "build-patterns.h"

#include <array>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#include "object/sp-gradient.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-pattern.h"
#include "object/sp-radial-gradient.h"

using Geom::X;
using Geom::Y;

namespace Inkscape::Extension::Internal::PdfBuilder {

PatternContext::PatternContext(Document &doc, Geom::Rect const &bbox)
    : DrawContext(doc, doc.generator().new_tiling_pattern_context(bbox.left(), bbox.top(), bbox.right(), bbox.bottom()),
                  false)
{}

/**
 * Return true if the SVG gradient is repeated and reflected, false if it's repeated but not reflected.
 */
static std::optional<bool> repeat_is_reflection(SPGradientSpread spread)
{
    switch (spread) {
        case SP_GRADIENT_SPREAD_REFLECT:
            return true;
        case SP_GRADIENT_SPREAD_REPEAT:
            return false;
        default: // Everything else is none
            break;
    }
    return {};
}

/**
 * Whether the pattern's tile or its art is sized by the painted object's box, so that each object
 * needs a tiling pattern of its own.
 */
static bool sized_to_object(SPPattern const *pat)
{
    return pat->patternUnits() == SPPattern::UNITS_OBJECTBOUNDINGBOX ||
           (!pat->viewbox() && pat->patternContentUnits() == SPPattern::UNITS_OBJECTBOUNDINGBOX);
}

/**
 * Returns true if the gradient has transparency.
 */
bool gradient_has_transparency(SPPaintServer const *paint)
{
    if (!paint || !paint->isValid())
        return false;

    if (auto linear = cast<SPLinearGradient>(paint)) {
        for (auto &stop : linear->getGradientVector()->stops) {
            if (stop.color->getOpacity() < 1.0) {
                return true;
            }
        }
    } else if (auto radial = cast<SPRadialGradient>(paint)) {
        for (auto &stop : radial->getGradientVector()->stops) {
            if (stop.color->getOpacity() < 1.0) {
                return true;
            }
        }
    } else if (auto mesh = cast<SPMeshGradient>(paint)) {
        SPMeshNodeArray array;
        array.read(const_cast<SPMeshGradient *>(mesh));
        for (auto &a : array.nodes) {
            for (auto &b : a) {
                if (b->color && b->color->getOpacity() < 1.0) {
                    return true;
                }
            }
        }
    }

    return false;
}

/**
 * Construct a PDF pattern object from the given paintserver. Gradients or patterns.
 *
 * @arg paint - The paint server vector.
 * @arg bbox - The bounding box for this pattern
 * @arg opacity - The total paint opacity, used for softmasking. Only used for soft-masking
 */
std::optional<CapyPDF_PatternId> Document::get_pattern(SPPaintServer const *paint, std::optional<double> opacity,
                                                      Geom::OptRect const &bbox)
{
    if (!paint || !paint->isValid())
        return {};

    // Patterns are cached so they can be reused
    std::string cache_key = get_id(paint);
    auto const *tiled = cast<SPPattern>(paint);
    if (bbox && (is<SPLinearGradient>(paint) || is<SPRadialGradient>(paint) || (tiled && sized_to_object(tiled)))) {
        // Different instances can share stops, but not an object-bounding-box
        // placement (or the paintable area used for repeat/reflect).
        std::ostringstream placement;
        placement.imbue(std::locale::classic());
        placement << std::setprecision(std::numeric_limits<double>::max_digits10)
                  << " [" << bbox->left() << ',' << bbox->top()
                  << ',' << bbox->right() << ',' << bbox->bottom() << ']';
        cache_key += placement.str();
    }
    if (opacity) {
        // Add the opacity of softmasks so they can include their fill/stroke-opacity attrbitues
        // without the cache colliding with either the color gradfient or other uses.
        cache_key += " @" + std::to_string((int)(*opacity * 100)) + "% mask";
    }

    if (auto const it = _pattern_cache.find(cache_key); it != _pattern_cache.end()) {
        return it->second;
    }

    std::optional<CapyPDF_PatternId> pattern_id;
    if (auto linear = cast<SPLinearGradient>(paint)) {
        pattern_id = get_linear_pattern(linear, opacity, bbox);
    } else if (auto radial = cast<SPRadialGradient>(paint)) {
        pattern_id = get_radial_pattern(radial, opacity, bbox);
    } else if (auto mesh = cast<SPMeshGradient>(paint)) {
        pattern_id = get_mesh_pattern(mesh, opacity);
    } else if (tiled) {
        // The art is the first pattern's along the href chain that has any; the tile and its
        // placement can come from any pattern along it.
        for (SPPattern const *root = tiled; root; root = root->ref.getObject()) {
            if (root->hasItemChildren()) {
                pattern_id = get_tiling_pattern(tiled, root, bbox);
                break;
            }
        }
    }
    if (pattern_id) {
        _pattern_cache[cache_key] = *pattern_id;
    }
    return pattern_id;
}

/**
 * Generate a linear gradient or linear gradient mask
 *
 * @arg linear - The Linear Gradient object to paint into the PDF Document
 * @arg opacity - If set, generate the linear gradient mask instead of the color gradient
 *
 * @returns A PDF Id of the new gradient object in the PDF document, if created.
 */
std::optional<CapyPDF_PatternId> Document::get_linear_pattern(SPLinearGradient const *linear,
                                                              std::optional<double> opacity,
                                                              Geom::OptRect const &item_bbox)
{
    // Repeat coverage also includes the stroke outside the geometric bounds.
    // Keep that existing path until placement and coverage are passed separately.
    auto bbox = item_bbox && linear->fetchSpread() == SP_GRADIENT_SPREAD_PAD
                    ? item_bbox : linear->getAllItemsBox();
    if (!bbox) {
        g_warning("Linear gradient has no paintable area: '%s'", get_id(linear).c_str());
        return {};
    }

    auto color_space = opacity ? CAPY_DEVICE_CS_GRAY : get_default_colorspace();
    auto to_userspace = Geom::Affine(bbox->width(), 0, 0, bbox->height(), bbox->left(), bbox->top());
    auto line = linear->getLine();
    auto cm = linear->gradientTransform;

    if (linear->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {
        cm *= to_userspace;
    }

    if (auto func = get_gradient_function(*linear->getGradientVector(), opacity, &color_space)) {
        auto d1 = 0.0;
        auto d2 = 1.0;

        if (auto is_reflection = repeat_is_reflection(linear->fetchSpread())) {
            // Bounding box is already in cm coords, so line must be transformed to compare them.
            auto line_cm = line * cm;
            auto o1 = line_cm.origin();
            auto o2 = line_cm.origin() + line_cm.vector();

            // Select the corners to calculate from
            int d1_corner = ((o1[X] < o2[X]) != (o1[Y] < o2[Y])) + ((o1[Y] >= o2[Y]) * 2);
            int d2_corner = (d1_corner + 2) % 4;

            // This maths was by Krlr17, with many thanks.
            d1 = line_cm.timeAtProjection(bbox->corner(d1_corner));
            d2 = line_cm.timeAtProjection(bbox->corner(d2_corner));

            func = get_repeat_function(*func, *is_reflection, std::floor(d1), std::ceil(d2));
        }

        // In PDF the shading space must be the total function space include repeats
        auto p1 = line.origin() + (line.vector() * d1);
        auto p2 = line.origin() + (line.vector() * d2);

        // x1, y1, x2, y2, interpolation function, extend past x1/y1, exend past x2/y2
        auto shading = capypdf::Type2Shading(color_space, p1[X], p1[Y], p2[X], p2[Y], *func);
        shading.set_extend(true, true);
        shading.set_domain(d1, d2);
        auto sid = _gen.add_shading(shading);

        auto pattern = capypdf::ShadingPattern(sid);
        pattern.set_matrix(cm[0], cm[1], cm[2], cm[3], cm[4], cm[5]);
        return _gen.add_shading_pattern(pattern);
    }
    return {};
}

/**
 * Generate a radial gradient or radial gradient mask
 *
 * @arg radial - The Radial Gradient object to paint into the PDF Document
 * @arg opacity - If set, generate the radial gradient mask instead of the color gradient
 *
 * @returns A PDF Id of the new gradient object in the PDF document, if created.
 */
std::optional<CapyPDF_PatternId> Document::get_radial_pattern(SPRadialGradient const *radial,
                                                              std::optional<double> opacity,
                                                              Geom::OptRect const &item_bbox)
{
    auto bbox = item_bbox && radial->fetchSpread() == SP_GRADIENT_SPREAD_PAD
                    ? item_bbox : radial->getAllItemsBox();
    if (!bbox) {
        g_warning("Radial gradient has no paintable area: '%s'", get_id(radial).c_str());
        return {};
    }

    auto color_space = opacity ? CAPY_DEVICE_CS_GRAY : get_default_colorspace();
    auto to_userspace = Geom::Affine(bbox->width(), 0, 0, bbox->height(), bbox->left(), bbox->top());
    auto cm = radial->gradientTransform;
    Geom::Point center(radial->cx.computed, radial->cy.computed);
    Geom::Point focal(radial->fx.computed, radial->fy.computed);

    double r = radial->r.computed;
    double fr = radial->fr.computed;

    if (radial->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {
        cm *= to_userspace;
    }

    if (auto func = get_gradient_function(*radial->getGradientVector(), opacity, &color_space)) {
        double d1 = 0.0;
        double d2 = 1.0;

        if (auto is_reflection = repeat_is_reflection(radial->fetchSpread())) {
            // Process X and Y radii to find the smallest one after transformation
            for (auto r_vector : {Geom::Point(r, 0.0), Geom::Point(0.0, r)}) {
                // Bounding box is already in cm coords, so radial must be transformed to compare them.
                auto line = Geom::Line::from_origin_and_vector(center, r_vector) * cm;
                auto r_cm = Geom::distance(line.origin(), line.origin() + line.vector());

                // Given the smallest radius and the largest distance to a corner, get repeats
                for (auto i : {0, 1, 2, 3}) { // Each bbox corner
                    // Number of steps to reach that corner from the center
                    auto steps = Geom::distance((*bbox * cm).corner(i), line.origin()) / r_cm + 1;
                    if (steps > d2) {
                        d2 = steps;
                    }
                }
            }
            func = get_repeat_function(*func, *is_reflection, std::floor(d1), std::ceil(d2));
        }

        // Coord radius is scaled to include repeating function
        auto coords = std::to_array({focal[X], focal[Y], fr * d2, center[X], center[Y], r * d2});

        auto shading = capypdf::Type3Shading(color_space, coords.data(), coords.size(), *func);
        shading.set_extend(true, true);
        shading.set_domain(d1, d2);
        auto sid = _gen.add_shading(shading);
        auto pattern = capypdf::ShadingPattern(sid);
        pattern.set_matrix(cm[0], cm[1], cm[2], cm[3], cm[4], cm[5]);
        return _gen.add_shading_pattern(pattern);
    }
    return {};
}

/**
 * Generate a mesh gradient or mesh gradient mask
 *
 * @arg mesh - The Mesh Gradient object to paint into the PDF Document
 * @arg opacity - If set, generate the mesh gradient mask instead of the color gradient
 *
 * @returns A PDF Id of the new gradient object in the PDF document, if created.
 */
std::optional<CapyPDF_PatternId> Document::get_mesh_pattern(SPMeshGradient const *mesh, std::optional<double> opacity)
{
    auto bbox = mesh->getAllItemsBox();
    if (!bbox) {
        g_warning("Mesh gradient has no paintable area: '%s'", get_id(mesh).c_str());
        return {};
    }

    auto color_space = opacity ? CAPY_DEVICE_CS_GRAY : get_default_colorspace();
    auto to_userspace = Geom::Affine(bbox->width(), 0, 0, bbox->height(), bbox->left(), bbox->top());
    auto cm = mesh->gradientTransform;

    SPMeshNodeArray array;
    // TODO: Find out why array read can't be const.
    array.read(const_cast<SPMeshGradient *>(mesh));

    if (array.nodes.size() == 0 || array.nodes[0].size() == 0) {
        g_warning("Mesh gradient has no paintable nodes.");
        return {};
    }

    // The first node is given as the color space of the whole gradient
    auto space = array.nodes[0][0]->color->getSpace();
    if (!opacity) {
        color_space = get_colorspace(space);
    }

    auto box = *bbox * cm.inverse();
    auto shading = capypdf::Type6Shading(color_space, box.left(), box.bottom(), box.right(), box.top());

    for (unsigned i = 0; i < array.patch_rows(); i++) {
        for (unsigned j = 0; j < array.patch_columns(); j++) {
            auto patch = SPMeshPatchI(&array.nodes, i, j);
            std::vector<double> coords;
            std::vector<capypdf::Color> colors;

            for (int k = 0; k < 4; k++) {
                if (patch.tensorIsSet(k)) {
                    g_warning("Can't set tensor for Type7Shading, not supported yet.");
                }

                // We only have 24 slots, not 32, we think the last point is a duplicate or zero
                for (unsigned l : {0, 1, 2}) {
                    auto p = patch.getPoint(k, l);
                    coords.emplace_back(p[X]);
                    coords.emplace_back(p[Y]);
                }
                colors.push_back(get_shading_color(*patch.getColor(k)->converted(space), opacity));
            }

            shading.add_patch(coords.data(), coords.size(), colors.data(), colors.size());
        }
    }

    auto sid = _gen.add_shading(shading);
    auto pattern = capypdf::ShadingPattern(sid);
    pattern.set_matrix(cm[0], cm[1], cm[2], cm[3], cm[4], cm[5]);
    return _gen.add_shading_pattern(pattern);
}

/**
 * Render a pattern out to a transparency group context, as the canvas draws it
 * (SPPattern::update_view): the tile from 0,0 in pattern space, placed at x, y by the pattern
 * transform; the art of `root` fitted to it by a view box, or in the units of the painted object's
 * box. Every value is looked up along the href chain from `pat`, the pattern painted with.
 *
 * The matrix maps pattern space into the space of the content stream the pattern paints in, which
 * is the painted object's own (item_to_transparency_group).
 */
std::optional<CapyPDF_PatternId> Document::get_tiling_pattern(SPPattern const *pat, SPPattern const *root,
                                                              Geom::OptRect const &bbox)
{
    // This means we don't want to inherit changes from calling group.
    auto style_map = _paint_memory.get_ifset(root->style);
    auto style_scope = _paint_memory.remember(style_map);

    double x = pat->x(), y = pat->y(), width = pat->width(), height = pat->height();
    if (bbox && pat->patternUnits() == SPPattern::UNITS_OBJECTBOUNDINGBOX) {
        x *= bbox->width();
        y *= bbox->height();
        width *= bbox->width();
        height *= bbox->height();
    }
    if (!(width > 0) || !(height > 0)) {
        return {};
    }
    auto const tile = Geom::Rect::from_xywh(0, 0, width, height);
    Geom::Affine content;
    if (auto const view = pat->viewbox()) {
        // The chain's view box, fitted by the painted pattern's own preserveAspectRatio.
        SPViewBox fit = *pat;
        fit.viewBox = *view;
        fit.c2p.setIdentity();
        fit.apply_viewbox(tile);
        content = fit.c2p;
    } else if (bbox && pat->patternContentUnits() == SPPattern::UNITS_OBJECTBOUNDINGBOX) {
        content = Geom::Scale(bbox->width(), bbox->height());
    }
    auto pattern = PdfBuilder::PatternContext(*this, tile);
    pattern.set_matrix(Geom::Translate(x, y) * pat->getTransform());
    pattern.set_paint_style(style_map, root->style, nullptr);
    pattern.transform(content);

    for (auto &obj : root->children) {
        if (auto child_item = cast<SPItem>(&obj)) {
            if (auto item_id = item_to_transparency_group(child_item)) {
                pattern.paint_group(*item_id, child_item->style);
            }
        }
    }

    return _gen.add_tiling_pattern(pattern._ctx);
}

/**
 * Generate a non-continous gradient from the gradient vector and add to the document.
 *
 * @returns The FunctionId for the new gradient and sets the pdf_space Color Space if needed.
 */
std::optional<CapyPDF_FunctionId> Document::get_gradient_function(SPGradientVector const &vector,
                                                                  std::optional<double> opacity,
                                                                  CapyPDF_Device_Colorspace *pdf_space)
{
    static auto domain = std::to_array({0.0, 1.0});
    unsigned int stops = vector.stops.size();
    if (stops == 0) {
        return {};
    }
    auto color_space = vector.stops[0].color->getSpace();

    // Update the caller on what the color space should be for this gradient
    if (!opacity) {
        *pdf_space = get_colorspace(color_space);
    }

    // PROOF: the pieces of the gradient, each up to its end and from one colour to another. Two
    // stops at one place (a hard stop) have nothing between them, and PDF's Bounds must increase:
    // Poppler, which PROOF's check of an .ai against its page and its PDF import draw with, draws
    // equal ones wrongly. Inside the gradient such a pair is left out, which changes nothing. At an
    // end it decides the colour the gradient extends with, the first stop before the start and the
    // last after the end as SVG has it, so it becomes a sliver of that colour too narrow to see.
    struct Piece
    {
        double to;
        Colors::Color const *from_color, *to_color;
    };
    std::vector<Piece> pieces;
    for (unsigned i = 0; i + 1 < stops; ++i) {
        auto const &a = vector.stops[i], &b = vector.stops[i + 1];
        if (b.offset > a.offset) {
            pieces.push_back({b.offset, &*a.color, &*b.color});
        }
    }
    if (pieces.empty()) {
        // A single stop gradient is a swatch, this makes a gradient but it might be better as a Spot Color
        auto const &only = *vector.stops.front().color;
        pieces.push_back({1.0, &only, &only});
    } else {
        auto const &front = vector.stops.front(), &back = vector.stops.back();
        double const first_width = pieces.front().to - front.offset;
        double const last_width = back.offset - (pieces.size() > 1 ? pieces[pieces.size() - 2].to : front.offset);
        if (vector.stops[1].offset <= front.offset) {
            double const sliver = std::min(1e-5, first_width / 3);
            pieces.insert(pieces.begin(), {front.offset + sliver, &*front.color, &*front.color});
        }
        if (vector.stops[stops - 2].offset >= back.offset) {
            double const sliver = std::min(1e-5, last_width / 3);
            pieces.back().to = back.offset - sliver;
            pieces.push_back({back.offset, &*back.color, &*back.color});
        }
    }

    // Type3 Function, a collection of Type2 functions between each color stop pair
    std::vector<CapyPDF_FunctionId> functs;
    std::vector<double> bounds;
    std::vector<double> encode;

    for (auto const &piece : pieces) {
        auto c1 = get_shading_color(*piece.from_color->converted(color_space), opacity);
        auto c2 = get_shading_color(*piece.to_color->converted(color_space), opacity);

        auto func = capypdf::Type2Function(domain.data(), domain.size(), c1, c2);
        functs.push_back(_gen.add_function(func));
        encode.emplace_back(0.0);
        encode.emplace_back(1.0);

        if (&piece != &pieces.back()) {
            bounds.emplace_back(piece.to);
        }
    }

    // One function means a single stop pair, one Type2 Function is enough
    if (functs.size() == 1) {
        return functs[0];
    }

    // A type3 Functon allows for more than 2 color stops in one gradient.
    auto func = capypdf::Type3Function(domain.data(), domain.size(), functs.data(), functs.size(), bounds.data(),
                                       bounds.size(), encode.data(), encode.size());
    return _gen.add_function(func);
}

/**
 * Generate a type3 gradient function which repeats the given gradient for the given ranges.
 */
std::optional<CapyPDF_FunctionId> Document::get_repeat_function(CapyPDF_FunctionId gradient, bool reflected, int from,
                                                                int to)
{
    auto domain = std::to_array({(double)from, (double)to});
    std::vector<CapyPDF_FunctionId> functs;
    std::vector<double> bounds;
    std::vector<double> encode;

    for (auto i = from + 1; i <= to; i++) {
        functs.push_back(gradient);
        if (i < to) {
            bounds.push_back(i);
        }
        if (reflected && i % 2 == 0) {
            encode.push_back(1.0);
            encode.push_back(0.0);
        } else {
            encode.push_back(0.0);
            encode.push_back(1.0);
        }
    }
    if (functs.empty()) {
        return gradient;
    }

    auto func = capypdf::Type3Function(domain.data(), domain.size(), functs.data(), functs.size(), bounds.data(),
                                       bounds.size(), encode.data(), encode.size());
    return _gen.add_function(func);
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
