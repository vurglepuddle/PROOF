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

## Turned box (2026-10-03)

As in Illustrator, a rotated object keeps a rotated box. Dragging an edge or
corner then stretches and scales along the object's own axes, so a rotated
rectangle stays a rectangle instead of being sheared.

What Illustrator does was checked against Illustrator 29 on this machine
(screenshots in the session scratchpad, script `ai-bbox-probe.ps1`). The user
expected only live shapes to keep a turned box, but plain paths keep one too:
- Every rotated object carries its accumulated rotation, the
  `BBAccumRotation` art tag, which Object > Transform > Reset Bounding Box
  clears. A pen path with the tag shows a turned box; one without it shows an
  upright box around the same shape.
- A group has no angle of its own. It, or several selected objects, gets a
  turned box when all its objects share an angle, and an upright one
  otherwise. Illustrator compares exact angles (30 and 120 degrees count as
  different); PROOF compares the boxes, which repeat every quarter turn.
- Live shapes keep their angle as a shape property, so Reset Bounding Box is
  greyed out for them until they are expanded.
- Shapes show their centre (the Attributes panel's Show Center, on by default
  for shapes and off for pen paths), also inside a group.

In PROOF:
- **The angle.** Rectangles, ellipses, stars, polygons, text, images and
  clones keep rotation in their `transform`, so their box follows it. Paths,
  lines and spirals take transforms into their nodes; they keep the angle in
  `proof:box-angle` (degrees from -180 to 180, in the object's own
  coordinates), the counterpart of `BBAccumRotation`. The full angle is kept,
  so a quarter turn reads 90 degrees even though the box looks upright. `SPItem::doWriteTransform` updates it
  whenever part of a transform goes into an object's coordinates, so the box
  never turns when a transform is embedded: moving, rotating, ungrouping and
  converting a rotated rectangle to a path all keep it. A transform that
  shears the box removes the angle. Path operations make new paths with
  upright boxes.
- **Several objects.** A group, or a multiple selection, has a turned box when
  every object in it (in nested groups too) has the same box. One upright,
  sheared or differently turned object makes it upright.
- **Interaction.** Scaling, stretching and skewing work in the box's frame,
  including snapping (`PureScale`, `PureStretchConstrained` and
  `PureSkewConstrained` take the frame). Corner, edge and rotation zones
  follow the turned edges. The resize cursors are the nearest of the four
  standard ones, and the rotate cursors come in eight directions. Rotation
  turns about the middle of the turned box. On-canvas alignment, the last
  Shift+S mode, uses an upright box.
- **Thin outlines** around each of several selected objects are turned with
  each object.
- **Centres.** With a group or several objects selected, every rectangle,
  ellipse, polygon and star in it shows its centre (up to 200). A single
  shape needs none: the box's centre dot marks it.
- **Object > Reset Bounding Box** (`app.transform-reset-box`) makes the box of
  each selected object, or of every object in a selected group, upright
  without moving anything. Unlike Illustrator it also works on shapes: their
  stored angle cancels the transform's. Clones keep their box. Undo turns the
  box back.
- **Files.** PROOF SVG keeps `proof:box-angle`, and changing it (also by undo
  or redo) redraws the box (`SPObject::notifyAttributeChanged`). Plain SVG
  drops it with the other editor data. The native `.ai` writer should map it
  to `BBAccumRotation` (radians) later.

## Rotate in Properties (2026-10-04)

The user rotated an object and saw "0°". That was the canvas rotation field in
the status bar, and nothing showed the object's angle. Properties > Transform
now has a Rotate row, as Illustrator's Transform panel does:
- **The angle** of the selection, counterclockwise on screen as in
  Illustrator. SVG's `rotate(30)` turns clockwise, so it reads -30°. A
  rectangle turned by 120 degrees reads 120, not 30. Objects that share a box
  show the first one's angle. Objects turned differently show 0, as their box
  is upright.
- **Typing an angle** turns the selection to it, about the same point as the
  transform box: a centre set on the object, or else the middle of the turned
  box. It is one undo step.
- **The button** at the end of the row is Reset Bounding Box. It is greyed
  out while the box is upright.
- **Mirroring:** a flipped object is read as turned by less than a quarter
  turn either side. Flipping an upright object leaves it at 0, and flipping
  one at 30 degrees gives -30.

Enter in a docked panel's field now applies the value and returns to the
canvas, so Ctrl+Z undoes the change straight away. Docked dialogs used to keep
the focus, because `DialogBase::onDefocus()` only returned to the main window
from a floating dialog. This applies to X, Y, W and H as well.

Not yet: W and H are still the upright bounds, as Illustrator shows them for
paths (Illustrator gives live shapes their own width and height in Shape
properties).

## Keyboard and Transform rotation (2026-10-04)

Keyboard rotation (`[` `]`, including Alt for small turns) and the Transform
dialog now use the same pivot as Properties and the canvas box: an explicit
centre set on the selection, or the middle of its turned box. A lopsided path
keeps that centre through repeated turns. A selection with different box
angles still uses an upright box. The chosen geometric or visual bounds apply
to the pivot too.

Transform's **Apply to each object separately** turns each object about its
own turned box or explicit centre. Keyboard anchors and the grouping of
repeated turns into one undo operation are preserved.

The live check also found that Enter committed the Transform field's number
without applying it. The numeric control consumed the key before its
activation signal fired. A successful Enter commit now emits that signal and
returns focus to the canvas; an invalid expression stays in the field.

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

`tools/turned-box-check.py` covers the turned box on
`artifacts/turned-box/source.svg` (23 checks, `artifacts/turned-box/run-*`):
- the box, corner squares and centre dot on a rectangle turned by 30 degrees,
  and no upright box
- turned resize cursors and rotate cursors
- stretching and scaling along the turned axes, read back from the saved file
  (still turned 30 degrees, not sheared), and undo
- a pen path rotated with Shift keeps `proof:box-angle="30"` and a turned box
- two objects at the same angle share a turned box
- a group shows its shapes' centres; Reset Bounding Box makes its box
  upright, and undo turns it back
- typing 45 in Properties > Rotate turns the rectangle to 45 degrees
  counterclockwise (read from the file), and Ctrl+Z right after undoes it
Reset Bounding Box has no shortcut, as in Illustrator, so the check binds one
in its disposable profile copy (`proof_live.start(extra_keys=...)`). The
Object menu entry is in `09-object-menu.png`. `test_box-frame` (17 cases)
covers the angle rules: shapes, paths, full angles and mirroring, moves and
scales along the box, shearing, shared angles, groups and ungrouping, reset,
the box's middle, and shape centres.

The eight added unit cases cover keyboard and Transform pivots on lopsided
paths, explicit centres, keyboard anchors, mixed angles, geometric/visual
bounds, empty selections and repeated-turn undo/redo.
`tools/rotation-centre-check.py` drives `tests/rotation-centre.svg` in a
disposable copy of the installed editor. Its 17 native-input checks cover
both brackets, Alt rotation, the actual Transform angle field and its
separate-object option, and undo/redo. Saved vertices are compared with the
expected rotation to within 0.01 px. Evidence:
`artifacts/rotation-centre/run-225652/result.json` and
`artifacts/rotation-centre-unit.xml`.

`tools/ui-fixes-check.py` covers the 2026-10-03 follow-ups on a rectangle with
a 24 px stroke: the box runs through the middle of the stroke, and a corner
click leaves the square unchanged while a corner drag still scales from the
opposite corner (`artifacts/ui-fixes/run-*`).
