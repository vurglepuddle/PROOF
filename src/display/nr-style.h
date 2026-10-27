// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Style information for rendering.
 * Only used by classes DrawingShape and DrawingText
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2010 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_DISPLAY_NR_STYLE_H
#define INKSCAPE_DISPLAY_NR_STYLE_H

#include <memory>
#include <array>
#include <cairo.h>
#include <2geom/rect.h>
#include "drawing-paintserver.h"
#include "initlock.h"

class SPPaintServer;
class SPStyle;
class SPIPaint;

namespace Inkscape {
class DrawingContext;
class DrawingPattern;
class RenderContext;

struct CairoPatternFreer { void operator()(cairo_pattern_t *p) const { cairo_pattern_destroy(p); } };
using CairoPatternUniqPtr = std::unique_ptr<cairo_pattern_t, CairoPatternFreer>;
inline CairoPatternUniqPtr copy(CairoPatternUniqPtr const &p)
{
    if (!p) return {};
    cairo_pattern_reference(p.get());
    return CairoPatternUniqPtr(p.get());
}

struct NRStyleData
{
    NRStyleData();
    explicit NRStyleData(SPStyle const *style, SPStyle const *context_style = nullptr);

    enum class PaintType
    {
        NONE,
        COLOR,
        SERVER
    };

    struct Paint
    {
        PaintType type = PaintType::NONE;
        std::optional<Colors::Color> color;
        std::unique_ptr<DrawingPaintServer> server;
        float opacity = 1.0;

        void clear();
        void set(Colors::Color const &c);
        void set(SPPaintServer *ps);
        void set(SPIPaint const *paint);
        bool ditherable() const;
    };

    Paint fill;
    Paint stroke;
    float stroke_width;
    bool hairline;
    double miter_limit;
    std::vector<double> dash;
    float dash_offset;
    cairo_fill_rule_t fill_rule;
    cairo_line_cap_t line_cap;
    cairo_line_join_t line_join;

    enum PaintOrderType
    {
        PAINT_ORDER_NORMAL,
        PAINT_ORDER_FILL,
        PAINT_ORDER_STROKE,
        PAINT_ORDER_MARKER
    };

    std::array<PaintOrderType, 3> paint_order_layer;

    enum TextDecorationLine
    {
        TEXT_DECORATION_LINE_CLEAR       = 0x00,
        TEXT_DECORATION_LINE_SET         = 0x01,
        TEXT_DECORATION_LINE_INHERIT     = 0x02,
        TEXT_DECORATION_LINE_UNDERLINE   = 0x04,
        TEXT_DECORATION_LINE_OVERLINE    = 0x08,
        TEXT_DECORATION_LINE_LINETHROUGH = 0x10,
        TEXT_DECORATION_LINE_BLINK       = 0x20
    };

    enum TextDecorationStyle
    {
        TEXT_DECORATION_STYLE_CLEAR      = 0x00,
        TEXT_DECORATION_STYLE_SET        = 0x01,
        TEXT_DECORATION_STYLE_INHERIT    = 0x02,
        TEXT_DECORATION_STYLE_SOLID      = 0x04,
        TEXT_DECORATION_STYLE_ISDOUBLE   = 0x08,
        TEXT_DECORATION_STYLE_DOTTED     = 0x10,
        TEXT_DECORATION_STYLE_DASHED     = 0x20,
        TEXT_DECORATION_STYLE_WAVY       = 0x40
    };

    int   text_decoration_line;
    int   text_decoration_style;
    Paint text_decoration_fill;
    Paint text_decoration_stroke;
    float text_decoration_stroke_width;

    // These are the same as in style.h
    float phase_length;
    bool  tspan_line_start;
    bool  tspan_line_end;
    float tspan_width;
    float ascender;
    float descender;
    float underline_thickness;
    float underline_position; 
    float line_through_thickness;
    float line_through_position;
    float font_size;

    int   text_direction;
};

class NRStyle
{
public:
    void set(NRStyleData &&data);
    CairoPatternUniqPtr prepareFill(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, DrawingPattern const *pattern) const;
    CairoPatternUniqPtr prepareStroke(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, DrawingPattern const *pattern) const;
    CairoPatternUniqPtr prepareTextDecorationFill(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, DrawingPattern const *pattern) const;
    CairoPatternUniqPtr prepareTextDecorationStroke(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, DrawingPattern const *pattern) const;
    void applyFill(DrawingContext &dc, CairoPatternUniqPtr const &cp) const;
    void applyStroke(DrawingContext &dc, CairoPatternUniqPtr const &cp, double expansion = 1.0,
                     double dash_expansion = 1.0, double offset_expansion = 1.0) const;
    void applyTextDecorationFill(DrawingContext &dc, CairoPatternUniqPtr const &cp) const;
    void applyTextDecorationStroke(DrawingContext &dc, CairoPatternUniqPtr const &cp) const;
    void invalidate();

   NRStyleData data;

private:
    struct CachedPattern
    {
        InitLock inited;
        mutable CairoPatternUniqPtr pattern;
        void reset() { inited.reset(); pattern.reset(); }
    };

    CairoPatternUniqPtr preparePaint(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, Geom::OptRect const &paintbox, DrawingPattern const *pattern, NRStyleData::Paint const &paint, CachedPattern const &cp) const;

    CachedPattern fill_pattern;
    CachedPattern stroke_pattern;
    CachedPattern text_decoration_fill_pattern;
    CachedPattern text_decoration_stroke_pattern;
};

} // namespace Inkscape

#endif // INKSCAPE_DISPLAY_NR_STYLE_H

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
