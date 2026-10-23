// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Inkscape::Text::Layout - text layout engine input functions
 *
 * Authors:
 *   Richard Hughes <cyreve@users.sf.net>
 *
 * Copyright (C) 2005 Richard Hughes
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#ifndef PANGO_ENABLE_ENGINE
#define PANGO_ENABLE_ENGINE
#endif

#include <cstring>

#include "Layout-TNG.h"
#include "object/sp-object.h"
#include "xml/node.h"
#include "font-instance.h"
#include "style-text.h"
#include "svg/svg-length.h"
#include "font-factory.h"
#include "text-editing.h"
#include "type-settings.h"


namespace Inkscape {
namespace Text {

void Layout::_clearInputObjects()
{
    for(auto & it : _input_stream) {
        delete it;
    }

    _input_stream.clear();
    _input_wrap_shapes.clear();
}

/**
 * PROOF: one character through text-transform.
 *
 * The result is always one character. The layout counts characters to tell where one of
 * its own stands in the source, so a mapping that makes two of one (a sharp s to SS) is
 * left undone: that letter stays as it was typed.
 */
static gunichar transformCharacter(gunichar c, unsigned transform, bool word_start)
{
    switch (transform) {
        case SP_CSS_TEXT_TRANSFORM_UPPERCASE:
            return g_unichar_toupper(c);
        case SP_CSS_TEXT_TRANSFORM_LOWERCASE:
            return g_unichar_tolower(c);
        case SP_CSS_TEXT_TRANSFORM_CAPITALIZE:
            return word_start ? g_unichar_totitle(c) : c;
        default:
            return c;
    }
}

// this function stores all its parameters for future reference, and the text as its style transforms it
void Layout::appendText(Glib::ustring const &text,
                        SPStyle *style,
                        SPObject *source,
                        OptionalTextTagAttrs const *optional_attributes,
                        unsigned optional_attributes_offset,
                        Glib::ustring::const_iterator text_begin,
                        Glib::ustring::const_iterator text_end)
{
    if (style == nullptr) return;

    InputStreamTextSource *new_source = new InputStreamTextSource;

    new_source->source = source;
    new_source->text = &text;
    new_source->text_begin = text_begin;
    new_source->text_end = text_end;
    new_source->style = style;
    /*
     * Clear all input sources when any input source's style is deleted.
     * This is ok, because this is always followed up shortly afterwards
     * with a full teardown + recreation, so there is no visible effect.
     */
    new_source->style_conn = style->object->connectRelease([this] (auto) { clear(); });

    new_source->text_length = 0;
    new_source->laid_begin = text_begin;
    new_source->laid_end = text_end;
    // PROOF: the property isn't inherited; it holds for what an element contains, down to
    // one inside that says something else. So the nearest element that says anything decides.
    for (auto const *holder = style; holder; ) {
        if (holder->unicode_bidi.set) {
            auto const how = holder->unicode_bidi.computed;
            if (how == SP_CSS_UNICODE_BIDI_OVERRIDE || how == SP_CSS_UNICODE_BIDI_ISOLATE_OVERRIDE) {
                new_source->bidi_override = holder->direction.computed == SP_CSS_DIRECTION_RTL ? 1 : 0;
            }
            break;
        }
        auto const *parent = holder->object ? holder->object->parent : nullptr;
        holder = parent && is_part_of_text_subtree(parent) ? parent->style : nullptr;
    }
    // The nearest paragraph ruler survives nested style spans and text edits.
    // Ignore malformed XML settings as a whole.
    for (auto const *holder = source; holder; holder = holder->parent) {
        auto const *repr = holder->getRepr();
        if (repr && repr->attribute("proof:tab-interval")) {
            new_source->tabs = LeftTabStops::read(repr);
            break;
        }
        if (holder != source && !is_part_of_text_subtree(holder)) break;
    }
    // So does whether its lines are aligned as Illustrator aligns them.
    for (auto const *holder = source; holder; holder = holder->parent) {
        auto const *repr = holder->getRepr();
        if (auto const *how = repr ? repr->attribute(LINE_ALIGNMENT_ATTRIBUTE) : nullptr) {
            new_source->illustrator_alignment = !std::strcmp(how, "illustrator");
            break;
        }
        if (holder != source && !is_part_of_text_subtree(holder)) break;
    }
    auto const transform = style->text_transform.computed;
    // A word can run on from the text before this one (a span in the middle of a word).
    bool word_start = true;
    if (transform == SP_CSS_TEXT_TRANSFORM_CAPITALIZE && !_input_stream.empty() && _input_stream.back()->Type() == TEXT_SOURCE) {
        auto const *before = static_cast<InputStreamTextSource const *>(_input_stream.back());
        if (before->text_length > 0) {
            auto last = before->text_end;
            --last;
            word_start = !g_unichar_isalnum(*last) && *last != '\'' && *last != 0x2019;
        }
    }
    // PROOF: small capitals. A font that has them sets its own (the feature string asks for
    // them). For one that hasn't, the small letters are set as capitals, smaller.
    bool made_small_caps = false;
    if (style->font_variant_caps.computed == SP_CSS_FONT_VARIANT_CAPS_SMALL) {
        auto font = new_source->styleGetFontInstance();
        made_small_caps = font && !font->get_opentype_tables().contains("smcp");
    }
    // PROOF: superscript and subscript are made too, whatever the font has: smaller, and
    // off the baseline. How much is the document's to say, as is the size of small capitals.
    auto const position = style->font_variant_position.computed;
    bool const positioned = position == SP_CSS_FONT_VARIANT_POSITION_SUPER || position == SP_CSS_FONT_VARIANT_POSITION_SUB;
    if (made_small_caps || positioned) {
        auto const settings = TypeSettings::of(style->document);
        new_source->small_caps_size = settings.small_caps_size;
        if (position == SP_CSS_FONT_VARIANT_POSITION_SUPER) {
            new_source->position_size = settings.superscript_size;
            new_source->position_shift = settings.superscript_shift;
        } else if (position == SP_CSS_FONT_VARIANT_POSITION_SUB) {
            new_source->position_size = settings.subscript_size;
            new_source->position_shift = -settings.subscript_shift;
        }
    }
    bool const copied = transform != SP_CSS_TEXT_TRANSFORM_NONE || made_small_caps;
    for ( ; text_begin != text_end && text_begin != text.end() ; ++text_begin) {
        new_source->text_length++;        // save this because calculating the length of a UTF-8 string is expensive
        if (copied) {
            auto c = transformCharacter(*text_begin, transform, word_start);
            if (made_small_caps) {
                auto const capital = g_unichar_toupper(c);
                new_source->reduced.push_back(capital != c);
                c = capital;
            }
            new_source->transformed.push_back(c);
            // An apostrophe is inside its word.
            word_start = !g_unichar_isalnum(*text_begin) && *text_begin != '\'' && *text_begin != 0x2019;
        }
    }
    if (copied) {
        new_source->laid_begin = new_source->transformed.begin();
        new_source->laid_end = new_source->transformed.end();
    }

    if (optional_attributes) {
        // we need to fill in x and y even if the text is empty so that empty paragraphs can be positioned correctly
        _copyInputVector(optional_attributes->x, optional_attributes_offset, &new_source->x, std::max(1, new_source->text_length));
        _copyInputVector(optional_attributes->y, optional_attributes_offset, &new_source->y, std::max(1, new_source->text_length));
        _copyInputVector(optional_attributes->dx, optional_attributes_offset, &new_source->dx, new_source->text_length);
        _copyInputVector(optional_attributes->dy, optional_attributes_offset, &new_source->dy, new_source->text_length);
        _copyInputVector(optional_attributes->rotate, optional_attributes_offset, &new_source->rotate, new_source->text_length);
        if (!optional_attributes->rotate.empty() && optional_attributes_offset >= optional_attributes->rotate.size()) {
            SVGLength last_rotate;
            last_rotate = 0.f;
            for (auto it : optional_attributes->rotate)
                if (it._set)
                    last_rotate = it;
            new_source->rotate.resize(1, last_rotate);
        }
        new_source->textLength._set = optional_attributes->textLength._set;
        new_source->textLength.value = optional_attributes->textLength.value;
        new_source->textLength.computed = optional_attributes->textLength.computed;
        new_source->textLength.unit = optional_attributes->textLength.unit;
        new_source->lengthAdjust = optional_attributes->lengthAdjust;
    }
    
    _input_stream.push_back(new_source);
}

void Layout::_copyInputVector(std::vector<SVGLength> const &input_vector, unsigned input_offset, std::vector<SVGLength> *output_vector, size_t max_length)
{
    output_vector->clear();
    if (input_offset >= input_vector.size()) return;
    output_vector->reserve(std::min(max_length, input_vector.size() - input_offset));
    while (input_offset < input_vector.size() && max_length != 0) {
        if (!input_vector[input_offset]._set)
            break;
        output_vector->push_back(input_vector[input_offset]);
        input_offset++;
        max_length--;
    }
}

// just save what we've been given, really
void Layout::appendControlCode(TextControlCode code, SPObject *source, double width, double ascent, double descent)
{
    InputStreamControlCode *new_code = new InputStreamControlCode;

    new_code->source = source;
    new_code->code = code;
    new_code->width = width;
    new_code->ascent = ascent;
    new_code->descent = descent;
    
    _input_stream.push_back(new_code);
}

// more saving of the parameters
void Layout::appendWrapShape(std::unique_ptr<Shape> shape, DisplayAlign display_align)
{
    _input_wrap_shapes.push_back({.shape = std::move(shape), .display_align = display_align});
}

Layout::Direction Layout::InputStreamTextSource::styleGetBlockProgression() const
{
    switch( style->writing_mode.computed ) {
        case SP_CSS_WRITING_MODE_LR_TB:
        case SP_CSS_WRITING_MODE_RL_TB:
            return TOP_TO_BOTTOM;

        case SP_CSS_WRITING_MODE_TB_RL:
            return RIGHT_TO_LEFT;

        case SP_CSS_WRITING_MODE_TB_LR:
            return LEFT_TO_RIGHT;

        default:
            std::cerr << "Layout::InputTextStream::styleGetBlockProgression: invalid writing mode." << std::endl;
    }
    return TOP_TO_BOTTOM;
}

SPCSSTextOrientation Layout::InputStreamTextSource::styleGetTextOrientation() const
{
    return ((SPCSSTextOrientation)style->text_orientation.computed);
}

SPCSSBaseline Layout::InputStreamTextSource::styleGetDominantBaseline() const
{
    return ((SPCSSBaseline)style->dominant_baseline.computed);
}

static Layout::Alignment text_anchor_to_alignment(unsigned anchor, Layout::Direction para_direction)
{
    switch (anchor) {
        default:
        case SP_CSS_TEXT_ANCHOR_START:  return para_direction == Layout::LEFT_TO_RIGHT ? Layout::LEFT : Layout::RIGHT;
        case SP_CSS_TEXT_ANCHOR_MIDDLE: return Layout::CENTER;
        case SP_CSS_TEXT_ANCHOR_END:    return para_direction == Layout::LEFT_TO_RIGHT ? Layout::RIGHT : Layout::LEFT;
    }
}

Layout::Alignment Layout::InputStreamTextSource::styleGetAlignment(Layout::Direction para_direction, bool try_text_align) const
{
    if (!try_text_align)
        return text_anchor_to_alignment(style->text_anchor.computed, para_direction);

    // there's no way to tell the difference between text-anchor set higher up the cascade to the default and
    // text-anchor never set anywhere in the cascade, so in order to detect which of text-anchor or text-align
    // to use we'll have to run up the style tree ourselves.
    SPStyle const *this_style = style;

    for ( ; ; ) {
        // If both text-align and text-anchor are set at the same level, text-align takes
        // precedence because it is the most expressive.
        if (this_style->text_align.set) {
            switch (style->text_align.computed) {
                default:
                case SP_CSS_TEXT_ALIGN_START:   return para_direction == LEFT_TO_RIGHT ? LEFT : RIGHT;
                case SP_CSS_TEXT_ALIGN_END:     return para_direction == LEFT_TO_RIGHT ? RIGHT : LEFT;
                case SP_CSS_TEXT_ALIGN_LEFT:    return LEFT;
                case SP_CSS_TEXT_ALIGN_RIGHT:   return RIGHT;
                case SP_CSS_TEXT_ALIGN_CENTER:  return CENTER;
                case SP_CSS_TEXT_ALIGN_JUSTIFY: return FULL;
            }
        }
        if (this_style->text_anchor.set)
            return text_anchor_to_alignment(this_style->text_anchor.computed, para_direction);
        if (this_style->object == nullptr || this_style->object->parent == nullptr) break;
        this_style = this_style->object->parent->style;
        if (this_style == nullptr) break;
    }
    return para_direction == LEFT_TO_RIGHT ? LEFT : RIGHT;
}

std::shared_ptr<FontInstance> Layout::InputStreamTextSource::styleGetFontInstance() const
{
    PangoFontDescription *descr = styleGetFontDescription();
    if (descr == nullptr) return nullptr;
    auto res = FontFactory::get().Face(descr);
    pango_font_description_free(descr);
    return res;
}

PangoFontDescription *Layout::InputStreamTextSource::styleGetFontDescription() const
{
    // This use to be done by code here but it duplicated more complete code in FontFactory.cpp.
    PangoFontDescription *descr = ink_font_description_from_style( style );

    // Font size not yet set
    // mandatory huge size (hinting workaround)
    pango_font_description_set_size(descr, FontFactory::get().fontSize * PANGO_SCALE);

    return descr;
}

Layout::InputStreamTextSource::~InputStreamTextSource() = default;

}//namespace Text
}//namespace Inkscape
