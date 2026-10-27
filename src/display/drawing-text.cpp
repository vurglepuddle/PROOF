// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Group belonging to an SVG drawing element.
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2011 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <2geom/pathvector.h>

#include <iostream>
#include <iomanip>

#include "style.h"

#include "cairo-utils.h"
#include "drawing-context.h"
#include "drawing-surface.h"
#include "drawing-text.h"
#include "drawing.h"

#include "helper/geom.h"

#include "libnrtype/font-instance.h"

namespace Inkscape {

DrawingGlyphs::DrawingGlyphs(Drawing &drawing)
    : DrawingItem(drawing)
    , _glyph(0)
{
}

struct SvgGlyphHash
{
    size_t operator()(const std::pair<unsigned int, unsigned int>&v) const
    {
        return ((long)v.first << sizeof(unsigned int)) + v.second;
    }
};

std::shared_ptr<Pixbuf> DrawingGlyphs::_get_svg_glyph(std::shared_ptr<FontInstance> const &font, unsigned int glyph_id) const
{
    // Inline so it can have more than 32 entries, we want to cache 1024 glyphs instead
    static Util::cached_map<std::pair<unsigned int, unsigned int>, Inkscape::Pixbuf, SvgGlyphHash> _svg_glyph_cache(1024);

    std::pair<unsigned int, unsigned int> key(font->get_hash(), glyph_id);

    if (auto res = _svg_glyph_cache.lookup(key)) {
        return res;
    }

    Inkscape::Pixbuf* pixbuf = nullptr;
    auto svg = font->SvgDocument(glyph_id);
    if (!svg.empty()) {
        pixbuf = Pixbuf::create_from_buffer(svg.raw());
        if (!pixbuf) {
            std::cerr << "Bad svg data for glyph " << glyph_id << "\n";
        }
    }
    if (!pixbuf) {
        // Either no glyph for Unicode point or glyph had bad SVG data.
        pixbuf = new Pixbuf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1));
    }

    // Ensure exists in cairo format before locking it down. (Rendering code requires cairo format.)
    pixbuf->ensurePixelFormat(Pixbuf::PF_CAIRO);

    _svg_glyph_cache.add(key, std::unique_ptr<Pixbuf>(pixbuf));

    return _svg_glyph_cache.lookup(key);
}

void DrawingGlyphs::setGlyph(std::shared_ptr<FontInstance> font, unsigned int glyph, Geom::Affine const &trans)
{
    defer([=, this, font = std::move(font)] {
        _markForRendering();

        assert(!_drawing.snapshotted());
        setTransform(trans);

        _font_data = font->share_data();
        _glyph = glyph;

        design_units = 1.0;
        pathvec = nullptr;
        pixbuf = nullptr;

        // Load pathvectors and pixbufs in advance, as must be done on main thread.
        if (font) {
            design_units = font->GetDesignUnits();
            pathvec      = font->PathVector(_glyph);
            bbox_exact   = font->BBoxExact(_glyph);
            bbox_pick    = font->BBoxPick( _glyph);
            bbox_draw    = font->BBoxDraw( _glyph);
            if (font->FontHasSVG()) {
                // CACHE the pixbuf here
                pixbuf = _get_svg_glyph(font, _glyph).get();
            }
            font_descr   = pango_font_description_to_string(font->get_descr());
            // std::cout << "DrawingGlyphs::setGlyph: " << std::setw(6) << glyph
            //           << "  design_units: " << design_units
            //           << "  bbox_exact: " << bbox_exact
            //           << "  bbox_pick: " << bbox_pick
            //           << "  bbox_draw: " << bbox_draw
            //           << std::endl;
        }

        _markForUpdate(STATE_ALL, false);
    });
}

void DrawingGlyphs::setStyle(SPStyle const *, SPStyle const *)
{
    std::cerr << "DrawingGlyphs: Use parent style" << std::endl;
}

unsigned DrawingGlyphs::_updateItem(Geom::IntRect const &/*area*/, UpdateContext const &ctx, unsigned /*flags*/, unsigned /*reset*/)
{
    auto ggroup = cast<DrawingText>(&std::as_const(*_parent));
    if (!ggroup) {
        throw InvalidItemException();
    }

    if (!pathvec) {
        // Bitmap font
        return STATE_ALL;
    }

    Geom::Rect bbox_pick_scaled_d  = bbox_pick  * ctx.ctm;
    Geom::Rect bbox_draw_scaled_d  = bbox_draw  * ctx.ctm;

    // Expand to make it easier to pick text when zoom out.
    bbox_pick_scaled_d.expandBy(1.0); // One pixel

    if (ggroup->_nrstyle.data.stroke.type != NRStyleData::PaintType::NONE) {
        // this expands the selection box for cases where the stroke is "thick"
        float scale = ctx.ctm.descrim();
        if (_transform) {
            scale /= _transform->descrim(); // FIXME temporary hack
        }
        if (ggroup->_stroke) {
            scale = ggroup->_stroke->transform.descrim() * ggroup->_stroke->width_expansion;
        }
        float width = std::max<double>(0.125, ggroup->_nrstyle.data.stroke_width * scale);
        if (std::fabs(ggroup->_nrstyle.data.stroke_width * scale) > 0.01) { // FIXME: this is always true
            bbox_pick_scaled_d.expandBy(0.5 * width);
            bbox_draw_scaled_d.expandBy(0.5 * width);
        }

        float miterMax = width * ggroup->_nrstyle.data.miter_limit;
        if (miterMax > 0.01) {
            // grunt mode. we should compute the various miters instead
            // (one for each point on the curve)
            bbox_draw_scaled_d.expandBy(miterMax);
        }
    }

    bbox_pick_scaled  = bbox_pick_scaled_d.roundOutwards();   // Used for picking
    bbox_draw_scaled  = bbox_draw_scaled_d.roundOutwards();   // Used for drawing

    // drawing-item variable
    _bbox = bbox_draw_scaled;

    // std::cout << "DrawingGlyphs::_updateItem: "
    //           << " glyph: " << std::setw(6) << _glyph
    //           << " bbox_pick_scaled: "  << bbox_pick_scaled
    //           << " bbox_draw_scaled: "  << bbox_draw_scaled
    //           << std::endl;

    return STATE_ALL;
}

DrawingItem *DrawingGlyphs::_pickItem(Geom::Point const &p, double /*delta*/, Geom::OptIntRect const &area_world, unsigned flags)
{
    auto ggroup = cast<DrawingText>(_parent);
    if (!ggroup) {
        throw InvalidItemException();
    }
    DrawingItem *result = nullptr;
    bool invisible = ggroup->_nrstyle.data.fill.type == NRStyleData::PaintType::NONE &&
                     ggroup->_nrstyle.data.stroke.type == NRStyleData::PaintType::NONE;
    bool outline = flags & PICK_OUTLINE;

    if (outline || !invisible) {
        // With text we take a simple approach: pick if the point is in a character pick bbox
        Geom::Rect temp(bbox_pick_scaled); // Convert from Geom::RectInt
        if (temp.contains(p)) {
            result = this;
        }
    }
    return result;
}

DrawingText::DrawingText(Drawing &drawing)
    : DrawingGroup(drawing)
    , style_vector_effect_stroke(false)
    , style_stroke_extensions_hairline(false)
    , style_clip_rule(SP_WIND_RULE_EVENODD)
{
}

bool DrawingText::addComponent(std::shared_ptr<FontInstance> const &font, unsigned int glyph, Geom::Affine const &trans, float width, float ascent, float descent, float phase_length)
{
    if (glyph == 0x0fffffff) {
        // 0x0fffffff is returned by Pango for a zero-width empty glyph which we can ignore (e.g. 0xFE0F, Emoji variant selector).
        return false;
    }

    if (!font) {
        std::cerr << "DrawingTExt::addComponent: no font!" << std::endl;
        return false;
    }

    defer([=, this, font = std::move(font)] () mutable {
        _markForRendering();
        auto ng = new DrawingGlyphs(_drawing);
        assert(!_drawing.snapshotted());
        ng->setGlyph(font, glyph, trans);
        ng->_width  = width;   // used especially when _drawable = false, otherwise, it is the advance of the font
        ng->_asc    = ascent;  // Of line, not of this one character. In pixels.
        ng->_dsc    = descent; // Of line, not of this one character. In pixels.
        ng->_pl     = phase_length; // used for phase of dots, dashes, and wavy
        appendChild(ng);
    });

    return true;
}

void DrawingText::setStyle(SPStyle const *style, SPStyle const *context_style)
{
    DrawingGroup::setStyle(style, context_style);

    auto vector_effect_stroke = false;
    auto stroke_extensions_hairline = false;
    auto clip_rule = SP_WIND_RULE_EVENODD;
    if (_style) {
        vector_effect_stroke = _style->vector_effect.stroke;
        stroke_extensions_hairline = _style->stroke_extensions.hairline;
        clip_rule = _style->clip_rule.computed;
    }

    defer([=, this, nrstyle = NRStyleData(_style, _context_style)] () mutable {
        _nrstyle.set(std::move(nrstyle));
        style_vector_effect_stroke = vector_effect_stroke;
        style_stroke_extensions_hairline = stroke_extensions_hairline;
        style_clip_rule = clip_rule;
    });
}

void DrawingText::setChildrenStyle(SPStyle const *context_style)
{
    DrawingGroup::setChildrenStyle(context_style);

    defer([this, nrstyle = NRStyleData(_style, _context_style)] () mutable {
        _nrstyle.set(std::move(nrstyle));
    });
}

unsigned DrawingText::_updateItem(Geom::IntRect const &area, UpdateContext const &ctx, unsigned flags, unsigned reset)
{
    _nrstyle.invalidate();
    return DrawingGroup::_updateItem(area, ctx, flags, reset);
}

void DrawingText::decorateStyle(DrawingContext &dc, double vextent, double xphase, Geom::Point const &p1, Geom::Point const &p2, double thickness) const
{
    double wave[16]={
        // clang-format off
        0.000000,  0.382499,  0.706825,  0.923651,   1.000000,  0.923651,  0.706825,  0.382499,
        0.000000, -0.382499, -0.706825, -0.923651,  -1.000000, -0.923651, -0.706825, -0.382499,
        // clang-format on
    };
    int dashes[16]={
        // clang-format off
        8,   7,   6,   5,
        4,   3,   2,   1,
        -8, -7,  -6,  -5,
        -4, -3,  -2,  -1
        // clang-format on
    };
    int dots[16]={
        // clang-format off
        4,     3,   2,   1,
        -4,   -3,  -2,  -1,
        4,     3,   2,   1,
        -4,   -3,  -2,  -1
        // clang-format on
    };
    double   step = vextent/32.0;
    unsigned i  = 15 & (unsigned) round(xphase/step);  // xphase is >= 0.0

    /* For most spans draw the last little bit right to p2 or even a little beyond.
       This allows decoration continuity within the line, and does not step outside the clip box off the end
       For the first/last section on the line though, stay well clear of the edge, or when the
       text is dragged it may "spray" pixels.
    */
    /* snap to nearest step in X */
    Geom::Point ps = Geom::Point(step * round(p1[Geom::X]/step),p1[Geom::Y]);
    Geom::Point pf = Geom::Point(step * round(p2[Geom::X]/step),p2[Geom::Y]);
    Geom::Point poff = Geom::Point(0,thickness/2.0);

    if (_nrstyle.data.text_decoration_style & NRStyleData::TEXT_DECORATION_STYLE_ISDOUBLE) {
        ps -= Geom::Point(0, vextent/12.0);
        pf -= Geom::Point(0, vextent/12.0);
        dc.rectangle( Geom::Rect(ps + poff, pf - poff));
        ps += Geom::Point(0, vextent/6.0);
        pf += Geom::Point(0, vextent/6.0);
        dc.rectangle( Geom::Rect(ps + poff, pf - poff));
    }
    /* The next three have a problem in that they are phase dependent.  The bits of a line are not
    necessarily passing through this routine in order, so we have to use the xphase information
    to figure where in each of their cycles to start.  Only accurate to 1 part in 16.
    Huge positive offset should keep the phase calculation from ever being negative.
    */
    else if(_nrstyle.data.text_decoration_style & NRStyleData::TEXT_DECORATION_STYLE_DOTTED){
        // FIXME: Per spec, this should produce round dots.
        Geom::Point pv = ps;
        while(true){
            Geom::Point pvlast = pv;
            if(dots[i]>0){
                if(pv[Geom::X] > pf[Geom::X]) break;

                pv += Geom::Point(step * (double)dots[i], 0.0);

                if(pv[Geom::X]>= pf[Geom::X]){
                    // Last dot
                    dc.rectangle( Geom::Rect(pvlast + poff, pf - poff));
                    break;
                } else {
                    dc.rectangle( Geom::Rect(pvlast + poff, pv - poff));
                }

                pv += Geom::Point(step * 4.0, 0.0);

            } else {
                pv += Geom::Point(step * -(double)dots[i], 0.0);
            }
            i = 0;  // once in phase, it stays in phase
        }
    }
    else if (_nrstyle.data.text_decoration_style & NRStyleData::TEXT_DECORATION_STYLE_DASHED) {
        Geom::Point pv = ps;
        while(true){
            Geom::Point pvlast = pv;
            if(dashes[i]>0){
                if(pv[Geom::X]> pf[Geom::X]) break;

                pv += Geom::Point(step * (double)dashes[i], 0.0);

                if(pv[Geom::X]>= pf[Geom::X]){
                    // Last dash
                    dc.rectangle( Geom::Rect(pvlast + poff, pf - poff));
                    break;
                } else {
                    dc.rectangle( Geom::Rect(pvlast + poff, pv - poff));
                }

                pv += Geom::Point(step * 8.0, 0.0);

            } else {
                pv += Geom::Point(step * -(double)dashes[i], 0.0);
            }
            i = 0;  // once in phase, it stays in phase
        }
    }
    else if (_nrstyle.data.text_decoration_style & NRStyleData::TEXT_DECORATION_STYLE_WAVY) {
        double   amp  = vextent/10.0;
        double   x    = ps[Geom::X];
        double   y    = ps[Geom::Y] + poff[Geom::Y];
        dc.moveTo(Geom::Point(x, y + amp * wave[i]));
        while(true){
           i = ((i + 1) & 15);
           x += step;
           dc.lineTo(Geom::Point(x, y + amp * wave[i]));
           if(x >= pf[Geom::X])break;
        }
        y = ps[Geom::Y] - poff[Geom::Y];
        dc.lineTo(Geom::Point(x, y + amp * wave[i]));
        while(true){
           i = ((i - 1) & 15);
           x -= step;
           dc.lineTo(Geom::Point(x, y + amp * wave[i]));
           if(x <= ps[Geom::X])break;
        }
        dc.closePath();
    }
    else { // TEXT_DECORATION_STYLE_SOLID, also default in case it was not set for some reason
        dc.rectangle( Geom::Rect(ps + poff, pf - poff));
    }
}

/* returns scaled line thickness */
void DrawingText::decorateItem(DrawingContext &dc, double phase_length, bool under) const
{
    if ( _nrstyle.data.font_size <= 1.0e-32 )return;  // might cause a divide by zero or overflow and nothing would be visible anyway
    double tsp_width_adj                = _nrstyle.data.tspan_width                     / _nrstyle.data.font_size;
    double tsp_asc_adj                  = _nrstyle.data.ascender                        / _nrstyle.data.font_size;
    double tsp_size_adj                 = (_nrstyle.data.ascender + _nrstyle.data.descender) / _nrstyle.data.font_size;

    double final_underline_thickness    = CLAMP(_nrstyle.data.underline_thickness,    tsp_size_adj/30.0, tsp_size_adj/10.0);
    double final_line_through_thickness = CLAMP(_nrstyle.data.line_through_thickness, tsp_size_adj/30.0, tsp_size_adj/10.0);

    double xphase = phase_length/ _nrstyle.data.font_size; // used to figure out phase of patterns

    Geom::Point p1;
    Geom::Point p2;
    // All lines must be the same thickness, in combinations, line_through trumps underline
    double thickness = final_underline_thickness;
    if ( thickness <= 1.0e-32 )return;  // might cause a divide by zero or overflow and nothing would be visible anyway
    dc.setTolerance(0.5); // Is this really necessary... could effect dots.

    if( under ) {

        if(_nrstyle.data.text_decoration_line & NRStyleData::TEXT_DECORATION_LINE_UNDERLINE){
            p1 = Geom::Point(0.0,          -_nrstyle.data.underline_position);
            p2 = Geom::Point(tsp_width_adj,-_nrstyle.data.underline_position);
            decorateStyle(dc, tsp_size_adj, xphase, p1, p2, thickness);
        }

        if(_nrstyle.data.text_decoration_line & NRStyleData::TEXT_DECORATION_LINE_OVERLINE){
            p1 = Geom::Point(0.0,          tsp_asc_adj -_nrstyle.data.underline_position + 1 * final_underline_thickness);
            p2 = Geom::Point(tsp_width_adj,tsp_asc_adj -_nrstyle.data.underline_position + 1 * final_underline_thickness);
            decorateStyle(dc, tsp_size_adj, xphase,  p1, p2, thickness);
        }

    } else {
        // Over

        if(_nrstyle.data.text_decoration_line & NRStyleData::TEXT_DECORATION_LINE_LINETHROUGH){
            thickness = final_line_through_thickness;
            p1 = Geom::Point(0.0,          _nrstyle.data.line_through_position);
            p2 = Geom::Point(tsp_width_adj,_nrstyle.data.line_through_position);
            decorateStyle(dc, tsp_size_adj, xphase,  p1, p2, thickness);
        }

        // Obviously this does not blink, but it does indicate which text has been set with that attribute
        if(_nrstyle.data.text_decoration_line & NRStyleData::TEXT_DECORATION_LINE_BLINK){
            thickness = final_line_through_thickness;
            p1 = Geom::Point(0.0,          _nrstyle.data.line_through_position - 2*final_line_through_thickness);
            p2 = Geom::Point(tsp_width_adj,_nrstyle.data.line_through_position - 2*final_line_through_thickness);
            decorateStyle(dc, tsp_size_adj, xphase,  p1, p2, thickness);
            p1 = Geom::Point(0.0,          _nrstyle.data.line_through_position + 2*final_line_through_thickness);
            p2 = Geom::Point(tsp_width_adj,_nrstyle.data.line_through_position + 2*final_line_through_thickness);
            decorateStyle(dc, tsp_size_adj, xphase,  p1, p2, thickness);
        }
    }
}

unsigned DrawingText::_renderItem(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, unsigned flags, DrawingItem const *stop_at) const
{
    cairo_matrix_t matrix;
    cairo_get_matrix(dc.raw(), &matrix);
    auto visible = area & _bbox;
    if (!visible) {
        return RENDER_OK;
    }

    bool outline = flags & RENDER_OUTLINE;

    if (outline) {
        Inkscape::DrawingContext::Save save(dc);
        dc.setSource(rc.outline_color);
        dc.setTolerance(0.5); // low quality, but good enough for outline mode

        for (auto & i : _children) {
            auto g = cast<DrawingGlyphs>(&i);
            if (!g) throw InvalidItemException();

            Inkscape::DrawingContext::Save save(dc);
            // skip glyphs with singular transforms
            if (g->_ctm.isSingular()) continue;
            dc.transform(g->_ctm);
            if (g->pathvec){
                dc.path(*g->pathvec);
                dc.fill();
            }
            // TODO If pathvec empty, draw box.
        }
        return RENDER_OK;
    }

    auto stroke_decoration = [&](CairoPatternUniqPtr const &paint) {
        DrawingContext::Save save(dc);
        _nrstyle.applyTextDecorationStroke(dc, paint);
        if (_stroke) {
            if (!_nrstyle.data.hairline) {
                dc.setLineWidth(_nrstyle.data.text_decoration_stroke_width * _stroke->decoration_expansion);
            }
            cairo_set_matrix(dc.raw(), &matrix);
            dc.transform(_stroke->transform);
        }
        dc.strokePreserve();
    };

    // NOTE: This is very similar to drawing-shape.cpp; the only differences are in path feeding
    // and in applying text decorations.

    // Do we have text decorations?
    bool decorate = (_nrstyle.data.text_decoration_line != NRStyleData::TEXT_DECORATION_LINE_CLEAR );

    // prepareFill / prepareStroke need to be called with _ctm in effect.
    // However, we might need to apply a different ctm for glyphs.
    // Therefore, only apply this ctm temporarily.
    CairoPatternUniqPtr has_stroke;
    CairoPatternUniqPtr has_fill;
    CairoPatternUniqPtr has_td_fill;
    CairoPatternUniqPtr has_td_stroke;

    {
        Inkscape::DrawingContext::Save save(dc);
        dc.transform(_ctm);

        has_fill   = _nrstyle.prepareFill  (dc, rc, *visible, _item_bbox, _fill_pattern);
        has_stroke = _nrstyle.prepareStroke(dc, rc, *visible, _item_bbox, _stroke_pattern);

        // Avoid creating patterns if not needed
        if (decorate) {
            has_td_fill   = _nrstyle.prepareTextDecorationFill  (dc, rc, *visible, _item_bbox, _fill_pattern);
            has_td_stroke = _nrstyle.prepareTextDecorationStroke(dc, rc, *visible, _item_bbox, _stroke_pattern);
        }
    }

    if (has_fill || has_stroke || has_td_fill || has_td_stroke) {

        // Determine order for fill and stroke.
        // Text doesn't have markers, we can do paint-order quick and dirty.
        bool fill_first = false;
        if( _nrstyle.data.paint_order_layer[0] == NRStyleData::PAINT_ORDER_NORMAL ||
            _nrstyle.data.paint_order_layer[0] == NRStyleData::PAINT_ORDER_FILL   ||
            _nrstyle.data.paint_order_layer[2] == NRStyleData::PAINT_ORDER_STROKE ) {
            fill_first = true;
        } // Won't get "stroke fill stroke" but that isn't 'valid'


        // Determine geometry of text decoration
        double phase_length = 0.0;
        Geom::Affine aff;
        if (decorate) {

            Geom::Affine rotinv;
            bool   invset    = false;
            double leftmost  = DBL_MAX;
            bool   first_y   = true;
            double start_y   = 0.0;
            for (auto & i : _children) {

                auto g = cast<DrawingGlyphs>(&i);
                if (!g) throw InvalidItemException();

                if (!invset) {
                    rotinv = g->_ctm.withoutTranslation().inverse();
                    invset = true;
                }

                Geom::Point pt = g->_ctm.translation() * rotinv;
                if (pt[Geom::X] < leftmost) {
                    leftmost     = pt[Geom::X];
                    aff          = g->_ctm;
                    phase_length = g->_pl;
                }

                // Check for text on a path. FIXME: This needs better test (and probably not here).
                if (first_y) {
                    first_y = false;
                    start_y = pt[Geom::Y];
                }
                else if (std::fabs(pt[Geom::Y] - start_y) > 1.0e-6) {
                    //  If the text has been mapped onto a path, which causes y to vary, drop the
                    //  text decorations.  To handle that properly would need a conformal map.
                    decorate = false;
                }
            }
        }

        // Draw text decorations that go UNDER the text (underline, over-line)
        if (decorate) {

            {
                Inkscape::DrawingContext::Save save(dc);
                dc.transform(aff);  // must be leftmost affine in span
                decorateItem(dc, phase_length, true);
            }

            {
                Inkscape::DrawingContext::Save save(dc);
                dc.transform(_ctm);  // Needed so that fill pattern rotates with text

                if (has_td_fill && fill_first) {
                    _nrstyle.applyTextDecorationFill(dc, has_td_fill);
                    dc.fillPreserve();
                }

                if (has_td_stroke) {
                    stroke_decoration(has_td_stroke);
                }

                if (has_td_fill && !fill_first) {
                    _nrstyle.applyTextDecorationFill(dc, has_td_fill);
                    dc.fillPreserve();
                }

            }

            dc.newPath(); // Clear text-decoration path
        }

        // Accumulate the path that represents the glyphs and/or draw SVG glyphs.
        for (auto &i : _children) {
            auto g = cast<DrawingGlyphs>(&i);
            if (!g) throw InvalidItemException();

            Inkscape::DrawingContext::Save save(dc);
            if (g->_ctm.isSingular()) {
                std::cerr << "DrawingText::_renderItem: glyph matrix is singular!" << std::endl;
                continue;
            }
            dc.transform(g->_ctm);

#if 0
            // Draw various boxes for debugging
            auto path_copy = cairo_copy_path(dc.raw()); // Cairo save/restore doesn't apply to path!
            {
                Inkscape::DrawingContext::Save save(dc);
                dc.newPath();
                dc.rectangle(g->bbox_exact);
                dc.setLineWidth(0.02);
                dc.setSource(0.0, 0.0, 1.0, 1.0); // Blue
                dc.stroke();
            }
            {
                Inkscape::DrawingContext::Save save(dc);
                dc.newPath();
                dc.rectangle(g->bbox_pick);
                dc.setLineWidth(0.02);
                dc.setSource(1.0, 0.0, 0.0, 1.0); // Red
                dc.stroke();
            }
            {
                Inkscape::DrawingContext::Save save(dc);
                dc.newPath();
                dc.rectangle(g->bbox_draw);
                dc.setLineWidth(0.02);
                dc.setSource(0.0, 1.0, 0.0, 1.0); // Green
                dc.stroke();
            }
            cairo_append_path(dc.raw(), path_copy);
            cairo_path_destroy(path_copy);
            // End debug boxes.
#endif

            if (g->pathvec) {

                // Draw various boxes for debugging
                // auto path_copy = cairo_copy_path(dc.raw()); // Cairo save/restore doesn't apply to path!
                // {
                //     Geom::OptRect box = bounds_exact(*g->pathvec);
                //     if (box) {
                //         Inkscape::DrawingContext::Save save(dc);
                //         dc.newPath();
                //         dc.rectangle(*box);
                //         dc.setLineWidth(0.02);
                //         dc.setSource(0xff000080);
                //         dc.stroke();
                //     }
                // }
                // cairo_append_path(dc.raw(), path_copy); // Restore path.
                // cairo_path_destroy(path_copy);
                // End debug boxes.

                if (g->pixbuf) {
                    {
                        // pixbuf is in font design units, scale to embox.
                        double scale = g->design_units;
                        if (scale <= 0) scale = 1000;
                        Inkscape::DrawingContext::Save save(dc);
                        dc.translate(g->bbox_draw.corner(3));
                        dc.scale(1.0 / scale, -1.0 / scale);
                        dc.setSource(g->pixbuf->getSurfaceRaw(), 0, 0);
                        dc.paint(1);
                    }
                } else {
                    dc.path(*g->pathvec);
                }
            }
        }

        // Draw the glyphs (non-SVG glyphs).
        {
            Inkscape::DrawingContext::Save save(dc);
            dc.transform(_ctm);
            if (has_fill && fill_first) {
                _nrstyle.applyFill(dc, has_fill);
                dc.fillPreserve();
            }
        }
        {
            Inkscape::DrawingContext::Save save(dc);
            cairo_matrix_t matrix;
            cairo_get_matrix(dc.raw(), &matrix);
            if (!style_vector_effect_stroke) {
                dc.transform(_ctm);
            }
            if (has_stroke) {
                _nrstyle.applyStroke(dc, has_stroke, _stroke ? _stroke->width_expansion : 1.0,
                                    _stroke ? _stroke->dash_expansion : 1.0, _stroke ? _stroke->offset_expansion : 1.0);
                if (_stroke && !style_vector_effect_stroke) {
                    cairo_set_matrix(dc.raw(), &matrix);
                    dc.transform(_stroke->transform);
                }

                // If the stroke is a hairline, set it to exactly 1px on screen.
                // If visible hairline mode is on, make sure the line is at least 1px.
                if (flags & RENDER_VISIBLE_HAIRLINES || style_stroke_extensions_hairline) {
                    double dx = 1.0, dy = 0.0;
                    dc.device_to_user_distance(dx, dy);
                    auto pixel_size = std::hypot(dx, dy);
                    if (style_stroke_extensions_hairline || _nrstyle.data.stroke_width < pixel_size) {
                       dc.setHairline();
                    }
                }

                dc.strokePreserve();
            }
        }
        {
            Inkscape::DrawingContext::Save save(dc);
            dc.transform(_ctm);
            if (has_fill && !fill_first) {
                _nrstyle.applyFill(dc, has_fill);
                dc.fillPreserve();
            }
        }
        dc.newPath(); // Clear glyphs path

        // Draw text decorations that go OVER the text (line through, blink)
        if (decorate) {

            {
                Inkscape::DrawingContext::Save save(dc);
                dc.transform(aff);  // must be leftmost affine in span
                decorateItem(dc, phase_length, false);
            }

            {
                Inkscape::DrawingContext::Save save(dc);
                dc.transform(_ctm);  // Needed so that fill pattern rotates with text

                if (has_td_fill && fill_first) {
                    _nrstyle.applyTextDecorationFill(dc, has_td_fill);
                    dc.fillPreserve();
                }

                if (has_td_stroke) {
                    stroke_decoration(has_td_stroke);
                }

                if (has_td_fill && !fill_first) {
                    _nrstyle.applyTextDecorationFill(dc, has_td_fill);
                    dc.fillPreserve();
                }

            }

            dc.newPath(); // Clear text-decoration path
        }

    }
    return RENDER_OK;
}

void DrawingText::_clipItem(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &/*area*/) const
{
    Inkscape::DrawingContext::Save save(dc);

    if (style_clip_rule == SP_WIND_RULE_EVENODD) {
        dc.setFillRule(CAIRO_FILL_RULE_EVEN_ODD);
    } else {
        dc.setFillRule(CAIRO_FILL_RULE_WINDING);
    }

    for (auto & i : _children) {
        auto g = cast<DrawingGlyphs>(&i);
        if (!g) {
            throw InvalidItemException();
        }

        Inkscape::DrawingContext::Save save(dc);
        dc.transform(g->_ctm);
        if (g->pathvec){
            dc.path(*g->pathvec);
        }
    }
    dc.fill();
}

DrawingItem *DrawingText::_pickItem(Geom::Point const &p, double delta, Geom::OptIntRect const &area_world, unsigned flags)
{
    return DrawingGroup::_pickItem(p, delta, area_world, flags) ? this : nullptr;
}

} // end namespace Inkscape

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
