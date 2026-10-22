// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_WRITER_MESH_H
#define SEEN_AI_NATIVE_WRITER_MESH_H

#include <cstddef>
#include <string>
#include <vector>
#include <2geom/affine.h>

class SPMeshGradient;
class SPShape;

namespace Inkscape::Extension::Internal::AiNative {

/** A shape's mesh-gradient fill as Illustrator's gradient mesh, an object of its own.
 *
 * `records` contains one version 2 Cartesian mesh per SVG patch, each `[1 1] /Size`,
 * `[0 0] /P`, `[0] /R`, its device colour space, four `/N` corners and `/E`, on hidden lines.
 * Each corner holds colour, opacity, position, the outgoing handle and weight 1, then the
 * incoming handle and weight 1. Illustrator smooths colour across a multi-patch mesh's lines;
 * SVG/Cairo blends each patch independently. Separate editable meshes preserve that surface.
 * The caller groups multiple patches so object transparency and a mask apply exactly once.
 *
 * With one patch, the caller adds its dictionary and `0 0 Xd`, `6 () XW`; with several,
 * `records` includes those ending markers for every patch. `outlined` says whether the shape's
 * complete outline is the mesh's; otherwise the caller clips the patches to the shape.
 * Smoothed (bicubic) meshes, moved tensor points, corners in more than one colour model or
 * other than standard RGB or CMYK, and
 * nonfinite or singular placement are refused by name, and `records` is empty.
 */
struct WrittenMesh
{
    std::string records;
    std::size_t patches = 0; ///< Separate editable meshes, grouped when more than one.
    bool outlined = false;
    std::vector<std::string> refused;
};

/// `item_to_art`: the shape's complete local-to-art transform. Requires an up-to-date document.
WrittenMesh write_mesh(SPShape const *shape, SPMeshGradient const *mesh, Geom::Affine const &item_to_art);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_AI_NATIVE_WRITER_MESH_H
