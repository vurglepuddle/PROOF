// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_WRITER_TEXT_H
#define SEEN_AI_NATIVE_WRITER_TEXT_H

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <2geom/affine.h>
#include <2geom/path.h>
#include <2geom/point.h>

class SPDocument;
class SPIPaint;
class SPText;

namespace Inkscape::Extension::Internal::AiNative {

/// What adding a text object's story gave.
struct WrittenStory
{
    std::size_t story = 0;            ///< Its index among the typed stories.
    bool empty = false;               ///< Nothing is typed in it: there is nothing to write.
    std::vector<std::string> refused; ///< Why it can't be written; then nothing was added.
    /// Area type's frame, or the path type is set on, in the text's own coordinates. The text
    /// object carries it too, as its `/Art` path.
    std::optional<Geom::Path> frame;
};

/**
 * Type as Illustrator keeps it: stories in the text documents of the records
 * (`%AI11_BeginTextDocument`), each text object an `/AI11Text` naming its story.
 *
 * A story is what was typed, its paragraphs and its runs of character features, in a frame.
 * Illustrator 29.2.1 lays stories out again from those when it opens a file, whatever layout
 * the story keeps (type spike, 2026-10-23): PROOF writes the layout only for its own reader,
 * its lines where PROOF sets them.
 *
 * Point type: each line of the text object one paragraph (or a line break inside one, where
 * the import kept one), the lines sharing one anchor and each set below the one before by its
 * leading. Area type: the frame an import kept, or the one shape Inkscape flows the text into;
 * its lines joined into paragraphs where the frame wrapped them, each paragraph aligned and
 * hyphenated as its lines say (an import's justified paragraph in a rectangular frame too,
 * its last line as it says), and what the frame was too small to show after them. Type on a
 * path: one line along one piece of path, between brackets an import kept (what Illustrator
 * showed of it) or, for Inkscape's own, where its start offset and alignment set it.
 * All in fonts that are installed (or, for type set in a font standing in for one that isn't,
 * that font's name), process colour and spot ink fills and strokes (an ink by an art style, as
 * Illustrator names one), tracking, baseline shift, kerning on or
 * off, standard and contextual ligatures, all caps, small caps, superscript and subscript,
 * underline and strikethrough (the last four and baseline shift not on a path). The frame's
 * matrix places it; a uniform scale goes into the sizes. Bounded horizontal Arabic/Hebrew,
 * uniform paragraph direction and LTR typed-order overrides are supported, with the Latin or
 * Middle Eastern composer kept independently. Imported LTR left-tab rulers are supported.
 * Vertical type, bidi path type, nested overrides, bidi tracking/trailing spaces/tabs, mixed
 * LTR type flowed into a shape, justified type Inkscape flows into a shape, justified type that
 * reads from the right and individually positioned characters remain refused by name; hidden
 * RTL overflow also needs separate qualification.
 */
class TextStories
{
public:
    explicit TextStories(SPDocument const *document);

    /// The story of `text`. `to_canvas` places the text's own coordinates on Illustrator's
    /// canvas (y down, in points); the document must be up to date.
    WrittenStory add(SPText const *text, Geom::Affine const &to_canvas);

    bool empty() const { return _stories.empty(); }

    /// The records' text section: both text documents, the stories in the first.
    /// `ruler_origin`: where art space's 0,0 lies on the canvas.
    std::string section(Geom::Point const &ruler_origin) const;

    /// The records' art styles: one for each ink type is painted with, each its ink's line, which
    /// the characters name (`Anon PROOF 1` and on). Empty when there are none. Before the text
    /// section, as Illustrator writes them.
    std::string art_styles() const;

    /// An ink's swatch as the colour operator that fills with it, the writer's own for paths
    /// (`c m y k (name) tint x`, `… type Xx`, Registration's `Xs` and `Xz`), declaring the ink;
    /// nothing when the writer refuses it, which it says. Illustrator's type takes an ink by an
    /// art style holding that line, for its fill and its stroke alike (Illustrator 29.2.1,
    /// 2026-10-24). Without it, type in an ink is refused.
    std::function<std::optional<std::string>(SPIPaint const &)> ink_operator;

private:
    std::size_t font(std::string const &name, bool truetype, std::vector<double> const &design);
    std::string document(bool typed) const;

    SPDocument const *_document;
    std::vector<std::string> _fonts;              ///< Each font's entry, in index order.
    std::map<std::string, std::size_t> _font_at;  ///< A font's name and axes, to its index.
    std::vector<std::string> _frames, _stories;
    std::vector<std::string> _styles; ///< Each art style's ink line, in the order of their names.
};

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_TEXT_H
