# Mesh objects

Implemented and checked on `collaborator/editing-ui`, 2026-10-17, based on
shared source `40ef374612`. The owner's mesh-object brief is the scope of
this round; importer integration remains on the owner's side.

## The behaviour

A mesh object owns its outline. Move an outer mesh point or its handles and
the outline follows, including the part outside the object's original bounds.
Move an inner point or change a point's colour and the outer outline stays.
The mesh and the outline change in the same undo operation.

The owner's specification is `brief/mesh-object-brief.md` in the private
workspace. Adobe's references, read on 2026-10-17:

- [Mesh objects overview](https://helpx.adobe.com/illustrator/desktop/paint-and-fill/create-and-edit-meshes/mesh-objects-overview.html)
- [Create mesh objects](https://helpx.adobe.com/illustrator/desktop/paint-and-fill/create-and-edit-meshes/create-mesh-objects.html)
- [Edit mesh objects](https://helpx.adobe.com/illustrator/desktop/paint-and-fill/create-and-edit-meshes/edit-mesh-objects.html)

Adobe describes points and lines as controls for the object's shape and its
colour transitions. The specification makes the boundary rule explicit for
PROOF. This round implements that rule; it does not claim the whole of
Illustrator's mesh editing interface.

## Creating one

The Mesh tool constructs its normal fill mesh on the original shape first,
then turns the shape into a path and marks it as a mesh object. Its identity,
position in the layer, transformation, style, clip and mask are preserved by
the existing Object to Path conversion. Selection follows the new path.

Rectangles, complete ellipses and ordinary stars keep the tool's existing
specialized mesh layout. A generic single closed contour is fitted to its
actual boundary instead of its bounding box. Four-sided contours use a
four-sided patch with the requested subdivisions; other contours use a radial
layout like the existing star mesh. Straight/cubic segments keep their
geometry; other curve types use the existing cubic conversion. Rounded and
randomized shapes use their actual generated outline.

The coincident radial seams of a star/radial layout are omitted from the
object's outline, so a stroke does not acquire a line through its middle.
These objects also store `proof:mesh-boundary="outer-ring"`: moving inner
points or handles cannot change which ring supplies their outline. This
choice survives saving and reopening.

Open paths and compound paths, groups and text, stroke mesh fills and the
conical creation mode retain their existing behaviour. They are not silently
converted into mesh objects by this round.

These changes are driven by Mesh tool edits. Direct Selection still exposes
ordinary path nodes; routing it to mesh points and providing Illustrator's
mesh-specific point controls remain separate work.

## Saved representation and importer handoff

The object is an SVG path with a mesh fill and:

```xml
<path xmlns:proof="urn:x-proof:document:1"
      proof:mesh-object="true" fill="url(#mesh)" d="..."/>
```

The `d` attribute is the current outer mesh boundary in item coordinates.
The fill gradient uses `gradientUnits="userSpaceOnUse"`; its
`gradientTransform` is applied to the boundary, and the path's own transform
remains separate. The stored path makes the result visible in SVG/PDF
without requiring an editor to understand the marker. An unmarked SVG path
continues to clip its mesh fill as before.

`object/mesh-object.h` exposes creation and outline synchronization. Array
writes synchronize marked users, including referenced mesh gradients. Knot
previews update the in-memory curve, and the committed edit writes `d` in the
same transaction as the mesh. Identical boundaries are not rewritten for
inner/colour edits. Stroke-only users and clones are excluded.

Mesh origins, stop paths and tensors are serialized with double round-trip
precision through the locale-independent SVG stream. The former six-digit
mesh path stream could round the mesh independently of the saved outline;
an inner edit after reopening would then rewrite that outline slightly.
The saved radial-object regression covers this case.

For the owner's importer: create one marked path per complete Illustrator
mesh, with a user-space mesh fill and matching initial outer boundary. The
default boundary is the rectangular array perimeter. Use the optional
`proof:mesh-boundary="outer-ring"` only for a radial layout whose top row
is its closed outer contour; the remaining rows are interior geometry. The
present patch-per-object importer is unchanged. No `.ai` reader/writer code
is modified here, and the native `.ai` writer's supported subset is unchanged.

## Qualification, 2026-10-17

- `test_sp-gradient`: **16/16 pass**, including 12 mesh cases. Outer/inner changes, handle and subdivision geometry,
  preview/commit, transformations, legacy fill/stroke isolation, shared mesh
  references, stars and generic contour creation, undo/redo, SVG copy, and
  interior edits of a reopened radial object.
- Private workspace `tools/mesh-object-check.py`: **11/11 pass**. Creation, an outward drag,
  visible fill beyond the old bounds, an inner drag, one-step undo/redo,
  clean closing, saved SVG reopening and a subsequent boundary edit.
- `testfiles/cli_tests/mesh-object-export-smoke.py`: **5/5 pass**. The real UI-edited saved
  SVG, rendered natively and exported to PDF; both must fill beyond the old
  rectangle and agree on coverage. It receives a copied synthetic input.
  At 720 x 500 pixels, unmatched ink after the one-pixel edge tolerance is
  zero; the sampled expanded fill differs by one RGB channel value.
- The synchronized workspace's 13 focused unit suites and 11 existing export
  smokes pass. GUI-only/opt-in unit cases remain skipped as reported by those
  suites; this is not a production AI-corpus rerun.

Desktop checks send real input only to their disposable editor instance and
require an announced idle keyboard/mouse. No production artwork is needed.

Windows PDF qualification used an ASCII output folder. A separate local
failure with Cyrillic export paths was isolated using the same binary and
recorded for the owner; Unicode output paths are not fixed by this round.
