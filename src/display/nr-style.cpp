// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Style information for rendering.
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2010 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "display/nr-style.h"
#include "style.h"

#include "colors/manager.h"
#include "display/cairo-utils.h"
#include "display/drawing-context.h"
#include "display/drawing-pattern.h"
#include "display/drawing-surface.h"
#include "display/stroke-transform.h"

#include "object/sp-paint-server.h"

namespace Inkscape {

StrokeTransformStyle stroke_style(SPStyle const &style)
{
    return {
        .width_set = style.stroke_width.set && !style.stroke_width.inherit,
        .width_inherit = style.stroke_width.inherit,
        .dash_set = style.stroke_dasharray.set && !style.stroke_dasharray.inherit,
        .offset_set = style.stroke_dashoffset.set && !style.stroke_dashoffset.inherit,
        .has_dashes = !style.stroke_dasharray.values.empty(),
        .own_decoration = !style.text_decoration.style_td || style.text_decoration.style_td == &style,
        .hairline = style.stroke_extensions.hairline
    };
}

void NRStyleData::Paint::clear()
{
    server.reset();
    type = PaintType::NONE;
}

void NRStyleData::Paint::set(Colors::Color const &c)
{
    clear();
    type = PaintType::COLOR;
    color = c;
}

void NRStyleData::Paint::set(SPPaintServer *ps)
{
    clear();
    if (ps) {
        type = PaintType::SERVER;
        server = create_drawing_paintserver(ps);
    }
}

void NRStyleData::Paint::set(SPIPaint const *paint)
{
    if (paint->isPaintserver()) {
        SPPaintServer* server = paint->href->getObject();
        if (server && server->isValid()) {
            set(server);
        } else if (paint->isColor()) {
            set(paint->getColor());
        } else {
            clear();
        }
    } else if (paint->isColor()) {
        set(paint->getColor());
    } else if (paint->isNone()) {
        clear();
    } else if (paint->paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL ||
               paint->paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
        // A marker in the defs section will result in ending up here.
        // std::cerr << "NRStyleData::Paint::set: Double" << std::endl;
    } else {
        g_assert_not_reached();
    }
}

NRStyleData::NRStyleData()
    : fill()
    , stroke()
    , stroke_width(0.0)
    , hairline(false)
    , miter_limit(0.0)
    , dash_offset(0.0)
    , fill_rule(CAIRO_FILL_RULE_EVEN_ODD)
    , line_cap(CAIRO_LINE_CAP_BUTT)
    , line_join(CAIRO_LINE_JOIN_MITER)
    , text_decoration_line(TEXT_DECORATION_LINE_CLEAR)
    , text_decoration_style(TEXT_DECORATION_STYLE_CLEAR)
    , text_decoration_fill()
    , text_decoration_stroke()
    , text_decoration_stroke_width(0.0)
    , phase_length(0.0)
    , tspan_line_start(false)
    , tspan_line_end(false)
    , tspan_width(0)
    , ascender(0)
    , descender(0)
    , underline_thickness(0)
    , underline_position(0)
    , line_through_thickness(0)
    , line_through_position(0)
    , font_size(0)
{
    paint_order_layer[0] = PAINT_ORDER_NORMAL;
}

bool NRStyleData::Paint::ditherable() const
{
    return type == PaintType::SERVER && server && server->ditherable();
}

NRStyleData::NRStyleData(SPStyle const *style, SPStyle const *context_style)
{
    // Handle 'context-fill' and 'context-stroke': Work in progress
    const SPIPaint *style_fill = &style->fill;
    if (style_fill->paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL) {
        if (context_style) {
            style_fill = &context_style->fill;
        } else {
            // A marker in the defs section will result in ending up here.
            //std::cerr << "NRStyleData::set: 'context-fill': 'context_style' is NULL" << std::endl;
        }
    } else if (style_fill->paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
        if (context_style) {
            style_fill = &context_style->stroke;
        } else {
            //std::cerr << "NRStyleData::set: 'context-stroke': 'context_style' is NULL" << std::endl;
        }
    }

    fill.set(style_fill);
    fill.opacity = style->fill_opacity.as_double();

    switch (style->fill_rule.as_enum()) {
        case SP_WIND_RULE_EVENODD:
            fill_rule = CAIRO_FILL_RULE_EVEN_ODD;
            break;
        case SP_WIND_RULE_NONZERO:
            fill_rule = CAIRO_FILL_RULE_WINDING;
            break;
        default:
            g_assert_not_reached();
    }

    const SPIPaint *style_stroke = &style->stroke;
    if (style_stroke->paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL) {
        if (context_style) {
            style_stroke = &context_style->fill;
        } else {
            //std::cerr << "NRStyleData::set: 'context-fill': 'context_style' is NULL" << std::endl;
        }
    } else if (style_stroke->paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
        if (context_style) {
            style_stroke = &context_style->stroke;
        } else {
            //std::cerr << "NRStyleData::set: 'context-stroke': 'context_style' is NULL" << std::endl;
        }
    }

    stroke.set(style_stroke);
    stroke.opacity = style->stroke_opacity.as_double();
    stroke_width = style->stroke_width.as_double();
    hairline = style->stroke_extensions.hairline;
    switch (style->stroke_linecap.as_enum()) {
        case SP_STROKE_LINECAP_ROUND:
            line_cap = CAIRO_LINE_CAP_ROUND;
            break;
        case SP_STROKE_LINECAP_SQUARE:
            line_cap = CAIRO_LINE_CAP_SQUARE;
            break;
        case SP_STROKE_LINECAP_BUTT:
            line_cap = CAIRO_LINE_CAP_BUTT;
            break;
        default:
            g_assert_not_reached();
    }
    switch (style->stroke_linejoin.as_enum()) {
        case SP_STROKE_LINEJOIN_ROUND:
            line_join = CAIRO_LINE_JOIN_ROUND;
            break;
        case SP_STROKE_LINEJOIN_BEVEL:
            line_join = CAIRO_LINE_JOIN_BEVEL;
            break;
        case SP_STROKE_LINEJOIN_MITER:
            line_join = CAIRO_LINE_JOIN_MITER;
            break;
        default:
            g_assert_not_reached();
    }
    miter_limit = style->stroke_miterlimit.as_double();

    int const n_dash = style->stroke_dasharray.values.size();
    if (n_dash > 0 && style->stroke_dasharray.is_valid()) {
        dash_offset = style->stroke_dashoffset.as_double();
        dash.resize(n_dash);
        for (int i = 0; i < n_dash; ++i) {
            dash[i] = style->stroke_dasharray.values[i].as_double();
        }
    }

    for (int i = 0; i < PAINT_ORDER_LAYERS; ++i) {
        switch (style->paint_order.layer[i]) {
            case SP_CSS_PAINT_ORDER_NORMAL:
                paint_order_layer[i]=PAINT_ORDER_NORMAL;
                break;
            case SP_CSS_PAINT_ORDER_FILL:
                paint_order_layer[i]=PAINT_ORDER_FILL;
                break;
            case SP_CSS_PAINT_ORDER_STROKE:
                paint_order_layer[i]=PAINT_ORDER_STROKE;
                break;
            case SP_CSS_PAINT_ORDER_MARKER:
                paint_order_layer[i]=PAINT_ORDER_MARKER;
                break;
        }
    }

    text_decoration_line = TEXT_DECORATION_LINE_CLEAR;
    if (style->text_decoration_line.inherit     ) { text_decoration_line |= TEXT_DECORATION_LINE_INHERIT;                                }
    if (style->text_decoration_line.underline   ) { text_decoration_line |= TEXT_DECORATION_LINE_UNDERLINE   + TEXT_DECORATION_LINE_SET; }
    if (style->text_decoration_line.overline    ) { text_decoration_line |= TEXT_DECORATION_LINE_OVERLINE    + TEXT_DECORATION_LINE_SET; }
    if (style->text_decoration_line.line_through) { text_decoration_line |= TEXT_DECORATION_LINE_LINETHROUGH + TEXT_DECORATION_LINE_SET; }
    if (style->text_decoration_line.blink       ) { text_decoration_line |= TEXT_DECORATION_LINE_BLINK       + TEXT_DECORATION_LINE_SET; }

    text_decoration_style = TEXT_DECORATION_STYLE_CLEAR;
    if (style->text_decoration_style.inherit ) { text_decoration_style |= TEXT_DECORATION_STYLE_INHERIT;                              }
    if (style->text_decoration_style.solid   ) { text_decoration_style |= TEXT_DECORATION_STYLE_SOLID    + TEXT_DECORATION_STYLE_SET; }
    if (style->text_decoration_style.isdouble) { text_decoration_style |= TEXT_DECORATION_STYLE_ISDOUBLE + TEXT_DECORATION_STYLE_SET; }
    if (style->text_decoration_style.dotted  ) { text_decoration_style |= TEXT_DECORATION_STYLE_DOTTED   + TEXT_DECORATION_STYLE_SET; }
    if (style->text_decoration_style.dashed  ) { text_decoration_style |= TEXT_DECORATION_STYLE_DASHED   + TEXT_DECORATION_STYLE_SET; }
    if (style->text_decoration_style.wavy    ) { text_decoration_style |= TEXT_DECORATION_STYLE_WAVY     + TEXT_DECORATION_STYLE_SET; }

    /* FIXME
       The meaning of text-decoration-color in CSS3 for SVG is ambiguous (2014-05-06).  Set
       it for fill, for stroke, for both?  Both would seem like the obvious choice but what happens
       is that for text which is just fill (very common) it makes the lines fatter because it
       enables stroke on the decorations when it wasn't present on the text.  That contradicts the
       usual behavior where the text and decorations by default have the same fill/stroke.

       The behavior here is that if color is defined it is applied to text_decoration_fill/stroke
       ONLY if the corresponding fill/stroke is also present.

       Hopefully the standard will be clarified to resolve this issue.
    */

    // Unless explicitly set on an element, text decoration is inherited from
    // closest ancestor where 'text-decoration' was set. That is, setting
    // 'text-decoration' on an ancestor fixes the fill and stroke of the
    // decoration to the fill and stroke values of that ancestor.
    auto style_td = style;
    if (style->text_decoration.style_td) style_td = style->text_decoration.style_td;
    text_decoration_stroke.opacity = style_td->stroke_opacity.as_double();
    text_decoration_stroke_width = style_td->stroke_width.as_double();

    // Priority is given in order:
    //   * text_decoration_fill
    //   * text_decoration_color (only if fill set)
    //   * fill
    if (style_td->text_decoration_fill.set) {
        text_decoration_fill.set(&(style_td->text_decoration_fill));
    } else if (style_td->text_decoration_color.set) {
        if(style->fill.isPaintserver() || style->fill.isColor()) {
            // SVG sets color specifically
            text_decoration_fill.set(style->text_decoration_color.getColor());
        } else {
            // No decoration fill because no text fill
            text_decoration_fill.clear();
        }
    } else {
        // Pick color/pattern from text
        text_decoration_fill.set(&style_td->fill);
    }

    if (style_td->text_decoration_stroke.set) {
        text_decoration_stroke.set(&style_td->text_decoration_stroke);
    } else if (style_td->text_decoration_color.set) {
        if(style->stroke.isPaintserver() || style->stroke.isColor()) {
            // SVG sets color specifically
            text_decoration_stroke.set(style->text_decoration_color.getColor());
        } else {
            // No decoration stroke because no text stroke
            text_decoration_stroke.clear();
        }
    } else {
        // Pick color/pattern from text
        text_decoration_stroke.set(&style_td->stroke);
    }

    if (text_decoration_line != TEXT_DECORATION_LINE_CLEAR) {
        phase_length           = style->text_decoration_data.phase_length;
        tspan_line_start       = style->text_decoration_data.tspan_line_start;
        tspan_line_end         = style->text_decoration_data.tspan_line_end;
        tspan_width            = style->text_decoration_data.tspan_width;
        ascender               = style->text_decoration_data.ascender;
        descender              = style->text_decoration_data.descender;
        underline_thickness    = style->text_decoration_data.underline_thickness;
        underline_position     = style->text_decoration_data.underline_position;
        line_through_thickness = style->text_decoration_data.line_through_thickness;
        line_through_position  = style->text_decoration_data.line_through_position;
        font_size              = style->font_size;
    }

    text_direction = style->direction.as_enum();
}

auto NRStyle::preparePaint(Inkscape::DrawingContext &dc, Inkscape::RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, Inkscape::DrawingPattern const *pattern, NRStyleData::Paint const &paint, CachedPattern const &cp) const -> CairoPatternUniqPtr
{
    if (paint.type == NRStyleData::PaintType::SERVER && pattern) {
        // If a DrawingPattern, then always regenerate the pattern, because it may depend on 'area'.
        // Even if not, regenerating the pattern is a no-op because DrawingPattern has a cache.
        return CairoPatternUniqPtr(pattern->renderPattern(rc, area, paint.opacity, dc.surface()->device_scale()));
    }

    // Otherwise, init or re-use cached pattern.
    cp.inited.init([&] {
        // Handle remaining non-DrawingPattern cases.
        switch (paint.type) {
            case NRStyleData::PaintType::SERVER:
                if (paint.server) {
                    cp.pattern = CairoPatternUniqPtr(paint.server->create_pattern(dc.raw(), paintbox, paint.opacity));
                    ink_cairo_pattern_set_dither(cp.pattern.get(), rc.dithering && paint.server->ditherable());
                } else {
                    std::cerr << "Null pattern detected" << std::endl;
                    cp.pattern = CairoPatternUniqPtr(cairo_pattern_create_rgba(0, 0, 0, 0));
                }
                break;
            case NRStyleData::PaintType::COLOR: {
                cp.pattern = CairoPatternUniqPtr(ink_cairo_pattern_create(paint.color->withOpacity(paint.opacity)));
                break;
            }
            default:
                cp.pattern.reset();
                break;
        }
    });

    return copy(cp.pattern);
}

void NRStyle::set(NRStyleData &&data_)
{
    data = std::move(data_);
    invalidate();
}

auto NRStyle::prepareFill(Inkscape::DrawingContext &dc, Inkscape::RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, Inkscape::DrawingPattern const *pattern) const -> CairoPatternUniqPtr
{
    return preparePaint(dc, rc, area, paintbox, pattern, data.fill, fill_pattern);
}

auto NRStyle::prepareStroke(Inkscape::DrawingContext &dc, Inkscape::RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, Inkscape::DrawingPattern const *pattern) const -> CairoPatternUniqPtr
{
    return preparePaint(dc, rc, area, paintbox, pattern, data.stroke, stroke_pattern);
}

auto NRStyle::prepareTextDecorationFill(Inkscape::DrawingContext &dc, Inkscape::RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, Inkscape::DrawingPattern const *pattern) const -> CairoPatternUniqPtr
{
    return preparePaint(dc, rc, area, paintbox, pattern, data.text_decoration_fill, text_decoration_fill_pattern);
}

auto NRStyle::prepareTextDecorationStroke(Inkscape::DrawingContext &dc, Inkscape::RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, Inkscape::DrawingPattern const *pattern) const -> CairoPatternUniqPtr
{
    return preparePaint(dc, rc, area, paintbox, pattern, data.text_decoration_stroke, text_decoration_stroke_pattern);
}

void NRStyle::applyFill(Inkscape::DrawingContext &dc, CairoPatternUniqPtr const &cp) const
{
    dc.setSource(cp.get());
    dc.setFillRule(data.fill_rule);
}

void NRStyle::applyTextDecorationFill(Inkscape::DrawingContext &dc, CairoPatternUniqPtr const &cp) const
{
    dc.setSource(cp.get());
    // Fill rule does not matter, no intersections.
}

void NRStyle::applyStroke(Inkscape::DrawingContext &dc, CairoPatternUniqPtr const &cp, double expansion,
                          double dash_expansion, double offset_expansion) const
{
    dc.setSource(cp.get());
    if (data.hairline) {
        dc.setHairline();
    } else {
        dc.setLineWidth(data.stroke_width * expansion);
    }
    dc.setLineCap(data.line_cap);
    dc.setLineJoin(data.line_join);
    dc.setMiterLimit(data.miter_limit);
    if (dash_expansion == 1.0 || data.dash.empty()) {
        dc.setDash(data.dash, data.dash_offset * offset_expansion);
    } else {
        auto dash = data.dash;
        for (auto &value : dash) value *= dash_expansion;
        dc.setDash(dash, data.dash_offset * offset_expansion);
    }
}

void NRStyle::applyTextDecorationStroke(Inkscape::DrawingContext &dc, CairoPatternUniqPtr const &cp) const
{
    dc.setSource(cp.get());
    if (data.hairline) {
        dc.setHairline();
    } else {
        dc.setLineWidth(data.text_decoration_stroke_width);
    }
    dc.setLineCap(CAIRO_LINE_CAP_BUTT);
    dc.setLineJoin(CAIRO_LINE_JOIN_MITER);
    dc.setMiterLimit(data.miter_limit);
    dc.setDash({}, 0.0);
}

void NRStyle::invalidate()
{
    // force pattern update
    fill_pattern.reset();
    stroke_pattern.reset();
    text_decoration_fill_pattern.reset();
    text_decoration_stroke_pattern.reset();
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
