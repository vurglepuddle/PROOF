# Document color management

Every document window is in **CMYK** or **RGB** mode, shown after the file
name in the title bar, for example `poster.svg (CMYK)`.

- **File > Document Color Mode > CMYK Color / RGB Color** converts the
  document using that mode's working profile, in one undoable step. The
  choice also becomes the mode for new documents. It is stored as
  `/options/workingcolors/newmode` and initially defaults to CMYK when a CMYK
  working profile is available.
- **A file opened without a color mode** gets one without being edited.
  It becomes CMYK if any process paint is CMYK, otherwise RGB. An embedded
  profile for that mode is kept, as Illustrator's "preserve embedded
  profiles" policy does. Otherwise the working profile is assigned. Paint
  values are not rewritten. The document is not marked modified, and the
  profile is written the next time it is saved.
- **An Illustrator file opens in its own profile** when it carries one
  (since 2026-10-11): the profile is taken from the file's PDF part, where
  Illustrator keeps it. A file that carries none is in the working profile,
  as in Illustrator. See "The document's own colour profile" in
  [PROOF-ai-native-records.md](PROOF-ai-native-records.md).
- **100% black shows as its profile makes it**, a dark grey, like every other
  colour: what Illustrator calls displaying all blacks accurately. There is no
  setting that shows it as the screen's black.
- **Don't color manage this document** is a mode without a profile and is
  respected on reopening.

Use **Document Properties > Color** for profile details.

1. Set working RGB and CMYK profiles and the mode for **New documents**
   under **Working Spaces**. These are application defaults; existing
   documents keep their assigned profiles.
2. Select **RGB** or **CMYK** under **Document Color**.
3. Choose **Use working space**, **Assign a profile**, or **Don't color manage
   this document**. The profile list contains only profiles for that mode.
4. Click **Assign Profile**, or **Convert to This Mode** when changing modes.

Assigning a CMYK profile to CMYK artwork preserves its C/M/Y/K values. The
canvas, process swatches and profiled picker then use that profile's ICC
conversion for their screen appearance. Changing RGB/CMYK mode converts
process colors; values may change. The picker retains the profile while
editing individual channels. A color wheel or RGB hex edit is a color-space
conversion and does not promise to preserve the other CMYK channels.

**CMYK pictures** keep their numbers the same way (since 2026-10-16): a placed
CMYK TIFF shows through the document's profile, whatever profile the picture
carries, and prints its numbers as they are. See
[PROOF-cmyk-pictures.md](PROOF-cmyk-pictures.md).

For the corporate 5/100/45/22 swatch, choose CMYK and the profile used for the
print job, then apply. Its four channels remain 5/100/45/22. Match Illustrator's
actual profile and conversion settings when comparing applications: GRACoL
2006 and FOGRA39 describe different printing conditions.

The conversion engine is **LittleCMS 2**. Relative Colorimetric with black
point compensation is the default. Other available intents are Perceptual,
Saturation and Absolute Colorimetric. Conversion options apply when the
document profile is assigned; changes to defaults do not silently reassign
open documents. New documents use the working profile of the new-document
mode. The working RGB profile is initially sRGB. The initial CMYK default is Coated GRACoL 2006 if installed; otherwise choose
a profile explicitly. A missing saved working profile is not substituted.

## Linked versus assigned

The old **Link Profile** control remains under **Additional Linked Profiles**.
It makes an ICC space available to individual colors. It does not assign that
space to plain `device-cmyk(...)` artwork. This explains why linking FOGRA
previously left the bright arithmetic CMYK preview unchanged.

The document assignment is stored as `proof:color-mode` and
`proof:color-profile` on the SVG root. Assigned profiles are embedded in SVG
as `color-profile` resources; explicit process paints use `icc-color(...)`
with an RGB fallback. Saving, reopening and undo/redo retain the assignment.
Documents have independent assignments. Files without a mode are given one
when opened in a window, as described above. Command-line exports do not do
this.

## Opening PROOF SVG files elsewhere

`icc-color()` and the `color-profile` element are SVG 1.1. Browsers and
Illustrator do not implement them. A browser discards a whole
`fill:#b51446 icc-color(...)` declaration, so the shape falls back to black,
and a stroke disappears. Illustrator 29 reads the leading RGB, except when
the profile name contains a colon, as in names generated before 2026-10-03
(`...12647-2:2004`); then it also shows black.

Saving as Inkscape SVG or Plain SVG now also writes the RGB preview of every
ICC or CMYK style paint as a presentation attribute. Example:
`style="fill:#b51446 icc-color(...)" fill="#b51446"`. Where the style is
understood, it wins. Elsewhere, the RGB preview is used. PROOF removes these
attributes when it reads a file, so they can never go stale. The CMYK value
in `style` remains the source of truth.

Verified 2026-10-03 with a copy of `TEST_HERE_FIXED.svg`: Edge rendered the
rectangles `#000000` before and `#b51446` after, and Illustrator 29.2.1 read
`RGB(0,0,0)` before and `RGB(181,20,70)` after. Other applications see an
RGB file. Only PROOF reads the CMYK channels and the embedded profile.

## Importing, dropping and pages

Paints refer to profiles by name. When an imported SVG (Include, or as new
pages) contains a profile whose name the document already uses, the
document's own profile is kept and the imported copy is not added. This
applies even if the ICC data differs, in which case a warning is logged. CMYK
numbers are preserved, as with Illustrator's default CMYK policy. A
differently named profile is added alongside it, and the imported artwork
keeps that profile. Previously, every PROOF CMYK file dropped into another
one crashed: both embed the same profile, and registering the second copy
threw out of the document signal. A duplicate name in a single file, or a
renamed profile resource, now logs a warning instead of aborting.

## Scope

This is ICC-managed process-vector color, not a complete press soft-proofing
pipeline. Spot ink definitions and placed images retain their own colors.
Paper-white simulation, overprint simulation and a calibrated monitor/press
match have not been qualified. The preview assumes the assigned profile
matches the intended printing conditions.

The PDF-based **Illustrator interchange (.ai)** writer remains RGB and does
not preserve CMYK separations or spot ink identity. Use the dedicated prepress
PDF workflow for press output and verify its output settings separately.
See [AI interchange](PROOF-ai-interchange.md) for supported interchange scope.

## Validation

The [tools-sidebar fill/stroke controls](PROOF-fill-stroke-controls.md) preserve
native CMYK paint when opening and editing the shared colour editor. Their
2026-10-11 live checks include exact channels, CMYK defaults and new-shape paint.

The CMYK regression harness drives real GTK controls and document objects. It
covers channel preservation and opacity, profile embedding and SVG reload,
undo/redo, disabling management, RGB/CMYK conversion, spot metadata retention,
profile-filtered settings, document switching and profiled cyan edits. GUI
tests require `INKSCAPE_TEST_GUI=1`; the optional
`INKSCAPE_COLOR_SETTINGS_SNAPSHOT` path captures the actual settings widget.

Related regression suites cover ColorSet, XML colors, spot inks and PDF output.
The existing document-CMS suite has a known toolchain-dependent one-level RGB
fallback mismatch (`#2b292a` versus `#2c292a`); see the
[picker investigation](PROOF-cmyk-picker.md) for the unchanged-code control.

On the Windows workspace build, 50 focused checks pass and AI interchange
passes 19/19 smoke checks. The wider CMS suite passes 12/13 with only that known
fallback mismatch. An installed-CLI rendering check using Coated GRACoL 2006
produces RGB (182, 19, 70) for assigned 5/100/45/22, identical to explicit ICC
color. Unmanaged and merely linked versions both produce (189, 0, 109).
Local evidence: `../../artifacts/working-colors-render/result.json`,
`../../artifacts/working-colors-settings.png`, `working-colors-verified-*.out`
and `../../artifacts/ai-interchange/working-colors/report.json`.

### GRACoL selection and clipboard drift fix

The generated GRACoL name previously included a colon (`ISO-12647-2:2004`).
That is an XML name character but cannot appear unescaped in a CSS identifier.
The CSS parser dropped the `icc-color(...)` suffix, leaving the RGB fallback;
reading it into the assigned CMYK document converted it back to different ink
values. Repeated style updates and copying compounded the error.

New profile identifiers exclude colons and periods. Before CSS parsing, legacy
ICC names are escaped without changing their profile lookup names or channels.
This applies to inline styles, inherited clipboard styles and stylesheets.
Previously saved files remain readable, but already corrupted channel values
must be restored from known originals.

The fix passes 80 focused color, picker, profile, parser, style, spot and PDF
checks (one optional locale test skipped and one existing spot test disabled).
Regression coverage includes manual CMYK assignment, repeated style reads,
ten inherited-style copies, SVG reload and stylesheet colors with legacy names.
An actual Windows Ctrl+C/Ctrl+V check with GRACoL 2006 and Perceptual intent
produced four successive pasted copies; all six saved rectangles retained
exactly 5/100/45/22. Local evidence: `../../artifacts/cmyk-final-*.out` and
`../../artifacts/cmyk-clipboard-live-result.json`. The user's original test SVG
was not modified.

## CMYK paint without a CMYK profile of its own (2026-10-20)

`device-cmyk(...)` paint in an RGB document, in a document that isn't colour
managed, or in a file exported from the command line without a mode is shown
through the working CMYK profile and intent, as Illustrator shows CMYK in an RGB
or untagged document through its working CMYK space. Until now CSS's
uncalibrated formula drew it: cyan as 0 255 255, where Coated GRACoL 2006 makes
it 0 159 227 and Poppler 0 173 239. A document with a CMYK profile of its own
still shows its paint through that profile.

- **The numbers stay.** The paint is still written `device-cmyk(c m y k)` and
  keeps its colour space, so the picker, swatches and comparisons see the same
  colour. What follows the profile is how it looks: on the canvas, in PNG
  export, in the RGB preview written beside it in SVG, and in conversions.
  Converting an RGB colour to CMYK (the picker's CMYK fields for an RGB colour;
  the CMYK alternate the `.ai` writer gives an RGB gradient stop) gives the
  profile's separation, as in Illustrator: red `#ff0000` is about 0/96/93/0 in
  GRACoL 2006, no longer 0/100/100/0.
- **It follows the setting.** Changing the working CMYK profile, the intent or
  black point compensation (Document Properties > Color > Working Spaces)
  redraws the open documents. Without a working CMYK profile (none chosen, and
  Coated GRACoL 2006 not installed) the formula is used, as before.
- **A document that isn't colour managed** shows its paint through the working
  profile too, as Illustrator shows an untagged document. The installed build
  draws the corporate 5/100/45/22 as (182, 19, 70) in such a CMYK document, in
  an RGB document and in a file without a mode, the colour an assigned GRACoL
  2006 gives; under Validation, the unmanaged and merely linked versions were
  (189, 0, 109) before this. Numbers and the absence of an embedded profile are
  unchanged.
- **CMYK pictures** in an RGB `.ai` file are shown through the same profile when
  the file is read.

Why: a PROOF-written `.ai` from an RGB document with large CMYK areas reopened
from its PDF page, because Poppler, which draws the page that the art is checked
against, uses a formula fitted to SWOP. Over 625 CMYK values, the canvas through
GRACoL 2006, FOGRA39 or SWOP is nowhere past the page check's limits from Poppler
(at most 64 in a channel, at 75% cyan, and 26 in lightness); the old formula was
past them for 21 to 29.

How: `Space::DeviceCMYK::setProfile` (`src/colors/spaces/cmyk.*`), what CSS
Color 5's `@color-profile device-cmyk` does; `DocumentColors::showDeviceCMYK()`
sets it from the working preferences at startup and when they change
(`inkscape-application.cpp`). The colour library's default stays the formula,
so tests that don't start the application see it. Colour spaces now cache their
transforms by both profiles, under a lock, as canvas tiles are drawn on several
threads. The corpus check (`AiNativeImportTest.Corpus`) shows CMYK as the
application does.

Checks: `ColorsSpacesCmyk.shownThroughProfile`,
`ColorDocumentCMSObjsTest.deviceCmykShownThroughWorkingProfile`, and the radial
smoke's cyan control, which reopens from its own records with its flat colours
within 17 of Poppler's (82 at `b83a8dcf39`, where it opened from its page).
