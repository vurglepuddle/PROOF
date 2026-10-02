# AI interchange donor review — 2026-10-02

Source review, not acceptance of README claims. No donor code has been added to
PROOF's runtime. No donor installer, build script, downloaded executable, or
package dependency installation was run. Only the reviewed Calvin Python
parser/SVG modules were probed in the workspace sandbox, using already decoded
local records and existing Python dependencies. Those probes did not execute
PostScript or exercise the donor PDF container writer.

## Findings

### Calvin-LLC/illustrator-exporter

Reviewed commit: `009a4a7b8150bcfa19368156f650ef55c217cd46`. Declares MIT.

- The [container writer](https://github.com/Calvin-LLC/illustrator-exporter/blob/009a4a7b8150bcfa19368156f650ef55c217cd46/src/ai_exporter/writer/container_builder.py)
  replaces private AI streams without rebuilding the PDF page contents. An edited
  file can therefore have two different artworks: new private data and an old PDF
  representation. The minimal builder creates a blank PDF page with private data.
- `_write_image` in `writer/pgf_writer.py` emits an image placeholder, without the
  image payload. On the supplied CMYK IMAGE native records, the parser produced
  one AIImage with **zero payload bytes**. It also reported 31,010 unknown operator
  names, including data it had not interpreted as image content.
- A controlled SVG test confirmed that `fill="#ff0000"` imports as red while
  `style="fill:#ff0000"` imports with no fill. The SVG importer reads presentation
  attributes without implementing the CSS cascade. This breaks ordinary editor
  output. Its path importer also does not apply a path's transform attribute.
- Colour recovery uses a map keyed by RGB hex values. Two distinct inks with the
  same preview colour cannot have their identities recovered from that key alone.
- The sample Pantone records produced four paths; Typical produced nine layers,
  2,834 AIPath objects, 627 groups and 191 compound paths. These counts are evidence
  of parsing work, **not** fidelity: private artwork can include hidden objects,
  and the PDF representation has a different object structure.
- Container tests mostly check file existence, PDF headers and re-extraction with
  the same parser. They do not establish Illustrator acceptance or agreement
  between the private and PDF representations.

Safety review: ordinary Python imports, no shell/network execution found in the
reviewed runtime modules. XML uses defusedxml; disk-loaded metadata resets its
trusted flag; there are input/depth limits. The optional `sync_corpus.py --clean`
recursively removes its destination without a workspace guard, and pytest's
autouse fixture deletes its own `_artifacts` directory. Neither was run. Runtime
dependencies are version ranges rather than a complete locked environment; their
distributions/transitive dependencies were not audited.

**Decision:** possible donor for individual PGF operators and colour-record
knowledge; reject as the current end-to-end converter.

### opendesigndev/illustrator-parser-pdfcpu

Reviewed commit: `225c40c9f5d0ea3ff0e793ee1dacc386c198c3d4`. Declares Apache-2.0.

- [Private-data parser](https://github.com/opendesigndev/illustrator-parser-pdfcpu/blob/225c40c9f5d0ea3ff0e793ee1dacc386c198c3d4/src/private-data/parser.ts)
  and text-document decoder recover names and native text records alongside PDF
  artwork. This is useful for a hybrid importer. No AI output writer was found.
- Go container code handles zlib and Zstandard private streams. The implementation
  is coupled to a vendored pdfcpu version and a TypeScript document representation.
- Native distribution targets in `Herebyfile.mjs` cover macOS/Linux; Windows is
  commented out. It is not a ready-made Windows converter for PROOF.

Safety review: the `postinstall` script creates a symlink to a platform helper;
the Node context launches that helper with `execFile`. Browser mode loads a local
WASM module. Optional build tasks download test files and invoke Go/Rollup. These
are explainable functions, not evidence of malware, but the binaries and install
hooks were not run. `wasm/private_data.go` indexes `chunk.Content[0]` and `[:20]`
without checking length and asserts dictionary types, so malformed input can panic.
Go dependencies include old pinned versions; no full dependency vulnerability
scan or binary/source equivalence audit was performed.

**Decision:** best focused lead here for native text recovery. Adapt data handling
with bounds checks and fixtures; do not bundle the whole helper stack blindly.

### Pilser/illustrator-rs

Reviewed commit: `7c2d554086980b231ec0980b5a7f8a12d68d0602`. Declares MIT.

- [CLI source](https://github.com/Pilser/illustrator-rs/blob/7c2d554086980b231ec0980b5a7f8a12d68d0602/src/cli.rs)
  has extract/inspect/rebuild stubs that log "not fully implemented" and return
  success. README examples therefore do not establish a working CLI.
- `parser/container.rs::extract_from_pdf` converts the raw file to lossy UTF-8
  and searches for PGF markers. It does not walk PDF objects/private streams;
  it can stop at the first end-layer marker. The optional lopdf dependency is
  not used by this function.
- `writer/container.rs::build_from_original` splices text into a decoded binary
  file without updating PDF stream lengths or cross-reference offsets. Its
  minimal builder supplies private streams but no rendered PDF page artwork.
- `icc.rs::load_icc_profile` always returns false.
- The writer can replay trusted raw layers. The handler's save method passes
  original metadata straight to that writer; edit invalidation needs proof before
  that path can be considered safe against silently discarding edits.
- The README's `full` feature is absent from the inspected Cargo.toml.

Safety review: no process launch, network client, unsafe block, or top-level
build.rs was found in the inspected project source. This does not audit Cargo
dependencies or their build scripts. No Rust code or Cargo build was run.

**Decision:** reject as the current foundation. The issue is implementation gaps,
not popularity or the language.

### Scribus

Reviewed the mirror's `scribus/plugins/import/ai/importai.{cpp,h}` only, fetched
2026-10-02. CPP SHA-256:
`5893bd579793d872e7eeea0f0852eb9f097ec712652b39735a18c4e052698112`.

- [AI importer](https://github.com/scribusproject/scribus/blob/master/scribus/plugins/import/ai/importai.cpp)
  contains real drawing, clipping, layer and spot-colour handling, with native
  PDF private-stream extraction via PoDoFo. It writes into Scribus's own model;
  it is not a standalone library to link unchanged into Inkscape.
- The fetched importer handles AI12 zlib compression but contains no AI24
  Zstandard handling. Our existing decoder already supplies that missing layer
  for the four local files. Combining these is a plausible bounded development
  route, not yet a tested native artwork importer.
- GPL-family source with a project-specific exception notice; keep original
  copyright/licence notices and review the exact reused files. The GitHub repo
  describes itself as a manually updated, unsupported mirror of official SVN.

Safety scope: only these two importer files were reviewed. They include a
Ghostscript thumbnail path, predictable temporary filenames in some paths, and
dependencies on the wider application. Do not transplant that process/temp-file
machinery. No Scribus build, binary, or whole-repository malware review occurred.

**Decision:** strongest native drawing-operator donor in this set; port selected
logic into a bounded parser that emits PROOF objects.

### sk1-project/uniconvertor

Reviewed commit: `973d5b6f8fccce7e7bf9bc88e91bc80f9f9d9472`.

- In this checkout, [format registration](https://github.com/sk1-project/uniconvertor/blob/973d5b6f8fccce7e7bf9bc88e91bc80f9f9d9472/src/uc2/uc2const.py)
  defines AI names/constants but does not include AI in MODEL_LOADERS or
  MODEL_SAVERS. No AI format implementation directory is present. This checkout
  does not supply the proposed modern AI bridge.
- Top-level LICENSE is AGPL-3.0; some source files say GPL-2-or-later. Treat
  provenance per file rather than assuming every component is permissive.
- `setup.py` can fetch an unpinned external build-utils Git repository and import
  it during setup. It also includes shell-based uninstall deletion commands.
  This is a supply-chain/installation concern, not evidence of malicious intent.

**Decision:** no install or runtime adoption for this task. Revisit a specifically
identified historical AI implementation only if needed for legacy files.

## Safety conclusion

No clear exfiltration, credential harvesting or obfuscated payload was found in
the inspected paths. That is a limited static finding, **not a malware-free
certificate**. Full dependency audits, fuzzing, and source-built binary review
remain prerequisites before adopting an external parser into the application.
Do not run unknown PostScript as part of native-record recovery. Keep parsing
bounded, process-isolated where practical, and fail with an explicit unsupported
feature report rather than silently dropping records.

Local evidence is in the workspace's `artifacts/donor-review/{pantone,typical,cmyk-image}`;
the reusable probe is `tools/probe-ai-donor.py` outside this nested source repo.

## Addendum: three further candidates and a native-writer spike (2026-10-02)

These three candidates were suggested externally. The review rules above still
apply. Evidence is in the workspace's `artifacts/native-ai-spike/`, and fetched
sources are in `donor-projects/new-candidates-review/` and
`donor-projects/inkscape-eps-export/`. Nothing was installed. The only donor code
executed was `aieps_output.py`, run once on its own bundled test file after a
full read.

### tzunghaor/inkscape-eps-export

Reviewed commit: `c875a3dcbb54022b8fb55cba25d35cfd65efac9f`. MIT, © 2024 Andras Prim.
Its arc-to-Bézier section is credited to Angel Kostadinov (MIT).
`aieps_output.py` SHA-256: `ca44d2d4756ec494fb7ba80972af23559c5325938db09854f404e005eec39391`.

- A single 1,190-line Python output extension. It writes Illustrator's **native
  operator language**, not PDF: layers (`Lb`/`Ln`/`LB`), groups (`u`/`U`),
  compound paths (`*u`/`*U`), clip groups (`q … W … Q`), linear/radial gradients
  (`Bd`/`BD`/`Bg`), RGB fill/stroke (`Xa`/`XA`) and line style. A fallback
  PostScript procset lets non-Illustrator interpreters render the same file.
- Its output for its own test matched `expected-output.eps` byte for byte.
  Illustrator 29.2.1 opened that output as native artwork. The Inkscape layer
  names (`clipart`, `Layer 1`) became Illustrator layers, with 14 groups and 30
  compound paths (`report-A.txt`).
- Limits found in the source: RGB only, so no CMYK, spot, overprint, text,
  images or opacity. It reads only `style=` CSS, so presentation attributes such
  as `fill="#f00"` are dropped. This is the inverse of Calvin's importer bug.
  CSS is split naively on `:` and the viewBox only on spaces. A `<use>` without
  `x`/`y` raises `TypeError` in `unitConv(None)`, and so does the skew-transform
  alert, which is missing an argument. `showAlerts` is Python 2 only and unused.
- Safety: no network, subprocess or file writes. It reads one SVG and prints
  to stdout.

**Decision:** the best writer-side reference found. Do not ship the Python
extension. Reimplement its operator mapping in PROOF, extended with the
operators the spike proved (below), and keep the MIT notice and credit with any
ported code.

### alchemy-fr/exiftool (`lib/Image/ExifTool/PDF.pm`)

Reviewed commit: `c417468fb112963ec49986d6a4d81695c36d5668`, a 2016 snapshot
(PDF.pm 1.41) kept as a Phraseanet dependency, not maintained ExifTool. Licence:
the same terms as Perl (Artistic or GPL).

- It recognises `AIPrivateData#`, `AIPDFPrivateData#`, `AIMetaData` and the
  `ContainerVersion`/`CreatorVersion`/`RoundTripVersion` keys. It joins the
  numbered streams and reads only their DSC comments as metadata, not artwork.
- Upstream ExifTool (PDF.pm 1.62, PostScript.pm 1.46) also handles only
  `%AI12_CompressedData` zlib, with **no AI24 Zstandard** support.
- `tools/ai-inspect.py` already covers all of the AI-specific knowledge here.
  ExifTool's incremental-update and xref-stream traversal is a useful reference
  if PROOF ever needs to preserve original PDF bytes through an incremental save.

**Decision:** reference only, no code adoption.

### flyfish-dev/file-viewer and the `illustrator-pgf` package

Reviewed `docs/guide/formats.md` at `533b60da7bdcd7c42d225441d54406dafc75e33c`
(Apache-2.0). That page describes a native AI renderer. The work behind it is
the separate npm package `illustrator-pgf@0.1.0` (MIT, © 2026 Flyfish Open
Source Studio; tarball SHA-256
`e49095d0788fc0f316bbf50eb32b514ea6be11a5ef727968fcd3d29861b012fd`). file-viewer
vendors it with a 321-line patch. Reviewed: the package's compiled `dist/`
(4,233 lines, source maps without embedded sources), operator table and sample
diff, plus file-viewer's patch, adapter and development spec.

- The development spec closely matches PROOF's rules. It never executes
  PostScript, uses a whitelisted and budgeted parser, and keeps unknown syntax as
  data. It treats the PDF and native representations as separate, and reports
  contradictory version fields instead of choosing one silently. Writing AI is
  explicitly out of scope. Read-only; no writer.
- **Not trustworthy as an oracle yet:**
  - Stock 0.1.0 rejects an `%AI24_ZStandard_Data` marker that is immediately
    followed by the zstd frame, which is how all four supplied Illustrator 29
    files are laid out.
  - Stock 0.1.0 also lacks uppercase `L`/`C`, the most common geometry
    operators in Typical (27,292 `C` and 12,539 `L`).
  - file-viewer's patch adds those, but its custom-colour handler takes the
    last number as the tint. AI tint is inverted (operand `0.7` is a 30% tint,
    confirmed by Illustrator below), and for `Xx` the last number is the colour
    type.
  - The renderers ignore tint entirely and draw spots through their alternate
    colour, with a warning.
  - Bare AI3–AI8 files require an Illustrator `%%Creator`.
  - The published sample diff is synthetic.
- Security audit:
  - No dependencies and no install hooks.
  - Its only external modules are `node:fs/promises` (CLI: read the input,
    write stdout or an explicit output path) and `node:zlib`. Built-in zstd
    and zlib are used with output caps, with no WASM.
  - No `child_process`, network, `eval`/`Function`, dynamic import outside the
    package, or encoded blobs.
  - Default budgets: 128 MB file, 128 MB decoded, plus token, node, nesting and
    raster limits. The 144 MB decoded Giant File would be refused rather than
    misread.
  - A `Worker` is created only on the browser path.
  - It was not executed: the correctness gaps make its output unreliable as
    evidence.

**Decision:** design reference for the importer and test methodology (fidelity
levels, diagnostics, Illustrator-oracle corpus). Not a runtime dependency, and
PROOF is C++. If used later as an independent reader in CI, pin a patched
version, run it under `node --permission` with read-only file access, and fix
the tint semantics first.

### Native-writer spike against Illustrator 29.2.1

`tools/native-ai-spike.py` writes small test files, and
`tools/inspect-native-spike.ps1` opens each one through Illustrator COM, reports
what Illustrator built, and closes it without saving. The records use the dialect
Illustrator 29 itself writes in the supplied files' private data. Its artwork
section is plain native operators (`Lb`/`Ln`, `k`/`K`, `x`/`Xx`, `O`/`R`,
`m L C`, `q h W n Q`), with optional `%_` metadata dictionaries. The hybrid PDF
pages carry an extra RGB marker that the native records lack. That shows which
representation Illustrator used.

| File | Result |
|---|---|
| B1 legacy PostScript `.ai`, `%%Creator: PROOF …` | **Native.** CMYK document; layers `Print`/`Dieline`; spot swatches `PROOF Spot Red` and `CutContour` with exact CMYK alternates; tints 100% (`x`, operand 0) and **30%** (`Xx`, operand 0.7); overprinting CutContour stroke; clip group; even-odd compound path; group. |
| C1 PDF page + plain `AIPrivateData1` | **Same native result; PDF page ignored** (no marker). Poppler renders the PDF page correctly in CMYK/Separation. |
| C2 as C1, `%AI12_CompressedData` zlib | Same native result. |
| D as C1, page `LastModified` newer than private data | **Same native result.** Illustrator does not detect stale private data. |
| E as C1 + `%AI3_Cropmarks`/`%%PageOrigin` | Art on an artboard at 0,300,400,0. A second, legacy artboard remains. |
| F as E + minimal `%AI9` document-data `ArtboardArray` | Rejected ("operation was cancelled"), no crash. **Resolved later the same day.** The section wrapper was missing, and the file also needs format 14.0, the modern coordinate frame and `CropAreaActive`. Probe P opens with named artboards `Left` and `Right` in place and survives an Illustrator save-and-reopen. See the [writer recipe](PROOF-ai-interchange.md#writer-recipe-named-and-multiple-artboards-verified-2026-10-02). |

The first batch run, before the files were opened one at a time, crashed
Illustrator (`0xc0000005` at `Illustrator.exe+0x12a619d`) on either `A2` (the
donor EPS renamed `.ai`) or `B2` (`%%Creator: Adobe Illustrator(R) 8.0`). Neither
is a planned output, so they were not retested. No user documents were open.

The honest creator string is accepted, so PROOF never needs to claim to be Adobe.
`tools/ai-inspect.py` independently reads C1, C2 and E: two layers and two spot
inks with correct alternates. PROOF's current PDF importer opens C1 with process
CMYK preserved (`device-cmyk()`), but **flattens both spots to RGB hex**, which
confirms the existing spot-identity gap.

**Conclusions:**

1. A dual-representation `.ai` written by PROOF is technically viable:
   Illustrator gets editable native artwork, and every PDF reader gets the
   page. This was proven for a small print-relevant subset, not for text,
   images, gradients, transparency or real jobs.
2. **Both halves must always be generated from the same document.** Illustrator
   trusts private data blindly (D), so stale private data silently discards
   edits. Never copy original private data into an edited file. Today's Cairo
   interchange export correctly writes none.
3. The PDF half needs real CMYK/Separation output, which is CapyPDF territory,
   not Cairo. Illustrator 29 rejected CapyPDF's original fixture. Retest with
   private data present before relying on it.
4. A writer can only emit the spots the model knows. Spot identity in PROOF's
   model and importer is on the critical path.
