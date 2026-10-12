# Direct Selection segments

Direct Selection (A) now treats a segment as a selection with its own identity.
Click its edge to select it; Shift-click adds or removes that edge. A selected
edge has a blue 3 px highlight. Clicking a different edge replaces the subset;
clicking the fill selects the whole path and clears the segment subset.
Selections can span objects. Anchors keep their existing click/Shift behavior.

Selected segment endpoints participate in ordinary keyboard and anchor movement.
Shared endpoints move once. Removing one adjacent segment leaves their common
endpoint selected for the remaining segment, and independently selected anchors
survive segment toggles. Dragging a selected straight edge moves the chosen
endpoint union, including other objects, with one Undo step. Shift constrains its
angle. Alt-drag bends a straight edge; a curved edge retains its native Bezier
reshaping behavior. Escape restores a staged straight-edge group drag.

Insert, insert-at-extremum, and line/curve commands act on explicit segments or
on pairs of independently selected anchors. They do not infer an intervening
segment from the endpoint union of unrelated selected edges. Delete cuts exact
selected segments into surviving open runs, preserving their Bezier handles.
Empty paths are removed. Independently selected anchors retain node deletion.
Native shapes expand only when an actual edit commits; selection alone keeps
their original representation, and Undo can restore that representation.

`ControlPointSelection` keeps explicit anchors and directed endpoint pairs
separately. Directed pairs distinguish both curves of a closed two-anchor path.
Its ordinary point set is the union used by existing movement, snapping and
alignment. `PathManipulator` draws selected edges, splits/reverses their
identities with topology operations, and restores them by subpath/node ordinal
on external geometry changes when topology is unchanged. Selection is temporary
UI state; it is not saved into SVG. Topology changes that rebuild controls can
discard a segment subset. Highlights refresh on an idle callback after selection
changes so destruction of a node list cannot traverse partially deleted nodes.

The workspace check is `tools/segment-selection-check.py` (also available as
`run-live-baseline.py --checks segment-selection`). It drives real Windows input
against synthetic disposable SVGs and compares saved geometry and highlights.
It covers Shift toggles, shared endpoints, explicit anchors, commands, group
movement/cancellation, cross-object native conversion/Undo, distinct opposite
curves, and open/closed/full-path segment deletion. Reports remain local.
The reviewed Windows 11 build passes 29/29 on 2026-10-12, including numeric
movement after a drag and separate Undo steps for the drag and numeric edit.
Local evidence: `../../artifacts/live-baseline/20261012-235226/segment-selection.stdout.log`.

References: [Adobe segment selection](https://helpx.adobe.com/illustrator/desktop/draw-shapes-and-paths/modify-paths/select-path-segments.html),
[Adobe segment adjustment](https://helpx.adobe.com/illustrator/using/adjust-path-segments.html).
