# PROOF type controls

Fonts that aren't installed, and Illustrator's four Character options: all
caps, small caps, superscript and subscript. First slice: 2026-10-11.

Reading these from `.ai` files is described in
[PROOF-ai-native-records.md](PROOF-ai-native-records.md). This document is
about the editor.

## What works now

- **Missing Fonts.** When a document opens with text whose font isn't
  installed, a window lists those fonts, each with the number of text objects
  that use it. Text > Missing Fonts… shows the same window at any time. On
  closing it can select the text still set in another font. The one that comes
  with a document can be told not to come again (Preferences > Text brings it
  back).
- **Replace.** Pick a missing font in the list, choose a family and a style
  under it, and press Replace. Every text and span that used the missing font
  is set in the chosen one, across the document, as one step that Undo takes
  back. The list then shows what is still missing.
- **A font that gets installed comes back by itself** the next time the
  document opens (since 2026-10-10).
- **Pink highlight.** Text that is set in another font because its own isn't
  installed has Illustrator's pink behind it on the canvas, line by line. It
  goes when the font is replaced, installed, or chosen by hand. It is drawn
  over the canvas only, so exports, prints and saved files never have it.
  Preferences > Text switches it off.
- **Four Character buttons in the text toolbar:** All Caps, Small Caps,
  Superscript, Subscript. Each shows whether the selected text is that. All
  caps and small caps are one or neither, as are superscript and subscript.
  With nothing selected they set what is typed next.
- **The text keeps what was typed and its size.** All caps and small caps
  don't change the characters, and superscript and subscript don't change the
  font size: the size field goes on showing the type's own size, as in
  Illustrator.
- **Superscript, subscript and small caps follow the document's settings**
  (Illustrator's Document Setup > Type): how much smaller, and how far off the
  baseline. A document from Illustrator brings its own.

Not yet:

- **Editing the document's settings.** They are read from `.ai` files and
  kept, and there is no control for them.
- **Underline and strikethrough buttons.** Both are read from `.ai` files and
  drawn.
- **Highlighting single characters a font lacks.** Text with such characters
  is marked (`proof:glyphs-missing`) when it is imported, and that mark isn't
  kept up when the text is edited.
- **Other software.** A browser draws `font-variant-position` at full size
  unless the font has superiors of its own, so superscripts and subscripts
  saved as plain SVG look different there. PDF and PNG output come from the
  layout and are right.

## How it is stored

All of it is ordinary style, so it survives editing, copying and pasting:

| Option | Style | Drawn by |
|---|---|---|
| All caps | `text-transform:uppercase` | the layout, one character for one |
| Small caps | `font-variant-caps:small-caps` | the font's own small capitals; else its capitals, smaller |
| Superscript | `font-variant-position:super` | the font's ordinary characters, smaller and raised |
| Subscript | `font-variant-position:sub` | the same, lowered |

Stock Inkscape asks the font for its superior and inferior figures
(`sups`, `subs`) for `font-variant-position`, and draws nothing different when
the font has none. PROOF always makes them from the ordinary characters, which
is what Illustrator's buttons do. A font's own superiors are still available as
`font-feature-settings:'sups'`.

Inkscape's earlier superscript button wrote `baseline-shift:super` and
`font-size:65%`. Text like that still draws as before, and the buttons show it
as down. Pressing a button puts such text back on its line at its size first.
A baseline shift given as a length is the type's own and is left alone: it adds
to the superscript's.

The sizes are on the document's root element. What is Illustrator's own isn't
written:

```xml
<svg proof:superscript="0.5 0.4" proof:subscript="0.6 0.2" proof:small-caps="0.65">
```

| Attribute | Values | When absent |
|---|---|---|
| `proof:superscript` | size, distance above the baseline | 0.583, 0.333 |
| `proof:subscript` | size, distance below the baseline | 0.583, 0.333 |
| `proof:small-caps` | size | 0.7 |

Each is a share of the type's size. Changing an attribute lays the document's
text out again.

`unicode-bidi:bidi-override` (and `isolate-override`) is known to the text
layout since 2026-10-11: every character of the element is then set the way
its `direction` says, whatever way its script reads. Hebrew that Illustrator
set from the left as typed comes in with it, and shows as it does there. The
property isn't inherited; it holds for what an element contains, down to one
inside that says otherwise. The other values are read and change nothing.

Text whose font isn't installed carries the font's PostScript name in
`proof:font-missing` on the text or span whose style sets the font, and its
style names no `-inkscape-font-specification`. Choosing a font, with Replace or
with the font controls, writes one; from then on the text is simply in that
font.

## Where the code is

| Part | File |
|---|---|
| The document's settings | `src/type-settings.h` |
| Layout: sizes and shifts | `src/libnrtype/Layout-TNG-Input.cpp` (`appendText`), `Layout-TNG-Compute.cpp` |
| Missing fonts: find, restore, replace, which characters | `src/extension/internal/ai/ai-native-fonts.h` |
| The Missing Fonts window | `src/ui/dialog/font-substitution.cpp` |
| The highlight | `src/display/control/font-highlight.h` |
| The buttons | `src/ui/toolbar/text-toolbar.cpp` (`caps_changed`, `script_changed`) |
| What the buttons read | `src/desktop-style.cpp` (`objects_query_fontvariants`) |

## Checks

A live check in the workspace repository, `tools	ype-controls-check.py`, 25
checks on `tests	ype-controls.svg` with the real mouse and keyboard: the
window at opening and from the menu, the highlight on stand-in type and on no
other, Replace with its highlight and its saved style, Undo, and each of the
four buttons with what it writes, how the text then measures on the canvas,
and whether the button shows down.

Unit tests, in `test_ai-native-import`:

- `SuperscriptAndSubscriptAreMadeByTheLayout`: sizes and places against type
  set at plain sizes and shifts, the document's settings, and a change of them.
- `SmallCapitalsAreMadeOfCapitalsForAFontWithoutThem`: the document's size, and
  small caps above the line.
- `AFontThatIsNotInstalledIsReplacedAcrossTheDocument`: Replace, and which
  characters a font stands in for.
- `TheHighlightLiesOnTheTypeAFontStandsInFor`: where the highlight goes.
- `TheCharacterButtonsReadWhatTheTextIs`: what the buttons show for text that
  agrees and text that differs.
