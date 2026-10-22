// SPDX-License-Identifier: GPL-2.0-or-later
#include "drawing-paintserver.h"

#include <utility>

#include "cairo-utils.h"
#include "colors/color.h"
#include "colors/spaces/base.h"
#include "object/sp-paint-server.h"
#include "object/sp-gradient.h"
#include "object/gradient-rendering.h"

namespace Inkscape {

namespace {

/// For Cairo, which takes sRGB: converted once here rather than again for every tile.
void to_srgb(std::optional<Colors::Color> &color)
{
    if (color && color->getSpace()->getType() != Colors::Space::Type::RGB) {
        if (auto rgb = color->converted(Colors::Space::Type::RGB)) {
            color = std::move(rgb);
        }
    }
}

/// PROOF: the stops and patches the canvas draws (a CMYK document's blended in CMYK, a
/// Perceptual gradient's in Oklab: object/gradient-rendering.h), in sRGB. A change of the
/// working colours restyles every object, which builds these again
/// (Colors::DocumentColors::refresh).
SPGradientVector canvas_stops(SPPaintServer *ps)
{
    auto const *gradient = cast<SPGradient>(ps);
    auto vector = gradient ? GradientRendering::canvas_vector(gradient) : *ps->getGradientVector();
    for (auto &stop : vector.stops) {
        to_srgb(stop.color);
    }
    return vector;
}

SPGradientMesh canvas_patches(SPPaintServer *ps)
{
    auto mesh = GradientRendering::canvas_mesh(ps->document, *ps->getGradientMesh());
    for (auto &row : mesh.patches) {
        for (auto &patch : row) {
            for (auto &color : patch.color) {
                to_srgb(color);
            }
        }
    }
    return mesh;
}

} // namespace

std::unique_ptr<Inkscape::DrawingPaintServer> create_drawing_paintserver(SPPaintServer *ps)
{
    switch (ps->getPaintType()) {
        case PaintServerType::SOLID_COLOR:
            return std::make_unique<Inkscape::DrawingSolidColor>(ps->getSolidColor());
            break;
        case PaintServerType::LINEAR_GRADIENT:
        {
            auto vector = canvas_stops(ps);
            return std::make_unique<Inkscape::DrawingLinearGradient>(ps->getSpread(), ps->getUnits(), ps->getGradientTransform(), &vector);
        }
            break;
        case PaintServerType::RADIAL_GRADIENT:
        {
            auto vector = canvas_stops(ps);
            return std::make_unique<Inkscape::DrawingRadialGradient>(ps->getSpread(), ps->getUnits(), ps->getGradientTransform(), &vector);
        }
            break;
        case PaintServerType::MESH_GRADIENT:
        {
            auto mesh = canvas_patches(ps);
            return std::make_unique<Inkscape::DrawingMeshGradient>(ps->getSpread(), ps->getUnits(), ps->getGradientTransform(), &mesh);
        }
            break;
    }
    return {};
}

DrawingPaintServer::~DrawingPaintServer() = default;

DrawingSolidColor::DrawingSolidColor(Colors::Color color)
    : color(std::move(color)) {}

cairo_pattern_t *DrawingSolidColor::create_pattern(cairo_t *, Geom::OptRect const &, double opacity) const
{
    return ink_cairo_pattern_create(color.withOpacity(opacity));
}

void DrawingGradient::common_setup(cairo_pattern_t *pat, Geom::OptRect const &bbox, double opacity) const
{
    // set spread type
    switch (spread) {
        case SP_GRADIENT_SPREAD_REFLECT:
            cairo_pattern_set_extend(pat, CAIRO_EXTEND_REFLECT);
            break;
        case SP_GRADIENT_SPREAD_REPEAT:
            cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
            break;
        case SP_GRADIENT_SPREAD_PAD:
        default:
            cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
            break;
    }

    // set pattern transform matrix
    auto gs2user = transform;
    if (units == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX && bbox) {
        auto bbox2user = Geom::Affine(bbox->width(), 0, 0, bbox->height(), bbox->left(), bbox->top());
        gs2user *= bbox2user;
    }
    ink_cairo_pattern_set_matrix(pat, gs2user.inverse());
}

cairo_pattern_t *DrawingLinearGradient::create_pattern(cairo_t *, Geom::OptRect const &bbox, double opacity) const
{
    auto pat = cairo_pattern_create_linear(x1, y1, x2, y2);

    common_setup(pat, bbox, opacity);

    // add stops
    for (auto &stop : stops) {
        // multiply stop opacity by paint opacity
        if (stop.color.has_value()) {
            ink_cairo_pattern_add_color_stop(pat, stop.offset, stop.color->withOpacity(opacity));
        }
    }

    return pat;
}

cairo_pattern_t *DrawingRadialGradient::create_pattern(cairo_t *ct, Geom::OptRect const &bbox, double opacity) const
{
    Geom::Point focus(fx, fy);
    Geom::Point center(cx, cy);

    double radius = r;
    double focusr = fr;
    double scale = 1.0;
    double tolerance = cairo_get_tolerance(ct);

    Geom::Affine gs2user = transform;

    if (units == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX && bbox) {
        Geom::Affine bbox2user(bbox->width(), 0, 0, bbox->height(), bbox->left(), bbox->top());
        gs2user *= bbox2user;
    }

    // we need to use vectors with the same direction to represent the transformed
    // radius and the focus-center delta, because gs2user might contain non-uniform scaling
    Geom::Point d(focus - center);
    Geom::Point d_user(d.length(), 0);
    Geom::Point r_user(radius, 0);
    Geom::Point fr_user(focusr, 0);
    d_user *= gs2user.withoutTranslation();
    r_user *= gs2user.withoutTranslation();
    fr_user *= gs2user.withoutTranslation();

    double dx = d_user.x(), dy = d_user.y();
    cairo_user_to_device_distance(ct, &dx, &dy);

    // compute the tolerance distance in user space
    // create a vector with the same direction as the transformed d,
    // with the length equal to tolerance
    double dl = hypot(dx, dy);
    double tx = tolerance * dx / dl, ty = tolerance * dy / dl;
    cairo_device_to_user_distance(ct, &tx, &ty);
    double tolerance_user = hypot(tx, ty);

    if (d_user.length() + tolerance_user > r_user.length()) {
        scale = r_user.length() / d_user.length();

        // nudge the focus slightly inside
        scale *= 1.0 - 2.0 * tolerance / dl;
    }

    auto pat = cairo_pattern_create_radial(scale * d.x() + center.x(), scale * d.y() + center.y(), focusr, center.x(), center.y(), radius);

    common_setup(pat, bbox, opacity);

    // add stops
    for (auto &stop : stops) {
        // multiply stop opacity by paint opacity
        if (stop.color.has_value()) {
            ink_cairo_pattern_add_color_stop(pat, stop.offset, *stop.color, opacity);
        }
    }

    return pat;
}

cairo_pattern_t *DrawingMeshGradient::create_pattern(cairo_t *, Geom::OptRect const &bbox, double opacity) const
{
#ifdef MESH_DEBUG
    std::cout << "sp_meshgradient_create_pattern: " << bbox << " " << opacity << std::endl;
#endif

    auto pat = cairo_pattern_create_mesh();

    for (auto &row : patchdata) {
        for (auto &data : row) {
            cairo_mesh_pattern_begin_patch(pat);
            cairo_mesh_pattern_move_to(pat, data.points[0][0].x(), data.points[0][0].y());

            for (int k = 0; k < 4; k++) {
                switch (data.pathtype[k]) {
                case 'l':
                case 'L':
                case 'z':
                case 'Z':
                    cairo_mesh_pattern_line_to(pat, data.points[k][3].x(), data.points[k][3].y());
                    break;
                case 'c':
                case 'C':
                    cairo_mesh_pattern_curve_to(pat, data.points[k][1].x(), data.points[k][1].y(),
                            data.points[k][2].x(), data.points[k][2].y(),
                            data.points[k][3].x(), data.points[k][3].y());
                    break;
                default:
                    // Shouldn't happen
                    std::cerr << "sp_mesh_create_pattern: path error" << std::endl;
                }

                if (data.tensorIsSet[k]) {
                    Geom::Point t = data.tensorpoints[k];
                    cairo_mesh_pattern_set_control_point(pat, k, t.x(), t.y());
                }

                if (data.color[k]) {
                    ink_cairo_mesh_pattern_set_corner_color(pat, k, data.color[k]->withOpacity(opacity));
                } else {
                    std::cerr << "Bad mesh color at pos " << k << "\n";
                    static auto const black = *Colors::Color::parse("black");
                    ink_cairo_mesh_pattern_set_corner_color(pat, k, black.withOpacity(opacity));
                }
            }

            cairo_mesh_pattern_end_patch(pat);
        }
    }

    // set pattern transform matrix
    Geom::Affine gs2user = transform;
    if (units == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX && bbox) {
        Geom::Affine bbox2user(bbox->width(), 0, 0, bbox->height(), bbox->left(), bbox->top());
        gs2user *= bbox2user;
    }
    ink_cairo_pattern_set_matrix(pat, gs2user.inverse());

    return pat;
}

} // namespace Inkscape

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
