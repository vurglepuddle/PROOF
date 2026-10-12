# Live Corners in Direct Selection

The A tool shows a circular corner widget just inside a corner anchor. Selecting
an entire supported path shows all its widgets; selecting anchors shows their
own widgets. Smooth joins and open endpoints have none. Click a widget to choose
that corner, Shift-click to add or remove corners, then drag one of the chosen
widgets to give them a common radius. With no individual widget selection,
dragging a visible widget edits all the visible corners on that object.
Selected anchors map to the new split endpoints on commit, so another drag
continues to edit the same corner subset.

Alt-click cycles Round, Inverted Round and Chamfer. Double-click opens Corners,
with a numeric radius in the document's display units, corner type, and Absolute
or Relative rounding. Absolute specifies a circular radius at straight-sided
corners; Relative specifies the distance along each side. Enter accepts the
popup, Escape cancels it. The widget and rounded curve become red when a drag
reaches the largest radius that fits the neighbouring sides. A completed drag, style change or popup
edit makes one Undo step. A cancelled edit makes none.

## Editable geometry and persistence

The visible SVG `d` contains the expanded outline, including actual split
anchors and cubic round geometry. There is no Live Path Effect hiding a sharp
outline. Ordinary node and handle editing continues to work on those anchors.

Auxiliary `inkscape:live-corners` data stores the source outline, each corner's
radius/type/rounding, and an affine frame. The version 1 format is
`1|source SVG path|path,node,radius,type,rounding[,handle-factor];…|a,b,c,d,e,f`.
The optional factor retains a native rectangle's original cubic approximation.
Plain SVG viewers use the expanded `d`; removing the auxiliary data leaves the
artwork intact.
Save/reopen and Undo/Redo preserve the live parameters. A whole-object affine
change preserves corners even when a rotation, scale, reflection or skew is
baked into `d`. Nonuniform transforms retain the transformed round geometry.

An ordinary geometry edit reconciles the visible outline with the stored data.
Damaged rounds become ordinary curves; intact corners remain live. Insertion of
an anchor on an unchanged side and edits to unrelated subpaths retain the
matching corners. Recovery requires agreement with the stored round and its
adjoining source portions; unmatched geometry is never replaced by stale data.

Native rectangles expose the same widgets, including existing equal or unequal
`rx`/`ry`. Rounding converts a native rectangle to an ordinary path on commit.
Its ID, parent, transform and paint are retained. Unequal native radii use an
affine frame so their original elliptical outline survives. A normal anchor
edit also captures the native rounds before conversion and retains unaffected
ones; their original `.554` cubic coefficient is preserved.

## Scope

This first implementation uses the ordinary path/shape support of
`PathManipulator`. Shapes with Live Path Effects or markers retain their
existing editors. Stars, spirals and geometry animations also retain their
existing editors. More than 100 visible corners on one object suppresses the
widgets to avoid crowding; selecting fewer anchors reveals their widgets.
[Selected widget groups](PROOF-corner-groups.md) now work across objects, with
one displayed amount, a shared geometric limit and one Undo step.

Straight-sided rounds use the standard cubic approximation to a circle.
Corners between curved sides use trimmed side portions and a cubic fitted to
their endpoint tangents; their numeric radius is an approximation. Exact
offset-circle intersection for arbitrary curves remains follow-up work.
[Ellipse arc handles](PROOF-ellipse-arcs.md), [explicit segment selection](PROOF-segment-selection.md),
[anchor Align](PROOF-anchor-alignment.md) and [widget visibility/angle limits](PROOF-corner-display.md)
are implemented as separate features from the owner's brief.

## Implementation and checks

- `src/ui/tool/live-corners.*`: geometry, parameter serialization and recovery.
- `src/ui/tool/live-corner-controller.*`: widgets, preview, popup and Undo.
- `PathManipulator`: preview/commit, deferred shape conversion and release guards.
- `NodeTool`: the corner cursor.
- `testfiles/src/live-corners-test.cpp`: headless geometry regressions.
- Workspace `tools/live-corners-check.py`: disposable SVG live acceptance cases.

The geometry suite covers subsets, shared radius limits, all three styles,
Absolute/Relative rounding, curved sides, smooth/end anchors, serialization,
affine edits, local damage and side insertion with unrelated subpaths.
The workspace handoff records the installed build and live results.

On 2026-10-12 the Windows 11 build succeeds and the built geometry suite passes
14/14. The installed live suite passes 34/34, including repeated subset edits,
type/rounding dropdowns with pending radius text, native equal/unequal rectangle
radii, ordinary native-anchor editing, and teardown with Corners open.
Local reports are `artifacts/live-corners/run-201859/result.json` and
`artifacts/live-corners-reopen/run-202131/result.json` in the workspace.
The related [grab ownership fix](PROOF-control-point-grabs.md) keeps widget
refreshes from cancelling an ordinary anchor drag. The full installed
corner/selection/colour/closing run passes 142/142 across 12 suites.

Reference: [Adobe Live Corners tutorial](https://www.adobe.com/learn/illustrator/web/live-corners-in-illustrator)
and the owner's workspace `brief/direct-selection-brief.md`.
