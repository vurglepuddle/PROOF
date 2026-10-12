# Corner groups across objects

Direct Selection corner widgets now edit a chosen group across objects. With
chosen anchors, a drag rounds their eligible corners together. Shift-clicked
widgets form an explicit group; dragging or Alt-clicking a selected member
changes that group. Dragging an unselected widget while another group is chosen
changes that widget alone. With no individually chosen anchors or widgets,
dragging rounds the visible eligible corners of the selected objects.

A plain click on an already selected widget retains its group, allowing a
double-click to open Corners for the whole group. Clicking an unselected widget
without Shift replaces the widget selection; Shift adds or removes one widget.
Numeric radius, type and Absolute/Relative settings use the same group scope.
Widget selection is temporary UI state; document Undo can clear it, so select
widgets again before a later grouped popup edit.

The common amount uses each object's existing displayed-radius scale, including
its live frame, object transform and document units. Uniformly scaled objects
therefore receive different local radii for the same displayed amount. The
smallest eligible limit constrains the whole group; the limiting preview shows
red widgets and rounded curves. Nonuniform frames retain the existing geometric
mean scale used by the Corners popup.

All members preview together and commit in one document Undo step. Escape
restores every member's preview. The leader retains peer path manipulators for
the edit, with no ownership cycle back to itself; release or teardown cancels
the group. Native shapes retain deferred conversion and ordinary SVG metadata.
Each object's live parameters still serialize independently.

## Implementation and verification

`LiveCornerController` coordinates controllers belonging to one
`MultiPathManipulator`, captures their before states and defers control rebuilds
until the group edit finishes. `setSharedRadius` probes each model's radius limit
in displayed units, then applies the common minimum from untouched snapshots.
`commitCorners` remains the geometry/metadata and native-conversion boundary;
the leader records one Undo after all changed members commit.

The geometry suite passes 18/18, including transformed shared limits, untouched
corners, mixed Absolute/Relative rounding at an acute corner and invalid scales.
Workspace `tools/corner-group-check.py` exercises native rectangles across a
rotated, scaled parent, selection modes, grouped popup/style edits, the common
limit, Undo/Redo, cancellation and closing during a drag. The workspace handoff
records the final live results and the valid 9/17 baseline.

On 2026-10-13, focused acceptance passes 18/18. The combined group, existing
corner, native-primitive and display regressions pass 119/119 on the same
installed candidate. Reports remain local in the workspace.

Exact circular fitting between arbitrary curved sides remains separate work;
the group feature retains the existing tangent-fit approximation.
