# CMYK picker investigation - 2026-10-03

Historical checkpoint: the assignment and profiled-picker limitations described
below are addressed by the subsequent [document color management](PROOF-document-colors.md)
work. The arithmetic preview remains available for explicitly unmanaged CMYK.

The reported 5/100/45/22 -> 0/100/42/26 drift is reproduced and fixed. It does
not require FOGRA or any linked profile.

## Cause and correction

`ColorSet::_set()` preserved an existing entry's previous colour space even
when the set had no explicit space constraint. `PaintSwitch` starts with RGB
black; swatch editors can also retain a previous RGB swatch. Loading the CMYK
selection into that entry therefore converted it to RGB. The CMYK slider page
then converted the RGB back to CMYK, displaying exactly
0/100/42.105263/25.9. The earlier test missed this because it started with an
empty set and inserted CMYK first.

Replacement now preserves the incoming colour space and alpha, subject to the
set's explicit constraints. `setAll(Color)` retains its existing conversion
semantics for operations such as the RGB hex entry and colour wheel.

The colour page's initial map also needed an echo guard: populating a newly
shown page must not send the converted page colour back into the selection.
Opening RGB/HSL pages now leaves the underlying CMYK values unchanged.

## Linked profile versus assigned profile

The document profile list is a registry of available profiles, not a working
CMYK-space setting. `DocumentCMS::parse()` only resolves an ICC space for
colours that explicitly reference it. Plain `device-cmyk(...)` retains the
uncalibrated arithmetic preview in `colors/spaces/cmyk.h`, regardless of which
profiles are linked. The same four values explicitly assigned to FOGRA39
produce a different profile-based preview through the existing CMS code.

This fix does **not** add a document working-CMYK profile, assign profiles to
existing artwork, or change the device-CMYK rendering formula. The regular
picker still offers built-in spaces rather than linked ICC profile pages.
Editing explicitly profiled colours through those built-in pages is not
qualified by this fix. A profile-assignment/editing workflow and coherent
document preview remain separate work; do not implement these by converting
the uncalibrated RGB preview back to CMYK.

Open `../../tests/cmyk-profile-comparison.svg` in PROOF to compare the same ink
values with and without explicit FOGRA assignment. It references the installed
Windows `CoatedFOGRA39.icc`. The installed editor successfully rendered
`../../artifacts/cmyk-profile-comparison.png`; this is a preview comparison,
not a print or Illustrator colour-match certification.

## Validation and installation

- `test_colors_color-set`: 15/15, including RGB-to-CMYK replacement and explicit
  space/alpha constraints. The new CMYK replacement test failed before the fix.
- `test_cmyk-picker`: 5/5 with `INKSCAPE_TEST_GUI=1`. Real GTK controls cover
  all three plate modes, cyan-only edits, RGB/CMYK reselection, read-only picker
  page changes and swatch edits. Only the cyan edit emits a swatch write.
  The linked-profile test verifies that FOGRA actually loaded and that explicit
  ICC serialization retains the four values.
- `test_spot-ink`: 17/17 (one pre-existing disabled diagnostic).
- `test_colors_xml-color`: 3/3; `test_extensions-pdfoutput`: 5/5.
- Broader `test_colors_document-cms`: 12/13. `loadDocument` expects RGB fallback
  `#2c292a` but this toolchain produces `#2b292a`, with identical ICC channels.
  A control rebuild with both production fixes removed reproduces the same
  failure. No fixture expectation or CMS conversion was changed to hide it.
- `git diff --check` passed. GTK sizing and locale warnings remain in test logs.

Evidence is under `../../artifacts/`: `*-before.out` records reproduction,
`*-verified.out` records the final focused suites, and `cms-control.out` records
the unchanged-code ICC control. The tests require the local UCRT64 and MSYS
`bin` directories on PATH, `INKSCAPE_DATADIR=build/install/share`, and an isolated
`INKSCAPE_PROFILE_DIR`. On Windows, use a waited process and check its exit code
and explicit test summary; initialise gtkmm, custom widget registration and
SVG import extensions when running the GUI harness.

Both `inkscape` and `inkscape_com` were rebuilt and copied to `build/install/bin`
with SHA-256 comparison against the build outputs. The preceding executables
are retained as `inkscape-before-cmyk-fix.exe` and `.com`. Restart through the
normal workspace launcher to use the fix. No user artwork or workspace
preferences were changed, and no commit was made. Re-enter known CMYK values
if they were already saved after a lossy RGB round trip; the original K cannot
be inferred from that RGB value.
