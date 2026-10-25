# PROOF's loading splash

Requested by the owner on 2026-10-17. Windows GUI launches show a compact,
square-cornered splash while the editor initializes, including when the welcome
screen is disabled. The picture adapts the existing social banner's mark,
Direct Selection handles, wordmark, crop marks and press colour strip.

The window is 680 by 440 logical pixels at 100% display scaling. It scales for
the launch monitor's DPI and fits within its work area. The editable picture is
`share/screens/proof-startup.svg`; `proof-startup.png` is its 2x export, 1360 by
768. The bottom 56 logical pixels are drawn by the application: a loading
message and an animated, indeterminate blue bar. There is no artificial wait
or claimed progress percentage.

## Which monitor

The splash opens where the editor will. GTK leaves a new window's place to
Windows, which puts it on the monitor the program was launched from: Explorer,
the taskbar and Start name it in the program's startup information
(`STARTF_HASSHELLDATA`, the monitor in `hStdOutput`), and without it (a
terminal, a script) the primary monitor. The splash reads the same
(`launch_monitor` in `src/ui/startup-splash.cpp`). It used the monitor under
the pointer, so it showed on another screen whenever the pointer had moved
there while PROOF loaded (the owner, 2026-10-22: "It should be on the screen
where the app will launch"). Checked by launching on each of two monitors with
the pointer on the other, and with no monitor named: the splash and the editor
on the same one each time.

## When it appears

It starts in `InkscapeApplication::on_startup`, after command-line parsing and
instance forwarding have resolved whether this process will create a GUI.
Ordinary empty launches, opening a file at launch, and the optional welcome
screen use it. The loading message changes while initializing extensions,
preparing the workspace, or opening the launch document. A low-priority idle
callback removes the splash after activation/opening returns to GTK, giving the
editor or welcome screen a chance to appear. Scope guards cover unsuccessful
and cancelled file opens; application destruction also joins the splash thread.

The first PROOF window shown ends it too (GTK's list of top-level windows is
watched until the splash goes): a dialog that asks about a file opened at
launch, such as the PDF import dialog, runs inside opening, and the splash
stayed behind it until it was answered. The owner, 2026-10-17: "It should be on
top of the PDF import screen though - cause it loads > then you load whatever
files": PROOF's loading first, then the file's.

## The welcome screen's theme

Profiles from before PROOF's theme remember Inkscape's default, "Colorful", as
the welcome screen's theme (`/options/boot/theme`), so it showed Colorful
although PROOF heads the list. Such a profile is moved to PROOF's theme once
(`/options/boot/proofThemeAdopted`); Colorful chosen afterwards stays.

Headless exports, queries, help/version requests, shell mode, batch processing,
and launches with command-line actions skip it. GUI automation may also pass
`--no-splash`, which does not change whether a GUI is created. A launch forwarded
to an existing application never reaches `on_startup` and creates no splash.
Opening more files in an already running editor does not start another splash.

## Why it uses a native window

Editor startup performs synchronous work on the GTK thread. A GTK progress
widget would stop animating during that work. `src/ui/startup-splash.cpp` uses a
small Win32 window on its own thread and message loop instead. The thread owns
its PNG decoder, painting, font, timer and native window. It never calls GTK or
accesses document/editor state. The main thread sends only a locked text string
and closes/joins the worker during teardown. The splash does not take keyboard
focus or stay above other applications. It has no frame, taskbar entry, rounded
corners or close control. If the image cannot be read, a plain PROOF title and
the loading indicator still appear; failure to create the optional thread/window
does not prevent startup.

The splash registers the executable's embedded `APPLICATION_ICON` at Windows'
large and small icon sizes. This gives Windows the PROOF mark for the early
startup window, before GTK creates the editor window.

Other platforms keep their existing startup behaviour for now. Large-file
loading feedback in an already open editor remains separate work.

## Regenerating the picture

Export a copy of the SVG with the installed editor in headless mode:

```powershell
inkscape.com --export-type=png --export-width=1360 --export-height=768 --export-filename=proof-startup.png proof-startup-copy.svg
```

Use an isolated profile as with the other checks. The text uses Windows'
Bahnschrift; the wordmark and the main mark are paths. The shipped PNG keeps
startup independent of SVG rendering and the installed fonts.
