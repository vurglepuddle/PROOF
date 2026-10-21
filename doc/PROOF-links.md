# PROOF linked pictures that can't be found

A document can link to a picture file instead of holding it. When the file is
gone, the document still opens, says so, and offers to link the picture again.
First slice: 2026-10-11.

How such links are read from `.ai` files is described in
[PROOF-ai-native-records.md](PROOF-ai-native-records.md). This document is
about the editor.

## What works now

- **What shows in the picture's place.** Illustrator keeps a small picture of
  every linked file inside the document (512 pixels at most on its longer
  side). PROOF keeps it too, and shows it in the box the file's own picture lay
  in while that file can't be found. A link from a document that keeps none
  (older Illustrator files, or any SVG) shows as Inkscape's marked box, white
  with a red frame and cross.
- **Pink highlight.** What stands in for a missing picture has the same pink
  over it on the canvas as type set in a stand-in font. It is drawn over the
  canvas only, so exports, prints and saved files never have it. Preferences >
  Text switches it off for both.
- **Missing Links.** When a document opens with linked pictures whose files
  can't be found, a window lists them, each with the folder it was in. File >
  Missing Links… shows the same window at any time. On closing it can select
  the pictures still missing. The one that comes with a document can be told
  not to come again (Preferences > Imported Images brings it back). When a
  document has both missing pictures and missing fonts, the pictures are asked
  about first, as in Illustrator.
- **Relink.** Pick a picture in the list and press Relink… (Alt+R) to choose
  its file. The other missing pictures are then looked for by name in the same
  folder and linked too, as one step that Undo takes back. The window says how
  many were found there.
- **The picture keeps its box.** A file of the same shape fills the box as the
  one before did. A file of another shape is fitted inside the box without
  being stretched.
- **A file that comes back is found by itself.** The link is kept as written,
  so nothing needs doing when the file is where the document says the next time
  it opens. Inkscape's own search for moved files (beside the document, and in
  the folders above it) still runs first.
- **The status bar and Properties say it.** A selected missing picture reads
  "file not found" with its link, not the size of what stands in for it.
  Properties > Embed is off for it: what stands in isn't the picture.
- **Undo of a relink puts everything back**, the box included. (Inkscape
  reshapes a picture's box when its file changes to one of another shape; what
  stands in for a missing file no longer counts as one.)

Not yet:

- **A panel of all links**, missing or not, like Illustrator's Links panel.
  Properties > Change replaces one picture's file.
- **A warning on export or print** while pictures are missing. What stands in
  is what is printed: the small picture, at its low resolution.
- **Linked files that aren't pictures** (Photoshop, PDF, Illustrator). A
  document from Illustrator that shows one still opens from its PDF page.
- **Taking the picture from an `.ai` file's own PDF page**, which holds it at
  full size.

## Links on other computers (2026-10-21)

A document can name a linked file on another computer: `\\server\share\logo.tif`,
`file://server/share/logo.tif`, or a web or network address. On Windows, looking
one up makes Windows contact that computer and offer the user's sign-in to it, so
a received file whose links name a server could collect it. VectorCraft stopped
following such links the same week (their `6e4fb6e4`, read as a delta only).

The owner's choice, 2026-10-21: follow links on this computer and on the share
the document itself is on. So PROOF looks up a linked file when it is:

- on a drive of this computer, **mapped network drives included** (the user
  mapped them, and Windows is already signed in to their servers);
- relative to the document;
- on the same `\\server\share` as the document (a job folder on a NAS keeps
  working when the document is opened from it), read through a mapped drive
  too: `Z:\Jobs\doc.ai` linking `\\nas\jobs\logo.tif` when `Z:` is `\\nas\jobs`;
- on a share the user chose a file from in this session: File > Place or
  Import, drag and drop, Relink, Properties > Change.

Any other share, a device path (`\\.\…`) and a web or network address
(`http:`, `smb:`) is not looked up. The picture shows as missing, with what the
document keeps of it, and Relink picks it up. An `.ai` import names the shares
it passed over in its notes: "linked files on another computer weren't looked
up (\\nas\other): …". A server spelt two ways (`\\nas` and `\\192.168.1.5`)
counts as two. The `.ai` writer refuses a link on another computer by name;
PDF export leaves it out, as it does a missing file. Elsewhere than Windows,
links are followed as before. None of the 480 placed-file links in the two
`.ai` sets checked here names a network share (all are drive letters), so no
file of ours opens differently.

The rule is `Inkscape::IO::may_follow_link` in `src/io/link-policy.*`, called
wherever a document's own words lead to a file: the picture loader
(`SPImage::readImage`, `sodipodi:absref` included), the search for moved files
that runs on opening (`io/fix-broken-links.cpp`), Save As rebasing
(`xml/rebase-hrefs.cpp`), filter pictures (`object/filters/image.cpp`), other
documents used by reference (`object/uri-references.cpp`), PDF export
(`pdfoutput/build-images.cpp`), the `.ai` importer's link search
(`Builder::linked_file`) and the `.ai` writer's link check.

## How it is stored

```xml
<image xlink:href="file:///C:/Jobs/photo.png" sodipodi:absref="C:\Jobs\photo.png"
       proof:link-preview="data:image/png;base64,…" x="…" y="…" width="…" height="…"
       preserveAspectRatio="none"/>
```

| Attribute | Meaning |
|---|---|
| `xlink:href`, `sodipodi:absref` | The link, as the document that was opened wrote it. Never changed because the file is missing |
| `proof:link-preview` | The small picture the document keeps of the file, as a PNG. Used only while the file can't be found. Removed when the picture is linked to another file |

Whether a picture is missing isn't stored. It is found out when the document
is read: `SPImage::missing`, and `linkMissing()` for a link as opposed to a
picture held in the document.

## Where the code is

| Part | File |
|---|---|
| What shows for a missing file, `linkedFile`, `relink` | `src/object/sp-image.cpp` |
| The missing pictures of a document, relinking from one folder | `sp_images_missing`, `sp_images_relink` in the same file |
| The Missing Links window | `src/ui/dialog/link-missing.cpp` |
| When a document opens | `src/ui/dialog/font-substitution.cpp` (`checkFontSubstitutions`) |
| The highlight | `src/display/control/font-highlight.h` (`link_highlight_shape`) |
| The menu entry | `win.document-missing-links` in `src/actions/actions-file-window.cpp` |
| The preview from an `.ai` file | `src/extension/internal/ai/ai-native-reader.cpp` (`preview_of`), `ai-native-import.cpp` (`Builder::placed`) |
| Which linked files may be looked up | `src/io/link-policy.*` (`may_follow_link`, `network_share`, `trust_link_share`) |

## Checks

A live check in the workspace repository, `tools\link-missing-check.py`, on
`tests\link-missing.svg` with the real mouse and keyboard: the window at
opening and from the menu, what shows for a missing picture with and without a
kept picture, the highlight, Relink through the system's file window with the
second picture found in the same folder, the saved file, and Undo.

Unit tests:

- `test_ai-native-reader`: `APlacedPreviewIsPassedOverByItsCount`,
  `PlacedPictureKeepsThePictureTheDocumentHasOfIt`.
- `test_ai-native-import`:
  `ALinkedPictureThatIsGoneKeepsItsLinkAndShowsWhatTheDocumentKeepsOfIt`,
  `PicturesThatAreGoneAreLinkedAgainFromOneFolder`,
  `LinkedPictureThatDoesNotShowOrCannotBeRead`, and
  `ALinkedPictureThatIsGoneIsLeftOutOfTheComparison` against an independently
  written PDF page.
- Links on other computers, in the same suite: `LinkPolicy.PathsNameTheirNetworkShare`,
  `LinkPolicy.OnlyTheDocumentsOwnShareOrAChosenOneIsFollowed` (the rule from
  the paths alone; no network is touched) and
  `ALinkedPictureOnAnotherComputerIsNotLookedUpAndIsNamed`.
