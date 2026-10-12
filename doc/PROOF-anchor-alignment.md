# Align and distribute selected anchors

Direct Selection's selected anchors work with the Align & Distribute panel in
node mode. PROOF retains its existing First selected, Last selected, Middle,
Minimum and Maximum targets. Distribution includes selected endpoints and leaves
unchosen anchors in place. These commands can span objects; a native shape
expands only when an anchor actually changes, and Undo restores it in one step.

Alignment and distribution now finish the same neighboring-handle correction
and bounds refresh used by ordinary node movement. Previously, a smooth node
beside a moved line endpoint kept its old handle direction. The selection's
cached bounds also stayed at their old coordinates: after aligning anchors from
Y=80/220/340 to Y=80, entering Y=90 moved them to Y=-40. Numeric coordinates now
use the updated selection bounds, and neighboring smooth/automatic handles are
adjusted after every chosen point has moved.

The change is in `ControlPointSelection::align` and `distribute`. Each updates
neighboring handles and the selection bounds after the existing point movement
loop. `MultiPathManipulator` retains its normal geometry commit and single Undo
step. No new alignment mode or separate object transformation is introduced.

The workspace's `tools/anchor-align-check.py` covers all five targets on both
axes, selection order, distribution, numeric coordinates afterward, a neighboring
smooth join, native shapes across objects, and the real panel buttons. It uses
disposable synthetic SVGs and copied keys/preferences. Its tool-state guard
refuses further input if the expected Direct Selection anchors are absent.
The corrected baseline on `4ca2966e26` passes 27/29, failing numeric Y after
Align and the neighboring smooth handle. The fixed Windows 11 build passes
33/33 on 2026-10-13, including four additional real-panel checks. Local evidence:
`../../artifacts/anchor-align/run-001254/result.json` (baseline) and
`../../artifacts/anchor-align/run-001653/result.json` (fixed).

Reference: [Adobe alignment of selected anchors](https://helpx.adobe.com/illustrator/desktop/manage-objects/arrange-objects/align-and-distribute-objects.html).
