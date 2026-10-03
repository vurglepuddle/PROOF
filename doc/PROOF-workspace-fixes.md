# Workspace fixes (2026-10-03)

Fixes from the user's trial of the transform box build. The transform box
changes (inert corner clicks, geometric bounds) are in
[transform box](PROOF-transform-box.md).

## Stray "Abstract 1" pattern

**Report:** clicking PROOF's taskbar button, usually after leaving with
Alt+Tab, turned the fill or stroke last edited into a wavy black-and-white
pattern. The pattern editor had not been used before.

**What it was:** "Abstract 1" is the first stock pattern. When nothing is
picked in the pattern list, `PatternEditor::get_selected_stock_pattern()`
returns that first pattern, so that switching to pattern mode has something to
apply. Three faults let it reach an object without a click on the Pattern
page:

- The fill and stroke popovers of the Properties panel share one
  `PaintSwitch` each (`PaintPopoverManager`). It stayed connected to the last
  object after the popover closed, so anything it emitted later changed that
  object.
- The hidden pattern, gradient, mesh, swatch and flat-colour editors still
  emitted changes. Several pattern-editor fields (scale, offset, name) emit
  on any value change, with no sensitivity check.
- `PaintSwitchImpl::_set_mode()` updates the mode buttons. That counted as a
  click, so the matching paint was applied again.

**Fixes:**
- The popover disconnects from the object when it closes. This happens on
  idle, so an entry that commits as focus leaves still applies.
- Each editor changes the paint only while its page is showing.
- `_set_mode()` updates the buttons under the update blocker. A mode button
  acts only while it is on screen.

The exact signal that fired on window activation was not reproduced. Each
fault on its own was enough to apply the pattern, and all three are closed.

## Floating dialogs opened tiny and lost their size on restart

**Report:** floating panels opened very small, and a size set by the user was
gone after closing PROOF.

**Cause:** building a floating `DialogWindow` adds the dialog to its notebook.
That ran `DialogBase::focus_dialog()`, which presented the window before its
size was set. GTK 4 ignores a default size once a window is visible, so:
- New floating dialogs opened at their minimum size (Preferences at 280 x 349).
- At startup, a dialog left open at quit came back at the initial 360 x 520.
  The next quit saved that, so the size stayed lost.

A dialog closed before quitting kept its size. Its reopening path applies the
saved size first.

**Fixes:**
- `focus_dialog()` raises only a window that is already showing. Every place
  that creates a `DialogWindow` shows it once it is sized.
- `load_container_state()` applies a saved size right after creating the
  window, before adding dialogs, as `recreate_dialogs_from_state()` does.
- A new floating window stays within three quarters of the main window, so
  Preferences no longer opens taller than the screen.

Floating windows restored at startup are still placed by Windows. GTK 4 has no
window positioning, so they appear near the top-left instead of where they
were.

## Panel dividers ignored straight drags

`DialogMultipaned::on_drag_update()` ignored any update unless the pointer had
moved at least 1 px along both axes. A perfectly vertical or horizontal drag
(a steady hand, a tablet or a script) did nothing. Only the axis the divider
moves along is tested now.

The docked layout itself (divider positions, column width) was saved and
restored correctly across minimise and restart in every test.

## Swatch import folder

**Load color palette** opens in the folder last loaded from, which is
remembered across sessions (`/dialogs/swatches/import-folder`). With no
remembered folder, or one that no longer exists, it opens in the profile's
palette folder, the one PROOF reads palettes from. It used to open in the home
folder each session.

## Square corners

`share/ui/proof-square.css` sets `border-radius: 0` on every widget. Radio
buttons stay round, since the circle tells them apart from check boxes. It
loads at `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10` (`ThemeContext::add_gtk_css`):
- above the GTK theme and `style.css`, so it wins whatever their selectors
- below `user.css`, which can still override it

The rounded rules in `workspace.css` were removed. Windows' own Open, Save and
Import dialogs are native and already square on Windows 10.

## Verification

- `tools/ui-fixes-check.py`: 9 of 9 pass (`artifacts/ui-fixes/run-*`):
  - geometric bounds
  - an inert corner click
  - opposite-corner scaling
  - the Fill popover applying a pattern and a flat colour
  - the fill surviving three real minimise/restore cycles after the popover
    closed
  - both swatch-folder cases
  - a straight divider drag
- `tools/dialog-restore-check.py`: 3 of 3 pass, over three launches on one
  profile (`artifacts/floating-dialogs/check-*`).
- `tools/transform-box-check.py` 28/28 and `tools/direct-selection-check.py`
  11/11 still pass.
- Unit suites: `test_sp-document` 84/84, `test_cmyk-picker` 20/20 (with
  `INKSCAPE_TEST_GUI=1`), `test_spot-ink` 17/17, `test_extensions-pdfoutput`
  5/5.

Earlier minimise tests in this session sent `WM_SYSCOMMAND` to a stale window
handle and did nothing. Only the runs above, which confirm `IsIconic`, count.
