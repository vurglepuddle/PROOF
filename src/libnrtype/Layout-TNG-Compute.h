// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Inkscape::Text::Layout::Calculator - text layout engine meaty bits
 *
 * Authors:
 *   Richard Hughes <cyreve@users.sf.net>
 *
 * Copyright (C) 2005 Richard Hughes
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_LAYOUT_TNG_COMPUTE_H
#define SEEN_LAYOUT_TNG_COMPUTE_H

#include <span>
#include <pango/pango.h>

#include "Layout-TNG-Scanline-Maker.h"
#include "Layout-TNG.h"

class LibnrtypeLayoutTNGComputeTest;

namespace Inkscape {
namespace Text {

//#define DEBUG_LAYOUT_TNG_COMPUTE

/** \brief private to Layout. Does the real work of text flowing.

This class does a standard greedy paragraph wrapping algorithm.

Very high-level overview:

<pre>
foreach(paragraph) {
  call pango_itemize() (_buildPangoItemizationForPara())
  break into spans, without dealing with wrapping (_buildSpansForPara())
  foreach(line in flow shape) {
    foreach(chunk in flow shape) {   (in _buildChunksInScanRun())
      // this inner loop in _measureUnbrokenSpan()
      if the line height changed discard the line and start again
      keep adding characters until we run out of space in the chunk, then back up to the last word boundary
      (do sensible things if there is no previous word break)
    }
    push all the glyphs, chars, spans, chunks and line to output (not completely trivial because we must draw rtl in character order) (in _outputLine())
  }
  push the paragraph (in calculate())
}
</pre>

...and all of that needs to work vertically too, and with all the little details that make life annoying
*/
class Layout::Calculator
{
    friend class ::LibnrtypeLayoutTNGComputeTest;

    class SpanPosition;
    friend class SpanPosition;
    Layout &_flow;
    ScanlineMaker *_scanline_maker;
    unsigned _current_shape_index;     /// index into Layout::_input_wrap_shapes
    PangoContext *_pango_context;
    Direction _block_progression;

    /**
      * For y= attributes in tspan elements et al, we do the adjustment by moving each
      * glyph individually by this number. The spec means that this is maintained across
      * paragraphs.
      *
      * To do non-flow text layout, only the first "y" attribute is normally used. If there is only one
      * "y" attribute in a <tspan> other than the first <tspan>, it is ignored. This allows Inkscape to
      * insert a new line anywhere. On output, the Inkscape determined "y" is written out so other SVG
      * viewers know where to place the <tspans>.
      */
    double _y_offset;

    /** to stop pango from hinting its output, the font factory creates all fonts very large.
    All numbers returned from pango have to be divided by this number \em and divided by
    PANGO_SCALE. See FontFactory::FontFactory(). */
    double _font_factory_size_multiplier;

    /** Temporary storage associated with each item in Layout::_input_stream. */
    struct InputItemInfo {
        bool in_sub_flow;
        Layout *sub_flow;    // this is only set for the first input item in a sub-flow

        InputItemInfo() : in_sub_flow(false), sub_flow(nullptr) {}

        /* fixme: I don't like the fact that InputItemInfo etc. use the default copy constructor and
         * operator= (and thus don't involve incrementing reference counts), yet they provide a free method
         * that does delete or Unref.
         *
         * I suggest using the garbage collector to manage deletion.
         */
        void free()
        {
            if (sub_flow) {
                delete sub_flow;
                sub_flow = nullptr;
            }
        }
    };

    /** Temporary storage associated with each item returned by the call to
        pango_itemize(). */
    struct PangoItemInfo {
        PangoItem *item;
        std::shared_ptr<FontInstance> font;

        PangoItemInfo() : item(nullptr) {}

        /* fixme: I don't like the fact that InputItemInfo etc. use the default copy constructor and
         * operator= (and thus don't involve incrementing reference counts), yet they provide a free method
         * that does delete or Unref.
         *
         * I suggest using the garbage collector to manage deletion.
         */
        void free()
        {
            if (item) {
                pango_item_free(item);
                item = nullptr;
            }
        }
    };

    /** These spans have approximately the same definition as that used for
      * Layout::Span (constant font, direction, etc), except that they are from
      * before we have located the line breaks, so bear no relation to chunks.
      * They are guaranteed to be in at most one PangoItem (spans with no text in
      * them will not have an associated PangoItem), exactly one input object and
      * will only have one change of x, y, dx, dy or rotate attribute, which will
      * be at the beginning. An UnbrokenSpan can cross a chunk boundary, c.f.
      * BrokenSpan.
      */
    struct UnbrokenSpan {
        PangoGlyphString *glyph_string;
        int pango_item_index;           /// index into _para.pango_items, or -1 if this is style only
        unsigned input_index;           /// index into Layout::_input_stream
        Glib::ustring::const_iterator input_stream_first_character;
        double font_size;
        FontMetrics line_height;         /// This is not the CSS line-height attribute!
        double line_height_multiplier;  /// calculated from the font-height css property
        double baseline_shift;          /// calculated from the baseline-shift css property
        SPCSSTextOrientation text_orientation;
        unsigned text_bytes;
        bool tab = false; ///< PROOF: one left-aligned tab, with no drawn glyph.
        double tab_kerning = 0; ///< Letter-to-space kerning applied after the tab advance.
        double tab_tracking = 0; ///< Tracking of the preceding non-tab character.
        unsigned char_index_in_para;    /// the index of the first character in this span in the paragraph, for looking up char_attributes
        SVGLength x, y, dx, dy, rotate;  // these are reoriented copies of the <tspan> attributes. We change span when we encounter one.

        UnbrokenSpan() : glyph_string(nullptr) {}
        void free()
        {
            if (glyph_string)
                pango_glyph_string_free(glyph_string);
            glyph_string = nullptr;
        }
    };


    /** Used to provide storage for anything that applies to the current
    paragraph only. Since we're only processing one paragraph at a time,
    there's only one instantiation of this struct, on the stack of
    calculate(). */
    struct ParagraphInfo {
        Glib::ustring text;
        unsigned first_input_index;      ///< Index into Layout::_input_stream.
        Direction direction;
        Alignment alignment;
        std::vector<InputItemInfo> input_items;
        std::vector<PangoItemInfo> pango_items;
        std::vector<PangoLogAttr> char_attributes;    ///< For every character in the paragraph.
        std::vector<UnbrokenSpan> unbroken_spans;

        template<typename T> static void free_sequence(T &seq)
        {
            for (typename T::iterator it(seq.begin()); it != seq.end(); ++it) {
                it->free();
            }
            seq.clear();
        }

        void free()
        {
            text = "";
            free_sequence(input_items);
            free_sequence(pango_items);
            free_sequence(unbroken_spans);
        }
    };


    /**
      * A useful little iterator for moving char-by-char across spans.
      */
    struct UnbrokenSpanPosition {
        std::vector<UnbrokenSpan>::iterator iter_span;
        unsigned char_byte;
        unsigned char_index;

        void increment();   ///< Step forward by one character.

        inline bool operator== (UnbrokenSpanPosition const &other) const
            {return char_byte == other.char_byte && iter_span == other.iter_span;}
        inline bool operator!= (UnbrokenSpanPosition const &other) const
            {return char_byte != other.char_byte || iter_span != other.iter_span;}
    };

    /**
      * The line breaking algorithm will convert each UnbrokenSpan into one
      * or more of these. A BrokenSpan will never cross a chunk boundary,
      * c.f. UnbrokenSpan.
      */
    struct BrokenSpan {
        UnbrokenSpanPosition start;
        UnbrokenSpanPosition end;    // the end of this will always be the same as the start of the next
        unsigned start_glyph_index;
        unsigned end_glyph_index;
        double width;
        unsigned whitespace_count;
        bool ends_with_whitespace;
        double each_whitespace_width;
        double letter_spacing; // Save so we can subtract from width at end of line (for center justification)
        double word_spacing;
        void setZero();
    };

    /** The definition of a chunk used here is the same as that used in Layout:
    A collection of contiguous broken spans on the same line. (One chunk per line
    unless shape splits line into several sections... then one chunk per section. */
    struct ChunkInfo {
        std::vector<BrokenSpan> broken_spans;
        double scanrun_width;
        double text_width;       ///< Total width used by the text (excluding justification).
        double x;
        int whitespace_count;
    };

    void _buildPangoItemizationForPara(ParagraphInfo *para) const;
    static double _computeFontLineHeight( SPStyle const *style ); // Returns line_height_multiplier
    unsigned _buildSpansForPara(ParagraphInfo *para) const;
    bool _goToNextWrapShape();
    void _createFirstScanlineMaker();

    bool _findChunksForLine(ParagraphInfo const &para,
                            UnbrokenSpanPosition *start_span_pos,
                            std::vector<ChunkInfo> *chunk_info,
                            FontMetrics *line_box_height,
                            FontMetrics const *strut_height);

    bool _buildChunksInScanRun(ParagraphInfo const &para,
                               UnbrokenSpanPosition const &start_span_pos,
                               ScanlineMaker::ScanRun const &scan_run,
                               std::vector<ChunkInfo> *chunk_info,
                               FontMetrics *line_height) const;

    bool _measureUnbrokenSpan(ParagraphInfo const &para,
                              BrokenSpan *span,
                              BrokenSpan *last_break_span,
                              BrokenSpan *last_emergency_break_span,
                              double maximum_width, double inline_position) const;

    double _getChunkLeftWithAlignment(ParagraphInfo const &para,
                                      std::vector<ChunkInfo>::const_iterator it_chunk,
                                      double *add_to_each_whitespace) const;

    void _outputLine(ParagraphInfo const &para,
                     FontMetrics const &line_height,
                     std::vector<ChunkInfo> const &chunk_info,
                     bool hidden);

    static inline PangoLogAttr const &_charAttributes(ParagraphInfo const &para,
                                                      UnbrokenSpanPosition const &span_pos)
    {
        return para.char_attributes[span_pos.iter_span->char_index_in_para + span_pos.char_index];
    }

    void _estimateLigatureSubcomponents(std::span<Character> characters, Glyph &glyph, int positions, float direction);

#ifdef DEBUG_LAYOUT_TNG_COMPUTE
    static void dumpPangoItemsOut(ParagraphInfo *para);
    static void dumpUnbrokenSpans(ParagraphInfo *para);
#endif //DEBUG_LAYOUT_TNG_COMPUTE

public:
    Calculator(Layout *text_flow)
        : _flow(*text_flow) {}

    bool calculate();
};


}//namespace Text
}//namespace Inkscape


#endif


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
