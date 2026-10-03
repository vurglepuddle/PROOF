# Transform box

The select tool's handles follow the professional convention (Figma's
4-corner box, which the user prefers), replacing Inkscape's arrow handles and
its click-again rotate/skew mode.

## Behaviour

- **Box:** a 1 px line in the selection blue (`#277fff`) around the selection
  bounds, with a small dot at the centre of rotation. Box, handles and dot are
  hidden while transforming and redrawn at the result on release.
- **Bounds:** geometric by default, without stroke, as in Illustrator with
  "Use Preview Bounds" off. The box, the Properties panel and the toolbar
  measure the shape itself. Preferences > Tools > Bounding box still switches
  to visual bounds. Profiles saved before 2026-10-03 are switched once
  (`/proof/upgrades/geometric-bounds`), and a later choice is kept.
- **Corners:** four small white squares with a blue edge, centred on the
  corners. Dragging scales from the opposite corner. Shift keeps proportions
  and Alt scales from the centre (Illustrator keymap modifiers).
- **Edges:** there are no midpoint handles. Each edge is an invisible zone
  along its whole length, with resize cursors, and dragging it stretches.
- **Rotation:** an invisible zone just outside each corner, with a curved
  double-arrow cursor turned to that corner. Rotation is always around the
  centre of rotation. Alt rotates around the opposite corner and Shift snaps
  the angle. Clicking the selection again no longer switches modes.
- **Corner radius:** with one rectangle selected and the pointer over it,
  small circles appear inside its corners, at the radius or 12 px in from a
  sharp corner. Dragging one rounds all four corners together, in one undo
  step. They do not appear for rectangles with path effects (the Corners
  effect has its own handles in the node tool), or for rectangles under 48 px
  on screen.
- **Several objects:** one box around the whole selection, and a thin solid
  outline around each object. A single object has no separate outline.
- **Clicks:** a click without a drag in an invisible zone acts as a click on
  the canvas there. It selects what is under the pointer, or deselects over
  empty canvas. A click on a corner square or the centre dot does nothing, as
  in Illustrator and Figma. Inkscape made the clicked handle the transform
  reference point (a filled square with guide lines). Shift+click on the
  centre still resets a moved centre of rotation.
- **Small objects:** zones reach only a quarter of the way into the box, at
  most 5 px (6 px for corners), and the edge zones of a box under 12 px across
  are not picked. A small or thin object can still be dragged by its middle.

Shift+S still reaches the remaining skew mode and on-canvas alignment.

## Moving keeps up with the pointer

A drag now measures the move from where the button went down. Inkscape
started the move only once the pointer passed the drag tolerance (4 px here),
so the selection stayed that far behind the pointer for the whole drag.

## Knot cursors

Knot hover cursors were set on the window. In GTK 4 the canvas's own cursor
takes precedence, so they never showed. `ToolBase::use_cursor` now sets the
canvas cursor and restores the tool's cursor afterwards. The page tool's
resize cursors go through the same path but have not been checked yet.

## Implementation

- `CanvasItemCtrl::set_pick_zone` lets a control pick through an area other
  than its drawn handle: a segment or point with a radius, limited toward the
  box centre, or the region just outside a corner. The zone is included in
  the bounds, because groups only pick within their children's bounds.
- `SelTrans` keeps Inkscape's handle table and transform code. Corners use
  the new `.proof-box-corner` control, and edges and rotation handles use
  invisible controls with zones and cursors (`_setBoxHandleZone`). They are
  raised so that corners win over edges, and edges over rotation zones. The
  origin of a transform now depends on the handle type, not on the handle
  mode.
- `SelCue::setTransformBox` changes only the select tool's cue. Other tools
  keep their own selection cues.
- Styles are in `share/ui/node-handles.css` (`.proof-box-corner`,
  `.proof-box-center`, `.proof-box-radius`). The rotate cursors are
  `share/icons/hicolor/cursors/proof-rotate-{nw,ne,se,sw}.svg`.

## Verification

`tools/transform-box-check.py` opens a disposable copy of
`artifacts/transform-box/source.svg` with a copy of the workspace
preferences and drives it with native input. It reads the real Windows
cursor and measures results from screenshots by colour. 28 of 28 checks pass:
- appearance
- every cursor zone
- stretch, scale and rotation, and their undo
- clicking again
- click-through
- moving a thin line and a 10 px square by exactly the pointer's travel
- radius handles on hover, rounding and undo

Evidence is in `artifacts/transform-box/run-*`.

`tools/ui-fixes-check.py` covers the 2026-10-03 follow-ups on a rectangle with
a 24 px stroke: the box runs through the middle of the stroke, and a corner
click leaves the square unchanged while a corner drag still scales from the
opposite corner (`artifacts/ui-fixes/run-*`).
