// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_WRITER_TEXT_H
#define SEEN_AI_NATIVE_WRITER_TEXT_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include <2geom/affine.h>
#include <2geom/point.h>

class SPDocument;
class SPText;

namespace Inkscape::Extension::Internal::AiNative {

/// What adding a text object's story gave.
struct WrittenStory
{
    std::size_t story = 0;            ///< Its index among the typed stories.
    bool empty = false;               ///< Nothing is typed in it: there is nothing to write.
    std::vector<std::string> refused; ///< Why it can't be written; then nothing was added.
};

/**
 * Type as Illustrator keeps it: stories in the text documents of the records
 * (`%AI11_BeginTextDocument`), each text object an `/AI11Text` naming its story.
 *
 * A story is what was typed, its paragraphs and its runs of character features, in a frame.
 * Illustrator 29.2.1 and 30.6 lay stories out again from those when they open a file, whatever
 * layout the story keeps (type spike, 2026-10-23): PROOF writes the layout only for its own
 * reader, its lines where PROOF sets them.
 *
 * The supported form is point type: each line of the text object one paragraph, the lines
 * sharing one anchor and each set below the one before by its leading, in fonts that are
 * installed (or, for type set in a font standing in for one that isn't, that font's name),
 * process colour fills and strokes, tracking, baseline shift, kerning on or off, standard and
 * contextual ligatures, all caps, small caps, superscript and subscript, underline and
 * strikethrough. The frame's matrix places it; a uniform scale goes into the sizes. Area type,
 * type on a path, vertical type, type that reads from right to left, tabs, characters moved or
 * turned one by one and other features are refused by name.
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

private:
    std::size_t font(std::string const &name, bool truetype, std::vector<double> const &design);
    std::string document(bool typed) const;

    SPDocument const *_document;
    std::vector<std::string> _fonts;              ///< Each font's entry, in index order.
    std::map<std::string, std::size_t> _font_at;  ///< A font's name and axes, to its index.
    std::vector<std::string> _frames, _stories;
};

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_TEXT_H
