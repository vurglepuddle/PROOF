# Document color management

Use **Document Setup > Color** to choose the document color mode and profile.

1. Set working RGB and CMYK profiles under **Working Spaces**. These are
   application defaults; existing documents keep their assigned profiles.
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

For the corporate 5/100/45/22 swatch, choose CMYK and the profile used for the
print job, then apply. Its four channels remain 5/100/45/22. Match Illustrator's
actual profile and conversion settings when comparing applications: GRACoL
2006 and FOGRA39 describe different printing conditions.

The conversion engine is **LittleCMS 2**. Relative Colorimetric with black
point compensation is the default. Other available intents are Perceptual,
Saturation and Absolute Colorimetric. Conversion options apply when the
document profile is assigned; changes to defaults do not silently reassign
open documents. New documents use the working RGB profile, initially sRGB.
The initial CMYK default is Coated GRACoL 2006 if installed; otherwise choose
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
Documents have independent assignments. Existing files are not silently
assigned the application's current working profile on open.

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
