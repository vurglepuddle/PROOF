# Canvas drag smoothness

## Symptom

Dragging an object felt rough even in a document with two rectangles and
snapping off, while Illustrator stays smooth.

## Measurement

Measurements use Framecheck (Preferences > Rendering > developer mode >
Framecheck), which writes `%TEMP%\framecheck.txt`. PROOF adds timing points
for:
- GTK's frame phases (`gtk_layout`, `gtk_paint`)
- the redraw pipeline (`redraw_scheduled`, `redraw_launched`, `redraw_finished`)
- document updates (`document_update`, `frame_document_update`)
- `selection_modified` and each visible panel's handler

They are written only while Framecheck is enabled.

A scripted drag was used: 375 moves at 8 ms (a 125 Hz mouse) over 3 s, in a
disposable profile at 1920x1080 and 100% scale, in the default workspace with
Properties docked.

## Cause

The fault was not slow rendering. The tiles for a frame took about 0.2 ms,
and the Properties panel's refresh took about 0.6 ms. The fault was
scheduling:

1. While the object moves, the panels, toolbar and rulers change, so GTK
   repaints the window. This took 9 ms per frame for paint and 1.4 ms for
   layout.
2. On Windows, GTK starts these frames back to back. It handles input,
   paints, and starts the next frame within about 0.3 ms.
3. The main loop therefore had almost no idle time. Idle callbacks, at any
   priority, were starved.
4. The canvas cannot redraw a moved object until the document's idle update
   applies the new transform. That update ran only about every 90 ms, so the
   canvas showed about 11 to 13 frames per second.

A rubberband drag stayed at 60 fps under the same GTK load. It needs no
document update, and the canvas starts a scheduled redraw from inside its
own paint.

Raising the canvas redraw's idle priority made no difference, because no
idle gap existed to use. That change was reverted.

## Fix

The canvas connects to its frame clock's `before-paint` signal and calls
`SPDocument::flushPendingUpdates()`. That runs the pending idle update and
the connector rerouting immediately: after input has been flushed and before
layout and paint. A moved object is therefore applied in the same frame,
whatever the tool (selection, node, handles). When nothing is pending, the
call does nothing.

## Results (2026-10-03, Windows 10, GTK 4.24, two-rectangle file)

| Two scripted 3 s drags | Canvas frames | Frame gap median / p95 / max | Gaps > 33 ms |
|---|---|---|---|
| Before | 35 and 33 | 36 / 122 / 165 ms and 66 / 269 / 283 ms | 33 and 31 |
| After | 158 and 177 | 16.5 / 18.4 / 22 ms and 16.5 / 18.5 / 23 ms | 0 and 0 |

A second, application-independent check sampled one screen row at about 120 Hz
during the same drags. It timed visible movement of the rectangle:

| | Visible updates/s | Gaps > 50 ms | Longest gap |
|---|---|---|---|
| Before | 12 | 16 and 21 | 350 ms |
| After | 33 to 42 (limited by the sampler) | 0 | 42 ms |

Illustrator 29.2.1 with the same script and a throwaway one-rectangle document
(`ai-drag-doc.ps1`): 47 visible updates/s, median gap 16.9 ms, p95 33 to 35 ms,
max 67 to 84 ms, 1 to 2 gaps over 50 ms. PROOF is now in the same range.
Illustrator's rectangle was larger on screen, so treat this as close parity,
not a precise ranking.

The dashed selection cue used to trail at the starting position during a drag
(`mid-drag-before.png`). It is now hidden from grab to release
(`mid-drag-after.png`, `after-release.png`), as in Illustrator.

The user's own judgement after the fix: "silky smooth". The renderer is not
the cause. `GSK_RENDERER` cairo, gl and vulkan all showed the same starvation.

One side effect: the Properties and toolbar X/Y fields still update during a
drag, but less often. Their selection signal is also an idle callback; the
fields are exact when the drag ends.

## Open item: first selection after opening

The first time an object is selected after a file opens, the Properties
panel's `selectionChanged` takes about 0.7 s while it builds that object's
sections. Inside it, hundreds of small preview-document updates run,
consistent with marker and paint previews being constructed. Later selections
are instant. Building those previews lazily, or ahead of time while idle,
should remove the pause.
