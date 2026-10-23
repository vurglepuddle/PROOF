// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Inkscape::Text::Layout - text layout engine output functions
 *
 * Authors:
 *   Richard Hughes <cyreve@users.sf.net>
 *
 * Copyright (C) 2005 Richard Hughes
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "Layout-TNG.h"
#include <limits>
#include "style-attachments.h"
#include "display/drawing-text.h"
#include "print.h"
#include "extension/print.h"
#include "livarot/Path.h"
#include "font-instance.h"
#include "extension/internal/cairo-render-context.h"
#include "path/path-curve.h"
#include "libnrtype/font-factory.h"
#include "util-string/symbol-convert.h"
#include "style-text.h"

using Inkscape::Extension::Internal::CairoRenderContext;
using Inkscape::Extension::Internal::CairoGlyphInfo;

namespace Inkscape {
namespace Text {

/*
 dx array (character widths) and
 ky (vertical kerning for entire span)
 rtl (+1 for LTR, -1 RTL)

 are smuggled through to the EMF (ignored by others) as:
    text<nul>N w1 w2 w3 ...wN<nul>y1 y2 y3 .. yN<nul><nul>
 The ndx, widths, y kern, and rtl are all 7 characters wide.  ndx and rtl are ints, the widths and ky are
    formatted as ' 6f'.
*/
char *smuggle_adxkyrtl_in(const char *string, int ndx, float *adx, float ky, float rtl){
    int slen = strlen(string);
    /* holds:  string
               fake terminator         (one \0)
               Number of widths        (ndx)
               series of widths        (ndx entries)
               fake terminator         (one \0)
               y kern value            (one float)
               rtl value               (one float)
               real terminator         (two \0)
    */
    int newsize=slen + 1 + 7 + 7*ndx + 1 + 7 + 7 + 2;
    newsize = 8*((7 + newsize)/8);            // suppress valgrind messages if it is a multiple of 8 bytes???
    char *smuggle=(char *)malloc(newsize);
    strcpy(smuggle,string);                   // text to pass, includes the first fake terminator
    char *cptr = smuggle + slen + 1;          // immediately after the first fake terminator
    sprintf(cptr,"%07d",ndx);                 // number of widths to pass
    cptr+=7;                                  // advance over ndx
    for(int i=0; i<ndx ; i++){                // all the widths
       sprintf(cptr," %6f",adx[i]);
       cptr+=7;                               // advance over space + width
    }
    *cptr='\0';
    cptr++;                                   // second fake terminator
    sprintf(cptr," %6f",ky);                  // y kern for span
    cptr+=7;                                  // advance over space + ky
    sprintf(cptr," %6d",(int) rtl);           // rtl multiplier for span
    cptr+=7;                                  // advance over rtl
    *cptr++ = '\0';                           // Set the real terminators
    *cptr   = '\0';
    return(smuggle);
}

void Layout::_clearOutputObjects()
{
    _paragraphs.clear();
    _lines.clear();
    _chunks.clear();
    _spans.clear();
    _characters.clear();
    _glyphs.clear();
    _path_fitted = nullptr;
    _path_fitted_closed = false;
    _path_fitted_length = 0;
    _path_spacing.clear();
    _path_auto_requested = false;
    _path_spacing_height = 0;
}

double Layout::_wrapOnFittedPath(double offset, double from) const
{
    double const period = _path_spacing.empty() ? _path_fitted_length : _path_spacing.back().second;
    if (!_path_fitted_closed || period <= Geom::EPSILON) return offset;
    return offset - std::floor((offset - from) / period) * period;
}

namespace {
// Extrapolate beyond an open path so that the ordinary overflow test still hides text.
// A closed path maps whole turns before interpolation, including negative aligned starts.
double path_metric_lookup(std::vector<std::pair<double, double>> const &map, double x, bool inverse, bool closed)
{
    if (map.empty()) return x;
    auto from = [inverse](auto const &p) { return inverse ? p.second : p.first; };
    auto to = [inverse](auto const &p) { return inverse ? p.first : p.second; };
    double const period = from(map.back()), result_period = to(map.back());
    double turns = 0;
    if (closed && period > Geom::EPSILON) {
        turns = std::floor(x / period);
        x -= turns * period;
    }
    double y;
    if (x <= 0) y = x;
    else if (x >= period) y = result_period + x - period;
    else {
        auto const upper = std::upper_bound(map.begin(), map.end(), x,
                                           [&](double a, auto const &p) { return a < from(p); });
        auto const &a = *(upper - 1), &b = *upper;
        y = to(a) + (x - from(a)) * (to(b) - to(a)) / (from(b) - from(a));
    }
    return y + turns * result_period;
}
}

double Layout::_pathDistance(double metric) const
{
    return path_metric_lookup(_path_spacing, metric, true, _path_fitted_closed);
}

double Layout::_pathMetric(double distance) const
{
    return path_metric_lookup(_path_spacing, distance, false, _path_fitted_closed);
}

void Layout::_buildPathSpacing(Path const &path, double points_per_unit)
{
    _path_spacing.clear();
    _path_spacing_height = 0;
    if (_spans.empty() || !std::isfinite(points_per_unit) || points_per_unit <= 1e-9) return;
    auto const &first = _spans.front();
    if (!first.font) return;
    // Illustrator 29.2.1's cache is round(size * (1 + cap-height/em) / 2), in points.
    // It is the first run's font, not the largest font in a mixed-size story. Recompute
    // from the live font; never use stale source cache numbers after a type edit.
    double const height = std::round(first.font_size * (1 + first.font->GetCapHeight()) *
                                     points_per_unit / 2) / points_per_unit;
    if (!std::isfinite(height) || height <= 0 || height > 1e7 || _path_fitted_length <= Geom::EPSILON) return;
    auto const paths = path.MakePathVector();
    if (paths.size() != 1) return;
    auto const &geometry = paths[0];
    std::optional<Geom::Point> previous, initial;
    for (auto const &curve : geometry) {
        if (Geom::L2(curve.finalPoint() - curve.initialPoint()) < Geom::EPSILON && curve.isDegenerate()) continue;
        auto const a = curve.unitTangentAt(0), b = curve.unitTangentAt(1);
        if (Geom::L2(a) < 0.9 || Geom::L2(b) < 0.9 || (previous && Geom::L2(*previous - a) > 0.01)) return;
        if (!initial) initial = a;
        previous = b;
    }
    if (!previous || (_path_fitted_closed && initial && Geom::L2(*previous - *initial) > 0.01)) return;
    unsigned const steps = unsigned(std::clamp(std::ceil(_path_fitted_length / (height * 0.025)), 64.0, 8192.0));
    // Sample the endpoint directly: the polyline lookup can omit a request a few ulps
    // past its accumulated length. A complete circle must not lose its final sample.
    std::vector<double> distances(steps);
    for (unsigned i = 0; i < steps; ++i) distances[i] = _path_fitted_length * i / steps;
    int count = 0;
    auto &editable = const_cast<Path &>(path);
    auto *positions = editable.CurvilignToPosition(distances.size(), distances.data(), count);
    if (!positions || count != int(distances.size())) { g_free(positions); return; }
    double angle = 0, metric = 0;
    for (unsigned i = 0; i <= steps; ++i) {
        Geom::Point at, tangent;
        if (i == steps) tangent = *previous;
        else editable.PointAndTangentAt(positions[i].piece, positions[i].t, at, tangent);
        double const distance = i == steps ? _path_fitted_length : distances[i];
        double const next = std::atan2(tangent.y(), tangent.x());
        if (Geom::L2(tangent) < 0.9 || !std::isfinite(next)) { _path_spacing.clear(); break; }
        if (i) {
            double const turn = std::remainder(next - angle, 2 * M_PI);
            double const ds = distance - distances[i - 1];
            // Outward bends space at one fifth of the reference height, inward bends
            // at half. This is a bounded numerical approximation, not Adobe's engine.
            double const advance = ds + height * (turn >= 0 ? 0.2 : 0.5) * turn;
            if (std::abs(turn) > 0.15 || (turn < 0 && -height * turn > ds * 0.4) ||
                advance <= ds * 0.1 || !std::isfinite(advance)) {
                _path_spacing.clear();
                break; // Very tight inward bends are outside the qualified approximation.
            }
            metric += advance;
        }
        _path_spacing.emplace_back(distance, metric);
        angle = next;
    }
    g_free(positions);
    if (!_path_spacing.empty()) _path_spacing_height = height;
}

void Layout::FontMetrics::set(FontInstance const *font)
{
    if (font) {
        ascent      = font->GetTypoAscent();
        descent     = font->GetTypoDescent();
        xheight     = font->GetXHeight();
        ascent_max  = font->GetMaxAscent();
        descent_max = font->GetMaxDescent();
    }
}

void Layout::FontMetrics::max(FontMetrics const &other)
{
    if (other.ascent      > ascent      ) ascent      = other.ascent;
    if (other.descent     > descent     ) descent     = other.descent;
    if( other.xheight     > xheight     ) xheight     = other.xheight;
    if( other.ascent_max  > ascent_max  ) ascent_max  = other.ascent_max;
    if( other.descent_max > descent_max ) descent_max = other.descent_max;
}

void Layout::FontMetrics::computeEffective( const double &line_height_multiplier ) {
    double half_leading = 0.5 * (line_height_multiplier - 1.0) * emSize();
    ascent  += half_leading;
    descent += half_leading;
}

// TODO: Refactor this so it can work without passing layout around
Geom::Affine Layout::Glyph::transform(Layout const &layout) const
{
    Geom::Affine matrix;

    auto const &glyph_span = span(&layout);
    double r = rotation;
    if ( (glyph_span.block_progression == LEFT_TO_RIGHT || glyph_span.block_progression == RIGHT_TO_LEFT) &&
        orientation == ORIENTATION_SIDEWAYS ) {
        // Vertical sideways text
        r += M_PI/2.0;
    }
    double sin_rotation = sin(r);
    double cos_rotation = cos(r);
    matrix[0] = glyph_span.font_size * cos_rotation;
    matrix[1] = glyph_span.font_size * sin_rotation;
    matrix[2] = glyph_span.font_size * sin_rotation;
    matrix[3] = -glyph_span.font_size * cos_rotation * vertical_scale; // unscale vertically so the specified text height is preserved if lengthAdjust=spacingAndGlyphs
    if (glyph_span.block_progression == LEFT_TO_RIGHT || glyph_span.block_progression == RIGHT_TO_LEFT) {
        // Vertical text
        // This effectively swaps x for y which changes handedness of coordinate system. This is a bit strange
        // and not what one would expect but the compute code already reverses y so OK.
        matrix[4] = line(&layout).baseline_y + y;
        matrix[5] = chunk(&layout).left_x + x;
    } else {
        // Horizontal text
        matrix[4] = chunk(&layout).left_x + x;
        matrix[5] = line(&layout).baseline_y + y;
    }
    return matrix;
}

void Layout::show(DrawingGroup *parent, StyleAttachments &style_attachments, Geom::OptRect const &paintbox) const
{
    int glyph_index = 0;
    double phase0 = 0.0;

    for (int i = 0; i < _spans.size(); i++) {
        if (_input_stream[_spans[i].in_input_stream_item]->Type() != TEXT_SOURCE) {
            continue;
        }

        if (_spans[i].line(this).hidden) {
            continue; // Line corresponds to text overflow. Don't show!
        }

        auto text_source = static_cast<InputStreamTextSource const *>(_input_stream[_spans[i].in_input_stream_item]);
        auto style = text_source->style;

        style->text_decoration_data.tspan_width = _spans[i].width();
        style->text_decoration_data.ascender    = _spans[i].line_height.getTypoAscent();
        style->text_decoration_data.descender   = _spans[i].line_height.getTypoDescent();

        auto line_of_span = [this] (int i) { return _chunks[_spans[i].in_chunk].in_line; };
        style->text_decoration_data.tspan_line_start = i == 0                 || line_of_span(i) != line_of_span(i - 1);
        style->text_decoration_data.tspan_line_end   = i == _spans.size() - 1 || line_of_span(i) != line_of_span(i + 1);
        // PROOF: letter-spacing follows every letter, the last one too. A line drawn under or
        // through type stops at the line's last letter, as Illustrator draws it, not after
        // the spacing that follows it (or short of the letter when the spacing is negative).
        if (style->text_decoration_data.tspan_line_end && _spans[i].width() > 0) {
            style->text_decoration_data.tspan_width =
                std::max(0.0, _spans[i].width() - style->letter_spacing.computed * getTextLengthMultiplierDue());
        }

        if (_spans[i].font) {
            double underline_thickness, underline_position, line_through_thickness, line_through_position;
            _spans[i].font->FontDecoration(underline_position, underline_thickness, line_through_position, line_through_thickness);
            style->text_decoration_data.underline_thickness    = underline_thickness;
            style->text_decoration_data.underline_position     = underline_position;
            style->text_decoration_data.line_through_thickness = line_through_thickness;
            style->text_decoration_data.line_through_position  = line_through_position;
        } else { // can this case ever occur?
            style->text_decoration_data.underline_thickness    = 0.0;
            style->text_decoration_data.underline_position     = 0.0;
            style->text_decoration_data.line_through_thickness = 0.0;
            style->text_decoration_data.line_through_position  = 0.0;
        }

        auto drawing_text = new DrawingText(parent->drawing());

        if (style->filter.set) {
            if (auto filter = style->getFilter()) {
                style_attachments.attachFilter(drawing_text, filter);
            }
        }

        if (style->fill.isPaintserver()) {
            if (auto fill = style->getFillPaintServer()) {
                style_attachments.attachFill(drawing_text, fill, paintbox);
            }
        }

        if (style->stroke.isPaintserver()) {
            if (auto stroke = style->getStrokePaintServer()) {
                style_attachments.attachStroke(drawing_text, stroke, paintbox);
            }
        }

        bool first_line_glyph = true;
        while (glyph_index < _glyphs.size() && _characters[_glyphs[glyph_index].in_character].in_span == i) {
            if (_characters[_glyphs[glyph_index].in_character].in_glyph != -1) {
                Geom::Affine glyph_matrix = _glyphs[glyph_index].transform(*this);
                if (first_line_glyph && style->text_decoration_data.tspan_line_start) {
                    first_line_glyph = false;
                    phase0 =  glyph_matrix.translation().x();
                }
                // Save the starting coordinates for the line - these are needed for figuring out dot/dash/wave phase.
                // Use maximum ascent and descent to ensure glyphs that extend outside the embox are fully drawn.
                drawing_text->addComponent(_spans[i].font,
                                           _glyphs[glyph_index].glyph,
                                           glyph_matrix,
                                           _glyphs[glyph_index].advance,
                                           _spans[i].line_height.getMaxAscent(),
                                           _spans[i].line_height.getMaxDescent(),
                                           glyph_matrix.translation().x() - phase0
                );
            }
            glyph_index++;
        }
        drawing_text->setStyle(style);
        drawing_text->setItemBounds(paintbox);
        // Text spans must be painted in the right order (see inkscape/685)
        parent->appendChild(drawing_text);
        // Set item bounds without filter enlargement
        parent->setItemBounds(paintbox);
    }
}

Geom::OptRect Layout::bounds(Geom::Affine const &transform, bool with_stroke, int start, int length) const
{
    Geom::OptRect bbox;
    for (auto const &glyph : _glyphs) {
        if (glyph.hidden) continue; // To do: This and the next line should represent the same thing, use on or the other.
        if (_characters[glyph.in_character].in_glyph == -1) continue;
        if (start != -1 && (int) glyph.in_character < start) continue;
        if (length != -1) {
            if (start == -1)
                start = 0;
            if ((int) glyph.in_character > start + length) continue;
        }
        // this could be faster
        Geom::Affine glyph_matrix = glyph.transform(*this);
        Geom::Affine total_transform = glyph_matrix;
        total_transform *= transform;
        if(glyph.span(this).font) {
            Geom::OptRect glyph_rect = glyph.span(this).font->BBoxExact(glyph.glyph);
            if (glyph_rect) {
                auto glyph_box = *glyph_rect * total_transform;
                // FIXME: Expand rectangle by half stroke width, this doesn't include meters
                // and so is not the most ideal calculation, we could use the glyph Path here.
                if (with_stroke) {
                    Span const &span = _spans[_characters[glyph.in_character].in_span];
                    auto text_source = static_cast<InputStreamTextSource const *>(_input_stream[span.in_input_stream_item]);
                    if (!text_source->style->stroke.isNone()) {
                        double scale = transform.descrim();
                        glyph_box.expandBy(0.5 * text_source->style->stroke_width.computed * scale);
                    }
                }
                bbox.unionWith(glyph_box);
            }
        }
    }
    return bbox;
}

/* This version is much simpler than the old one
*/
void Layout::print(SPPrintContext *ctx,
                   Geom::OptRect const &pbox, Geom::OptRect const &dbox, Geom::OptRect const &bbox,
                   Geom::Affine const &ctm) const
{
bool text_to_path         = ctx->module->textToPath();
#define MAX_DX 2048
float    hold_dx[MAX_DX]; // For smuggling dx values (character widths) into print functions, unlikely any simple text output will be longer than this.

Geom::Affine glyph_matrix;

    if (_input_stream.empty()) return;
    if (!_glyphs.size()) return; // yes, this can happen.
    if (text_to_path || _path_fitted) {
        for (auto const &glyph : _glyphs) {
            if (_characters[glyph.in_character].in_glyph == -1)continue; //invisible glyphs
            Span const &span = _spans[_characters[glyph.in_character].in_span];
            Geom::PathVector const *pv = span.font->PathVector(glyph.glyph);
            InputStreamTextSource const *text_source = static_cast<InputStreamTextSource const *>(_input_stream[span.in_input_stream_item]);
            if (pv) {
                glyph_matrix = glyph.transform(*this);
                Geom::PathVector temp_pv = (*pv) * glyph_matrix;
                if (!text_source->style->fill.isNone())
                    ctx->fill(temp_pv, ctm, text_source->style, pbox, dbox, bbox);
                if (!text_source->style->stroke.isNone())
                    ctx->stroke(temp_pv, ctm, text_source->style, pbox, dbox, bbox);
            }
        }
    }
    else {
        /* index by characters, referencing glyphs and spans only as needed  */
        double char_x;
        int    doUTN  = CanUTN();  // Unicode to Nonunicode translation enabled if true
        Direction block_progression = _blockProgression();
        int oldtarget = 0;
        int ndx = 0;
        double rtl = 1.0;        // 1 L->R, -1 R->L, constant across a span. 1.0 for t->b b->t???

        for (unsigned char_index = 0 ; char_index < _characters.size() ; ) {
            Glib::ustring text_string;  // accumulate text for record in this
            Geom::Point g_pos(0,0);     // all strings are output at (0,0) because we do the translation using the matrix
            int glyph_index = _characters[char_index].in_glyph;
            if(glyph_index == -1){  // if the character maps to an invisible glyph we cannot know its geometry, so skip it and move on
                char_index++;
                continue;
            }
            float ky = _glyphs[glyph_index].y;  // For smuggling y kern value for span // same value for all positions in a span
            unsigned span_index  = _characters[char_index].in_span;
            Span const &span = _spans[span_index];
            char_x = 0.0;
            Glib::ustring::const_iterator text_iter = span.input_stream_first_character;
            InputStreamTextSource const *text_source = static_cast<InputStreamTextSource const *>(_input_stream[span.in_input_stream_item]);
            glyph_matrix = Geom::Scale(1.0, -1.0) * (Geom::Affine)Geom::Rotate(_glyphs[glyph_index].rotation);
            if (block_progression == LEFT_TO_RIGHT || block_progression == RIGHT_TO_LEFT) {
                glyph_matrix[4] = span.line(this).baseline_y + span.baseline_shift;
                // since we're outputting character codes, not glyphs, we want the character x
                glyph_matrix[5] = span.chunk(this).left_x + span.x_start + _characters[_glyphs[glyph_index].in_character].x;
            } else {
                glyph_matrix[4] = span.chunk(this).left_x + span.x_start + _characters[_glyphs[glyph_index].in_character].x;
                glyph_matrix[5] = span.line(this).baseline_y + span.baseline_shift;
            }
            switch(span.direction){
                case Layout::TOP_TO_BOTTOM:
                case Layout::BOTTOM_TO_TOP:
                case Layout::LEFT_TO_RIGHT: rtl =  1.0; break;
                case Layout::RIGHT_TO_LEFT: rtl = -1.0; break;
            }
            if(doUTN){
                oldtarget=SingleUnicodeToNon(*text_iter); // this should only ever be with a 1:1 glyph:character situation
            }

            // accumulate a record to write

            unsigned lc_index  = char_index;
            unsigned hold_iisi = _spans[span_index].in_input_stream_item;
            int newtarget = 0;
            while(true){
                glyph_index = _characters[lc_index].in_glyph;
                if(glyph_index == -1){  // end of a line within a paragraph, for instance
                    lc_index++;
                    break;
                }

                // always append if here
                text_string += *text_iter;

                // figure out char widths, used by EMF, not currently used elsewhere
                double cwidth;
                if(lc_index == _glyphs[glyph_index].in_character){  // Glyph width is used only for the first character, these may be 0
                    cwidth = rtl * _glyphs[glyph_index].advance; // advance might be zero
                }
                else {
                    cwidth = 0;
                }
                char_x += cwidth;
/*
std:: cout << "DEBUG Layout::print in while "
<< " char_index "  << char_index
<< " lc_index "    << lc_index
<< " character "   << std::hex << (int) *text_iter << std::dec
<< " glyph_index " << glyph_index
<< " glyph_xy " << _glyphs[glyph_index].x << " , " << _glyphs[glyph_index].y
<< " span_index "  << span_index
<< " hold_iisi  "  << hold_iisi
<< std::endl; //DEBUG
*/
                if(ndx < MAX_DX){
                    hold_dx[ndx++] = fabs(cwidth);
                }
                else { // silently truncate any text line silly enough to be longer than MAX_DX
                    lc_index = _characters.size();
                    break;
                }


                // conditions that prevent this character from joining the record
                lc_index++;
                if(lc_index >= _characters.size()) break; // nothing more to process, so it must be the end of the record
                ++text_iter;
                if(doUTN)newtarget=SingleUnicodeToNon(*text_iter); // this should only ever be with a 1:1 glyph:character situation
                if(newtarget != oldtarget)break;     // change in unicode to nonunicode translation status
                // MUST exit on any major span change, but not on some little events, like a font substitution event irrelevant for the file save
                unsigned next_span_index = _characters[lc_index].in_span;
                if(span_index != next_span_index){
                    /* on major changes break out of loop.
                       1st case usually indicates an entire input line has been processed (out of several in a paragraph)
                       2nd case usually indicates that a format change within a line (font/size/color/etc) is present.
                    */
/*
std:: cout << "DEBUG Layout::print in while  ---  "
<< " char_index "  << char_index
<< " lc_index "    << lc_index
<< " cwidth " << cwidth
<< " _char.x (next) " << (lc_index < _characters.size() ? _characters[lc_index].x : -1)
<< " char_x (end this)" << char_x
<< " diff " << fabs(char_x - _characters[lc_index].x)
<< " oldy " << ky
<< " nexty " << _glyphs[_characters[lc_index].in_glyph].y
<< std::endl; //DEBUG
*/
                    if(hold_iisi != _spans[next_span_index].in_input_stream_item)break; // major change, font, size, color, etc, must exit
                    if(fabs(char_x - _spans[next_span_index].x_start) >= 1e-4)break;    // xkerning change
                    if(ky != _glyphs[_characters[lc_index].in_glyph].y)break;           // ykerning change
                    /*
                       None of the above?  Then this is a minor "pangito", update span_index and keep going.
                       The font used by the display may have failed over, but print does not care and can continue to use
                       whatever was specified in the XML.
                    */
                    span_index  = next_span_index;
                    text_iter   = _spans[span_index].input_stream_first_character;
                }

            }
            // write it
            ctx->bind(glyph_matrix, 1.0);

            // the dx array is smuggled through to the EMF driver (ignored by others) as:
            //    text<nul>w1 w2 w3 ...wn<nul><nul>
            // where the widths are floats 7 characters wide, including the space

            char *smuggle_string=smuggle_adxkyrtl_in(text_string.c_str(),ndx, &hold_dx[0], ky, rtl);
            ctx->text(smuggle_string, g_pos, text_source->style);
            free(smuggle_string);
            ctx->release();
            ndx = 0;
            char_index = lc_index;
        }
    }
}


void Layout::showGlyphs(CairoRenderContext *ctx) const
{
    if (_input_stream.empty()) return;
    std::vector<CairoGlyphInfo> glyphtext;

    // The second pass is used to draw fill over stroke in a way that doesn't
    // cause some glyph chunks to be painted over others.
    bool second_pass = false;

    for (unsigned pass = 0; pass <= second_pass; pass++) {
        for (unsigned glyph_index = 0 ; glyph_index < _glyphs.size() ; ) {
            if (_characters[_glyphs[glyph_index].in_character].in_glyph == -1) {
                // invisible glyphs
                unsigned same_character = _glyphs[glyph_index].in_character;
                while (_glyphs[glyph_index].in_character == same_character) {
                    glyph_index++;
                    if (glyph_index == _glyphs.size())
                        return;
                }
                continue;
            }
            Span const &span = _spans[_characters[_glyphs[glyph_index].in_character].in_span];
            InputStreamTextSource const *text_source = static_cast<InputStreamTextSource const *>(_input_stream[span.in_input_stream_item]);

            Geom::Affine glyph_matrix = _glyphs[glyph_index].transform(*this);
            Geom::Affine font_matrix = glyph_matrix;
            font_matrix[4] = 0;
            font_matrix[5] = 0;

            Glib::ustring::const_iterator span_iter = span.input_stream_first_character;
            unsigned char_index = _glyphs[glyph_index].in_character;
            unsigned original_span = _characters[char_index].in_span;
            while (char_index && _characters[char_index - 1].in_span == original_span) {
                char_index--;
                ++span_iter;
            }

            // try to output as many characters as possible in one go
            Glib::ustring span_string;
            unsigned this_span_index = _characters[_glyphs[glyph_index].in_character].in_span;
            unsigned int first_index = glyph_index;
            glyphtext.clear();
            do {
                span_string += *span_iter;
                ++span_iter;

                unsigned same_character = _glyphs[glyph_index].in_character;
                while (glyph_index < _glyphs.size() && _glyphs[glyph_index].in_character == same_character) {
                    if (glyph_index != first_index)
                        glyph_matrix = _glyphs[glyph_index].transform(*this);

                    CairoGlyphInfo info;
                    info.index = _glyphs[glyph_index].glyph;
                    // this is the translation for x,y-offset
                    info.x = glyph_matrix[4];
                    info.y = glyph_matrix[5];

                    glyphtext.push_back(info);

                    glyph_index++;
                }
            } while (glyph_index < _glyphs.size()
                     && _path_fitted == nullptr
                     && (font_matrix * glyph_matrix.inverse()).isIdentity()
                     && _characters[_glyphs[glyph_index].in_character].in_span == this_span_index);

            // remove vertical flip
            Geom::Affine flip_matrix;
            flip_matrix.setIdentity();
            flip_matrix[3] = -1.0;
            font_matrix = flip_matrix * font_matrix;

            SPStyle const *style = text_source->style;
            double opacity = style->opacity.as_double();

            if (opacity != 1.0) {
                ctx->pushState();
                ctx->setStateForStyle(style);
                ctx->pushLayer();
            }
            if (glyph_index - first_index > 0) {
                second_pass |= ctx->renderGlyphtext(span.font->get_font(), font_matrix, glyphtext, style, pass);
            }
            if (opacity != 1.0) {
                ctx->popLayer();
                ctx->popState();
            }
        }
    }
}

#if DEBUG_TEXTLAYOUT_DUMPASTEXT
// these functions are for dumpAsText() only. No need to translate
static char const *direction_to_text(Layout::Direction d)
{
    switch (d) {
        case Layout::LEFT_TO_RIGHT: return "ltr";
        case Layout::RIGHT_TO_LEFT: return "rtl";
        case Layout::TOP_TO_BOTTOM: return "ttb";
        case Layout::BOTTOM_TO_TOP: return "btt";
    }
    return "???";
}

static char const *style_to_text(PangoStyle s)
{
    switch (s) {
        case PANGO_STYLE_NORMAL: return "upright";
        case PANGO_STYLE_ITALIC: return "italic";
        case PANGO_STYLE_OBLIQUE: return "oblique";
    }
    return "???";
}

static std::string weight_to_text(PangoWeight w)
{
    switch (w) {
        case PANGO_WEIGHT_THIN      : return "thin";
        case PANGO_WEIGHT_ULTRALIGHT: return "ultralight";
        case PANGO_WEIGHT_LIGHT     : return "light";
        case PANGO_WEIGHT_SEMILIGHT : return "semilight";
        case PANGO_WEIGHT_BOOK      : return "book";
        case PANGO_WEIGHT_NORMAL    : return "normalweight";
        case PANGO_WEIGHT_MEDIUM    : return "medium";
        case PANGO_WEIGHT_SEMIBOLD  : return "semibold";
        case PANGO_WEIGHT_BOLD      : return "bold";
        case PANGO_WEIGHT_ULTRABOLD : return "ultrabold";
        case PANGO_WEIGHT_HEAVY     : return "heavy";
        case PANGO_WEIGHT_ULTRAHEAVY: return "ultraheavy";
    }
    return std::to_string(w);
}
#endif //DEBUG_TEXTLAYOUT_DUMPASTEXT

Glib::ustring Layout::getFontFamily(unsigned span_index) const
{
    if (span_index >= _spans.size())
        return "";

    if (_spans[span_index].font) {
        return sp_font_description_get_family(_spans[span_index].font->get_descr());
    }

    return "";
}

#if DEBUG_TEXTLAYOUT_DUMPASTEXT
Glib::ustring Layout::dumpAsText() const
{
    Glib::ustring result;
    Glib::ustring::const_iterator icc;
    char line[256];

    result = Glib::ustring::compose("spans     %1\nchars     %2\nglyphs    %3\n", _spans.size(), _characters.size(), _glyphs.size());
    if(_characters.size() > 1){
        unsigned lastspan=5000;
        for(unsigned j = 0; j < _characters.size() ; j++){
            if(lastspan != _characters[j].in_span){
                lastspan = _characters[j].in_span;
                icc = _spans[lastspan].input_stream_first_character;
            }
            snprintf(line, sizeof(line), "char     %4u: '%c' 0x%4.4x x=%8.4f glyph=%3d span=%3d\n", j, *icc, *icc, _characters[j].x,  _characters[j].in_glyph,  _characters[j].in_span);
            result += line;
            ++icc;
        }
    }
    if(_glyphs.size()){
        for(unsigned j = 0; j < _glyphs.size() ; j++){
            snprintf(line, sizeof(line), "glyph    %4u: %4d (%8.4f,%8.4f) rot=%8.4f cx=%8.4f char=%4d\n",
                j, _glyphs[j].glyph, _glyphs[j].x, _glyphs[j].y, _glyphs[j].rotation, _glyphs[j].width, _glyphs[j].in_character);
            result += line;
        }
    }

    for (unsigned span_index = 0 ; span_index < _spans.size() ; span_index++) {
        result += Glib::ustring::compose("==== span %1 \n", span_index)
               +  Glib::ustring::compose("  in para %1 (direction=%2)\n", _lines[_chunks[_spans[span_index].in_chunk].in_line].in_paragraph,
                 direction_to_text(_paragraphs[_lines[_chunks[_spans[span_index].in_chunk].in_line].in_paragraph].base_direction))
               +  Glib::ustring::compose("  in source %1 (type=%2, cookie=%3)\n", _spans[span_index].in_input_stream_item,
                 _input_stream[_spans[span_index].in_input_stream_item]->Type(),
                 _input_stream[_spans[span_index].in_input_stream_item]->source)
               +  Glib::ustring::compose("  in line %1 (baseline=%2, shape=%3)\n", _chunks[_spans[span_index].in_chunk].in_line,
                 _lines[_chunks[_spans[span_index].in_chunk].in_line].baseline_y,
                 _lines[_chunks[_spans[span_index].in_chunk].in_line].in_shape)
               +  Glib::ustring::compose("  in chunk %1 (x=%2, baselineshift=%3)\n", _spans[span_index].in_chunk, _chunks[_spans[span_index].in_chunk].left_x, _spans[span_index].baseline_shift);

        if (_spans[span_index].font) {
            const char* variations = pango_font_description_get_variations(_spans[span_index].font->descr);
            result += Glib::ustring::compose(
                "    font '%1' %2 %3 %4 %5\n",
                sp_font_description_get_family(_spans[span_index].font->descr),
                _spans[span_index].font_size,
                style_to_text( pango_font_description_get_style(_spans[span_index].font->descr) ),
                weight_to_text( pango_font_description_get_weight(_spans[span_index].font->descr) ),
                (variations?variations:"")
            );
        }
        result += Glib::ustring::compose("    x_start = %1, x_end = %2\n", _spans[span_index].x_start, _spans[span_index].x_end)
               +  Glib::ustring::compose("    line height: ascent %1, descent %2\n", _spans[span_index].line_height.ascent, _spans[span_index].line_height.descent)
               +  Glib::ustring::compose("    direction %1, block-progression %2\n", direction_to_text(_spans[span_index].direction), direction_to_text(_spans[span_index].block_progression))
               +  "    ** characters:\n";
        Glib::ustring::const_iterator iter_char = _spans[span_index].input_stream_first_character;
        // very inefficient code. what the hell, it's only debug stuff.
        for (unsigned char_index = 0 ; char_index < _characters.size() ; char_index++) {
            union {const PangoLogAttr* pattr; const unsigned* uattr;} u;
            u.pattr = &_characters[char_index].char_attributes;
            if (_characters[char_index].in_span != span_index) continue;
            if (_input_stream[_spans[span_index].in_input_stream_item]->Type() != TEXT_SOURCE) {
                snprintf(line, sizeof(line), "      %u: control x=%f flags=%03x glyph=%d\n", char_index, _characters[char_index].x, *u.uattr, _characters[char_index].in_glyph);
            } else {  // some text has empty tspans, iter_char cannot be dereferenced
                snprintf(line, sizeof(line), "      %u: '%c' 0x%4.4x x=%f flags=%03x glyph=%d\n", char_index, *iter_char, *iter_char, _characters[char_index].x, *u.uattr, _characters[char_index].in_glyph);
                ++iter_char;
            }
            result += line;
        }
        result += "    ** glyphs:\n";
        for (unsigned glyph_index = 0 ; glyph_index < _glyphs.size() ; glyph_index++) {
            if (_characters[_glyphs[glyph_index].in_character].in_span != span_index) continue;
            snprintf(line, sizeof(line), "      %u: %d (%f,%f) rot=%f cx=%f char=%d\n", glyph_index, _glyphs[glyph_index].glyph, _glyphs[glyph_index].x, _glyphs[glyph_index].y, _glyphs[glyph_index].rotation, _glyphs[glyph_index].width, _glyphs[glyph_index].in_character);
            result += line;
        }
        result += "\n";
    }
    result += "EOT\n";
    return result;
}
#endif //DEBUG_TEXTLAYOUT_DUMPASTEXT

void Layout::fitToPathAlign(SVGLength const &startOffset, Path const &path,
                            std::optional<std::pair<double, double>> const &active,
                            bool automatic_spacing, double points_per_unit)
{
    auto const pathv = path.MakePathVector();
    if (pathv.empty()) return;
    bool const closed = pathv[0].closed();
    double const path_length = const_cast<Path &>(path).Length();
    _path_fitted_closed = closed;
    _path_fitted_length = path_length;
    _path_auto_requested = automatic_spacing;
    if (automatic_spacing) _buildPathSpacing(path, points_per_unit);
    double const period = _path_spacing.empty() ? path_length : _path_spacing.back().second;
    double offset = 0.0;

    if (startOffset._set) {
        if (startOffset.unit == SVGLength::PERCENT)
            offset = startOffset.computed * const_cast<Path&>(path).Length();
        else
            offset = startOffset.computed;
    }
    offset = _pathMetric(offset);

    double fitted_width = _getChunkWidth(0);
    double prefix_limit = std::numeric_limits<double>::infinity();
    if (active && !_characters.empty()) {
        double const last = std::min(active->second, closed ? active->first + path_length : path_length);
        double const available = std::max(0.0, _pathMetric(last) - _pathMetric(active->first));
        if (fitted_width > available) {
            // Illustrator aligns the prefix that fits, not the full overflowing story.
            // Keep the story untouched, and let the normal visibility test hide its tail.
            fitted_width = 0;
            double last_break = 0, break_width = 0, ink_width = 0;
            for (unsigned i = 0; i < _characters.size();) {
                double const begin = _characters[i].span(this).x_start + _characters[i].x;
                if (i && _characters[i].char_attributes.is_line_break) {
                    last_break = begin;
                    break_width = ink_width;
                }
                unsigned next = i + 1;
                while (next < _characters.size() &&
                       (_characters[next].in_glyph == -1 || !_characters[next].char_attributes.is_cursor_position)) ++next;
                auto const first_glyph = _characters[i].in_glyph;
                auto const last_glyph = next == _characters.size() ? _glyphs.size() : std::size_t(_characters[next].in_glyph);
                double width = 0;
                if (first_glyph >= 0) for (auto k = std::size_t(first_glyph); k < last_glyph; ++k) width += _glyphs[k].advance;
                double const end = _characters[i].span(this).x_start + _characters[i].x + width;
                if (end > available + 1e-6) {
                    // Path type breaks at a word boundary when one exists. An unbroken
                    // word still clips by cluster. Trailing spaces do not shift alignment.
                    prefix_limit = last_break > 0 ? last_break : begin;
                    if (last_break > 0) fitted_width = break_width;
                    break;
                }
                fitted_width = std::max(fitted_width, end);
                if (!g_unichar_isspace(_characters[i].the_char)) ink_width = fitted_width;
                i = next;
            }
        }
    }
    Alignment alignment = _paragraphs.empty() ? LEFT : _paragraphs.front().alignment;
    switch (alignment) {
        case CENTER:
            offset -= fitted_width * 0.5;
            break;
        case RIGHT:
            offset -= fitted_width;
            break;
        default:
            break;
    }

    if (_characters.empty()) {
        int unused = 0;
        double position = _pathDistance(offset);
        Path::cut_position *point_otp = const_cast<Path&>(path).CurvilignToPosition(1, &position, unused);
        if (position >= 0.0 && point_otp != nullptr && point_otp[0].piece >= 0) {
            Geom::Point point;
            Geom::Point tangent;
            const_cast<Path&>(path).PointAndTangentAt(point_otp[0].piece, point_otp[0].t, point, tangent);
            _empty_cursor_shape.position = point;
            if (_directions_are_orthogonal(_blockProgression(), TOP_TO_BOTTOM)) {
                _empty_cursor_shape.rotation = atan2(-tangent[Geom::X], tangent[Geom::Y]);
            } else {
                _empty_cursor_shape.rotation = atan2(tangent[Geom::Y], tangent[Geom::X]);
            }
        }
        g_free(point_otp);
    }

    // A hidden cluster must not be undone by a later visible one on a closed path.
    bool truncated = false;
    for (unsigned char_index = 0 ; char_index < _characters.size() ; ) {
        Span const &span = _characters[char_index].span(this);

        size_t next_cluster_char_index = 0; // TODO refactor to not bump via for loops
        for (next_cluster_char_index = char_index + 1 ; next_cluster_char_index < _characters.size() ; next_cluster_char_index++) {
            if (_characters[next_cluster_char_index].in_glyph != -1 && _characters[next_cluster_char_index].char_attributes.is_cursor_position)
            {
                break;
            }
        }

        size_t next_cluster_glyph_index = 0;
        if (next_cluster_char_index == _characters.size()) {
            next_cluster_glyph_index = _glyphs.size();
        } else {
            next_cluster_glyph_index = _characters[next_cluster_char_index].in_glyph;
        }

        double start_offset = offset + span.x_start + _characters[char_index].x;
        if (closed) start_offset += period;

        double cluster_width = 0.0;
        size_t const current_cluster_glyph_index = _characters[char_index].in_glyph;
        for (size_t glyph_index = current_cluster_glyph_index ; glyph_index < next_cluster_glyph_index ; glyph_index++)
        {
            cluster_width += _glyphs[glyph_index].advance;
        }
        // TODO block progression?
        if (span.direction == RIGHT_TO_LEFT)
        {
            start_offset -= cluster_width;
        }
        double end_offset = start_offset + cluster_width;

        int unused = 0;
        double const unwrapped_midpoint = _pathDistance((start_offset + end_offset) * 0.5 -
                                                       (closed ? period : 0));
        double midpoint_offset = _pathDistance((start_offset + end_offset) * 0.5);
        start_offset = _pathDistance(start_offset);
        end_offset = _pathDistance(end_offset);

        if (closed && path_length > Geom::EPSILON) {
            midpoint_offset = std::fmod(midpoint_offset, path_length);
        }

        // as far as I know these functions are const, they're just not marked as such
        Path::cut_position *midpoint_otp = const_cast<Path&>(path).CurvilignToPosition(1, &midpoint_offset, unused);
        // Round a closed path the active portion can run on past the path's start (PROOF,
        // 2026-10-23: Illustrator's brackets), but never a second time round.
        double const unwrapped_end = end_offset - (closed ? path_length : 0);
        // The path's editable polyline is flattened at 0.01 units; brackets use exact
        // cubic lengths. Do not hide the final right-aligned glyph for that tiny error.
        double const end_tolerance = active ? std::min(0.01, double(span.font_size) * 0.001) : 0;
        bool const within_active = active ? (unwrapped_midpoint >= active->first &&
                                              unwrapped_end <=
                                                  std::min(active->second, closed ? active->first + path_length : path_length) + end_tolerance) :
                                   !automatic_spacing || !closed ||
                                   (unwrapped_midpoint >= _pathDistance(offset) && unwrapped_midpoint <= _pathDistance(offset + period));
        bool const in_prefix = span.x_start + _characters[char_index].x < prefix_limit;
        if (within_active && in_prefix && !((automatic_spacing || active) && truncated) && midpoint_offset >= 0.0 && midpoint_otp != nullptr && midpoint_otp[0].piece >= 0) {
            Geom::Point midpoint;
            Geom::Point tangent;
            const_cast<Path&>(path).PointAndTangentAt(midpoint_otp[0].piece, midpoint_otp[0].t, midpoint, tangent);

            if (start_offset >= 0.0 && end_offset >= 0.0) {
                Path::cut_position *start_otp = const_cast<Path&>(path).CurvilignToPosition(1, &start_offset, unused);
                if (start_otp != nullptr && start_otp[0].piece >= 0) {
                    Path::cut_position *end_otp = const_cast<Path&>(path).CurvilignToPosition(1, &end_offset, unused);
                    if (end_otp != nullptr && end_otp[0].piece >= 0) {
                        bool on_same_subpath = true;
                        for (const auto & pt : path.pts) {
                            if (pt.piece <= start_otp[0].piece) continue;
                            if (pt.piece >= end_otp[0].piece) break;
                            if (pt.isMoveTo == polyline_moveto) {
                                on_same_subpath = false;
                                break;
                            }
                        }
                        if (on_same_subpath) {
                            // both points were on the same subpath (without this test the angle is very weird)
                            Geom::Point startpoint, endpoint;
                            const_cast<Path&>(path).PointAt(start_otp[0].piece, start_otp[0].t, startpoint);
                            const_cast<Path&>(path).PointAt(end_otp[0].piece, end_otp[0].t, endpoint);
                            if (endpoint != startpoint) {
                                tangent = endpoint - startpoint;
                                tangent.normalize();
                            }
                        }
                        g_free(end_otp);
                    }
                    g_free(start_otp);
                }
            }

            if (_directions_are_orthogonal(_blockProgression(), TOP_TO_BOTTOM)) {
                double rotation = atan2(-tangent[Geom::X], tangent[Geom::Y]);
                for (size_t glyph_index = current_cluster_glyph_index; glyph_index < next_cluster_glyph_index ; glyph_index++) {
                    _glyphs[glyph_index].x = midpoint[Geom::Y] - tangent[Geom::X] * _glyphs[glyph_index].y - span.chunk(this).left_x;
                    _glyphs[glyph_index].y = midpoint[Geom::X] + tangent[Geom::Y] * _glyphs[glyph_index].y - _lines.front().baseline_y;
                   _glyphs[glyph_index].rotation += rotation;
                }
            } else {
                double rotation = atan2(tangent[Geom::Y], tangent[Geom::X]);
                for (size_t glyph_index = current_cluster_glyph_index; glyph_index < next_cluster_glyph_index ; glyph_index++) {
                    double tangent_shift = -cluster_width * 0.5 + _glyphs[glyph_index].x - (_characters[char_index].x + span.x_start);
                    if (span.direction == RIGHT_TO_LEFT)
                    {
                        tangent_shift += cluster_width;
                    }
                    _glyphs[glyph_index].x = midpoint[Geom::X] + tangent[Geom::X] * tangent_shift - tangent[Geom::Y] * _glyphs[glyph_index].y - span.chunk(this).left_x;
                    _glyphs[glyph_index].y = midpoint[Geom::Y] + tangent[Geom::Y] * tangent_shift + tangent[Geom::X] * _glyphs[glyph_index].y - _lines.front().baseline_y;
                    _glyphs[glyph_index].rotation += rotation;
                }
            }
        } else {  // outside the bounds of the path: hide the glyphs
            // PDF output consumes the glyph visibility flag, whereas canvas and
            // editing use the character's glyph reference. Keep both in sync.
            for (size_t glyph_index = current_cluster_glyph_index; glyph_index < next_cluster_glyph_index; ++glyph_index) {
                _glyphs[glyph_index].hidden = true;
            }
            _characters[char_index].in_glyph = -1;
            truncated = true;
        }
        g_free(midpoint_otp);

        char_index = next_cluster_char_index;
    }

    for (auto & _span : _spans) {
        _span.x_start += offset;
        _span.x_end += offset;
    }

    _input_truncated = truncated;
    _path_fitted = &path;
    _path_fitted_closed = closed;
    _path_fitted_length = path_length;
}

Geom::PathVector Layout::convertToCurves() const
{
    return convertToCurves(begin(), end());
}

Geom::PathVector Layout::convertToCurves(iterator const &from_glyph, iterator const &to_glyph) const
{
    Geom::PathVector curve;

    for (int glyph_index = std::max(0, from_glyph._glyph_index) ; glyph_index < to_glyph._glyph_index ; glyph_index++) {
        if (_characters[_glyphs[glyph_index].in_character].in_glyph == -1) continue;
        Span const &span = _glyphs[glyph_index].span(this);
        Geom::Affine glyph_matrix = _glyphs[glyph_index].transform(*this);
        Geom::PathVector const *pathv = span.font->PathVector(_glyphs[glyph_index].glyph);
        if (pathv) {
            Geom::PathVector pathv_trans = *pathv * glyph_matrix;
            pathvector_append(curve, std::move(pathv_trans));
        }
    }

    return curve;
}

Geom::PathVector Layout::convertToSVG(iterator const &from_glyph, iterator const &to_glyph,
                                      std::vector<std::pair<std::string, Geom::Affine>> &svgOut) const
{
    Geom::PathVector curve;

    for (int glyph_index = std::max(0, from_glyph._glyph_index); glyph_index < to_glyph._glyph_index; glyph_index++) {
        if (_characters[_glyphs[glyph_index].in_character].in_glyph == -1) continue;
        Span const &span = _glyphs[glyph_index].span(this);
        Geom::Affine glyph_matrix = _glyphs[glyph_index].transform(*this);
        auto glyph_id = _glyphs[glyph_index].glyph;
        auto &font = span.font;
        auto glyph = font->LoadGlyph(glyph_id);
        if (!glyph) {
            continue;
        }
        if (font->FontHasSVG()) {
            auto svg = font->GlyphSvg(glyph_id);
            if (!svg.empty()) {
                auto scale = 1.0 / font->GetDesignUnits();
                svgOut.emplace_back(std::move(svg), Geom::Scale(scale, -scale) * glyph_matrix);
            }
            continue;
        }
        Geom::PathVector const *pathv = span.font->PathVector(glyph_id);
        if (pathv) {
            Geom::PathVector pathv_trans = *pathv * glyph_matrix;
            pathvector_append(curve, std::move(pathv_trans));
        }
    }

    return curve;
}

void Layout::transform(Geom::Affine const &transform)
{
    // this is all massively oversimplified
    // I can't actually think of anybody who'll want to use it at the moment, so it'll stay simple
    for (auto & _glyph : _glyphs) {
        Geom::Point point(_glyph.x, _glyph.y);
        point *= transform;
        _glyph.x = point[0];
        _glyph.y = point[1];
    }
}

double Layout::getTextLengthIncrementDue() const
{
    if (textLength._set && textLengthIncrement != 0 && lengthAdjust == Inkscape::Text::Layout::LENGTHADJUST_SPACING) {
        return textLengthIncrement;
    }
    return 0;
}


double Layout::getTextLengthMultiplierDue() const
{
    if (textLength._set && textLengthMultiplier != 1 && (lengthAdjust == Inkscape::Text::Layout::LENGTHADJUST_SPACINGANDGLYPHS)) {
        return textLengthMultiplier;
    }
    return 1;
}

double Layout::getActualLength() const
{
    double length = 0;
    for (std::vector<Span>::const_iterator it_span = _spans.begin() ; it_span != _spans.end() ; it_span++) {
        // take x_end of the last span of each chunk
        if (it_span == _spans.end() - 1 || (it_span + 1)->in_chunk != it_span->in_chunk)
            length += it_span->x_end;
    }
    return length;


}

}//namespace Text
}//namespace Inkscape

std::ostream &operator<<(std::ostream &out, const Inkscape::Text::Layout::FontMetrics &f) {
    out << " emSize: "  << f.emSize()
        << " ascent: "  << f.ascent
        << " descent: " << f.descent
        << " xheight: " << f.xheight;
    return out;
}

std::ostream &operator<<(std::ostream &out, const Inkscape::Text::Layout::FontMetrics *f) {
    out << " emSize: "  << f->emSize()
        << " ascent: "  << f->ascent
        << " descent: " << f->descent
        << " xheight: " << f->xheight;
    return out;
}



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
