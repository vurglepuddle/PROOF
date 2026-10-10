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

## Remaining scope

Stars and spirals, shapes with live path effects or markers, and geometry
animations retain their existing shape editors. Their conversion needs separate
handling. Circle, polygon and polyline support uses the same staging code;
those three primitives still need individual live acceptance cases.

## Implementation

- `NodeTool::_directPress`, `_directDrag`, `_directRelease` and
  `_directCancel` live in `src/ui/tools/node-tool.cpp`. Anchors move through
  `ControlPointSelection::transform` and are committed with
  `COMMIT_MOUSE_MOVE`, as keyboard moves are. Whole objects move through
  `ObjectSet::moveRelative`.
- `CurveDragPoint::_moveSegment` is in `src/ui/tool/curve-drag-point.cpp`.
- `SelectionHelper::selectNone` is in `src/selection-chemistry.cpp`.
- Ordinary-shape staging/conversion is in `PathManipulator` in
  `src/ui/tool/path-manipulator.cpp`; `MultiPathManipulator` retains its live
  manipulator while the replacement object is installed.

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
