// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Bounded reading of Illustrator point-type and area-type stories. The numeric key
 * meanings follow VectorCraft's ate.rs at 4cf912f (MIT; LICENSES/MIT-VectorCraft.txt),
 * Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors.
 * PROOF keeps explicit line origins and UTF-16 run lengths, reads area type's frames,
 * recorded lines, baseline shifts, all caps and small caps, superscript and subscript,
 * underline and strikethrough, paragraphs that read from right to left, and the axis values
 * of a variable font's instance (worked out from real files, 2026-10-10), which line ends
 * were typed and how each line's paragraph is aligned and hyphenated (2026-10-23), and refuses
 * unsupported frames and styles rather than guessing at them.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_TEXT_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_TEXT_H

#include "ai-native-reader.h"
#include "type-settings.h"
#include "libnrtype/tab-stops.h"

namespace Inkscape::Extension::Internal::AiNative {

/// A pattern that paints type: which one, and where its tile lies.
struct TextPattern
{
    std::string name;    ///< The pattern's name, as the records define it.
    Geom::Affine matrix; ///< The pattern's own space to art space, as for a path's.
    bool operator==(TextPattern const &) const = default;
};

struct TextStyle
{
    std::string font; ///< Exact PostScript font name.
    /// For an instance of a variable font: its value on each axis, in the font's own order.
    std::vector<double> design;
    double size = 12;
    std::optional<Color> fill, stroke;
    double fill_opacity = 1, stroke_opacity = 1, stroke_width = 1;
    double tracking = 0; ///< Thousandths of an em.
    double horizontal_scale = 1, vertical_scale = 1;
    double baseline_shift = 0; ///< Points above the baseline; below it when negative.
    /// How pairs of characters are kerned: 0 not at all, 1 by the font's own kerning,
    /// 2 optically (Illustrator's own measure of the shapes), 3 by the font's for Roman only.
    int kerning = 1;
    bool ligatures = true;  ///< Standard ligatures.
    bool contextual = true; ///< Contextual alternates.
    bool all_caps = false;  ///< Set in capitals, whatever was typed.
    /// Small letters set as small capitals: the font's own where it has them, else its
    /// capitals at the size the document says (TextDocument::type).
    bool small_caps = false;
    bool underline = false;
    bool strikethrough = false;
    /// The fill or the stroke is a gradient by name. Illustrator doesn't draw a gradient on
    /// type: the characters are black, which is what `fill` or `stroke` then says.
    bool gradient = false;
    /// The fill or the stroke is a pattern, by name. `fill` or `stroke` is then set too, to
    /// the black the text document keeps beside the name, and isn't what is drawn.
    std::optional<TextPattern> fill_pattern, stroke_pattern;
    /// Where the characters stand: 0 on the line, 1 superscript, 2 subscript. Illustrator
    /// makes those of the same font, smaller and moved, by what the document says
    /// (TextDocument::type). `size` and `baseline_shift` stay the type's own.
    int position = 0;
    double position_size = 1; ///< Their size, as a share of `size`.
    /// The size the characters are set at.
    double set_size() const { return size * position_size; }
};

struct TextRun
{
    std::string text;
    TextStyle style;
};

struct TextLine
{
    Geom::Point origin; ///< Explicit line anchor in text layout space (y down).
    std::vector<TextRun> runs;
    /// Which end of the line `origin` is: 0 left, 1 right, 2 the middle, whichever way the
    /// paragraph reads. It preserves the editable anchor.
    unsigned alignment = 0;
    std::optional<Text::LeftTabStops> tabs; ///< Live left-aligned paragraph tabs.
    /// How the line ends: with its paragraph (a return), at a line break typed inside the
    /// paragraph (Illustrator's forced line break, U+0003), or where area type's frame wrapped
    /// it, with nothing typed.
    enum class End { Paragraph, Break, Wrap } end = End::Paragraph;
    /// How its paragraph is aligned, 0 left, 1 right, 2 centred, whichever end `origin` is.
    unsigned paragraph_alignment = 0;
    bool hyphenate = false; ///< Its paragraph is hyphenated where area type wraps it.
};

/// Ordinary path type retains its full geometry and active text interval.
struct PathText
{
    Geom::PathVector outline; ///< Full editable path, in text layout space.
    double start = 0, end = 0; ///< Active portion, as distances along that path.
};

/**
 * A story as Illustrator laid it out: point, area, or ordinary path type in one frame.
 * Area type retains recorded line breaks; path type stays attached to its full path.
 */
struct PointText
{
    Geom::Affine to_art; ///< Text layout (y down) to art space (y up).
    std::vector<TextLine> lines;
    std::optional<PathText> path; ///< Ordinary baseline-aligned type on a path.
    bool area = false;      ///< Area type.
    bool rtl = false;       ///< Its paragraphs read from right to left (Hebrew, Arabic).
    /// It has letters of a script read from the right that Illustrator set from the left,
    /// one after another as they were typed: it shows them backwards, and so are they set
    /// here. Its paragraphs read from the left.
    bool unordered = false;
    Geom::PathVector frame; ///< Area type: the frame's outline, in the layout's space.
    /// Area type: how the frame places its first baseline, when it says (its 10): the rule, 0
    /// fixed at the minimum, 1 Ascent (Illustrator's default), 2 cap height, 3 leading,
    /// 4 x-height, 5 em box, 6 legacy; and the minimum, in points.
    std::optional<std::pair<unsigned, double>> first_baseline;
    /// Area type: the text its frame is too small to show, a line feed for each line end.
    /// Illustrator doesn't draw it either.
    std::string overflow;
    bool empty = false;     ///< Nothing typed and nothing laid out: an empty frame.
    /// Named paragraph styles were resolved to inline formatting; their links are not retained.
    bool paragraph_styles_expanded = false;
    /// Where Illustrator set each run of glyphs, in the layout's space: what its type covers
    /// on the page, whatever font sets it here.
    std::vector<Geom::Rect> set;
};

/**
 * The stories of the records.
 *
 * There are two documents of them. The first holds what was typed. The second
 * (AI11UndoFreeTextDocument) holds the type that appearances drew: an object with an
 * appearance is written as what that draws, and the type among it is a text object marked
 * FreeUndo, whose story number counts in this second document, not the first.
 */
struct TextDocument
{
    /// How the document sets superscripts, subscripts and made-up small capitals.
    TypeSettings type;
    std::vector<std::optional<PointText>> stories;
    std::vector<std::string> errors;
    std::string error; ///< Malformed/missing document or template centre.
    std::vector<std::optional<PointText>> drawn; ///< The stories of type appearances drew.
    std::vector<std::string> drawn_errors;
};

/**
 * The text documents of `records`.
 *
 * Stories are placed on Illustrator's canvas. Where the art lies on it is what the text
 * section's own `/RulerOrigin` says; `template_center` (the document's centre in art space,
 * taken to be the middle of the canvas) stands in when the section doesn't say.
 */
std::shared_ptr<TextDocument const> read_text_document(std::string_view records,
                                                      std::optional<Geom::Point> template_center);

/// Whether two stories set the same type in the same place: what an appearance drew is
/// then nothing but the object it stands for.
bool same_drawing(PointText const &a, PointText const &b);

} // namespace Inkscape::Extension::Internal::AiNative
#endif
