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

## Delay before a drag starts changing the shape (2026-10-04)

**Report:** with a handle, the shape started changing only after a short delay.
Moving, rotating and corner scaling then felt smooth.

**Measured** with `tools/redraw-bench.py`. It presses a handle, moves the pointer
1 px every 8 ms, and samples the screen about 65 times a second with GDI (PIL's
grab manages only about 23). Framecheck logs what the editor did in between.
The handle grabbed and applied the first transform 28 to 44 ms after the press,
which is the 4 px drag tolerance. The canvas then finished its first redraw
anywhere from 10 to 330 ms later. In one slow case, `redraw_scheduled` came at
31 ms and `redraw_launched` at 365 ms.

**Cause:** the starvation described above, at another step. The canvas starts a
redraw from an idle callback. Once one runs, the next ones follow on from it.
The first one of a drag waited for an idle gap, though, and the panels
repainting every frame left none. `paint_widget()` already launches a waiting
redraw, but only when the canvas itself is repainted, and nothing had changed
on it yet.

**Fix:** the canvas's `before-paint` handler launches a waiting redraw too,
right after it flushes document updates.

| Start of a drag, ms from press to the first visible change | Before | After |
|---|---|---|
| Move, corner scale, edge stretch, rotate, corner radius (6 trials each) | 60 to 387 | 52 to 78 |

About 32 ms of that is the 4 px drag tolerance at the test's 1 px per 8 ms. A
faster flick passes the tolerance sooner.

## Torn redraws while dragging fast (2026-10-04)

**Report:** a turned rectangle dragged fast showed pieces of itself at several
positions, cut along tile edges.

**Measured** with the same script. The turned rectangle is dragged back and
forth at up to about 1,900 px/s while the screen is sampled. A frame counts as
torn when its pixels do not fill one turned rectangle. Saved torn frames
(`torn-*.png`) show tile-shaped cuts both ways, as in the user's screenshot.

**Causes:**
1. `paint_widget()` committed the tiles of a redraw that was still in
   progress. Every tile of one redraw comes from the same snapshot, so a
   finished redraw is consistent. A part-finished one shows some of a moving
   object at its new place and some at its old.
2. Inkscape's default update strategy, Multiscale, deliberately holds back
   regions it has just redrawn when new changes arrive mid-redraw. That
   staggers the regions of a moving object.

**Fixes:** `paint_widget()` commits tiles only once their redraw has finished
(`RedrawData::finished`). After a timeout the redraw still commits what it
has, so very heavy drawings keep updating. The default update strategy is now
Responsive, which redraws everything changed together. Preferences >
Rendering still offers all three.

| Fast drag, torn frames | Multiscale | Responsive |
|---|---|---|
| Before the fixes (one run each) | 8 of 129 | 0 of 131 |
| With the launch fix only | 1 to 52 of 125 (runs varied) | 0 to 43 of 125 |
| With all fixes (three runs each) | 0 | 0 |
| All fixes, 40,000-path page (two runs each) | 0 | 0 |

The rate of visible updates was the same across strategies: about 30 to 45 a
second on the two-object page, and 24 to 29 on the heavy page.

## First selection after opening (2026-10-04)

**Measured** with `tools/first-selection-bench.py`, using Framecheck. The
Properties panel builds a set of sections for each kind of object the first
time one is selected.

| First selection of | Before | After |
|---|---|---|
| A rectangle (also loads the shared paint editors) | 753 ms | 8 to 10 ms |
| A path | 158 ms | 8 ms |
| A group | 131 ms | 8 to 10 ms |
| Any of them again | 4 ms | 4 ms |

The first selection's extra 0.5 s was the shared fill and stroke editors
(`PaintPopoverManager`). They were created, then given the document, which
loads the pattern library's previews (186 small document updates).

**Fix:** after a document is attached, Properties builds its sections ahead
of time (`ObjectAttributes::schedule_prebuild()`). Half a second later it
starts doing one piece per low-priority idle call: the fill and stroke
editors, giving each the document, then rectangle, path, group, ellipse,
star, text, image, clone and multiple-selection sections. The pieces take
about 50, 50, 470, 4 and then 110 to 160 ms each, while nothing else waits.
The 470 ms step is the one-time pattern-library load. Splitting it further
would mean reworking Inkscape's pattern editor.

## Measuring

Developer-only Framecheck timing points: `seltrans_grab` and
`seltrans_transform` (transform box), `props_create_panel`,
`props_update_panel` and `props_prebuild` (Properties). Turn on Framecheck
under Preferences > Rendering in developer mode; it writes
`%TEMP%\framecheck.txt`.

```powershell
python .\tools\redraw-bench.py [--strategy 1|2|3] [--trials N] [--framecheck] [--no-tearing] [--source file.svg]
python .\tools\first-selection-bench.py
```

`artifacts/redraw-bench/heavy.svg` is the 40,000-path page.
