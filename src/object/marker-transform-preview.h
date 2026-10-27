// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_OBJECT_MARKER_TRANSFORM_PREVIEW_H
#define INKSCAPE_OBJECT_MARKER_TRANSFORM_PREVIEW_H

#include <optional>
#include <2geom/affine.h>

class SPMarker;

void sp_marker_set_transform_preview(SPMarker &marker, unsigned key,
                                   std::optional<Geom::Affine> const &transform,
                                   std::optional<unsigned> position = {});

#endif
