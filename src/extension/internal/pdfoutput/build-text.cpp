// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Specific functionality for text handling
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "build-text.h"

#include "build-drawing.h"
#include "libnrtype/Layout-TNG.h"
#include "libnrtype/font-instance.h"
#include "style.h"
#include "style-text.h"
#include "util-string/string-convert.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

static std::string unicodeToUtf8(std::vector<gunichar> const &chars)
{
    std::string text;
    for (auto c : chars) {
        text += unicode_char_to_utf8(c);
    }
    return text;
}

TextContext::TextContext(Document &doc, capypdf::DrawContext &ctx, bool soft_mask)
    : _doc(doc)
    , _ctx(ctx)
    , _tx(ctx.text_new())
    , _soft_mask(soft_mask)
{}

// Because soft masks negate the use of draw opacities, we must fold them in.
std::optional<double> TextContext::get_softmask(double opacity) const
{
    if (_soft_mask) {
        return opacity;
    }
    return {};
}

/**
 * Set the text/font style, these are comment between clipping and painting.
 */
bool TextContext::set_text_style(std::shared_ptr<FontInstance> const &font, SPStyle *style)
{
    if (auto ret = _doc.get_font(font)) {
        auto &[key, font_id] = *ret;
        if (key != last_font) {
            // Transformation has consumed the font size
            _tx.cmd_Tf(font_id, 1); // style->font_size.computed);
            last_font = key;
        }
    } else {
        return false;
    }
    if (style->letter_spacing.set && style->letter_spacing.computed != last_letter_spacing) {
        _tx.cmd_Tc(style->letter_spacing.computed / 1000);
        last_letter_spacing = style->letter_spacing.computed;
    }
    return true;
}

/**
 * Set the painting style, this is not applicable to clipping.
 */
void TextContext::set_paint_style(StyleMap const &map, SPStyle const *style, SPStyle const *context_style)
{
    // NOTE: The pattern and gradients applied to tspans are currently not positioned correctly
    // but this bug is left in because it is not trivial to fix and is not possible to make an
    // SVG with the senario using Inkscape at the present time.
    if (map.contains(SPAttr::FILL)) {
        if (auto color = _doc.get_paint(style->fill, context_style, get_softmask(style->fill_opacity.as_double()))) {
            _tx.set_nonstroke(*color);
        }
    }
    if (map.contains(SPAttr::STROKE)) {
        if (auto color = _doc.get_paint(style->stroke, context_style, get_softmask(style->stroke_opacity.as_double()))) {
            _tx.set_stroke(*color);
        }
    }
    if (map.contains(SPAttr::STROKE_WIDTH)) {
        _tx.cmd_w(style->stroke_width.computed);
    }
    if (map.contains(SPAttr::STROKE_MITERLIMIT)) {
        _tx.cmd_M(style->stroke_miterlimit.value);
    }
    if (map.contains(SPAttr::STROKE_LINECAP)) {
        _tx.cmd_J(get_linecap(style->stroke_linecap.computed));
    }
    if (map.contains(SPAttr::STROKE_LINEJOIN)) {
        _tx.cmd_j(get_linejoin(style->stroke_linejoin.computed));
    }
    if (map.contains(SPAttr::STROKE_DASHARRAY)) {
        auto values = style->stroke_dasharray.get_computed();
        if (values.size() > 1) {
            _tx.cmd_d(values.data(), values.size(), style->stroke_dashoffset.computed);
        }
    }
    if (!_soft_mask) {
        auto soft_mask = _doc.style_to_transparency_mask(style, nullptr);

        if (soft_mask || last_ca < 1.0 || last_CA < 1.0 || style->fill_opacity.as_double() < 1.0 || style->stroke_opacity.as_double() < 1.0) {
            auto gstate = capypdf::GraphicsState();
            if (soft_mask) {
                auto smask = capypdf::SoftMask(CAPY_SOFT_MASK_LUMINOSITY, *soft_mask);
                gstate.set_SMask(_doc.generator().add_soft_mask(smask));
                last_ca = 0.0; // Force new gstate for next tspan
                gstate.set_ca(1.0);
                gstate.set_CA(1.0);
            } else {
                gstate.set_ca(style->fill_opacity.as_double());
                gstate.set_CA(style->stroke_opacity.as_double());
                last_ca = style->fill_opacity.as_double();
                last_CA = style->stroke_opacity.as_double();
            }
            auto gsid = _doc.generator().add_graphics_state(gstate);
            _tx.cmd_gs(gsid);
        }
    }
}

void TextContext::set_text_mode(CapyPDF_Text_Mode mode)
{
    if (last_text_mode != mode) {
        _tx.cmd_Tr(mode);
        last_text_mode = mode;
    }
}

void TextContext::finalize()
{
    _ctx.render_text_obj(_tx);
}

/**
 * Render the text span into the text context.
 */
void TextContext::render_text(Text::Layout const &layout, Text::Layout::Span const &span)
{
    double prev_advance = 0.0;
    Geom::Affine prev_Tm;
    std::unique_ptr<capypdf::TextSequence> seq;

    for (auto &glyph : layout.glyphs()) {
        if (glyph.hidden) {
            continue;
        }

        // If this glyph is in this span (this is bad)
        if (&glyph.span(&layout) == &span) {
            auto Tm = glyph.transform(layout);
            auto delta = Tm * prev_Tm.inverse();
            auto tr = delta.translation();
            auto chars = glyph.characters(&layout);
            auto text = unicodeToUtf8(chars);

            // Our layout engine has a bug where CR/LFs are given an out of memory glyph code.
            if (chars.size() == 1 && (chars[0] == '\n' || chars[0] == '\r')) {
                continue; // We don't know why Glyphs would have this not set to hidden
            }

            // TODO: We could detect vertical text and support top-down progression and kerning
            // but this is not within the scope of this work so is left for a future adventure.

            // Each time the glyph position changes, we calculate it's change and if it's not
            // A simple progression we can control with kerning, we have to finish the sequence
            // and add a new transform for the next glyph.
            if (seq && delta.isTranslation() && Geom::are_near(tr[Geom::Y], Geom::EPSILON)) {
                // The kerning is the change in glyph position minus the glyph's advance.
                // This is because the transform is known-good and will position glyphs perfectly
                // But the kerning is *relative* to the glyph_width
                int kerning = (tr[Geom::X] - prev_advance) * -1000;
                // Kerning is the left-ward shift in integer thous, negate for rightward shift;
                if (kerning != 0) {
                    seq->append_kerning(kerning);
                }
            } else {
                // Finish previous sequence
                if (seq) {
                    _tx.cmd_TJ(*seq);
                }
                // Start a new sequence of sequential glyphs
                seq = std::make_unique<capypdf::TextSequence>();
                _tx.cmd_Tm(Tm[0], Tm[1], Tm[2], Tm[3], Tm[4], Tm[5]);
            }

            if (chars.size() == 1) {
                // std::cout << "  seq->append_raw_glyph(" << glyph.glyph << ", '" << chars[0] << "');\n";
                seq->append_raw_glyph(glyph.glyph, chars[0]);
            } else if (chars.size() > 1) {
                // std::cout << "  seq->append_ligature_glyph(" << glyph.glyph << ", \"" << text.c_str() << "\");\n";
                seq->append_ligature_glyph(glyph.glyph, text.c_str());
            }

            prev_advance = span.font->Advance(glyph.glyph, false); // glyph.advance;
            prev_Tm = Tm;
        }
    }
    if (seq) {
        _tx.cmd_TJ(*seq);
    }
}

/**
 * Use the text object as a clipping mask in the PDF
 */
void DrawContext::clip_text_layout(Text::Layout const &layout)
{
    using Inkscape::Text::Layout;

    if (!layout.outputExists()) {
        return;
    }

    auto tx = TextContext(_doc, _ctx, _soft_mask);
    tx.set_text_mode(CAPY_TEXT_CLIP);

    auto &input_stream = layout.input_stream();
    for (Layout::Span const &span : layout.spans()) {
        auto text_source = static_cast<Layout::InputStreamTextSource const *>(input_stream[span.in_input_stream_item]);
        if (text_source->Type() != Layout::InputStreamItemType::TEXT_SOURCE || span.line(&layout).hidden ||
            !span.font) {
            continue;
        }
        if (!tx.set_text_style(span.font, text_source->style)) {
            std::cerr << "Can't clip to text, no font available\n";
            continue;
        }
        tx.render_text(layout, span);
    }
    try {
        tx.finalize();
    } catch (std::exception const &err) {
        std::cerr << "Can't output text block:" << err.what() << "\n";
    }
}

/**
 * Paint the given layout into the PDF document Drawing content.
 *
 * @arg layout - The Inkscape Text::Layout (libnrtype) to add to the PDF.
 */
void DrawContext::paint_text_layout(Text::Layout const &layout, SPStyle const *context_style)
{
    using Inkscape::Text::Layout;

    if (!layout.outputExists()) {
        return;
    }

    auto tx = TextContext(_doc, _ctx, _soft_mask);

    // Copy the paint style memory as the entire text block has a continuous style which does
    // inherit from what was set just before this call, but may also modify styles in a linear fashion.
    StyleMemory text_paint_memory = _doc.paint_memory();

    auto &input_stream = layout.input_stream();
    for (Layout::Span const &span : layout.spans()) {
        auto text_source = static_cast<Layout::InputStreamTextSource const *>(input_stream[span.in_input_stream_item]);

        if (text_source->Type() != Layout::InputStreamItemType::TEXT_SOURCE || span.line(&layout).hidden ||
            !span.font) {
            // Hidden spans correspond to text overlfow.
            continue;
        }

        // This non-scoped memory means the PDF lacks style scope within the Text Block.
        auto style = text_source->style;
        auto style_map = text_paint_memory.get_changes_and_remember(style);
        tx.set_paint_style(style_map, style, context_style);

        if (!tx.set_text_style(span.font, style)) {
            std::cerr << "Can't export text, no font available\n";
            continue;
        }

        for (auto layer : get_paint_layers(style, context_style)) {
            switch (layer) {
                case PAINT_FILLSTROKE:
                    tx.set_text_mode(CAPY_TEXT_FILL_STROKE);
                    tx.render_text(layout, span);
                    break;
                case PAINT_FILL:
                    tx.set_text_mode(CAPY_TEXT_FILL);
                    tx.render_text(layout, span);
                    break;
                case PAINT_STROKE:
                    tx.set_text_mode(CAPY_TEXT_STROKE);
                    tx.render_text(layout, span);
                    break;
                case PAINT_MARKERS:
                    break; // NOT ALLOWED
            }
        }
    }

    try {
        tx.finalize();
    } catch (std::exception const &err) {
        std::cerr << "Can't output text block:" << err.what() << "\n";
    }
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
