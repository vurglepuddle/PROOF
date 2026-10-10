// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: how a document sets superscripts, subscripts and small capitals.
 *
 * Illustrator makes all three of a font's ordinary characters: a superscript or subscript
 * is the character smaller and moved off the baseline, and small capitals a font has none
 * of are its capitals, smaller. How much smaller and how far is the document's to say
 * (Document Setup, Type), each a share of the type's size. They are kept on the document's
 * root element:
 *
 *   proof:superscript  size and distance above the baseline, e.g. "0.583 0.333"
 *   proof:subscript    size and distance below it
 *   proof:small-caps   size
 *
 * A document that says nothing has Illustrator's own, the values written here.
 *
 * In the document the type says which it is with font-variant-position (super, sub) and
 * font-variant-caps (small-caps), and keeps its font-size: the text layout does the rest.
 */
#ifndef SEEN_INKSCAPE_TYPE_SETTINGS_H
#define SEEN_INKSCAPE_TYPE_SETTINGS_H

class SPDocument;

namespace Inkscape {

struct TypeSettings
{
    double superscript_size = 0.583, superscript_shift = 0.333;
    double subscript_size = 0.583, subscript_shift = 0.333;
    double small_caps_size = 0.7;

    bool operator==(TypeSettings const &) const = default;

    /// Whether these are values a document can have: Illustrator takes sizes up to twice
    /// the type's own, and a distance can be none.
    bool valid() const;

    /// The document's own; Illustrator's for what it doesn't say, or says wrongly.
    static TypeSettings of(SPDocument const *document);

    /// Says them on the document. What is Illustrator's own isn't written.
    void write(SPDocument *document) const;

    /// Whether `name` is an attribute of the root element that holds one of these.
    static bool is_attribute(char const *name);
};

} // namespace Inkscape

#endif // SEEN_INKSCAPE_TYPE_SETTINGS_H
