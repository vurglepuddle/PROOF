# Fill and stroke controls in the tools sidebar

The tools sidebar contains two overlapping rounded squares: a solid fill target
and a hollow stroke target. A red diagonal marks None. The active target appears
in front; click either exposed square to activate it, or double-click to open
the existing native paint editor. Mixed paints have an indicator rather than
an averaged colour. Gradients retain their paint when the editor is opened.

## Commands and paint application

The small buttons provide default paints, swap and None. Under the Illustrator
keymap, X switches targets, Shift+X swaps fill/stroke, D applies white fill and
black stroke, and / removes the active paint. These shortcuts require canvas
focus and are disabled while the Text tool is active or an entry has focus.
In a CMYK document, D uses zero-ink white and K-only black. Default stroke width
is 1 document unit and default paint opacity is 100%.

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
