// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_GRADIENT_CHEMISTRY_H
#define SEEN_SP_GRADIENT_CHEMISTRY_H

/*
 * Various utility methods for gradients
 *
 * Author:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2010 Authors
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2001-2002 Lauris Kaplinski
 * Copyright (C) 2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "fill-or-stroke.h"
#include "object/sp-gradient.h"

class SPCSSAttr;
class SPItem;
class SPGradient;
class SPDesktop;

namespace Inkscape::Colors {
class Color;
}
using Inkscape::Colors::Color;

/**
 * Either normalizes given gradient to vector, or returns fresh normalized
 * vector - in latter case, original gradient is flattened and stops cleared.
 * No transrefing - i.e. caller shouldn't hold reference to original and
 * does not get one to new automatically (doc holds ref of every object anyways)
 */
SPGradient *sp_gradient_ensure_vector_normalized(SPGradient *gradient);

 
/**
 * Sets item fill or stroke to the gradient of the specified type with given vector, creating
 * new private gradient, if needed.
 * gr has to be a normalized vector.
 */
SPGradient *sp_item_set_gradient(SPItem *item, SPGradient *gr, SPGradientType type, Inkscape::PaintTarget fill_or_stroke);

/*
 * Get default normalized gradient vector of document, create if there is none
 */
SPGradient *sp_document_default_gradient_vector( SPDocument *document, Color const &color, double opacity, bool singleStop );

/**
 * Return the preferred vector for \a o, made from (in order of preference) its current vector,
 * current fill or stroke color, or from desktop style if \a o is NULL or doesn't have style.
 */
SPGradient *sp_gradient_vector_for_object( SPDocument *doc, SPDesktop *desktop, SPObject *o, Inkscape::PaintTarget fill_or_stroke, bool singleStop = false );

void sp_object_ensure_fill_gradient_normalized (SPObject *object);
void sp_object_ensure_stroke_gradient_normalized (SPObject *object);

SPGradient *sp_gradient_convert_to_userspace (SPGradient *gr, SPItem *item, const char *property);
SPGradient *sp_gradient_reset_to_userspace (SPGradient *gr, SPItem *item);

SPGradient *sp_gradient_get_forked_vector_if_necessary(SPGradient *gradient, bool force_vector = false);


SPStop* sp_last_stop(SPGradient *gradient);
SPStop* sp_get_stop_i(SPGradient *gradient, unsigned int i);
// return n-th stop counting from 0; make no assumptions about offsets 
SPStop* sp_get_nth_stop(SPGradient* gradient, unsigned int index);
std::pair<SPStop*, SPStop*> sp_get_before_after_stops(SPStop* stop);
unsigned int sp_number_of_stops(SPGradient const *gradient);
unsigned int sp_number_of_stops_before_stop(SPGradient* gradient, SPStop *target);

SPStop *sp_vector_add_stop(SPGradient *vector, SPStop* prev_stop, SPStop* next_stop, gfloat offset);

void sp_gradient_delete_stop(SPGradient* gradient, SPStop* stop);
SPStop* sp_gradient_add_stop(SPGradient* gradient, SPStop* current);
SPStop* sp_gradient_add_stop_at(SPGradient* gradient, double offset);
void sp_set_gradient_stop_color(SPDocument* document, SPStop* stop, Color const &color);

void sp_gradient_transform_multiply(SPGradient *gradient, Geom::Affine postmul, bool set);

void sp_gradient_reverse_selected_gradients(SPDesktop *desktop);

void sp_gradient_invert_selected_gradients(SPDesktop *desktop, Inkscape::PaintTarget fill_or_stroke);

void sp_gradient_unset_swatch(SPDesktop *desktop, std::string const &id);

SPGradient* sp_item_get_gradient(SPItem *item, bool fillorstroke);

int sp_get_gradient_refcount(SPDocument* document, SPGradient* gradient);

void sp_gradient_reverse_vector(SPGradient* gradient);

/**
 * Fetches either the fill or the stroke gradient from the given item.
 *
 * @param fill_or_stroke the gradient type (fill/stroke) to get.
 * @return the specified gradient if set, null otherwise.
 */
SPGradient *getGradient(SPItem *item, Inkscape::PaintTarget fill_or_stroke);


void sp_item_gradient_set_coords(SPItem *item, GrPointType point_type, unsigned int point_i, Geom::Point p_desk, Inkscape::PaintTarget fill_or_stroke, bool write_repr, bool scale);

/**
 * Returns the position of point point_type of the gradient applied to item (either fill_or_stroke),
 * in desktop coordinates.
*/
Geom::Point getGradientCoords(SPItem *item, GrPointType point_type, unsigned int point_i, Inkscape::PaintTarget fill_or_stroke);

SPGradient *sp_item_gradient_get_vector(SPItem *item, Inkscape::PaintTarget fill_or_stroke);
SPGradientSpread sp_item_gradient_get_spread(SPItem *item, Inkscape::PaintTarget fill_or_stroke);

SPStop* sp_item_gradient_get_stop(SPItem *item, GrPointType point_type, guint point_i, Inkscape::PaintTarget fill_or_stroke);

void sp_item_gradient_stop_set_style(SPItem *item, GrPointType point_type, unsigned int point_i, Inkscape::PaintTarget fill_or_stroke, SPCSSAttr *stop);
Color sp_item_gradient_stop_query_style(SPItem *item, GrPointType point_type, unsigned int point_i, Inkscape::PaintTarget fill_or_stroke);
void sp_item_gradient_reverse_vector(SPItem *item, Inkscape::PaintTarget fill_or_stroke);
void sp_item_gradient_invert_vector_color(SPItem *item, Inkscape::PaintTarget fill_or_stroke);

// Apply gradiant (or swatch) to given item; pass nullptr to create a new gradient and apply it
void sp_item_apply_gradient(SPItem* item, SPGradient* vector, SPDesktop* desktop, SPGradientType gradient_type, bool create_swatch, FillOrStroke kind);

// Apply mesh to given item; create a new mesh is none is passed
void sp_item_apply_mesh(SPItem* item, SPGradient* mesh, SPDocument* document, FillOrStroke kind);

// Mark swatch in given "item" for auto collection, then replace it with "replacement", so it can be deleted
void sp_delete_item_swatch(SPItem* item, FillOrStroke kind, SPGradient* to_delete, SPGradient* replacement);

// Check if 'swatch' can be deleted:
// - it is referenced at most ones (so we can unlink it easily)
// - there are two or more swatchs total in a document (so we can use another swatch as a replacement)
bool sp_can_delete_swatch(SPGradient* swatch);

// Find a replacement for 'swatch' that we want to delete.
// We want object using swatch to keep using some other swatch to prevent mode switch.
SPGradient* sp_find_replacement_swatch(SPDocument* document, SPGradient* swatch);

// Change swatch's color. Possibly impacting many objects fill/stroke.
void sp_change_swatch_color(SPGradient* swatch, const Color& color);

// PROOF: rename a swatch; for a spot swatch this renames its ink (and every tint swatch of it).
void sp_rename_swatch(SPGradient* swatch, Glib::ustring const& label);

// Create swatches in the document for each given color
void sp_create_document_swatches(SPDocument* document, const std::vector<Color>& colors);

// Remove unused (unreferenced) swatches from the document; returns number of removed swatches
int sp_cleanup_document_swatches(SPDocument* document);

// Scan document gradient resources and return all swatches
std::vector<SPGradient*> sp_collect_all_swatches(SPDocument* document);

// find matching swatch, if any
SPGradient* sp_find_matching_swatch(SPDocument* document, const Color& color);

#endif // SEEN_SP_GRADIENT_CHEMISTRY_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
