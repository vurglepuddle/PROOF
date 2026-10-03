# Credits

PROOF builds on the work of many open-source communities. Thank you to the
authors and maintainers who made this editor possible.

## Foundation and components

Our foundation is [Inkscape](https://inkscape.org/) and its contributors. Their
vector editor, SVG document model, tools, and years of work underpin PROOF.
The inherited [AUTHORS](AUTHORS) list and source-file copyright notices retain
those individual credits.

Some of the major projects used by the editor are:

| Project | Role |
| --- | --- |
| [Inkscape extensions](https://gitlab.com/inkscape/extensions) and [themes](https://gitlab.com/inkscape/themes) | Extension infrastructure and inherited interface assets. |
| [lib2geom](https://gitlab.com/inkscape/lib2geom) | Vector geometry and path operations. |
| [LittleCMS 2](https://www.littlecms.com/) | ICC color management and color conversion. |
| [Poppler](https://poppler.freedesktop.org/) | PDF and PDF-compatible Illustrator artwork import. |
| [Cairo](https://www.cairographics.org/) | Rendering and PDF-based Illustrator interchange output. |
| [CapyPDF](https://github.com/jpakkane/capypdf) | PDF output infrastructure inherited from Inkscape. |
| [GTK](https://www.gtk.org/) and [gtkmm](https://www.gtkmm.org/) | The desktop interface. |
| [Zstandard](https://github.com/facebook/zstd) and [zlib](https://zlib.net/) | Compressed Illustrator private-record decoding. |

This is a highlights list. Other inherited libraries, assets, and extensions
retain their credits and license notices in their own files and directories.

## Illustrator research and references

The following projects were reviewed during PROOF's Illustrator-format work.
They helped us investigate existing approaches, format behavior, and possible
future reuse. The [donor review](doc/PROOF-ai-donor-review.md) records the scope,
reviewed revisions, licenses, and findings; it does not record runtime adoption
of these donor projects.

| Project | Reviewed work |
| --- | --- |
| [Scribus](https://github.com/scribusproject/scribus) | Native Illustrator import, drawing operators, layers, and spot colors. |
| [inkscape-eps-export](https://github.com/tzunghaor/inkscape-eps-export) | Illustrator native output operators; work by Andras Prim, including arc-to-Bézier code credited to Angel Kostadinov. |
| [illustrator-parser-pdfcpu](https://github.com/opendesigndev/illustrator-parser-pdfcpu) | Private-record extraction and native text/name recovery. |
| [illustrator-exporter](https://github.com/Calvin-LLC/illustrator-exporter) | PGF parsing, SVG conversion, and container-writing approaches. |
| [illustrator-rs](https://github.com/Pilser/illustrator-rs) | Rust parser and writer approaches evaluated during research. |
| [UniConvertor](https://github.com/sk1-project/uniconvertor) | Vector-format conversion and possible legacy Illustrator support. |
| [ExifTool](https://exiftool.org/) ([reviewed snapshot](https://github.com/alchemy-fr/exiftool)) | Illustrator private-stream and PDF metadata handling. |
| [file-viewer](https://github.com/flyfish-dev/file-viewer) / [illustrator-pgf](https://www.npmjs.com/package/illustrator-pgf) | Native-format parser design, diagnostics, and fidelity testing. |

These acknowledgments complement [COPYING](COPYING), the full texts in
[LICENSES](LICENSES/), and component-specific notices. Individual files retain
the terms and attribution of their authors.
