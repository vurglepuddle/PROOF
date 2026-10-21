// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Provide a capypdf interface that understands 2geom, styles, etc.
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "build-drawing.h"

#include <cmath>

#include "colors/cms/profile.h"
#include "colors/color.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/cmyk.h"
#include "object/sp-gradient.h"
#include "libnrtype/font-instance.h"
#include "object/sp-paint-server.h"
#include "spot-ink.h"
#include "style.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

namespace {

/// The bundled C++ Color wrapper omits Separation, while the C API supports it.
class RegistrationColor : public capypdf::Color
{
public:
    RegistrationColor(CapyPDF_SeparationId ink, double tint)
    {
        auto const error = capy_color_set_separation(*this, ink, tint);
        if (error) throw capypdf::PdfException(capy_error_message(error));
    }
};

} // namespace

CapyPDF_SeparationId DocumentGenerator::add_registration_ink(CapyPDF_FunctionId tint_transform)
{
    CapyPDF_SeparationId separation;
    auto const error = capy_generator_add_separation(
        *this, "All", 3, CAPY_DEVICE_CS_CMYK, tint_transform, &separation);
    if (error) throw capypdf::PdfException(capy_error_message(error));
    return separation;
}

/**
 * Get the blend mode for capyPDF output.
 */
CapyPDF_Blend_Mode get_blendmode(SPBlendMode mode)
{
    switch (mode) {
        case SP_CSS_BLEND_MULTIPLY:
            return CAPY_BM_MULTIPLY;
        case SP_CSS_BLEND_SCREEN:
            return CAPY_BM_SCREEN;
        case SP_CSS_BLEND_DARKEN:
            return CAPY_BM_DARKEN;
        case SP_CSS_BLEND_LIGHTEN:
            return CAPY_BM_LIGHTEN;
        case SP_CSS_BLEND_OVERLAY:
            return CAPY_BM_OVERLAY;
        case SP_CSS_BLEND_COLORDODGE:
            return CAPY_BM_COLORDODGE;
        case SP_CSS_BLEND_COLORBURN:
            return CAPY_BM_COLORBURN;
        case SP_CSS_BLEND_HARDLIGHT:
            return CAPY_BM_HARDLIGHT;
        case SP_CSS_BLEND_SOFTLIGHT:
            return CAPY_BM_SOFTLIGHT;
        case SP_CSS_BLEND_DIFFERENCE:
            return CAPY_BM_DIFFERENCE;
        case SP_CSS_BLEND_EXCLUSION:
            return CAPY_BM_EXCLUSION;
        case SP_CSS_BLEND_HUE:
            return CAPY_BM_HUE;
        case SP_CSS_BLEND_SATURATION:
            return CAPY_BM_SATURATION;
        case SP_CSS_BLEND_COLOR:
            return CAPY_BM_COLOR;
        case SP_CSS_BLEND_LUMINOSITY:
            return CAPY_BM_LUMINOSITY;
        default:
            break;
    }
    return CAPY_BM_NORMAL;
}

CapyPDF_Line_Cap get_linecap(SPStrokeCapType mode)
{
    switch (mode) {
        case SP_STROKE_LINECAP_SQUARE:
            return CAPY_LC_PROJECTION;
        case SP_STROKE_LINECAP_ROUND:
            return CAPY_LC_ROUND;
        case SP_STROKE_LINECAP_BUTT:
        default:
            break;
    }
    return CAPY_LC_BUTT;
}

CapyPDF_Line_Join get_linejoin(SPStrokeJoinType mode)
{
    switch (mode) {
        case SP_STROKE_LINEJOIN_ROUND:
            return CAPY_LJ_ROUND;
        case SP_STROKE_LINEJOIN_BEVEL:
            return CAPY_LJ_BEVEL;
        case SP_STROKE_LINEJOIN_MITER:
        default:
            break;
    }
    return CAPY_LJ_MITER;
}

/**
 * Returns true if the gradient has transparency.
 */
bool style_has_gradient_transparency(SPStyle const *style)
{
    if (style->fill.set && style->fill.href) {
        if (gradient_has_transparency(style->fill.href->getObject())) {
            return true;
        }
    }
    if (style->stroke.set && style->stroke.href) {
        if (gradient_has_transparency(style->stroke.href->getObject())) {
            return true;
        }
    }
    return false;
}

/**
 * Get a PDF specific layer painting pattern for fill, stroke and markers.
 */
std::vector<PaintLayer> get_paint_layers(SPStyle const *style, SPStyle const *context_style)
{
    std::vector<PaintLayer> output;

    // If context paint is used outside of a marker or clone, we do not output them if not context_style is provided.
    auto context_paint_is_none = [context_style](SPIPaint const &paint) {
        return (paint.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL && (!context_style || context_style->fill.isNone())) ||
               (paint.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE && (!context_style || context_style->stroke.isNone()));
    };
    bool no_fill = style->fill.isNone() || style->fill_opacity.value < 1e-9 || context_paint_is_none(style->fill);
    bool no_stroke = style->stroke.isNone() ||
                     (!style->stroke_extensions.hairline && style->stroke_width.computed < 1e-9) ||
                     style->stroke_opacity.value == 0 ||
                     context_paint_is_none(style->stroke);

    if (no_fill && no_stroke) {
        return output;
    }

    auto layers = style->paint_order.get_layers();

    for (auto i = 0; i < 3; i++) {
        auto layer = layers[i];
        /* PDF's FillStroke paint operator is Atomic, not two operations like it is in SVG:

        https://github.com/pdf-association/pdf-differences/tree/main/Atomic-Fill%2BStroke

        auto next = i < 2 ? layers[i + 1] : SP_CSS_PAINT_ORDER_NORMAL;
        if (layer == SP_CSS_PAINT_ORDER_FILL && next == SP_CSS_PAINT_ORDER_STROKE && !no_fill && !no_stroke) {
            output.push_back(PAINT_FILLSTROKE);
            i++; // Stroke is already done, skip it.
        } else */
        if (layer == SP_CSS_PAINT_ORDER_FILL && !no_fill) {
            output.push_back(PAINT_FILL);
        } else if (layer == SP_CSS_PAINT_ORDER_STROKE && !no_stroke) {
            output.push_back(PAINT_STROKE);
        } else if (layer == SP_CSS_PAINT_ORDER_MARKER) {
            output.push_back(PAINT_MARKERS);
        }
    }
    return output;
}

/**
 * Return true if this shape's style requires a PDF transparency group.
 */
bool style_needs_group(SPStyle const *style)
{
    // These things are in the graphics-state, plus gradients and pattern use.
    return style->opacity.as_double() < 1.0 || get_blendmode(style->mix_blend_mode.value) ||
           (style->fill.set && style->fill.href && style->fill.href->getObject()) ||
           (style->stroke.set && style->stroke.href && style->stroke.href->getObject());
}

/**
 * Turn a paint into a string for use in caching keys.
 */
std::string paint_to_cache_key(SPIPaint const &paint, std::optional<double> opacity)
{
    // We don't use SPIPaint::get_value because we need a value from the inherited style.
    if (paint.isNone()) {
        return "none";
    }
    if (opacity) {
        return std::to_string(*opacity);
    } else if (paint.isColor()) {
        return paint.getColor().toString();
    }
    if (paint.isPaintserver()) {
        return paint.href->getObject()->getId();
    }
    return "";
}

/**
 * Find out if any of the item, or its decendents use context-fill and context-stroke
 */
void get_context_use_recursive(SPItem const *item, bool &fill, bool &stroke)
{
    // Both styles must be checked for both values; four total
    fill |= item->style->fill.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL;
    fill |= item->style->stroke.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL;
    stroke |= item->style->fill.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE;
    stroke |= item->style->stroke.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE;
    if (fill && stroke) {
        return;
    }
    for (auto &obj : item->children) {
        if (auto child_item = cast<SPItem>(&obj)) {
            get_context_use_recursive(child_item, fill, stroke);
            if (fill && stroke) {
                return;
            }
        }
    }
}

/**
 * Set the style for any graphic from the SVG style
 *
 * @param style - The SPStyle for this SPObject
 *
 * @returns A GraphicsStateId for the object added to the document, or none if none is needed.
 */
std::optional<CapyPDF_GraphicsStateId>
Document::get_group_graphics_state(SPStyle const *style, std::optional<CapyPDF_TransparencyGroupId> soft_mask)
{
    if (!style) {
        return {};
    }

    auto gstate = capypdf::GraphicsState();
    bool gs_used = false;

    if (soft_mask) {
        auto smask = capypdf::SoftMask(CAPY_SOFT_MASK_LUMINOSITY, *soft_mask);
        gstate.set_SMask(_gen.add_soft_mask(smask));
        gs_used = true;
    }
    if (style->mix_blend_mode.set) {
        gstate.set_BM(get_blendmode(style->mix_blend_mode.value));
        gs_used = true;
    }
    if (style->opacity.as_double() < 1.0) {
        gstate.set_ca(style->opacity.as_double());
        gs_used = true;
    }
    if (gs_used) {
        return _gen.add_graphics_state(gstate);
    }

    return {};
}

/**
 * Like get_graphics_style but for drawing shapes (paths)
 *
 * @param style - The style from the SPObject
 *
 * @returns the GraphicsStateId for the object added to the document, or none if not needed.
 */
std::optional<CapyPDF_GraphicsStateId> Document::get_shape_graphics_state(SPStyle const *style)
{
    // PDF allows a lot more to exist in the graphics state, but capypdf does not allow them
    // to be added into the gs and instead they get added directly to the draw context obj.
    auto gstate = capypdf::GraphicsState();
    bool gs_used = false;

    if (auto soft_mask = style_to_transparency_mask(style, nullptr)) {
        auto smask = capypdf::SoftMask(CAPY_SOFT_MASK_LUMINOSITY, *soft_mask);
        gstate.set_SMask(_gen.add_soft_mask(smask));
        gs_used = true;
    } else { // The draw opacities can not be set at the same time as a soft mask
        if (style->fill_opacity.as_double() < 1.0) {
            gstate.set_ca(style->fill_opacity.as_double());
            gs_used = true;
        }
        if (style->stroke_opacity.as_double() < 1.0) {
            gstate.set_CA(style->stroke_opacity.as_double());
            gs_used = true;
        }
    }
    if (gs_used) {
        return _gen.add_graphics_state(gstate);
    }
    return {};
}

/**
 * Load a font and cache the results.
 *
 * @param filename - The filename to the font to use.
 *
 * @returns the FontId in capypdf to use.
 */
std::optional<std::pair<std::string, CapyPDF_FontId>> Document::get_font(std::shared_ptr<FontInstance> font, SPIFontVariationSettings &var)
{
    auto filename = font->GetFilename();
    auto key = filename;
    if (!var.axes.empty()) {
        key += "-" + var.toString();
    }

    // TODO: It's possible for the font loading to fail but we don't know how yet.
    if (!_font_cache.contains(key)) {
        try {
            auto fontprops = capypdf::FontProperties();
            auto axes = font->get_opentype_varaxes();
            for (auto &axis : axes) {
                auto iter = var.axes.find(axis.tag);
                fontprops.set_variation(axis.tag, iter == var.axes.end()
                        ? (int)axis.def
                        : iter->second);
            }
            _font_cache[key] = _gen.load_font(filename.c_str(), fontprops);
        } catch (std::exception const &err) {
            std::cerr << "Can't load font: '" << filename.c_str() << "'\n";
            return {};
        }
    }
    return {{key, _font_cache[key]}};
}

/**
 * Generate a solid color, gradient or pattern based on the SPIPaint
 */
std::optional<capypdf::Color> Document::get_paint(SPIPaint const &paint, SPStyle const *context_style,
                                                  std::optional<double> opacity, Geom::OptRect const &bbox)
{
    if (context_style) {
        if (paint.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL) {
            // A marker's bounds do not describe its context paint's source.
            // Preserve that existing placement until the source item is passed.
            return get_paint(context_style->fill, nullptr, opacity);
        } else if (paint.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
            return get_paint(context_style->stroke, nullptr, opacity);
        }
    }

    if (paint.isNone()) {
        return {};
    }

    if (paint.isColor()) {
        return get_color(paint.getColor(), opacity);
    }

    capypdf::Color out;
    if (paint.isPaintserver()) {
        auto swatch = cast<SPGradient>(paint.href ? paint.href->getObject() : nullptr);
        if (swatch && swatch->isSolid()) {
            // PROOF: the imported Registration swatch is the reserved ink All. Its
            // canonical CMYK definition prints on every plate, rather than becoming
            // four process inks or an ordinary spot. A mask still needs its preview
            // luminosity; do not introduce an ink into that rendering pass.
            auto ink = SpotInk::read(swatch->getRepr());
            if (!ink && swatch->getVector()) ink = SpotInk::read(swatch->getVector()->getRepr());
            if (!opacity && ink && ink->name == "All") {
                auto const &alternate = ink->alternate;
                auto const space = alternate.getSpace();
                bool const canonical = space && space->getComponentType() == Colors::Space::Type::CMYK &&
                    alternate.getOpacity() == 1.0 && alternate[0] == 1.0 && alternate[1] == 1.0 &&
                    alternate[2] == 1.0 && alternate[3] == 1.0 && std::isfinite(ink->tint) &&
                    ink->tint >= 0.0 && ink->tint <= 1.0;
                if (canonical) {
                    if (!_registration_ink) {
                        // CapyPDF accepts only calculator functions for Separation
                        // tint transforms. This maps t to the CMYK alternate t,t,t,t.
                        double domain[]{0, 1};
                        double range[]{0, 1, 0, 1, 0, 1, 0, 1};
                        capypdf::Type4Function tint_transform(domain, 2, range, 8, "{ dup dup dup }", -1);
                        auto const function = _gen.add_function(tint_transform);
                        _registration_ink = _gen.add_registration_ink(function);
                    }
                    return capypdf::Color{RegistrationColor(*_registration_ink, ink->tint)};
                }
            }
            return get_color(swatch->getPreviewAverageColor(), opacity);
        } else if (auto pattern_id = get_pattern(paint.href ? paint.href->getObject() : nullptr, opacity, bbox)) {
            out.set_pattern(*pattern_id);
        } else {
            g_warning("Couldn't generate pattern for fill '%s'", paint.get_value().c_str());
        }
    } else if (!context_style && (paint.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL ||
                                  paint.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE)) {
        g_warning("Context style requested but no context style available.");
        out.set_rgb(0, 0, 0);
    } else {
        g_warning("Fill style not supported: '%s'", paint.get_value().c_str());
        out.set_rgb(0, 0, 0); // Black default on error
    }
    return out;
}

capypdf::Color Document::get_color(Colors::Color const &color, std::optional<double> opacity)
{
    auto space = color.getSpace();

    capypdf::Color out;
    if (opacity) {
        out.set_gray(*opacity * color.getOpacity());
    } else if (auto cmyk = std::dynamic_pointer_cast<Colors::Space::DeviceCMYK>(space)) {
        out.set_cmyk(color[0], color[1], color[2], color[3]);
    } else if (auto cms = std::dynamic_pointer_cast<Colors::Space::CMS>(space)) {
        if (auto icc_id = get_icc_profile(cms)) {
            auto vals = color.getValues();
            out.set_icc(*icc_id, vals.data(), vals.size());
        } else {
            g_warning("Couldn't set icc color, icc profile didn't load.");
        }
    } else if (auto rgb = color.converted(Colors::Space::Type::RGB)) {
        out.set_rgb(rgb->get(0), rgb->get(1), rgb->get(2));
    } else {
        g_warning("Problem outputting color '%s' to PDF.", color.toString().c_str());
        out.set_rgb(0, 0, 0); // Black default on error
    }
    return out;
}

capypdf::Color Document::get_shading_color(Colors::Color const &color, std::optional<double> opacity)
{
    // CapyPDF's shading functions accept device components, not ICC color objects.
    // Keep the profile's RGB/CMYK channels in the device space selected below.
    if (!opacity) {
        if (auto cms = std::dynamic_pointer_cast<Colors::Space::CMS>(color.getSpace())) {
            capypdf::Color out;
            if (cms->getComponentType() == Colors::Space::Type::CMYK) {
                out.set_cmyk(color[0], color[1], color[2], color[3]);
                return out;
            }
            if (cms->getComponentType() == Colors::Space::Type::RGB) {
                out.set_rgb(color[0], color[1], color[2]);
                return out;
            }
            // Other profile models use the same RGB fallback as get_colorspace().
            return get_color(*color.converted(Colors::Space::Type::RGB), opacity);
        }
    }
    return get_color(color, opacity);
}

std::optional<CapyPDF_IccColorSpaceId> Document::get_icc_profile(std::shared_ptr<Colors::Space::CMS> const &profile)
{
    auto key = profile->getName();
    if (auto it = _icc_cache.find(key); it != _icc_cache.end()) {
        return it->second;
    }

    if (auto cms_profile = profile->getProfile()) {
        auto channels = profile->getComponentCount();
        auto vec = cms_profile->dumpData();
        CapyPDF_IccColorSpaceId id =
            _gen.add_icc_profile(reinterpret_cast<char const *>(vec.data()), vec.size(), channels);
        _icc_cache[key] = id;
        return id;
    }
    return {};
}

CapyPDF_Device_Colorspace Document::get_default_colorspace() const
{
    // TODO: Make this return the correct color space (icc, etc) for the document
    return CAPY_DEVICE_CS_RGB;
}

CapyPDF_Device_Colorspace Document::get_colorspace(std::shared_ptr<Colors::Space::AnySpace> const &space) const
{
    if (*space == Colors::Space::Type::CMYK) {
        return CAPY_DEVICE_CS_CMYK;
    } else if (*space == Colors::Space::Type::RGB) {
        return CAPY_DEVICE_CS_RGB;
    } else if (auto cms = std::dynamic_pointer_cast<Colors::Space::CMS>(space)) {
        // TODO: Support icc profiles here, which are missing from capypdf atm
        g_warning("ICC profile color space expressed as device color space!");
        switch (cms->getComponentType()) {
            case Colors::Space::Type::RGB:
                return CAPY_DEVICE_CS_RGB;
            case Colors::Space::Type::CMYK:
                return CAPY_DEVICE_CS_CMYK;
            default:
                break;
        }
        // Return IccColorSpaceId here, somehow.
    }
    return CAPY_DEVICE_CS_RGB;
}

// Because soft masks negate the use of draw opacities, we must fold them in.
std::optional<double> DrawContext::get_softmask(double opacity) const
{
    if (_soft_mask) {
        return opacity;
    }
    return {};
}

/**
 * Set the style for drawing shapes from the SVG style, this is all the styles
 * that relate to how vector paths are drawn with stroke, fill and other shape
 * properties. But NOT item styles such as opacity, blending mode etc.
 *
 * @arg map - The style map indicating changes in the PDF remndering stack
 * @arg style - The style to apply to the stream
 */
void DrawContext::set_paint_style(StyleMap const &map, SPStyle const *style, SPStyle const *context_style,
                                  Geom::OptRect const &bbox)
{
    // NOTE: We might find out that fill_opacity.set is important for style cascading
    // An inherited paint server still needs this shape's own placement.
    if (map.contains(SPAttr::FILL) || (bbox && style->fill.isPaintserver())) {
        if (auto color = _doc.get_paint(style->fill, context_style, get_softmask(style->fill_opacity.as_double()), bbox)) {
            _ctx.set_nonstroke(*color);
        }
    }
    if (map.contains(SPAttr::STROKE) || (bbox && style->stroke.isPaintserver())) {
        if (auto color = _doc.get_paint(style->stroke, context_style, get_softmask(style->stroke_opacity.as_double()), bbox)) {
            _ctx.set_stroke(*color);
        }
    }
    if (map.contains(SPAttr::STROKE_WIDTH)) {
        // TODO: if (style->stroke_extensions.hairline) {
        // ink_cairo_set_hairline(_cr);
        _ctx.cmd_w(style->stroke_width.computed);
    }
    if (map.contains(SPAttr::STROKE_MITERLIMIT)) {
        _ctx.cmd_M(style->stroke_miterlimit.value);
    }
    if (map.contains(SPAttr::STROKE_LINECAP)) {
        _ctx.cmd_J(get_linecap(style->stroke_linecap.computed));
    }
    if (map.contains(SPAttr::STROKE_LINEJOIN)) {
        _ctx.cmd_j(get_linejoin(style->stroke_linejoin.computed));
    }
    if (map.contains(SPAttr::STROKE_DASHARRAY)) {
        auto values = style->stroke_dasharray.get_computed();
        if (!values.empty()) {
            _ctx.cmd_d(values.data(), values.size(), style->stroke_dashoffset.computed);
        }
    }
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
