# Blend modes reach what lies under a group

2026-10-13. Two faults with one cause, on the canvas and in the exported PDF.

- Code: `src/display/drawing-item.cpp` (`DrawingItem::render`) and
  `src/extension/internal/pdfoutput/build-drawing.cpp` (`is_isolated`).
- Checks: `testfiles/src/clip-blend-test.cpp` (`test_clip-blend`) and
  `testfiles/cli_tests/blend-export-smoke.py`.

## What was wrong

A shape set to Multiply, Overlay or another blend mode is meant to blend with
whatever lies under it. A shadow in Multiply darkens the art below. That is how
Illustrator draws it and how a PDF page draws it, and illustration files are
full of it: shading clipped to a body, a texture in Overlay over a poster.

In two places PROOF drew such a shape on a surface of its own first, where it
met nothing, and it came out as its own flat colour.

| Where the shape lies | Canvas before | Exported PDF before | Both now |
|---|---|---|---|
| Straight on the page | Right | Right | Right |
| In a plain group | Right | **Flat: no darkening** | Right |
| In a layer | Right | **Flat** | Right |
| In a clipped group | **Flat** | **Flat** | Right |
| In a group with a blend mode of its own | Right | Right | Right |

"Flat" on the PDF side is the serious one: a document with its shadows in a
layer showed correctly and printed without them.

## What isolates a group, and still does

A group is drawn on its own, and what is in it blends only inside it, when it
has any of these. Nothing changed for them, on the canvas or in the PDF.

- Opacity below 1.
- A mask or a filter.
- A blend mode of its own.
- `isolation:isolate`.

A plain group, a layer and a group that only has a clipping path are not
isolated. A clipping path says where the art shows, and nothing more.

## The canvas

Inkscape's own bookkeeping already agreed. A group is told whether it holds a
blend that isn't isolated (`_contains_unisolated_blend`), a clipping path was
never counted as isolating there, and such a group is never cached. Only the
drawing itself put the clipped group on an empty surface.

For a group that has a clipping path, holds a blend that isn't isolated, and
has none of the things in the list above:

1. The clipping path is drawn to a surface of its own: where it lets through.
2. A second surface starts as a copy of what is already drawn under the group.
3. The group is drawn onto that copy, so what is in it blends with the copy.
4. The result goes back through the clipping path: what was there, less the
   path's share, plus the result's share.

Every other group takes the path it always took. The cost is one more surface
for the groups it applies to.

## The exported PDF

The exporter writes every group as a transparency group, and marked each one
isolated. Such a group starts from nothing, whatever lies under it on the page.
It now marks a group isolated only when the canvas isolates it (`is_isolated`,
the list above). Shapes, clones, text and pictures are written as before.

## What to know

- **An SVG from PROOF can look different in a browser.** Browsers draw a
  clipped group on its own. A document with a blend inside a clipped group
  shows and prints from PROOF as in Illustrator, and shows in a browser as
  PROOF showed it before.
- **A clone of a group** (`<use>`) is still written to the PDF as an isolated
  group. A blend inside a cloned group prints flat. Not done.
- **Knockout groups are still drawn as ordinary groups.** The `.ai` import
  says how many a document has.
- **A group with opacity below 1 that isn't isolated in Illustrator** is
  isolated here. In Illustrator a blend inside such a group reaches what is
  under it unless Isolate Blending is on. Not done.

## Checks

`test_clip-blend`, five cases on the canvas: a shade in a clipped group darkens
the backdrop and nothing outside the path; drawn in parts it is the same
picture as drawn whole (the canvas draws in tiles); one clipped group in
another; a shape under the shade in the same group counts; groups with opacity
or isolation, and clipped groups with no blend, are as before.

`blend-export-smoke.py`, eight drawings, each exported as a picture and as a
PDF and read at its middle and at a corner: 32 checks. On the build before this
change 8 of them fail: the cases the table above marks flat.
`tools\run-focused-suites.ps1` in the workspace runs both.
