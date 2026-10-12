# Corner widget display

View > Corner Widgets shows or hides the Direct Selection corner controls.
Preferences > Tools > Node has the same visibility checkbox and an angle limit,
"Hide widgets for angles above". Visibility defaults to on; the limit defaults
to 180 degrees, allowing every otherwise eligible corner.

These are application preferences. Changing them updates existing document
windows and their checked View menu items. They do not alter SVG geometry,
corner radii, selection, or document Undo history. Anchor editing remains
available when corner widgets are hidden.

The limit measures the angle between the incoming and outgoing corner tangents
after the live-corner frame and object transform. A nonuniform scale can therefore
change which corners are visible. Corners at the limit remain visible. Existing
selection and crowding rules still apply; this does not expand the geometry that
Live Corners supports. [Corner groups](PROOF-corner-groups.md) subsequently extend
selected-widget editing across objects.

Preference changes queue an idle widget refresh. An active corner drag or popup
keeps its controls until the edit finishes, then applies the display setting.
The C++ window owns its action preference observer. Closing it removes the
observer before GTK disposes its action group, including actions retained by
menus. The callback does not retain the window or action.

## Implementation and checks

- `Model::displayAngle` measures transformed tangents and rejects collapsed or
  invalid geometry.
- `LiveCornerController` observes the two preferences and filters its widgets.
- `actions-view-mode.cpp`, `menus.ui`, and the Node Preferences page expose the
  controls.
- `test_live-corners` covers ordinary, rotated, nonuniform and collapsed angle
  calculations alongside the existing rounding and recovery regressions.
- Workspace `tools/corner-display-check.py` drives disposable synthetic SVGs
  and copied profiles, checking visibility, editing, Undo, multiple windows and
  transformed angle limits. The workspace handoff records installed results.

On 2026-10-13, the rebuilt geometry suite passes 16/16 and the complete Windows
live display suite passes 39/39. Existing rounding and unsaved-close regressions
pass another 39/39. The active-drag check verifies that unrelated shortcuts do
not interrupt the grab or change its visibility; ordinary display settings are
changed through the real menu and Preferences fields while the editor is idle.

Reference: [Adobe anchor display preferences](https://helpx.adobe.com/au/illustrator/desktop/draw-shapes-and-paths/learn-drawing-basics/adjust-anchor-point-handle-and-bounding-box-display-size.html)
and the owner's `brief/direct-selection-brief.md`, priority 6.
