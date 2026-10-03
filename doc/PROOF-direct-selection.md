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
  - Objects without anchors in the node tool (rectangles, ellipses, stars,
    text, images) move whole.
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

## Not yet

- Rectangles and other shapes still show Inkscape's shape handles in the node
  tool, not four anchors. Illustrator shows anchors and turns the live shape
  into a path when one is moved.
- While moving one anchor, snapping ignores the rest of its own path, because
  the whole selection is excluded from snapping. The node tool's own anchor
  drags still snap to the path's other anchors.
- The node tool still draws Inkscape's dashed box around the object. In
  Illustrator, Direct Selection shows only the path and its anchors.

## Implementation

- `NodeTool::_directPress`, `_directDrag`, `_directRelease` and
  `_directCancel` live in `src/ui/tools/node-tool.cpp`. Anchors move through
  `ControlPointSelection::transform` and are committed with
  `COMMIT_MOUSE_MOVE`, as keyboard moves are. Whole objects move through
  `ObjectSet::moveRelative`.
- `CurveDragPoint::_moveSegment` is in `src/ui/tool/curve-drag-point.cpp`.
- `SelectionHelper::selectNone` is in `src/selection-chemistry.cpp`.

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
