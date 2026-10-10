// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Bounded reading of Illustrator point-type and area-type stories. The numeric key
 * meanings follow VectorCraft's ate.rs at 4cf912f (MIT; LICENSES/MIT-VectorCraft.txt),
 * Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors.
 * PROOF keeps explicit line origins and UTF-16 run lengths, reads area type's frames,
 * recorded lines and baseline shifts (worked out from real files, 2026-10-10), and
 * refuses missing fonts in the builder and unsupported frames/styles rather than
 * substituting.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_TEXT_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_TEXT_H

#include "ai-native-reader.h"

namespace Inkscape::Extension::Internal::AiNative {

struct TextStyle
{
    std::string font; ///< Exact PostScript font name.
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
    unsigned alignment = 0; ///< 0 left, 1 right, 2 centre; preserves the editable anchor.
};

/**
 * A story as Illustrator laid it out: point type, or area type in one frame.
 *
 * Area type keeps Illustrator's own lines: the text document records where each line
 * starts and how many characters it holds, so a paragraph breaks where it broke there.
 * The frame is kept beside the lines; nothing here flows text into it again.
 */
struct PointText
{
    Geom::Affine to_art; ///< Text layout (y down) to art space (y up).
    std::vector<TextLine> lines;
    bool area = false;      ///< Area type.
    Geom::PathVector frame; ///< Area type: the frame's outline, in the layout's space.
    /// Area type: the text its frame is too small to show, a line feed for each line end.
    /// Illustrator doesn't draw it either.
    std::string overflow;
    bool empty = false;     ///< Nothing typed and nothing laid out: an empty frame.
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
    std::vector<std::optional<PointText>> stories;
    std::vector<std::string> errors;
    std::string error; ///< Malformed/missing document or template centre.
    std::vector<std::optional<PointText>> drawn; ///< The stories of type appearances drew.
    std::vector<std::string> drawn_errors;
};

std::shared_ptr<TextDocument const> read_text_document(std::string_view records,
                                                      std::optional<Geom::Point> template_center);

/// Whether two stories set the same type in the same place: what an appearance drew is
/// then nothing but the object it stands for.
bool same_drawing(PointText const &a, PointText const &b);

} // namespace Inkscape::Extension::Internal::AiNative
#endif
