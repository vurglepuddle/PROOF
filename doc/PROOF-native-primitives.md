# Direct Selection on native SVG primitives

Circle, polygon and polyline elements use the same deferred path editing as
rectangles and ellipses. Clicking an anchor leaves the native element intact.
A real anchor edit commits an ordinary path and removes the primitive geometry
attributes. One Undo restores the exact native element; Redo restores the path.
Escape during a drag restores the native geometry without committing it.

The conversion retains identity, paint, transform, parent and child metadata.
Circles and polygons keep closed topology; polylines stay open. A transformed
polyline's anchor movement is resolved in its own coordinates. Dragging the
fill of a circle or polygon moves the whole object and keeps its native type.

`PathManipulator::canEditShape`, staged drawing geometry and `_convertShape`
implement this behavior. This acceptance round verifies their existing support
individually; it does not extend the supported shape set. Shapes with markers,
Live Path Effects or geometry animations keep their existing editors.

## Verification, 2026-10-13

Workspace `tools/primitive-anchor-check.py` passes 28/28 on Windows 11 at the
tested 1920 x 1080, 100% setup. It uses synthetic disposable artwork and a copied
profile. Each primitive has selection, committed movement, topology,
identity/paint/metadata, Undo/Redo and cancellation checks; filled primitives
also have whole-object movement and Undo checks.

Paint checks retain exact non-paint attributes and child metadata. If native RGB
paint is written with equivalent built-in ICC spelling, saved rendered pixels
must remain unchanged, using the existing shape-acceptance helper. Native
numeric CMYK retention has separate paint acceptance evidence.

The workspace handoff records the local reports. Circle arc editing is covered
separately by [ellipse arc controls](PROOF-ellipse-arcs.md).
