# PROOF's own name and face

PROOF is based on Inkscape and no longer presents itself as Inkscape. The
editor says PROOF where it speaks of itself, and Inkscape where something is
Inkscape's. First slice: 2026-10-11.

## The rule

- **PROOF** is the name of this editor: its windows, its menus, its About
  window, its start screen, its icon, and the place its faults are reported.
- **Inkscape** stays wherever the thing named is Inkscape's: the version PROOF
  is based on, the developers and translators, the licence, the manual and the
  tutorials, the file format "Inkscape SVG", and the requests to support the
  Inkscape project. Donate leads to Inkscape.

## What says PROOF

| Where | What it shows |
|---|---|
| A window's title | `name (CMYK) - PROOF`, followed by the build's revision and date when Preferences > Interface > Windows > "Show version in title" is on |
| File and Help menus | "Quit PROOF", "About PROOF", with PROOF's mark beside About |
| About | PROOF's mark and wordmark; "PROOF is based on Inkscape." with links to inkscape.org and to PROOF's repository; "PROOF, based on Inkscape 1.5-dev (revision, date)" as the version, which a click copies |
| The start screen | PROOF's banner while loading and above the setup and the files tabs |
| Windows' details of `inkscape.exe` | Product name and description PROOF (Task Manager, the taskbar, the file's properties) |
| Help > Learn more > Report a Bug | PROOF's list of faults on GitHub |
| The crash window and the crash text | PROOF's name and PROOF's list of faults |
| Debug info (About, `--debug-info`) | Begins with "PROOF, based on Inkscape ..." |

## What keeps Inkscape's name

- About: the Authors and Translators tabs and their invitations, the licence,
  and the copyright line, which names the Inkscape Developers first and
  PROOF's contributors after them.
- The start screen's "Supported by You" tab, with Inkscape's picture and both
  of its links. Its first sentence says that PROOF is based on Inkscape.
- Help: "Inkscape Manual", the tutorials named "Inkscape: ...", "New in This
  Version", FAQ, "Ask Us a Question" and Donate. They are Inkscape's pages.
  Some of what they describe works differently in PROOF.
- `inkscape --version` still prints `Inkscape 1.5-dev (...)`. Extensions and
  scripts read that line.
- The executable is still `inkscape.exe`, the application id is still
  `org.inkscape.Inkscape`, and the settings folder is unchanged. Renaming the
  executable is a step of its own: every build, install and check script names
  it.
- Messages elsewhere in the editor that mention Inkscape have not been gone
  through.

Labels that changed have no translations yet. In another language they show in
English until someone translates them.

## Pictures

| File | Use |
|---|---|
| `share/branding/inkscape.ico`, `share/icons/application/` | The application's icon |
| `share/icons/*/actions/inkscape-logo*.svg` | The mark beside Help > About: copies of the application's icon and of its one-colour form |
| `share/screens/proof-banner.svg` | The drawing of the start screen's banner, 700 by 220 |
| `share/screens/proof-banner.png` | That drawing as the start screen loads it |
| `share/screens/proof-about.svg` | The About window's picture, 750 by 440, drawn by the editor itself |

After changing the banner's drawing, export it again:

```
inkscape --export-type=png --export-width=700 --export-height=220 --export-filename=share/screens/proof-banner.png share/screens/proof-banner.svg
```

The size of the About picture is also written in
`share/ui/inkscape-about.glade` (the aspect frame and its two pictures).

Inkscape's own banners and its About artwork are still in `share/screens`.
About falls back to them when `proof-about.svg` is missing.

The mark is two paths in a 512 by 512 square: a white stem with a round bite,
and a blue half disc that fills the bite's right half. The wordmark is five
stroked paths, so it needs no font.

| Colour | Value |
|---|---|
| Blue | `#3155FF`, in a gradient from `#5378FF` to `#2441DF` |
| Paper | `#FFFFFF` to `#DCD9D1`; the wordmark is `#F2F0E9` |
| Ground | `#14161C` |

These are screen colours, not inks.

## In the code

`src/inkscape-version-info.h` holds the three things the rest asks for:

- `PROOF_TITLE_SUFFIX`, what follows a document's name in a title. The window
  cuts the title there to show the name alone in its own title row.
- `proof_version()`, "PROOF, based on Inkscape 1.5-dev (revision, date)".
- `proof_build()`, the revision and date in brackets. PROOF has no version
  number of its own, and Inkscape's number after PROOF's name would read as
  PROOF's.

`inkscape_version()` is unchanged and is what `--version` prints.

## Not done

- A version number of PROOF's own.
- The executable's name.
- The first-start setup tab still offers Inkscape's themes and keys by their
  names.
- Translations of the changed labels.
