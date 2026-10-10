// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Bounded reading of Illustrator point-type stories. The numeric key meanings
 * follow VectorCraft's ate.rs at 4cf912f (MIT; LICENSES/MIT-VectorCraft.txt),
 * Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors.
 * PROOF keeps explicit line origins and UTF-16 run lengths, and refuses missing
 * fonts in the builder and unsupported frames/styles rather than substituting.
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

struct PointText
{
    Geom::Affine to_art; ///< Text layout (y down) to art space (y up).
    std::vector<TextLine> lines;
};

struct TextDocument
{
    std::vector<std::optional<PointText>> stories;
    std::vector<std::string> errors;
    std::string error; ///< Malformed/missing document or template centre.
};

std::shared_ptr<TextDocument const> read_text_document(std::string_view records,
                                                      std::optional<Geom::Point> template_center);

} // namespace Inkscape::Extension::Internal::AiNative
#endif
