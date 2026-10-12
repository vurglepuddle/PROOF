# Ellipse arc controls in Direct Selection

Direct Selection keeps the native start and end controls of circles and
ellipses alongside their editable path anchors. Dragging an arc control keeps
the ellipse's centre, radii, identity, paint and transform. It writes the native
ellipse/arc parameters rather than expanding an ordinary path.

The controls are offset 14 screen pixels outside the outline. Coincident ends
on a whole ellipse are separated by eight pixels in each tangent direction,
leaving both controls and the original path anchor accessible. Their placement
follows the object's affine transform and the current zoom.

Ctrl snaps angles using the existing rotation increment. Shift moves both
ends together; Shift-click makes the ellipse whole. The native inside/outside
drag convention chooses an open arc or a closed slice, with its existing dead
band. Escape restores the preview without adding an undo step. An accepted
change is one undo step; save/reopen retains the native parameters.
Tool shortcuts keep the active mouse grab, as other path controls do: release
accepts it and Escape cancels it. Controller teardown restores a staged preview
when the controller actually goes, including during window closure.

Editing a path anchor still defers conversion until commit. Conversion removes
all native ellipse geometry tags so reopening cannot reconstruct the old
ellipse over an anchor edit. One Undo restores the live ellipse or arc.

`src/ui/tool/ellipse-arc-controller.{h,cpp}` owns these two controls through
`PathManipulator`. Refreshes are queued outside the active grab. The existing
native shape editor remains responsible for ellipses with live path effects,
markers or geometry animation. Illustrator is the reference for selecting
anchors; these restored native arc controls retain PROOF's existing ellipse
editing contract requested in the owner's brief.

The workspace's `tools/ellipse-arcs-check.py` exercises ordinary ellipses,
circles, rotated/scaled ellipses, modifiers, cancellation, Undo/Redo,
anchor conversion and save/reopen against disposable synthetic SVGs.

The 2026-10-12 installed acceptance passes 25/25. Existing shape-anchor,
Direct Selection, Live Corners and stationary snapping checks pass 33/33,
15/15, 34/34 and 2/2 respectively. The focused teardown mode checks a shortcut
during an active grab, its single Undo step and window close during a preview;
all three pass on the guarded build.
