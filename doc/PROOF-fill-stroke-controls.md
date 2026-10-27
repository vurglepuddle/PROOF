# Fill and stroke controls in the tools sidebar

The tools sidebar contains two overlapping squares: a solid fill target
and a hollow stroke target. A red diagonal marks None. The active target appears
in front; click either exposed square to activate it, or double-click to open
the existing native paint editor. Mixed paints have an indicator rather than
an averaged colour. Gradients retain their paint when the editor is opened.

Each square is 20 px, about as large as a tool's icon, and the stroke's lies
10 px down and right of the fill's: 30 px for the pair. Illustrator's are 18,
10 and 28 beside tools of that size. (They were 32, 14 and 46 until 2026-10-11.)

The picker sits immediately below the last visible tool, as requested, with
unused sidebar space below it. The tool scroller propagates its natural height
and does not expand; shorter windows retain scrolling for the tools. At the
tested 1080-pixel desktop, the fill target centre moved from y989 to y625.

## Commands and paint application

The small buttons provide default paints, swap and None. Under the Illustrator
keymap, X switches targets, Shift+X swaps fill/stroke, D applies white fill and
black stroke, and / removes the active paint. These shortcuts require canvas
focus and are disabled while the Text tool is active or an entry has focus.
In a CMYK document, D uses zero-ink white and K-only black. Default stroke width
is 1 pt, whatever the document's unit, and default paint opacity is 100%.
Shift+X belongs to this swap alone: with nothing selected it swaps the drawing
defaults, which the older `app.swap-fill-and-stroke` (still an action, without
a key) does not.

Selected objects, group children and text use the same application functions
as Fill and Stroke. Numeric colour edits preserve the other paint, geometry
and stroke width. A popup editing session has its own Undo tag. Native CMYK/ICC
colours retain their colour space and channels when loaded into the editor;
opening, closing or reopening it does not repaint the selection.

With no selection, solid and None choices update the creation tools' current
style caches. Newly drawn shapes use that paint without changing existing
artwork. Tools configured to use a fixed style retain their existing preference.
Gradient, pattern and mesh creation defaults with no selection remain outside
this round; their existing editors still require selected objects.

## A text shows its characters' paint (2026-10-26)

A `<text>` element can say fill and stroke none while its spans hold the paint:
the Text tool paints the selected characters' spans (Default fill and stroke
with characters selected, for one), and imported type keeps its paint on spans.
The panels read the text element alone, so the owner saw none/none on text that
was visibly white and black. Now a selected text stands for the spans holding
its visible characters (white space alone left out), as Illustrator's panels
show a type object's character fill and stroke. This holds for the fill,
stroke, stroke width, join, cap, miter and paint order queries: these controls,
the Fill and Stroke dialog (its colour from the first character's span, or from
the Text tool's first selected character), the stroke style's dashes, the
status bar and the Swatches. Spans of different paints read as mixed. Applying
paint with the Selection tool already reached the spans.
`sp_desktop_paint_sources` in `desktop-style.cpp`; tested in
`object-style-test` (`TextPaintSources`).

## Every commit redraws: no counter as the modified tag (2026-10-27)

The popover passed a counter (one more each time it opened) where `selection-paint`
hands the document a modified tag. That parameter takes `SP_OBJECT_USER_MODIFIED_TAG_n`
bits or 0. A count of 1, 2 or 3 carries modified-state bits: the root refused the
notification (GLib: "assertion '!(flags & ~SP_OBJECT_MODIFIED_CASCADE)' failed",
then 32 update passes), and whatever waits for it waited for the next change made
elsewhere. A mesh point's colour reached its stop, but the canvas kept the old
patches. The owner: "it DOES set colors. You need to do about 3 other things to the
mesh for colors to propagate"; another point broke it again (the next popover's
count). The legacy panels only seemed to work because their dash controls wrote the
object's `stroke-dasharray` as they refreshed, which carried the pending
notification through (a fault of its own, in the todo). The popover passes 0 now (it
never read its tag back), the parameter is called `modified_tag`, and
`SPDocument::_emitModified` drops bits that aren't user tags, with a warning, so a
wrong tag can't stall the document again. Tested in `sp-gradient-test`
(`EveryPointColourSetReachesWhatTheCanvasDraws`, which failed for tags 1, 2 and 3
before).

## Showing a selection writes nothing (2026-10-27)

While the owner picked mesh point colours, the docked Stroke style tab and the
sidebar's paint attributes wrote `stroke-dasharray` to the object, each with an
undo step nobody made (a gdb watch of their session). Two causes: a numeric entry
committed its own rounded display text when it lost focus, and the dash selector
announced the values it was given as if the user had edited them. A spin entry
now commits only text that differs from what it showed, and the dash selector
blocks its own updates. Found with them: the sidebar's stroke options connected
another paint-order handler on every refresh (one click, several writes) and
passed on miter changes made while refreshing; the docked paint order used the
desktop it was built with. `test_stroke-style` (ten widget cases, run with
`INKSCAPE_TEST_GUI=1`) checks that refreshing changes neither the document nor
the undo history and that real edits still apply.

## Implementation and lifetime

`FillStrokeControls` renders the previews and binds to selection/document changes.
`selection-paint` shares the dialog's native paint operations. The controls reuse
`PaintPopoverManager` rather than constructing a second set of colour resources.
Context guards invalidate pending commits when the selection, document or
desktop changes. Reordering a target keeps its parent so a double-click on an
inactive target is not cancelled.

The window owns its paint actions. Toolbar destruction must disconnect child
callbacks and disable retained actions without accessing the parent action map:
GTK disposes that map before destroying the toolbar. Shared paint popup
registrations detach callbacks and desktop/document bindings on teardown.

The user's crash when closing without saving was reproduced during this round.
Executable offsets e93b22/e93e4e resolved to ToolToolbar's destructor removing
actions through the disposed parent. Explicit activation-connection ownership
and popup unregister cleanup cover this teardown path. Ordinary WM_CLOSE,
unsaved discard and open-editor close now finish with exit code zero.

## Validation, 2026-10-11

The installed Windows 11 build passes the real mouse/keyboard checks:

- `paint-controls-check.py`: 22/22, including fill/stroke cyan-only edits,
  exact CMYK channel retention, None, swap, defaults, Undo, read-only gradient
  and mixed-paint opening, and three minimize/restore cycles without stale paint.
- `paint-selection-check.py`: 13/13, applying colours to mixed objects, a group
  and text, retaining geometry/font/content, one Undo, and numeric creation defaults.
- `paint-layout-check.py`: 7/7 at Windows 100% scale, maximized and 1100 by 760.
- Normal close, unsaved discard and open fill/stroke popup close: 8/8 across
  the four dedicated close runs. The stroke/discard case is repeated after
  changing the sidebar position.
- The existing GUI colour suite passes 20/20 with its six GUI cases enabled
  (one pre-existing disabled test). The existing UI-fixes check passes its
  Pattern/Flat application and stale-popup assertions.

Evidence stays local under `../../artifacts/paint-controls/run-003919`,
`../../artifacts/paint-selection/run-005625`,
`../../artifacts/paint-layout/run-004938` and `../../artifacts/window-close/`.
Higher Windows display scaling remains unqualified: requesting GDK_SCALE=2
did not change the native Win32 UI scale on this machine. Dedicated cross-document
switching, spot/pattern/gradient editing through the new sidebar, and stroke
editing on mixed text/groups need broader acceptance runs.

Reference: [Adobe fill/stroke controls](https://helpx.adobe.com/illustrator/desktop/paint-and-fill/learn-painting-basics/fill-and-stroke-controls.html).

## A compact pass: the commands round the tiles (2026-10-20)

The owner: "The bottom part of it extends the menu, cause it's 3 buttons in a
row." The row of three 16 px buttons under the tiles needed 54 px with its
margins, wider than the 42 px tools column, so it widened it. The commands now
sit round the tiles, drawn with Cairo like them, after Illustrator
(`Illustrator_reference/size_reference.png`) and, for the swap, another
application the owner chose (`Illustrator_reference/color_flip.png`):

- **Over the tiles**, 12 px: the default paints at the left (a white square in
  front of a black ring, as the tiles show fill and stroke) and swap at the
  right, two curved arrows chasing each other.
- **Under the tiles**, Illustrator's three 8 px squares, 2 px apart:
  **Color** (white), **Gradient** (a white-to-black ramp) and **None** (white
  crossed out in red). None does what `/` does. Color and Gradient are drawn
  faint and can't be clicked yet: in Illustrator they give the active target
  its last solid colour (`,`) and its last gradient (`.`), which PROOF doesn't
  remember yet (todo). Their tooltips say so.

The whole control is 30 px wide, the tile pair's width, and 58 px high; the
tiles are unchanged. Tooltips, accessible names and the actions (D, Shift+X,
/, X) are as before.

Checked on the installed build at 1920 x 1080, 100%: the tools column stays
42 px wide, the control lies within x 6 to 34. The paint live checks pass:
layout 7/7, defaults 7/7, selection 13/13 and controls 27/27; the controls check
now also clicks None, swap and the default paints and compares each with its
key, and clicks Color and Gradient, which change nothing.

**Square tiles** (2026-10-20, the same day). The tiles had 3 px rounded
corners. The owner: "yes, let's make them square, rounded corners look weird,
after all." They are square now, as in Illustrator, and the small buttons'
hover backgrounds too (`border-radius: 0`).
