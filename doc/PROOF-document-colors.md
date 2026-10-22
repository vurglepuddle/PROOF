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
  [PROOF-ai-native-records.md](PROOF-ai-native-records.md). Native Save As
  writes the document's profile there too (since 2026-10-20), so PROOF's own
  `.ai` reopens in it; before, it reopened in the working profile. See
  [PROOF-ai-interchange.md](PROOF-ai-interchange.md), "Gradient strokes, spot
  and ICC-tagged gradients, and the document's profile".
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

## A profile is saved as it came (2026-10-21)

A document's profile is written into its SVG (`color-profile`), its PDF pages
and its `.ai` copies as the bytes it was read from: the embedded data, or its
file while that file still holds the same profile (the same checksum). Before,
lcms wrote it back, and lcms writes the tables a conversion has parsed in its
own encoding: Coated GRACoL 2006's 8-bit tables came back 16-bit, the profile
grew from 654 KB to 945 KB under the same name, and it converted differently
(Lab 48 68 36 became 7.8% cyan instead of 7.1%). A document therefore changed
profile on its first save after any colour conversion, and the converted
colours with it. Only profiles made in memory are still encoded by lcms.

How: `CMS::Profile::dumpData()` (`src/colors/cms/profile.*`); the static
`dumpData(handle)` is lcms's encoding, still used for checksums. Found by the
`.ai` writer's spot-ink control, which converts Lab inks through the document's
profile and writes the result three times. A profile taken from a PDF through
Poppler (`Profile::create_from_copy`, the PDF import) is still re-encoded.

Check: `ColorCmsProfile.dumpDataKeepsTheBytesItCameFrom` (a transform parses
the profile, then its dump must be the file's bytes, from a file and from
data).

### Why three colour tests fail by one level

`ColorsSpacesCms.realColor`, `ColorsSpacesCms.renderingIntent` and
`ColorDocumentCMSDefsTest.loadDocument` expect 100% black in the test CMYK
profile (`testfiles/data/colors/default_cmyk.icc`, which converts as US Web
Coated SWOP does) to show as `#2c292a`, and this toolchain gives `#2b292a`.
They failed so before this change too (built and run without it,
2026-10-21), and in every note of them since the first, 2026-10-03; they are
not in the focused suites.
The cause is lcms, not PROOF: the toolchain's lcms 2.19 converts that black,
perceptual, to red 42.87 (doubles and 16 bits alike), which rounds to 43
(`2b`); lcms 2.16 gives 44 (`2c`). Upstream recorded the expectation in
January 2024 (`1f6e0b4c0b`), when 2.16 was current. Which lcms release
changed it is not established. The values are correct for the lcms in use;
the expectations could follow it once that change is known.

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

## Oklab gradient interpolation (2026-10-22)

This is a gradient method, separate from the ICC rendering intent also named
Perceptual. Native AI `Bd` settings `1 0` map to `proof:gradient-method="oklab"`
on the SVG stop vector. Original stop colours/midpoints stay editable; the
canvas, previews and bundled PDF backend compute a bounded vector ramp.

The owner's updated CMYK decision is to follow Illustrator's appearance:
convert through RGB, interpolate in Oklab, then re-separate through the CMYK
profile, including rendered endpoints. The original editable CMYK values are
retained, but the printed values can change. No K-free constraint is imposed.
An earlier claim of black added between K-free stops was incorrect: the
23-47% K row belonged to a gradient ending at K=49; the bright all-K-free
gradient stays K-free. See the corrected evidence and limits in
[the interchange contract](PROOF-ai-interchange.md#native-oklab-gradients-2026-10-22).

This changes shared gradient rendering and PDF shading, not Classic blending
or the ICC profile conversion code. A method chooser and live gradient UI
acceptance are not part of this round. The qualified CMYK comparison uses
the supplied SWOP v2 controls; other RGB working-space setups need separate
qualification rather than a claim of exact Illustrator agreement.

### Illustrator 30.6 return qualification (2026-10-23)

Two synthetic RGB/CMYK documents from `209a170c14` were opened in Illustrator
30.6.0, saved in its newest non-legacy AI format with PDF compatibility and ICC
embedding, and exported at 72 ppi, artboard bounds, white, Art Optimized. All
four objects retain their expected Classic/Perceptual methods, original stops
and channels, 35% linear and 60/40% radial midpoints, 65% final radial opacity
and 13.5-pt stroke. Both returns reopen natively with identical original stop
vectors and a byte-preserved CMYK SWOP profile. Six focused reader/importer
tests pass. No artwork corpus or broader setting qualification was run.

The supplied references and Illustrator PNGs are **not pixel-identical**.
Perceptual linear interiors differ by mean/max 9.436/23 RGB and 7.909/27 CMYK
display-channel levels. RGB Classic and the symmetric Perceptual stroke agree
within one level. A diagnostic of the RGB 35% ramp gives mean/max .346/1.351
for `1-(1-u)^(log(.5)/log(1-m))`, versus 9.427/23.282 for the current
`u^(log(.5)/log(m))` mapping. The radial also favours mirroring its .4 interval
while keeping its .6 interval unchanged. This suggests a midpoint-dependent
mirrored branch below .5, but is not a generally qualified replacement.
No sampler or acceptance threshold was changed. Dither-on, freeform, named-ink
Perceptual and other profile controls remain open.

### Perceptual midpoint symmetry (2026-10-23)

The Illustrator 30.6 returns above confirm a mirrored branch below a 50%
midpoint. For midpoint `m < .5`, rendering now uses
`1 - (1-u)^(log(.5)/log(1-m))`; at or above .5 it retains
`u^(log(.5)/log(m))`. Reversing the stops, their opacity and the midpoint now
reverses the rendered ramp. The change is confined to Perceptual rendering
samples shared by the canvas and vector PDF; Classic interpolation, original
editable stops, native definitions and the CMYK re-separation policy stay intact.

Eight focused gradient/importer tests pass, including independent Illustrator
RGB probes and reversal across midpoints .13/.35/.5/.65/.87. Thirty-two checks
on the two exact synthetic source/return controls pass: methods, original
channels/offsets/midpoints/opacity, byte-exact CMYK ICC, native acceptance,
untouched symmetric/Classic interiors and vector PDF without pictures. No
artwork corpus, lite run or fresh Illustrator session was needed.

Against the unchanged Illustrator 72-ppi PNGs, off-centre linear interiors:

| Route | RGB mean / max | CMYK mean / max |
|---|---:|---:|
| Native return canvas | .242 / 1 | 1.447 / 8 |
| Vector PDF, independent profile-managed rendering | .525 / 3 | 1.053 / 6 |

The old native canvas values were 9.436/23 and 7.909/27. PDF comparison must
use the embedded SWOP profile as DefaultCMYK and sRGB for the display, rather
than comparing a generic unprofiled CMYK preview with Illustrator's output.

Full appearance qualification remains open. The inner radial region now has
mean/max .874/30 RGB and 1.270/8 CMYK; RGB's 99th percentile is 6. The largest
RGB differences concentrate near the 45% colour stop; away from that stop and
the centre the maximum is 7. Illustrator's saved PDF stores the radial in a
150-by-67-pixel image. Different rasterization is a possible contributor, not
a proven explanation for every residual. Original geometry is preserved; no
file-specific adjustment, raster replacement or acceptance-gate relaxation.
The independent vector-PDF radial means/maxima are 2.285/30 RGB and 2.162/11
CMYK. Optional Dither/unknown settings, named inks and other profiles retain
the qualification limits above.

## A CMYK document's gradients and meshes blended in CMYK on the screen (2026-10-22)

The owner, 2026-10-22: "of course we want gradients blended in CMYK".
Illustrator blends a CMYK document's gradients and meshes in CMYK, as they
print, and shows the result through the document's profile. Cairo, like a
browser, blends whatever colours it is given in RGB, so PROOF's canvas drew
the same stops with a different middle: up to 31 levels from Illustrator
mid-ramp for two process stops, 63 for yellow to a Lab ink, 78 inside a mesh.
PDF output already blended in CMYK (a shading in a CMYK space does).

In a CMYK document (`proof:color-mode="CMYK"`) the canvas and the gradient
previews now draw from samples blended in CMYK
(`src/object/gradient-rendering.h`):

- **Linear and radial (Classic):** each stop's inks in the document's CMYK,
  a CMYK stop with its own numbers as the document reads them (`interpret`),
  other paint converted as the document converts it (its profile, or the
  working CMYK profile when it has none). Between two stops, up to 64 samples
  blended in CMYK, fewer for a smaller step (a whole channel's step gets 64);
  none across a hard edge. Cairo then joins the samples in RGB. Opacity
  blends as before.
- **Meshes:** a patch whose corners differ is cut along its own parameters
  into up to 8 by 8 smaller patches, the same surface exactly (the Coons
  patch's tensor points by ISO 32000, as Cairo computes them, then
  de Casteljau along both directions); each small corner is coloured from
  the four corners blended bilinearly in CMYK, as Cairo would have blended
  them in RGB.
- **Perceptual gradients** keep their Oklab samples (above).
- **RGB documents are unchanged**, to the pixel.

Drawing only. The stops and patches in the document, and what the writers
and PDF output take, stay as they are. The samples are converted to sRGB once,
when the canvas builds the gradient's paint, instead of again for every tile;
a change of the working colours restyles every object, which builds them
again.

**Against Illustrator 29.2.1** (its renders of the round's controls, cells
composited on white): the mixed-model control's CMYK document, process to RGB
31 to 20 at most (within 1 to 4 along most of the ramp; the 20 is where cyan
leaves the screen's gamut, which the two colour engines clip differently), RGB
to a Lab ink 63 to 11, RGB, CMYK, RGB 17 to 12, an RGB ink to CMYK 22 to 9;
its RGB document is unchanged. The mesh control's CMYK document, mean
difference per cell from 10 to 22 levels down to 1.8 to 2.9, the 99th
percentile from 37 to 69 down to 8 to 18, except two cells: the reflected
mesh's 35 is its own antialiased edge inside the cell, and the masked mesh
stays near 10 on average, because the round's writer gives Illustrator mask
art in Gray, which Illustrator blends in black ink alone.

**The ink/process difference was fixed later the same day**, "An ink beside
process colour" below. **Still differs from Illustrator:** grey (Gray) stops
blended in black alone. Transparency
composited in CMYK (the canvas composites in RGB).

**PDF readers differ here.** A CMYK mesh shading in the PDF is unchanged, and
a conforming reader blends it in CMYK. Poppler converts mesh corners to RGB
first: against Illustrator's render of the mesh control, Poppler's Cairo
drawing of PROOF's PDF is 10 to 21 levels off on average and its Splash
drawing 17 to 52, the canvas under 3. So `ai-mesh-output-smoke.py` checks the
CMYK document's PDF and `.ai` page, through Poppler, against the canvas's RGB
blend of the same corners (the control rendered as an RGB document, whose
CMYK paint shows through the same working profile): the same patches and
corners in the same places. `ai-mixed-gradient-output-smoke.py` now
composites PNGs on white before comparing, as Illustrator's renders are.

**Checks:** `test_sp-gradient` 25 (eight new, `GradientRenderingTest`: RGB
documents untouched, the CMYK blend and its sample counts, hard edges, RGB
stops converted as the document converts them, Perceptual left to Oklab, a
mesh patch cut 8 by 8 on its own surface with corners blended in CMYK, curved
sides traced exactly); `test_ai-native-import` 181 (two opt-in skips); 15
export groups, 361 checks; the lite gradient and mesh cases, 6 of 6, no
regression. Exporting a CMYK document of 2,000 gradients and 40 sixteen-patch
meshes takes 6.2 s instead of 5.5 s, start-up included; the same document in
RGB is unchanged.

## An ink beside process colour (2026-10-22)

The owner, 2026-10-22: "yes, let's follow illustrator in spot ink next to
process colour". Illustrator doesn't blend an ink stop and a process stop as
two colours. It keeps the ink an ink and overprints it on the process ramp,
and the canvas now draws such gradients that way (`ink_blend` in
`src/object/gradient-rendering.cpp`), read from Illustrator 29.2.1's own pages
of the mixed-stops controls:

- **CMYK document.** The page paints the gradient in DeviceN (the ink and the
  process channels it uses, `/NChannel`). The shading blends the ink's tint
  and the process channels linearly, and the tint transform gives each CMYK
  channel `1 - (1 - tint x the ink's CMYK)(1 - process)`: the ink printed over
  the process colour. The ink's CMYK is its definition in the document's CMYK
  (a CMYK-defined ink's own numbers, a Lab or RGB one converted).
- **RGB document.** The page paints the ink's Separation shading, then the RGB
  shading over it with overprint on, marked `/MultiChannelPaint`. Illustrator
  shows the ink's tint ramp multiplied over the process ramp, each against
  white: the same rule, in what each channel takes away from white. The
  Separation's tints are the ink's own (`SpotInk::display_color`): a
  CMYK-defined ink's inks scaled, a Lab or RGB one blended towards white in
  its own space, as Illustrator's page declares the Lab ink's Separation.

So mid-ramp the result is lighter than a blend of the two colours, by the
product of what each takes away. It applies wherever a stop's ink stands
beside a process stop or beside another ink; a gradient of one ink's tints is
blended as before, and Perceptual gradients keep Oklab. The stops and the
`.ai` writers are unchanged: Illustrator reads the inks and draws them its way.

**PDF output draws such a gradient as the canvas does** (`pdf_vector`, the same
samples), so PROOF's own pages, the PDF viewers and the check of a written
`.ai` against its page all agree. It **still prints in process colour**:
writing it as DeviceN (CMYK) or `/MultiChannelPaint` (RGB), with the ink on
its own plate, belongs to the separations work. Before this, the page blended
the ink's alternate linearly; with the canvas alone changed, PROOF's own `.ai`
with such a gradient fell back to its page on reopening (the gate saw the
difference), which this keeps from happening. Poppler, which doesn't simulate
overprint, shows Illustrator's RGB pages of such gradients with one of the
two shadings alone; those documents still open through their page.

**Against Illustrator's renders** of the mixed-stops control (cells on white;
before is the canvas of that round): RGB document, a CMYK-defined ink to RGB
18 to 3 levels at most, an ink at 90% with an off-centre midpoint 15 to 4,
an RGB-defined ink to CMYK 26 to 3, RGB to a Lab ink 24 to 13 at most (one
pixel; mean 3.2, and along the radius 3.8 against 6.1 for a tint ramp linear
in RGB); CMYK document, those cells 18 to 4, 18 to 5, 22 to 10 and 63 to 11,
the rest within the colour engines' differences at the ink's own colour.

Illustrator's own saved copies of the control: the CMYK one still opens
natively; the RGB one still opens through its page (15.3% by lightness, was
16.6%), because Poppler doesn't simulate the overprint of `/MultiChannelPaint`.

**Checks:** `test_sp-gradient` 30 (five new: the CMYK rule worked by hand, the
RGB rule, one ink's tints left to the blend, inks found when the stops don't
reach the ends, PDF output's vector the canvas's); `test_ai-native-import`
181; 15 export groups, 361 checks (with only the canvas changed, the
gradient-paint control's written `.ai` fell back to its page: the reason PDF
output follows); the lite gradient, non-CMYK spot and mesh cases 7 of 7.

### RGB PDF overprint preview follows the mixed ramp (2026-10-22)

The RGB page-check/import limit above is now corrected for the bounded
`/MultiChannelPaint` Separation/DeviceRGB pairs. A disposable PDF preview uses
Multiply on the second shading; the source's ink records and PDF separations
are untouched. Both comparison threads and the default editable PDF importer
use it. The qualified Illustrator-saved RGB control now opens natively, within
13 display levels of Illustrator; default PDF import is within 11. Unknown,
shared-state and broader overprint forms remain outside this simulation; the
optional Cairo-to-SVG route and embedded foreign-PDF subset are unchanged.
DeviceGray soft-mask art also keeps its numeric coverage instead of going
through print-black conversion. Ordinary PDF grey paint still retains K.
[Current scope](PROOF-ai-interchange.md#rgb-pdf-overprint) and
[qualification](PROOF-ai-interchange-history.md#rgb-pdf-overprint-2026-10-22).
