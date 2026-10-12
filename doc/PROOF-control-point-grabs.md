# Mouse grab ownership for editing controls

Refreshing Live Corner widgets while dragging an already visible anchor can
delete idle control points. Previously every `ControlPoint` destructor cleared
the shared mouse-grab and drag flags. Deleting an idle widget could therefore
cancel the anchor's grab: its preview moved, but mouse release no longer committed
the geometry. An unchanged SVG on save was the visible result.

`src/ui/tool/control-point.cpp` now tracks the actual grab owner. Press and grab
transfer assign it; release and Escape clear it. Destruction releases the shared
grab only when the destroyed point owns it. Deleting the owner still performs
the existing cleanup.

The workspace's `tools/live-corners-check.py` covers the trigger by rounding and
undoing a native rectangle, selecting its fill, then dragging a visible anchor.
The saved SVG must be an edited path and preserve unaffected native rounds.
The Direct Selection, shape, snapping and close suites cover ordinary gestures,
cancellation and teardown. The latest workspace handoff records their results.

On 2026-10-12 the installed Live Corners suite passes 34/34, including this
native-anchor regression. The full corner/selection/colour/closing run passes
142/142 across 12 suites on Windows 11 at 100% scale.
