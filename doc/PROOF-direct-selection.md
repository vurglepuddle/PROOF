# Direct Selection in the node tool

Under the Illustrator keymap, A switches to the node tool. These changes bring
its mouse behaviour closer to Illustrator's Direct Selection tool.

## Behaviour

- **Press on an object** outside any handle, with no modifier: the object is
  selected on its own, including an object inside a group.
  - If the pointer is on one of its anchors (within the grab sensitivity),
    only that anchor is selected. Otherwise all its anchors are.
  - A drag then moves what was selected. An unselected object's anchor can be
    selected and dragged in one gesture, and dragging the fill moves the whole
    object.
  - Shift during the drag keeps it horizontal, vertical or diagonal. Snapping
    uses the grabbed anchor, or the anchor nearest the press.
  - Escape during the drag puts everything back. The move is one undo step.
  - Dragging a live shape's fill moves the whole shape without converting it.
    Stars, text, images and shapes that retain their native handles also move whole.
- **Previously** (Inkscape), the first click only selected the object. Its
  anchors were selectable from the second click. A drag over an object drew a
  selection box instead of moving anything.
- **Empty canvas** still starts a selection box. Presses with Shift, Ctrl or
  Alt keep Inkscape's behaviour (toggle, select in groups, touch selection).
- **Straight segments:** dragging one moves it parallel, both anchors
  following, with the same Shift constraint, snapping and Escape. Alt+drag
  bends it into a curve, as Inkscape always did. Curved segments still
  reshape as before.
- **Escape and Deselect** with nothing selected no longer switch to the
  selection tool.

- **Ordinary live shapes:** rectangles (including rounded ones), circles,
  ellipses, polygons and polylines expose their path anchors. A click keeps the
  original SVG shape. Moving an anchor stages a preview, then replaces the shape
  with a path on commit, preserving ID, paint, parent, stacking and transforms.
  Escape restores the original shape; one Undo restores a committed conversion.
- **Presentation:** Direct Selection shows the path and anchors without the
  dashed object bounding box. Stationary anchors from the selected path are
  supplied to snapping during a direct anchor drag.
- **A path that paints nothing** (`paints_nothing` in `box-frame.h`: no fill
  or stroke that shows, fully transparent, or set invisible) shows its outline
  as a 1 px line in the selection box's blue, whatever the outline preference,
  and loses it when it shows again. Inkscape showed only its anchors, and the
  path itself was hard to find (the owner, 2026-10-22, reporting it for masks,
  whose objects are invisible). The Selection tool outlines it the same way.
- **Clipping paths** (the owner, 2026-10-22: "We should follow Illustrator for
  this"): a clipping path is picked by its outline, as Illustrator's Direct
  Selection picks a mask's path, the outline being all that shows of it.
  - With nothing pressed, the outline of a clipping path within the grab
    sensitivity of the pointer shows as a 1 px line in the selection box's
    blue, wherever it runs, and the status bar says what a click does.
  - A press on it picks it: the object the clipping path is on (the clipped
    group, or a clipped object) is selected, and the clipping path's anchors
    are shown in place of the object's own, outlined in the same blue (no
    longer Inkscape's green). The same press takes the anchor or segment under
    the pointer, as on any path, so one gesture can pick and reshape; anywhere
    else along the outline the drag moves the clipping path alone.
  - A picked clipping path shows Illustrator's centre mark. Dragging it moves
    the picked clipping paths alone, with Shift, snapping, Escape and one Undo
    step as for any move.
  - Shift+press on an outline adds that clipping path; on a picked one it puts
    it down. Escape, or selecting something else, puts it down too.
  - A press on what shows inside the mask selects that object, as before; a
    press where the content is hidden outside the mask selects nothing. In
    Inkscape such a press reached the hidden object, because picking inside a
    group tested the object's own drawing and not its group's clip;
    `SPDesktop::find_items_at_point` now tests every clipped group above it,
    so the Selection tool's picks in groups follow the clip too.
  - The document keeps SVG's form, a `<clipPath>` in `<defs>`: only the path
    there changes. Clipping paths in `objectBoundingBox` units are not picked.
- **No box around selected anchors:** Inkscape's transformation handles for
  selected nodes (scale, then rotate and skew) are not part of Direct Selection.
  While they showed, a click on a selected anchor switched their mode where it
  should have selected that anchor alone. The node toolbar's button and the
  preference for single nodes are gone with them.
- **Anchor size:** a corner anchor is a diamond 7 px across at the default
  handle size and 9 px when selected. Its preceding 9/11 px pick area remains
  available independently of the visible marker. Smooth anchors are smaller
  squares too. The end of a Bezier handle
  is a smaller circle with a type of its own, `.proof-node-handle` in
  `share/ui/node-handles.css`, so that the handles of other tools keep theirs.
- **An object that goes:** a path's manipulator forgets its object when the
  object is released. The selection only reports what is selected, and a shape
  inside a clip or a mask is edited without being selected.

## Remaining scope

[Live Corners](PROOF-live-corners.md) now adds corner widgets, numeric rounding
and editable split anchors to supported paths and rectangles.
[Ellipse arc controls](PROOF-ellipse-arcs.md) retain native arc editing alongside
their anchors. [Independent segments](PROOF-segment-selection.md) add click and
Shift selection, highlights, group movement and exact-edge deletion.
[Align and distribution](PROOF-anchor-alignment.md) now refresh bounds and
neighboring handles after editing selected anchors. [Corner widget display](PROOF-corner-display.md)
adds View visibility and a transformed-angle limit in Node Preferences.
[Corner groups](PROOF-corner-groups.md) now apply selected radius/type edits
across objects with one shared limit and one document Undo step.

Clipping masks continue in the owner's brief (`brief/direct-selection-brief.md`,
"Clipping masks in Direct Selection"): Edit Mask, Edit Contents and Select >
Object > Clipping Masks; Make and Release without paint; paint on a clipping
path (with the owner's side, as the writers read it); the clipping path in
Layers and Objects. Finding an outline walks the visible clipped objects at
each pointer motion, which has not been measured on very large documents.

Stars and spirals, shapes with live path effects or markers, and geometry
animations retain their existing shape editors. Their conversion needs separate
handling. [Circle, polygon and polyline](PROOF-native-primitives.md) use the same
staging code and now have individual live acceptance: 28/28 on the tested
Windows setup, including transformed open polylines and child metadata.

## Implementation

- `NodeTool::_directPress`, `_directDrag`, `_directRelease` and
  `_directCancel` live in `src/ui/tools/node-tool.cpp`. Anchors move through
  `ControlPointSelection::transform` and are committed with
  `COMMIT_MOUSE_MOVE`, as keyboard moves are. Whole objects move through
  `ObjectSet::moveRelative`.
- Clipping paths: `NodeTool::_pressClip`, `_hoverClip`, `_moveClipShapes` and
  `_updateClipCentres`, with `_clip_targets` (the objects whose clipping path is
  picked) consulted by `NodeTool::selection_changed`, which gathers their
  clipping path with `SHAPE_ROLE_CLIPPING_PATH` instead of the object. The
  geometry, `clip_outlines` and `clip_outline_at`, is in `src/box-frame.h`.
  A whole clipping path moves through each shape's `doWriteTransform`, in the
  clipped object's user space.
- `CurveDragPoint::_moveSegment` is in `src/ui/tool/curve-drag-point.cpp`.
- `SelectionHelper::selectNone` is in `src/selection-chemistry.cpp`.
- Ordinary-shape staging/conversion is in `PathManipulator` in
  `src/ui/tool/path-manipulator.cpp`; `MultiPathManipulator` retains its live
  manipulator while the replacement object is installed.
- [Control point grab ownership](PROOF-control-point-grabs.md) ensures deleting
  idle corner widgets during a node drag does not cancel its active grab.

## Verification

`tools/direct-selection-check.py` drives the node tool on
`artifacts/direct-selection/source.svg` with native input. 11 of 11 checks
pass:
- segment move, Shift and Alt, and undo
- one-gesture anchor drag and anchor click on an unselected object
- fill drag for a path and for a rectangle
- Escape mid-drag
- selection box from empty canvas
- Escape keeps the tool

`tools/direct-selection-explore.py` takes screenshots of the remaining
differences.

Clipping paths (2026-10-22, collaborator): `test_box-frame` 19/19, with
`ClippingPathsArePickedByTheirOutline` for the outlines, their transforms and
the nearest pick. `tools/clip-selection-check.py` passes 14/14 on the installed
build: hover outline shown and gone, a click picks without changing anything,
Escape puts it down, a one-gesture edge drag moves that edge alone and Undo
restores it, a centre drag moves the clipping path alone and Undo restores
it, Escape mid-drag, and clicks inside and outside the mask. Its first run
failed only the last check, which led to the picking fix above. The same build
passes `direct-selection-check.py` 15/15 and `segment-selection-check.py`
30/30. Windows 11, 1858x1080 window at 100%.

The collaborator's 2026-10-11 installed build passes
`tools/shape-anchor-check.py` 33/33: rectangle, ellipse, rounded rectangle,
transformed/grouped rectangle and native CMYK rectangle, with deferred
conversion, single-anchor movement, paint/parent retention, Undo/Redo,
cancellation and whole live-shape movement. Native CMYK channels are exact;
RGB-to-ICC serialization additionally requires unchanged rendered paint.
The original path check was rerun on the installed editing build: 11/11 pass.
`tools/selection-snap-check.py` passes 2/2: a drag ending several pixels away
from a stationary anchor snaps to its exact saved position, and one Undo
restores the original path. Local evidence is under
`../../artifacts/shape-anchors/run-001650`,
`../../artifacts/selection-snap/run-003808` and
`../../artifacts/direct-selection/run-004133`.

Reference: [Adobe selection tools](https://helpx.adobe.com/illustrator/desktop/manage-objects/select-objects/select-transform-objects-selection-tools.html).
