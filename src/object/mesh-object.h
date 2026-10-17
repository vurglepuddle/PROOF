// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_OBJECT_MESH_OBJECT_H
#define INKSCAPE_OBJECT_MESH_OBJECT_H

class SPItem;
class SPPath;
class SPMeshGradient;

namespace Inkscape::MeshObject {

inline constexpr char ATTRIBUTE[] = "proof:mesh-object";
inline constexpr char BOUNDARY_ATTRIBUTE[] = "proof:mesh-boundary";

// Turn a newly mesh-filled shape into a path whose outline belongs to its mesh.
// The mesh must be constructed on the original shape before converting it.
SPPath *create(SPItem *item, SPMeshGradient *mesh);

// Preview or commit the outer boundary in item coordinates. Unmarked SVG mesh
// fills, stroke meshes and cloned objects retain ordinary SVG clipping behavior.
bool sync(SPItem *item, SPMeshGradient *mesh, bool write_repr = true);

// Called when an array is written, including row/column and toolbar operations.
void sync_users(SPMeshGradient *mesh);

} // namespace Inkscape::MeshObject

#endif
