// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Various utility methods for gradients
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Tavmjong Bah <tavmjong@free.fr>
 *
 * Copyright (C) 2012 Tavmjong Bah
 * Copyright (C) 2010 Authors
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2001-2005 authors
 * Copyright (C) 2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/i18n.h>

#include <2geom/transforms.h>
#include <2geom/bezier-curve.h>
#include <2geom/crossing.h>
#include <2geom/line.h>

#include "desktop-style.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "gradient-chemistry.h"

#include <cmath>

#include "spot-ink.h"

#include <numeric>

#include "fill-or-stroke.h"
#include "gradient-drag.h"
#include "selection.h"

#include "object/sp-defs.h"
#include "object/sp-gradient-reference.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-stop.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "object/sp-root.h"
#include "object/sp-object-iterator.h"
#include "style.h"

#include "svg/svg.h"
#include "svg/css-ostringstream.h"

#include "ui/icon-names.h"
#include "ui/tools/tool-base.h"
#include "ui/widget/gradient-vector-selector.h"
#include "xml/href-attribute-helper.h"

#include "colors/color.h"

#define noSP_GR_VERBOSE

using Inkscape::DocumentUndo;
namespace Colors = Inkscape::Colors;

namespace {

Inkscape::PaintTarget paintTargetItems[] = {Inkscape::FOR_FILL, Inkscape::FOR_STROKE};

std::vector<Inkscape::PaintTarget> vectorOfPaintTargets(paintTargetItems, paintTargetItems + (sizeof(paintTargetItems) / sizeof(paintTargetItems[0])));

} // namespace

namespace Inkscape {

std::vector<PaintTarget> const &allPaintTargets()
{
    return vectorOfPaintTargets;
}

} // namespace Inkscape

// Terminology:
//
// "vector" is a gradient that has stops but not position coords. It can be referenced by one or
// more privates. Objects should not refer to it directly. It has no radial/linear distinction.
//
// "array" is a gradient that has mesh rows and patches. It may or may not have "x" and "y" attributes.
// An array does have spacial information so it cannot be normalized like a "vector".
//
// "shared" is either a "vector" or "array" that is shared between multiple objects.
//
// "private" is a gradient that is not shared. A private linear or radial gradient has no stops but
// has position coords (e.g. center, radius etc for a radial); it references a "vector" for the
// actual colors. A mesh may or may not reference an array. Each private is only used by one object.

static void sp_gradient_repr_set_link(Inkscape::XML::Node *repr, SPGradient *gr);

SPGradient *sp_gradient_ensure_vector_normalized(SPGradient *gr)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_ensure_vector_normalized(%p)", gr);
#endif
    g_return_val_if_fail(gr != nullptr, NULL);
    g_return_val_if_fail(!is<SPMeshGradient>(gr), NULL);

    /* If we are already normalized vector, just return */
    if (gr->state == SP_GRADIENT_STATE_VECTOR) return gr;
    /* Fail, if we have wrong state set */
    if (gr->state != SP_GRADIENT_STATE_UNKNOWN) {
        g_warning("file %s: line %d: Cannot normalize private gradient to vector (%s)", __FILE__, __LINE__, gr->getId());
        return nullptr;
    }

    /* First make sure we have vector directly defined (i.e. gr has its own stops) */
    if ( !gr->hasStops() ) {
        /* We do not have stops ourselves, so flatten stops as well */
        gr->ensureVector();
        g_assert(gr->vector.built);
        // this adds stops from gr->vector as children to gr
        gr->repr_write_vector ();
    }

    /* If gr hrefs some other gradient, remove the href */
    if (gr->ref){
        if (gr->ref->getObject()) {
            // We are hrefing someone, so require flattening
            gr->updateRepr(SP_OBJECT_WRITE_EXT | SP_OBJECT_WRITE_ALL);
            sp_gradient_repr_set_link(gr->getRepr(), nullptr);
        }
    }

    /* Everything is OK, set state flag */
    gr->state = SP_GRADIENT_STATE_VECTOR;
    return gr;
}

/**
 * Creates new private gradient for the given shared gradient.
 */

static SPGradient *sp_gradient_get_private_normalized(SPDocument *document, SPGradient *shared, SPGradientType type)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_get_private_normalized(%p, %p, %d)", document, shared, type);
#endif

    g_return_val_if_fail(document != nullptr, NULL);
    g_return_val_if_fail(shared != nullptr, NULL);
    g_return_val_if_fail(shared->hasStops() || shared->hasPatches(), NULL);

    SPDefs *defs = document->getDefs();

    Inkscape::XML::Document *xml_doc = document->getReprDoc();
    // create a new private gradient of the requested type
    Inkscape::XML::Node *repr;
    if (type == SP_GRADIENT_TYPE_LINEAR) {
        repr = xml_doc->createElement("svg:linearGradient");
    } else if(type == SP_GRADIENT_TYPE_RADIAL) {
        repr = xml_doc->createElement("svg:radialGradient");
    } else {
        repr = xml_doc->createElement("svg:meshgradient");
    }

    // make auto collection optional
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/option/gradient/auto_collect", true)) {
        repr->setAttribute("inkscape:collect", "always");
    } else {
        repr->setAttribute("inkscape:collect", "never");
    }

    // link to shared
    sp_gradient_repr_set_link(repr, shared);

    /* Append the new private gradient to defs */
    defs->getRepr()->appendChild(repr);
    Inkscape::GC::release(repr);

    // get corresponding object
    SPGradient *gr = static_cast<SPGradient *>(document->getObjectByRepr(repr));
    g_assert(gr != nullptr);

    return gr;
}

/**
Count how many times gr is used by the styles of o and its descendants
*/
static guint count_gradient_hrefs(SPObject *o, SPGradient *gr)
{
    if (!o)
        return 1;

    guint i = 0;

    SPStyle *style = o->style;
    if (style
        && style->fill.isPaintserver()
        && is<SPGradient>(SP_STYLE_FILL_SERVER(style))
        && cast<SPGradient>(SP_STYLE_FILL_SERVER(style)) == gr)
    {
        i ++;
    }
    if (style
        && style->stroke.isPaintserver()
        && is<SPGradient>(SP_STYLE_STROKE_SERVER(style))
        && cast<SPGradient>(SP_STYLE_STROKE_SERVER(style)) == gr)
    {
        i ++;
    }

    for (auto& child: o->children) {
        i += count_gradient_hrefs(&child, gr);
    }

    return i;
}


/**
 * If gr has other users, create a new shared; also check if gr links to shared, relink if not
 */
static SPGradient *sp_gradient_fork_private_if_necessary(SPGradient *gr, SPGradient *shared,
                                                         SPGradientType type, SPObject *o)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_fork_private_if_necessary(%p, %p, %d, %p)", gr, shared, type, o);
#endif
    g_return_val_if_fail(gr != nullptr, NULL);

    // Orphaned gradient, no shared with stops or patches at the end of the line; this used to be
    // an assert
    if ( !shared || !(shared->hasStops() || shared->hasPatches()) ) {
        std::cerr << "sp_gradient_fork_private_if_necessary: Orphaned gradient" << std::endl;
        return (gr);
    }

    // user is the object that uses this gradient; normally it's item but for tspans, we
    // check its ancestor text so that tspans don't get different gradients from their
    // texts.
    SPObject *user = o;
    while (is<SPTSpan>(user)) {
        user = user->parent;
    }

    // Check the number of uses of the gradient within this object;
    // if we are private and there are no other users,
    if (!shared->isSwatch() && (gr->hrefcount <= count_gradient_hrefs(user, gr))) {
        // check shared
        if ( gr != shared && gr->ref->getObject() != shared ) {
            /* our href is not the shared, and shared is different from gr; relink */
            sp_gradient_repr_set_link(gr->getRepr(), shared);
        }
        return gr;
    }

    SPDocument *doc = gr->document;
    SPObject *defs = doc->getDefs();

    if ((gr->hasStops()) ||
        (gr->hasPatches()) ||
        (gr->state != SP_GRADIENT_STATE_UNKNOWN) ||
        (gr->parent != defs) ||
        (gr->hrefcount > 1)) {

        // we have to clone a fresh new private gradient for the given shared

        // create an empty one
        SPGradient *gr_new = sp_gradient_get_private_normalized(doc, shared, type);

        // copy all the attributes to it
        Inkscape::XML::Node *repr_new = gr_new->getRepr();
        Inkscape::XML::Node *repr = gr->getRepr();
        repr_new->setAttribute("gradientUnits", repr->attribute("gradientUnits"));
        repr_new->setAttribute("gradientTransform", repr->attribute("gradientTransform"));
        if (is<SPRadialGradient>(gr)) {
            repr_new->setAttribute("cx", repr->attribute("cx"));
            repr_new->setAttribute("cy", repr->attribute("cy"));
            repr_new->setAttribute("fx", repr->attribute("fx"));
            repr_new->setAttribute("fy", repr->attribute("fy"));
            repr_new->setAttribute("r",  repr->attribute("r" ));
            repr_new->setAttribute("fr", repr->attribute("fr"));
            repr_new->setAttribute("spreadMethod", repr->attribute("spreadMethod"));
        } else if (is<SPLinearGradient>(gr)) {
            repr_new->setAttribute("x1", repr->attribute("x1"));
            repr_new->setAttribute("y1", repr->attribute("y1"));
            repr_new->setAttribute("x2", repr->attribute("x2"));
            repr_new->setAttribute("y2", repr->attribute("y2"));
            repr_new->setAttribute("spreadMethod", repr->attribute("spreadMethod"));
        } else { // Mesh
            repr_new->setAttribute("x", repr->attribute("x"));
            repr_new->setAttribute("y", repr->attribute("y"));
            repr_new->setAttribute("type", repr->attribute("type"));

            // We probably want a completely separate mesh gradient so
            // copy the children and unset the link to the shared.
            for ( Inkscape::XML::Node *child = repr->firstChild() ; child ; child = child->next() ) {
                Inkscape::XML::Node *copy = child->duplicate(doc->getReprDoc());
                repr_new->appendChild( copy );
                Inkscape::GC::release( copy );
            }
            sp_gradient_repr_set_link(repr_new, nullptr);

            // Need to generate SPObjects (which fills in node array and handle arrays).
            gr->requestModified(SP_OBJECT_MODIFIED_FLAG);
            gr->document->ensureUpToDate();
       }
        return gr_new;
    } else {
        return gr;
    }
}

static SPGradient *sp_gradient_fork_vector_if_necessary(SPGradient *gr)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_fork_vector_if_necessary(%p)", gr);
#endif
    // Some people actually prefer their gradient vectors to be shared...
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (!prefs->getBool("/options/forkgradientvectors/value", true))
        return gr;

    if (gr->hrefcount > 1) {
        SPDocument *doc = gr->document;
        Inkscape::XML::Document *xml_doc = doc->getReprDoc();

        Inkscape::XML::Node *repr = gr->getRepr()->duplicate(xml_doc);
        doc->getDefs()->getRepr()->appendChild(repr);
        SPGradient *gr_new = static_cast<SPGradient *>(doc->getObjectByRepr(repr));
        gr_new = sp_gradient_ensure_vector_normalized (gr_new);
        Inkscape::GC::release(repr);
        return gr_new;
    }
    return gr;
}

/**
 *  Obtain the vector from the gradient. A forked vector will be created and linked to this gradient if another gradient uses it.
 */
SPGradient *sp_gradient_get_forked_vector_if_necessary(SPGradient *gradient, bool force_vector)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_get_forked_vector_if_necessary(%p, %d)", gradient, force_vector);
#endif
    auto vector = gradient->getVector(force_vector);
    if (!vector) {
        return vector;
    }

    vector = sp_gradient_fork_vector_if_necessary(vector);
    if (gradient != vector && gradient->ref->getObject() != vector) {
        sp_gradient_repr_set_link(gradient->getRepr(), vector);
        gradient->repr_clear_vector(); // remove any stops now that we link to a vector
    }

    return vector;
}


/**
 * Convert an item's gradient to userspace _without_ preserving coords, setting them to defaults
 * instead. No forking or reapplying is done because this is only called for newly created privates.
 * @return The new gradient.
 */
SPGradient *sp_gradient_reset_to_userspace(SPGradient *gr, SPItem *item)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_reset_to_userspace(%p, %p)", gr, item);
#endif
    Inkscape::XML::Node *repr = gr->getRepr();

    // calculate the bbox of the item
    item->document->ensureUpToDate();
    Geom::OptRect bbox = item->visualBounds(); // we need "true" bbox without item_i2d_affine

    if (!bbox)
        return gr;

    Geom::Coord const width = bbox->dimensions()[Geom::X];
    Geom::Coord const height = bbox->dimensions()[Geom::Y];

    Geom::Point const center = bbox->midpoint();

    if (is<SPRadialGradient>(gr)) {
        repr->setAttributeSvgDouble("cx", center[Geom::X]);
        repr->setAttributeSvgDouble("cy", center[Geom::Y]);
        repr->setAttributeSvgDouble("fx", center[Geom::X]);
        repr->setAttributeSvgDouble("fy", center[Geom::Y]);
        repr->setAttributeSvgDouble("r", width/2);

        // we want it to be elliptic, not circular
        Geom::Affine squeeze = Geom::Translate (-center) *
            Geom::Scale(1, height/width) *
            Geom::Translate (center);

        gr->gradientTransform = squeeze;
        gr->setAttributeOrRemoveIfEmpty("gradientTransform", sp_svg_transform_write(gr->gradientTransform));
    } else if (is<SPLinearGradient>(gr)) {

        // Assume horizontal gradient by default (as per SVG 1.1)
        Geom::Point pStart = center - Geom::Point(width/2, 0);
        Geom::Point pEnd = center + Geom::Point(width/2, 0);

        // Get the preferred gradient angle from prefs
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        double angle = prefs->getDouble("/dialogs/gradienteditor/angle", 0.0);

        if (angle != 0.0) {

            Geom::Line grl(center, Geom::rad_from_deg(angle));
            Geom::LineSegment bbl1(bbox->corner(0), bbox->corner(1));
            Geom::LineSegment bbl2(bbox->corner(1), bbox->corner(2));
            Geom::LineSegment bbl3(bbox->corner(2), bbox->corner(3));
            Geom::LineSegment bbl4(bbox->corner(3), bbox->corner(0));

            // Find where our gradient line intersects the bounding box.
            if (!bbl1.isDegenerate() && intersection(bbl1, grl)) {
                pStart = bbl1.pointAt((*intersection(bbl1, grl)).ta);
                pEnd = bbl3.pointAt((*intersection(bbl3, grl)).ta);
                if (intersection(bbl1, grl.ray(grl.angle()))) {
                    std::swap(pStart, pEnd);
                }
            } else if (!bbl2.isDegenerate() && intersection(bbl2, grl)) {
                pStart = bbl2.pointAt((*intersection(bbl2, grl)).ta);
                pEnd = bbl4.pointAt((*intersection(bbl4, grl)).ta);
                if (intersection(bbl2, grl.ray(grl.angle()))) {
                    std::swap(pStart, pEnd);
                }
            }

        }

        repr->setAttributeSvgDouble("x1", pStart[Geom::X]);
        repr->setAttributeSvgDouble("y1", pStart[Geom::Y]);
        repr->setAttributeSvgDouble("x2", pEnd[Geom::X]);
        repr->setAttributeSvgDouble("y2", pEnd[Geom::Y]);

    } else {
        // Mesh
        // THIS IS BEING CALLED TWICE WHENEVER A NEW GRADIENT IS CREATED, WRITING HERE CAUSES PROBLEMS
        // IN SPMeshNodeArray::create()
        //repr->setAttributeSvgDouble("x", bbox->min()[Geom::X]);
        //repr->setAttributeSvgDouble("y", bbox->min()[Geom::Y]);

        // We don't create a shared array gradient.
        auto mg = cast<SPMeshGradient>( gr );
        mg->array.create( mg, item, bbox );
    }

    // set the gradientUnits
    repr->setAttribute("gradientUnits", "userSpaceOnUse");

    return gr;
}

/**
 * Convert an item's gradient to userspace if necessary, also fork it if necessary.
 * @return The new gradient.
 */
SPGradient *sp_gradient_convert_to_userspace(SPGradient *gr, SPItem *item, gchar const *property)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_convert_to_userspace(%p, %p, \"%s\")", gr, item, property);
#endif
    g_return_val_if_fail(gr, NULL);

    if ( gr && gr->isSolid() ) {
        return gr;
    }

    // First, fork it if it is shared
    if (is<SPLinearGradient>(gr)) {
        gr = sp_gradient_fork_private_if_necessary(gr, gr->getVector(), SP_GRADIENT_TYPE_LINEAR, item);
    } else if (is<SPRadialGradient>(gr)) {
        gr = sp_gradient_fork_private_if_necessary(gr, gr->getVector(), SP_GRADIENT_TYPE_RADIAL, item);
    } else {
        gr = sp_gradient_fork_private_if_necessary(gr, const_cast<SPGradient *>(gr->getArray()),  SP_GRADIENT_TYPE_MESH,   item);
    }

    if (gr->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {

        Inkscape::XML::Node *repr = gr->getRepr();

        // calculate the bbox of the item
        item->document->ensureUpToDate();
        Geom::Affine bbox2user;
        Geom::OptRect bbox = item->visualBounds(); // we need "true" bbox without item_i2d_affine
        if ( bbox ) {
            bbox2user = Geom::Affine(bbox->dimensions()[Geom::X], 0,
                                   0, bbox->dimensions()[Geom::Y],
                                   bbox->min()[Geom::X], bbox->min()[Geom::Y]);
        } else {
            // would be degenerate otherwise
            bbox2user = Geom::identity();
        }

        /* skew is the additional transform, defined by the proportions of the item, that we need
         * to apply to the gradient in order to work around this weird bit from SVG 1.1
         * (http://www.w3.org/TR/SVG11/pservers.html#LinearGradients):
         *
         *   When gradientUnits="objectBoundingBox" and gradientTransform is the identity
         *   matrix, the stripes of the linear gradient are perpendicular to the gradient
         *   vector in object bounding box space (i.e., the abstract coordinate system where
         *   (0,0) is at the top/left of the object bounding box and (1,1) is at the
         *   bottom/right of the object bounding box). When the object's bounding box is not
         *   square, the stripes that are conceptually perpendicular to the gradient vector
         *   within object bounding box space will render non-perpendicular relative to the
         *   gradient vector in user space due to application of the non-uniform scaling
         *   transformation from bounding box space to user space.
         */
        Geom::Affine skew = bbox2user;
        double exp = skew.descrim();
        skew[0] /= exp;
        skew[1] /= exp;
        skew[2] /= exp;
        skew[3] /= exp;
        skew[4] = 0;
        skew[5] = 0;

        // apply skew to the gradient
        gr->gradientTransform = skew;
        gr->setAttributeOrRemoveIfEmpty("gradientTransform", sp_svg_transform_write(gr->gradientTransform));

        // Matrix to convert points to userspace coords; postmultiply by inverse of skew so
        // as to cancel it out when it's applied to the gradient during rendering
        Geom::Affine point_convert = bbox2user * skew.inverse();

        if (is<SPLinearGradient>(gr)) {
            auto lg = cast<SPLinearGradient>(gr);

            Geom::Point p1_b = Geom::Point(lg->x1.computed, lg->y1.computed);
            Geom::Point p2_b = Geom::Point(lg->x2.computed, lg->y2.computed);

            Geom::Point p1_u = p1_b * point_convert;
            Geom::Point p2_u = p2_b * point_convert;

            repr->setAttributeSvgDouble("x1", p1_u[Geom::X]);
            repr->setAttributeSvgDouble("y1", p1_u[Geom::Y]);
            repr->setAttributeSvgDouble("x2", p2_u[Geom::X]);
            repr->setAttributeSvgDouble("y2", p2_u[Geom::Y]);

            // set the gradientUnits
            repr->setAttribute("gradientUnits", "userSpaceOnUse");

        } else if (is<SPRadialGradient>(gr)) {
            auto rg = cast<SPRadialGradient>(gr);

            // original points in the bbox coords
            Geom::Point c_b = Geom::Point(rg->cx.computed, rg->cy.computed);
            Geom::Point f_b = Geom::Point(rg->fx.computed, rg->fy.computed);
            double r_b = rg->r.computed;

            // converted points in userspace coords
            Geom::Point c_u = c_b * point_convert;
            Geom::Point f_u = f_b * point_convert;
            double r_u = r_b * point_convert.descrim();

            repr->setAttributeSvgDouble("cx", c_u[Geom::X]);
            repr->setAttributeSvgDouble("cy", c_u[Geom::Y]);
            repr->setAttributeSvgDouble("fx", f_u[Geom::X]);
            repr->setAttributeSvgDouble("fy", f_u[Geom::Y]);
            repr->setAttributeSvgDouble("r", r_u);

            // set the gradientUnits
            repr->setAttribute("gradientUnits", "userSpaceOnUse");

        } else {
            std::cerr << "sp_gradient_convert_to_userspace: Conversion of mesh to userspace not implemented" << std::endl;
        }
    }

    // apply the gradient to the item (may be necessary if we forked it); not recursive
    // generally because grouped items will be taken care of later (we're being called
    // from sp_item_adjust_paint_recursive); however text and all its children should all
    // refer to one gradient, hence the recursive call for text (because we can't/don't
    // want to access tspans and set gradients on them separately)
    if (is<SPText>(item)) {
        sp_style_set_property_url(item, property, gr, true);
    } else {
        sp_style_set_property_url(item, property, gr, false);
    }

    return gr;
}

void sp_gradient_transform_multiply(SPGradient *gradient, Geom::Affine postmul, bool set)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_transform_multiply(%p, , %d)", gradient, set);
#endif
    if (set) {
        gradient->gradientTransform = postmul;
    } else {
        gradient->gradientTransform *= postmul; // fixme: get gradient transform by climbing to hrefs?
    }
    gradient->gradientTransform_set = TRUE;

    auto c = sp_svg_transform_write(gradient->gradientTransform);
    gradient->setAttributeOrRemoveIfEmpty("gradientTransform", c);
}

SPGradient *getGradient(SPItem *item, Inkscape::PaintTarget fill_or_stroke)
{
    SPStyle *style = item->style;
    SPGradient *gradient = nullptr;

    switch (fill_or_stroke)
    {
        case Inkscape::FOR_FILL:
            if (style && (style->fill.isPaintserver())) {
                SPPaintServer *server = item->style->getFillPaintServer();
                if ( is<SPGradient>(server) ) {
                    gradient = cast<SPGradient>(server);
                }
            }
            break;
        case Inkscape::FOR_STROKE:
            if (style && (style->stroke.isPaintserver())) {
                SPPaintServer *server = item->style->getStrokePaintServer();
                if ( is<SPGradient>(server) ) {
                    gradient = cast<SPGradient>(server);
                }
            }
            break;
    }

   return gradient;
}

SPStop *sp_last_stop(SPGradient *gradient)
{
    for (SPStop *stop = gradient->getFirstStop(); stop != nullptr; stop = stop->getNextStop()) {
        if (stop->getNextStop() == nullptr)
            return stop;
    }
    return nullptr;
}

std::pair<SPStop*, SPStop*> sp_get_before_after_stops(SPStop* stop) {
   SPStop* before = nullptr;
   SPStop* after = nullptr;

   if (stop) {
      before = stop->getPrevStop();
      after = stop->getNextStop();
   }

   return std::make_pair(before, after);
}

static std::pair<SPStop*, SPStop*> get_before_after_stops(SPGradient* gradient, double offset) {
   SPStop* before = nullptr;
   SPStop* after = nullptr;

   SPStop* stop = gradient->getFirstStop();
   while (stop && stop->offset < offset) {
      before = stop;
      stop = stop->getNextStop();
   }

   if (stop && stop->offset > offset) {
      after = stop;
   }

   return std::make_pair(before, after);
}

guint sp_number_of_stops_before_stop(SPGradient* gradient, SPStop* target) {
    if (!gradient) return 0;

    guint n = 0;
    for (SPStop* stop = gradient->getFirstStop(); stop != nullptr; stop = stop->getNextStop()) {
        if (stop == target) {
            return n;
        }
        n++;
    }
    return n;
}


SPStop* sp_get_nth_stop(SPGradient* gradient, guint index) {
    SPStop* stop = gradient->getFirstStop();
    if (!stop) return nullptr;

    for (guint i = 0; i < index; ++i) {
        if (!stop) return nullptr;

        stop = stop->getNextStop();
    }

    return stop;
}


SPStop *sp_get_stop_i(SPGradient *gradient, guint stop_i)
{
    SPStop *stop = gradient->getFirstStop();
    if (!stop) {
        return nullptr;
    }

    // if this is valid but weird gradient without an offset-zero stop element,
    // inkscape has created a handle for the start of gradient anyway,
    // so when it asks for stop N that corresponds to stop element N-1
    if (stop->offset != 0)
    {
        stop_i--;
    }

    for (guint i = 0; i < stop_i; i++) {
        if (!stop) {
            return nullptr;
        }
        stop = stop->getNextStop();
    }

    return stop;
}

void sp_repr_set_css_double(Inkscape::XML::Node* node, const char* key, double value) {
    if (node) {
        node->setAttributeCssDouble(key, value);
    }
}

SPStop *sp_vector_add_stop(SPGradient *vector, SPStop* prev_stop, SPStop* next_stop, gfloat offset)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_vector_add_stop(%p, %p, %p, %f)", vector, prev_stop, next_stop, offset);
#endif
    SPStop* newstop = nullptr;
    // this function doesn't deal with empty gradients
    if (!prev_stop && !next_stop) return newstop;

    // This function completely breaks CMYK gradients.
    Color cnew(0x000000ff); // new color
    Inkscape::XML::Node *new_stop_repr = nullptr;

    if (!prev_stop || !next_stop) {
       // inserting stop past next or before previous is supported
       SPStop* stop = prev_stop ? prev_stop : next_stop;
       auto repr = stop->getRepr();
       new_stop_repr = repr->duplicate(vector->getRepr()->document());
       vector->getRepr()->addChild(new_stop_repr, prev_stop ? repr : nullptr);

       cnew = stop->getColor();
    }
    else {
        auto repr = prev_stop->getRepr();
        new_stop_repr = repr->duplicate(vector->getRepr()->document());
        vector->getRepr()->addChild(new_stop_repr, repr);
        cnew = prev_stop->getColor().averaged(next_stop->getColor(), (offset - prev_stop->offset) / (next_stop->offset - prev_stop->offset));
    }

    newstop = reinterpret_cast<SPStop *>(vector->document->getObjectByRepr(new_stop_repr));
    newstop->offset = offset;
    newstop->getRepr()->setAttributeCssDouble("offset", (double)offset);
    newstop->setColor(cnew);
    Inkscape::GC::release(new_stop_repr);

    return newstop;
}

// delete gradient's stop
void sp_gradient_delete_stop(SPGradient* gradient, SPStop* stop) {

    if (!stop || !gradient) {
        return;
    }

    if (gradient->getStopCount() > 2) { // 2 is the minimum
        gradient->getRepr()->removeChild(stop->getRepr());
        DocumentUndo::done(gradient->document, RC_("Undo", "Delete gradient stop"), INKSCAPE_ICON("color-gradient"));
    }
}

// make gradient well-formed if needed; from gradient-vector.cpp
static bool verify_grad(SPGradient* gradient) {
    bool modified = false;
    int i = 0;
    SPStop *stop = nullptr;
    /* count stops */
    for (auto& ochild: gradient->children) {
        if (is<SPStop>(&ochild)) {
            i++;
            stop = cast<SPStop>(&ochild);
        }
    }

    Inkscape::XML::Document *xml_doc;
    xml_doc = gradient->getRepr()->document();

    if (i < 1) {
        Inkscape::XML::Node *child;

        child = xml_doc->createElement("svg:stop");
        sp_repr_set_css_double(child, "offset", 0.0);
        SPStop::setColorRepr(child, Colors::Color(0x000000ff));
        gradient->getRepr()->addChild(child, nullptr);
        Inkscape::GC::release(child);

        child = xml_doc->createElement("svg:stop");
        sp_repr_set_css_double(child, "offset", 1.0);
        SPStop::setColorRepr(child, Colors::Color(0x000000ff));
        gradient->getRepr()->addChild(child, nullptr);
        Inkscape::GC::release(child);
        modified = true;
    }
    else if (i < 2) {
        sp_repr_set_css_double(stop->getRepr(), "offset", 0.0);
        Inkscape::XML::Node *child = stop->getRepr()->duplicate(gradient->getRepr()->document());
        sp_repr_set_css_double(child, "offset", 1.0);
        gradient->getRepr()->addChild(child, stop->getRepr());
        Inkscape::GC::release(child);
        modified = true;
    }

    return modified;
}

// add new stop to a gradient; function lifted from gradient-vector.cpp
SPStop* sp_gradient_add_stop(SPGradient* gradient, SPStop* current) {
    if (!gradient || !current) return nullptr;

    if (verify_grad(gradient)) {
        // gradient has been fixed by adding stop(s), don't insert another one
        return nullptr;
    }

    SPStop *stop = current;
    Inkscape::XML::Node *new_stop_repr = nullptr;
    SPStop *next = stop->getNextStop();

    if (next == nullptr) {
        SPStop *prev = stop->getPrevStop();
        if (prev != nullptr) {
            next = stop;
            stop = prev;
        }
    }

    if (next != nullptr) {
        new_stop_repr = stop->getRepr()->duplicate(gradient->getRepr()->document());
        gradient->getRepr()->addChild(new_stop_repr, stop->getRepr());
    } else {
        next = stop;
        new_stop_repr = stop->getPrevStop()->getRepr()->duplicate(gradient->getRepr()->document());
        gradient->getRepr()->addChild(new_stop_repr, stop->getPrevStop()->getRepr());
    }

    SPStop *newstop = reinterpret_cast<SPStop *>(gradient->document->getObjectByRepr(new_stop_repr));

    newstop->offset = (stop->offset + next->offset) * 0.5 ;

    newstop->setColor(stop->getColor().averaged(next->getColor()));
    sp_repr_set_css_double(newstop->getRepr(), "offset", (double)newstop->offset);
    Inkscape::GC::release(new_stop_repr);
    DocumentUndo::done(gradient->document, RC_("Undo", "Add gradient stop"), INKSCAPE_ICON("color-gradient"));

    return newstop;
}

SPStop* sp_gradient_add_stop_at(SPGradient* gradient, double offset) {
    if (!gradient) return nullptr;

    verify_grad(gradient);

    // find stops before and after given offset

    std::pair<SPStop*, SPStop*> stops = get_before_after_stops(gradient, offset);

    if (stops.first || stops.second) {
        auto stop = sp_vector_add_stop(gradient, stops.first, stops.second, offset);
        if (stop) {
           DocumentUndo::done(gradient->document, RC_("Undo", "Add gradient stop"), INKSCAPE_ICON("color-gradient"));
        }
        return stop;
    }
    else {
        return nullptr;
    }
}

void sp_set_gradient_stop_color(SPDocument* document, SPStop* stop, Colors::Color const &color) {
   sp_repr_set_css_double(stop->getRepr(), "offset", stop->offset);
   stop->setColor(color);
   DocumentUndo::maybeDone(document, "gradient:stop:color", RC_("Undo", "Change gradient stop color"), INKSCAPE_ICON("color-gradient"));
}

SPStop* sp_item_gradient_get_stop(SPItem *item, GrPointType point_type, guint point_i, Inkscape::PaintTarget fill_or_stroke) {
    SPGradient *gradient = getGradient(item, fill_or_stroke);

    if (!gradient) {
        return nullptr;
    }

    if (is<SPLinearGradient>(gradient) || is<SPRadialGradient>(gradient) ) {

        SPGradient *vector = gradient->getVector();

        if (!vector) // orphan!
            return nullptr;

        switch (point_type) {
            case POINT_LG_BEGIN:
            case POINT_RG_CENTER:
            case POINT_RG_FOCUS:
                return vector->getFirstStop();

            case POINT_LG_END:
            case POINT_RG_R1:
            case POINT_RG_R2:
                return sp_last_stop (vector);

            case POINT_LG_MID:
            case POINT_RG_MID1:
            case POINT_RG_MID2:
                return sp_get_stop_i (vector, point_i);

            default:
                g_warning( "Bad linear/radial gradient handle type" );
                break;
        }
    }
    return nullptr;
}

Colors::Color sp_item_gradient_stop_query_style(SPItem *item, GrPointType point_type, guint point_i, Inkscape::PaintTarget fill_or_stroke)
{
    SPGradient *gradient = getGradient(item, fill_or_stroke);

    if (!gradient)
        return Colors::Color(0x000000ff);

    if (is<SPLinearGradient>(gradient) || is<SPRadialGradient>(gradient) ) {

        SPGradient *vector = gradient->getVector();

        if (!vector) // orphan!
            return Colors::Color(0x000000ff);

        switch (point_type) {
            case POINT_LG_BEGIN:
            case POINT_RG_CENTER:
            case POINT_RG_FOCUS:
            {
                SPStop *first = vector->getFirstStop();
                if (first) {
                    return first->getColor();
                }
            }
            break;

            case POINT_LG_END:
            case POINT_RG_R1:
            case POINT_RG_R2:
            {
                SPStop *last = sp_last_stop (vector);
                if (last) {
                    return last->getColor();
                }
            }
            break;

            case POINT_LG_MID:
            case POINT_RG_MID1:
            case POINT_RG_MID2:
            {
                SPStop *stopi = sp_get_stop_i (vector, point_i);
                if (stopi) {
                    return stopi->getColor();
                }
            }
            break;

            default:
                g_warning( "Bad linear/radial gradient handle type" );
                break;
        }
    } else if (is<SPMeshGradient>(gradient)) {

        // Mesh gradient
        auto mg = cast<SPMeshGradient>(gradient);

        switch (point_type) {
            case POINT_MG_CORNER: {
                if (point_i >= mg->array.corners.size()) {
                    return Colors::Color(0x000000ff);
                }
                SPMeshNode const* cornerpoint = mg->array.corners[ point_i ];

                if (cornerpoint) {
                    return *cornerpoint->color;
                }
                break;
            }

            case POINT_MG_HANDLE:
            case POINT_MG_TENSOR:
            {
                // Do nothing. Handles and tensors don't have color
                break;
            }

            default:
                g_warning( "Bad mesh handle type" );
        }
    }
    return Colors::Color(0x000000ff);
}

void sp_item_gradient_stop_set_style(SPItem *item, GrPointType point_type, guint point_i, Inkscape::PaintTarget fill_or_stroke, SPCSSAttr *stop)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_item_gradient_stop_set_style(%p, %d, %d, %d, %p)", item, point_type, point_i, fill_or_stroke, stop);
#endif
    SPGradient *gradient = getGradient(item, fill_or_stroke);

    if (!gradient)
        return;

    if (is<SPLinearGradient>(gradient) || is<SPRadialGradient>(gradient) ) {

        auto vector = sp_gradient_get_forked_vector_if_necessary(gradient);
        if (!vector) // orphan!
            return;

        switch (point_type) {
            case POINT_LG_BEGIN:
            case POINT_RG_CENTER:
            case POINT_RG_FOCUS:
            {
                SPStop *first = vector->getFirstStop();
                if (first) {
                    sp_repr_css_change(first->getRepr(), stop, "style");
                }
            }
            break;

            case POINT_LG_END:
            case POINT_RG_R1:
            case POINT_RG_R2:
            {
                SPStop *last = sp_last_stop (vector);
                if (last) {
                    sp_repr_css_change(last->getRepr(), stop, "style");
                }
            }
            break;

            case POINT_LG_MID:
            case POINT_RG_MID1:
            case POINT_RG_MID2:
            {
                SPStop *stopi = sp_get_stop_i (vector, point_i);
                if (stopi) {
                    sp_repr_css_change(stopi->getRepr(), stop, "style");
                }
            }
            break;

            default:
                g_warning( "Bad linear/radial gradient handle type" );
                break;
        }
    } else {

        // Mesh gradient
        auto mg = cast<SPMeshGradient>(gradient);

        switch (point_type) {
            case POINT_MG_CORNER: {

                // Update mesh array (which is not updated automatically when stop is changed?)
                gchar const* color_str = sp_repr_css_property(stop, "stop-color", nullptr);
                if (auto color = Colors::Color::parse(color_str)) {
                    color->addOpacity(sp_repr_css_double_property(stop, "stop-opacity", 1.0));
                    if (*color != mg->array.corners[point_i]->color) {
                        mg->array.corners[ point_i ]->color = *color;
                        // Update stop
                        SPStop *stopi = mg->array.corners[ point_i ]->stop;
                        if (stopi) {
                            sp_repr_css_change(stopi->getRepr(), stop, "style");
                        } else {
                            std::cerr << "sp_item_gradient_stop_set_style: null stopi" << std::endl;
                        }
                    }
                }
                break;
            }

            case POINT_MG_HANDLE:
            case POINT_MG_TENSOR:
            {
                // Do nothing. Handles and tensors don't have colors.
                break;
            }

            default:
                g_warning( "Bad mesh handle type" );
        }
    }
}

void sp_item_gradient_reverse_vector(SPItem *item, Inkscape::PaintTarget fill_or_stroke)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_item_gradient_reverse_vector(%p, %d)", item, fill_or_stroke);
#endif
    SPGradient *gradient = getGradient(item, fill_or_stroke);
    sp_gradient_reverse_vector(gradient);
}

void sp_gradient_reverse_vector(SPGradient* gradient) {
    if (!gradient)
        return;

    auto vector = sp_gradient_get_forked_vector_if_necessary(gradient);
    if (!vector) // orphan!
        return;

    std::vector<SPObject *> child_objects;
    std::vector<Inkscape::XML::Node *>child_reprs;
    std::vector<double> offsets;
    double offset;
    for (auto& child: vector->children) {
        child_reprs.push_back(child.getRepr());
        child_objects.push_back(&child);
        offset = child.getRepr()->getAttributeDouble("offset", 0);
        offsets.push_back(offset);
    }

    std::vector<Inkscape::XML::Node *> child_copies;
    for (auto repr:child_reprs) {
        Inkscape::XML::Document *xml_doc = vector->getRepr()->document();
        child_copies.push_back(repr->duplicate(xml_doc));
    }


    for (auto i:child_objects) {
        i->deleteObject();
    }

    std::vector<double>::reverse_iterator o_it = offsets.rbegin();
    for (auto c_it = child_copies.rbegin(); c_it != child_copies.rend(); ++c_it, ++o_it) {
        vector->appendChildRepr(*c_it);
        (*c_it)->setAttributeSvgDouble("offset", 1 - *o_it);
        Inkscape::GC::release(*c_it);
    }
}

void sp_item_gradient_invert_vector_color(SPItem *item, Inkscape::PaintTarget fill_or_stroke)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_item_gradient_invert_vector_color(%p, %d)", item, fill_or_stroke);
#endif
    SPGradient *gradient = getGradient(item, fill_or_stroke);
    if (!gradient)
        return;

    auto vector = sp_gradient_get_forked_vector_if_necessary(gradient);
    if (!vector) // orphan!
        return;

    for (auto &child: vector->children) {
        if (auto stop = cast<SPStop>(&child)) {
            auto color = stop->getColor();
            color.invert();
            stop->setColor(color);
        }
    }
}

// HACK: linear and radial gradients may have first and/or last stops moved from their default positions
// of 0 and 1 respectively; this is not what gradient tool was built to handle; instead of making extensive
// changes to try to fix it, this hack just makes sure that midpoint draggers don't move to the true 0/1 limits;
// with that, code relying on sp_get_stop_i will work correctly
double midpoint_offset_hack(double offset) {
    const double EPS = 0.0001;

    if (offset <= 0) {
        offset = EPS;
    }
    else if (offset >= 1) {
        offset = 1 - EPS;
    }

    return offset;
}

/**
Set the position of point point_type of the gradient applied to item (either fill_or_stroke) to
p_w (in desktop coordinates). Write_repr if you want the change to become permanent.
*/
void sp_item_gradient_set_coords(SPItem *item, GrPointType point_type, guint point_i, Geom::Point p_w, Inkscape::PaintTarget fill_or_stroke, bool write_repr, bool scale)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_item_gradient_set_coords(%p, %d, %d, (%f, %f), ...)", item, point_type, point_i, p_w[Geom::X], p_w[Geom::Y] );
#endif
    SPGradient *gradient = getGradient(item, fill_or_stroke);

    if (!gradient)
        return;

    // Needed only if units are set to SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX
    gradient = sp_gradient_convert_to_userspace(gradient, item, (fill_or_stroke == Inkscape::FOR_FILL) ? "fill" : "stroke");

    Geom::Affine i2d (item->i2dt_affine ());
    Geom::Point p = p_w * i2d.inverse();
    p *= (gradient->gradientTransform).inverse();
    // now p is in gradient's original coordinates

    Inkscape::XML::Node *repr = gradient->getRepr();

    if (is<SPLinearGradient>(gradient)) {
        auto lg = cast<SPLinearGradient>(gradient);
        switch (point_type) {
            case POINT_LG_BEGIN:
                if (scale) {
                    lg->x2.computed += (lg->x1.computed - p[Geom::X]);
                    lg->y2.computed += (lg->y1.computed - p[Geom::Y]);
                }
                lg->x1.computed = p[Geom::X];
                lg->y1.computed = p[Geom::Y];
                if (write_repr) {
                    if (scale) {
                        repr->setAttributeSvgDouble("x2", lg->x2.computed);
                        repr->setAttributeSvgDouble("y2", lg->y2.computed);
                    }
                    repr->setAttributeSvgDouble("x1", lg->x1.computed);
                    repr->setAttributeSvgDouble("y1", lg->y1.computed);
                } else {
                    gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
                }
                break;
            case POINT_LG_END:
                if (scale) {
                    lg->x1.computed += (lg->x2.computed - p[Geom::X]);
                    lg->y1.computed += (lg->y2.computed - p[Geom::Y]);
                }
                lg->x2.computed = p[Geom::X];
                lg->y2.computed = p[Geom::Y];
                if (write_repr) {
                    if (scale) {
                        repr->setAttributeSvgDouble("x1", lg->x1.computed);
                        repr->setAttributeSvgDouble("y1", lg->y1.computed);
                    }
                    repr->setAttributeSvgDouble("x2", lg->x2.computed);
                    repr->setAttributeSvgDouble("y2", lg->y2.computed);
                } else {
                    gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
                }
                break;
            case POINT_LG_MID:
            {
                // using X-coordinates only to determine the offset, assuming p has been snapped to the vector from begin to end.
                Geom::Point begin(lg->x1.computed, lg->y1.computed);
                Geom::Point end(lg->x2.computed, lg->y2.computed);
                double offset = Geom::LineSegment(begin, end).nearestTime(p);
                offset = midpoint_offset_hack(offset);
                auto vector = sp_gradient_get_forked_vector_if_necessary(lg);
                lg->ensureVector();
                lg->vector.stops.at(point_i).offset = offset;
                if (SPStop* stopi = sp_get_stop_i(vector, point_i)) {
                    stopi->offset = offset;
                    if (write_repr) {
                        stopi->getRepr()->setAttributeCssDouble("offset", stopi->offset);
                    } else {
                        stopi->requestModified(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
                    }
                }
            }
            break;
            default:
                g_warning( "Bad linear gradient handle type" );
                break;
        }
    } else if (is<SPRadialGradient>(gradient)) {
        auto rg = cast<SPRadialGradient>(gradient);
        Geom::Point c (rg->cx.computed, rg->cy.computed);
        Geom::Point c_w = c * gradient->gradientTransform * i2d; // now in desktop coords
        if ((point_type == POINT_RG_R1 || point_type == POINT_RG_R2) && Geom::L2 (p_w - c_w) < 1e-3) {
            // prevent setting a radius too close to the center
            return;
        }
        Geom::Affine new_transform;
        bool transform_set = false;

        switch (point_type) {
            case POINT_RG_CENTER:
                rg->fx.computed = p[Geom::X] + (rg->fx.computed - rg->cx.computed);
                rg->fy.computed = p[Geom::Y] + (rg->fy.computed - rg->cy.computed);
                rg->cx.computed = p[Geom::X];
                rg->cy.computed = p[Geom::Y];
                if (write_repr) {
                    repr->setAttributeSvgDouble("fx", rg->fx.computed);
                    repr->setAttributeSvgDouble("fy", rg->fy.computed);
                    repr->setAttributeSvgDouble("cx", rg->cx.computed);
                    repr->setAttributeSvgDouble("cy", rg->cy.computed);
                } else {
                    gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
                }
                break;
            case POINT_RG_FOCUS:
                rg->fx.computed = p[Geom::X];
                rg->fy.computed = p[Geom::Y];
                if (write_repr) {
                    repr->setAttributeSvgDouble("fx", rg->fx.computed);
                    repr->setAttributeSvgDouble("fy", rg->fy.computed);
                } else {
                    gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
                }
                break;
            case POINT_RG_R1:
            {
                Geom::Point r1_w = (c + Geom::Point(rg->r.computed, 0)) * gradient->gradientTransform * i2d;
                double r1_angle = Geom::atan2(r1_w - c_w);
                double move_angle = Geom::atan2(p_w - c_w) - r1_angle;
                double move_stretch = Geom::L2(p_w - c_w) / Geom::L2(r1_w - c_w);

                Geom::Affine move = Geom::Affine (Geom::Translate (-c_w)) *
                    Geom::Affine (Geom::Rotate(-r1_angle)) *
                    Geom::Affine (Geom::Scale(move_stretch, scale? move_stretch : 1)) *
                    Geom::Affine (Geom::Rotate(r1_angle)) *
                    Geom::Affine (Geom::Rotate(move_angle)) *
                    Geom::Affine (Geom::Translate (c_w));

                new_transform = gradient->gradientTransform * i2d * move * i2d.inverse();
                transform_set = true;

                break;
            }
            case POINT_RG_R2:
            {
                Geom::Point r2_w = (c + Geom::Point(0, -rg->r.computed)) * gradient->gradientTransform * i2d;
                double r2_angle = Geom::atan2(r2_w - c_w);
                double move_angle = Geom::atan2(p_w - c_w) - r2_angle;
                double move_stretch = Geom::L2(p_w - c_w) / Geom::L2(r2_w - c_w);

                Geom::Affine move = Geom::Affine (Geom::Translate (-c_w)) *
                    Geom::Affine (Geom::Rotate(-r2_angle)) *
                    Geom::Affine (Geom::Scale(move_stretch, scale? move_stretch : 1)) *
                    Geom::Affine (Geom::Rotate(r2_angle)) *
                    Geom::Affine (Geom::Rotate(move_angle)) *
                    Geom::Affine (Geom::Translate (c_w));

                new_transform = gradient->gradientTransform * i2d * move * i2d.inverse();
                transform_set = true;

                break;
            }
            case POINT_RG_MID1:
            {
                Geom::Point start = Geom::Point (rg->cx.computed, rg->cy.computed);
                Geom::Point end   = Geom::Point (rg->cx.computed + rg->r.computed, rg->cy.computed);
                double offset = Geom::LineSegment(start, end).nearestTime(p);
                offset = midpoint_offset_hack(offset);
                auto vector = sp_gradient_get_forked_vector_if_necessary(rg);
                rg->ensureVector();
                rg->vector.stops.at(point_i).offset = offset;
                if (SPStop* stopi = sp_get_stop_i(vector, point_i)) {
                    stopi->offset = offset;
                    if (write_repr) {
                        stopi->getRepr()->setAttributeCssDouble("offset", stopi->offset);
                    } else {
                        stopi->requestModified(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
                    }
                }
                break;
            }
            case POINT_RG_MID2:
            {
                Geom::Point start = Geom::Point (rg->cx.computed, rg->cy.computed);
                Geom::Point end   = Geom::Point (rg->cx.computed, rg->cy.computed - rg->r.computed);
                double offset = Geom::LineSegment(start, end).nearestTime(p);
                offset = midpoint_offset_hack(offset);
                auto vector = sp_gradient_get_forked_vector_if_necessary(rg);
                rg->ensureVector();
                rg->vector.stops.at(point_i).offset = offset;
                if (SPStop* stopi = sp_get_stop_i(vector, point_i)) {
                    stopi->offset = offset;
                    if (write_repr) {
                        stopi->getRepr()->setAttributeCssDouble("offset", stopi->offset);
                    } else {
                        stopi->requestModified(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
                    }
                }
                break;
            }
            default:
                g_warning( "Bad radial gradient handle type" );
                break;
        }

        if (transform_set) {
            gradient->gradientTransform = new_transform;
            gradient->gradientTransform_set = TRUE;
            if (write_repr) {
                auto s = sp_svg_transform_write(gradient->gradientTransform);
                gradient->setAttributeOrRemoveIfEmpty("gradientTransform", s);
            } else {
                gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
            }
        }
    } else if (is<SPMeshGradient>(gradient)) {
        auto mg = cast<SPMeshGradient>(gradient);
        //Geom::Affine new_transform;
        //bool transform_set = false;

        switch (point_type) {
            case POINT_MG_CORNER:
            {
                if (point_i < mg->array.corners.size()) {
                    mg->array.corners[ point_i ]->p = p;
                    // Handles are moved in gradient-drag.cpp
                    gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
                } else {
                    std::cerr << "sp_item_gradient_set_coords: bad point number" << std::endl;
                }
                break;
            }

            case POINT_MG_HANDLE: {
                if (point_i < mg->array.handles.size()) {
                    mg->array.handles[ point_i ]->p = p;
                    gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
                } else {
                    std::cerr << "sp_item_gradient_set_coords: bad point number" << std::endl;
                }
                break;
            }

            case POINT_MG_TENSOR: {
                if (point_i < mg->array.tensors.size()) {
                    mg->array.tensors[ point_i ]->p = p;
                    gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
                } else {
                    std::cerr << "sp_item_gradient_set_coords: bad point number" << std::endl;
                }
                break;
            }

            default:
                g_warning( "Bad mesh handle type" );
        }
        if( write_repr ) {
            mg->array.write( mg );
        }
    }

}

SPGradient *sp_item_gradient_get_vector(SPItem *item, Inkscape::PaintTarget fill_or_stroke)
{
    SPGradient *gradient = getGradient(item, fill_or_stroke);

    if (gradient) {
        return gradient->getVector();
    }
    return nullptr;
}

SPGradientSpread sp_item_gradient_get_spread(SPItem *item, Inkscape::PaintTarget fill_or_stroke)
{
    SPGradientSpread spread = SP_GRADIENT_SPREAD_PAD;
    SPGradient *gradient = getGradient(item, fill_or_stroke);

    if (gradient) {
        spread = gradient->fetchSpread();
    }
    return spread;
}


/**
Returns the position of point point_type of the gradient applied to item (either fill_or_stroke),
in desktop coordinates.
*/
Geom::Point getGradientCoords(SPItem *item, GrPointType point_type, guint point_i, Inkscape::PaintTarget fill_or_stroke)
{
    SPGradient *gradient = getGradient(item, fill_or_stroke);
#ifdef SP_GR_VERBOSE
    g_message("getGradientCoords(%p, %d, %d, %d, %p)", item, point_type, point_i, fill_or_stroke, gradient);
#endif

    Geom::Point p (0, 0);

    if (!gradient)
        return p;

    if (is<SPLinearGradient>(gradient)) {
        auto lg = cast<SPLinearGradient>(gradient);
        switch (point_type) {
            case POINT_LG_BEGIN:
                p = Geom::Point (lg->x1.computed, lg->y1.computed);
                break;
            case POINT_LG_END:
                p = Geom::Point (lg->x2.computed, lg->y2.computed);
                break;
            case POINT_LG_MID:
                {
                    if (lg->vector.stops.size() < point_i) {
                        g_message("POINT_LG_MID bug trigger, see LP bug #453067");
                        break;
                    }
                    gdouble offset = lg->vector.stops.at(point_i).offset;
                    p = (1-offset) * Geom::Point(lg->x1.computed, lg->y1.computed) + offset * Geom::Point(lg->x2.computed, lg->y2.computed);
                }
                break;
            default:
                g_warning( "Bad linear gradient handle type" );
                break;
        }
    } else     if (is<SPRadialGradient>(gradient)) {
        auto rg = cast<SPRadialGradient>(gradient);
        switch (point_type) {
            case POINT_RG_CENTER:
                p = Geom::Point (rg->cx.computed, rg->cy.computed);
                break;
            case POINT_RG_FOCUS:
                p = Geom::Point (rg->fx.computed, rg->fy.computed);
                break;
            case POINT_RG_R1:
                p = Geom::Point (rg->cx.computed + rg->r.computed, rg->cy.computed);
                break;
            case POINT_RG_R2:
                p = Geom::Point (rg->cx.computed, rg->cy.computed - rg->r.computed);
                break;
            case POINT_RG_MID1:
                {
                    if (rg->vector.stops.size() < point_i) {
                        g_message("POINT_RG_MID1 bug trigger, see LP bug #453067");
                        break;
                    }
                    gdouble offset = rg->vector.stops.at(point_i).offset;
                    p = (1-offset) * Geom::Point (rg->cx.computed, rg->cy.computed) + offset * Geom::Point(rg->cx.computed + rg->r.computed, rg->cy.computed);
                }
                break;
            case POINT_RG_MID2:
                {
                    if (rg->vector.stops.size() < point_i) {
                        g_message("POINT_RG_MID2 bug trigger, see LP bug #453067");
                        break;
                    }
                    gdouble offset = rg->vector.stops.at(point_i).offset;
                    p = (1-offset) * Geom::Point (rg->cx.computed, rg->cy.computed) + offset * Geom::Point(rg->cx.computed, rg->cy.computed - rg->r.computed);
                }
                break;
            default:
                g_warning( "Bad radial gradient handle type" );
                break;
        }
    } else     if (is<SPMeshGradient>(gradient)) {
        auto mg = cast<SPMeshGradient>(gradient);
        switch (point_type) {

            case POINT_MG_CORNER:
                p = mg->array.corners[ point_i ]->p;
                break;

            case POINT_MG_HANDLE: {
                p = mg->array.handles[ point_i ]->p;
                break;
            }

            case POINT_MG_TENSOR: {
                p = mg->array.tensors[ point_i ]->p;
                break;
            }

            default:
                g_warning( "Bad mesh handle type" );
        }
    }


    if (gradient->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {
        item->document->ensureUpToDate();
        Geom::OptRect bbox = item->visualBounds(); // we need "true" bbox without item_i2d_affine
        if (bbox) {
            p *= Geom::Affine(bbox->dimensions()[Geom::X], 0,
                            0, bbox->dimensions()[Geom::Y],
                            bbox->min()[Geom::X], bbox->min()[Geom::Y]);
        }
    }
    p *= Geom::Affine(gradient->gradientTransform) * (Geom::Affine)item->i2dt_affine();
    return p;
}

/**
 * Sets item fill or stroke to the gradient of the specified type with given vector, creating
 * new private gradient, if needed.
 * gr has to be a normalized vector.
 */

SPGradient *sp_item_set_gradient(SPItem *item, SPGradient *gr, SPGradientType type, Inkscape::PaintTarget fill_or_stroke)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_item_set_gradient(%p, %p, %d, %d)", item, gr, type, fill_or_stroke);
#endif
    g_return_val_if_fail(item != nullptr, NULL);
    g_return_val_if_fail(gr != nullptr, NULL);
    g_return_val_if_fail(gr->state == SP_GRADIENT_STATE_VECTOR, NULL);

    SPStyle *style = item->style;
    g_assert(style != nullptr);

    SPPaintServer *ps = nullptr;
    if ((fill_or_stroke == Inkscape::FOR_FILL) ? style->fill.isPaintserver() : style->stroke.isPaintserver()) {
        ps = (fill_or_stroke == Inkscape::FOR_FILL) ? SP_STYLE_FILL_SERVER(style) : SP_STYLE_STROKE_SERVER(style);
    }

    if (ps
        && ( (type == SP_GRADIENT_TYPE_LINEAR && is<SPLinearGradient>(ps)) ||
             (type == SP_GRADIENT_TYPE_RADIAL && is<SPRadialGradient>(ps))   ) )
    {

        /* Current fill style is the gradient of the required type */
        auto current = cast<SPGradient>(ps);
        //g_message("hrefcount %d   count %d\n", current->hrefcount, count_gradient_hrefs(item, current));

        if (!current->isSwatch()
            && (current->hrefcount == 1 ||
            current->hrefcount == count_gradient_hrefs(item, current))) {

            // current is private and it's either used once, or all its uses are by children of item;
            // so just change its href to vector

            if ( current != gr && current->getVector() != gr ) {
                // href is not the vector
                sp_gradient_repr_set_link(current->getRepr(), gr);
                current->repr_clear_vector(); // remove any stops now that we link to a vector
            }
            item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            return current;

        } else {

            // the gradient is not private, or it is shared with someone else;
            // normalize it (this includes creating new private if necessary)
            SPGradient *normalized = sp_gradient_fork_private_if_necessary(current, gr, type, item);

            g_return_val_if_fail(normalized != nullptr, NULL);

            if (normalized != current) {

                /* We have to change object style here; recursive because this is used from
                 * fill&stroke and must work for groups etc. */
                sp_style_set_property_url(item, (fill_or_stroke == Inkscape::FOR_FILL) ? "fill" : "stroke", normalized, true);
            }
            item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            return normalized;
        }

    } else {
        /* Current fill style is not a gradient or wrong type, so construct everything */
        /* This is where mesh gradients are constructed. */
        g_assert(gr); // TEMP
        SPGradient *constructed = sp_gradient_get_private_normalized(item->document, gr, type);
        constructed = sp_gradient_reset_to_userspace(constructed, item);
        sp_style_set_property_url(item, ( (fill_or_stroke == Inkscape::FOR_FILL) ? "fill" : "stroke" ), constructed, true);
        item->requestDisplayUpdate(( SP_OBJECT_MODIFIED_FLAG |
                                     SP_OBJECT_STYLE_MODIFIED_FLAG ));
        return constructed;
    }
}

static void sp_gradient_repr_set_link(Inkscape::XML::Node *repr, SPGradient *link)
{
#ifdef SP_GR_VERBOSE
    g_message("sp_gradient_repr_set_link(%p, %p)", repr, link);
#endif
    g_return_if_fail(repr != nullptr);

    if (link) {
        Glib::ustring ref("#");
        ref += link->getId();
        Inkscape::setHrefAttribute(*repr, ref);
    } else {
        repr->removeAttribute("xlink:href");
        repr->removeAttribute("href");
    }
}


static void addStop(Inkscape::XML::Node *parent, Colors::Color const &color, double opacity, gchar const *offset)
{
#ifdef SP_GR_VERBOSE
    g_message("addStop(%p, %s, %f, %s)", parent, color.toString().c_str(), opacity, offset);
#endif
    auto doc = parent->document();
    Inkscape::XML::Node *repr = doc->createElement("svg:stop");

    Colors::Color copy = color;
    copy.addOpacity(opacity);
    SPStop::setColorRepr(repr, copy);
    repr->setAttribute( "offset", offset );
    parent->appendChild(repr);
    Inkscape::GC::release(repr);
}

/*
 * Get default normalized gradient vector of document, create if there is none
 */
SPGradient *sp_document_default_gradient_vector(SPDocument *document, Colors::Color const &color, double opacity, bool singleStop)
{
    SPDefs *defs = document->getDefs();

    Inkscape::XML::Node *repr = document->getReprDoc()->createElement("svg:linearGradient");
    defs->getRepr()->appendChild(repr);

    if ( !singleStop ) {
        // make auto collection optional
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        if (prefs->getBool("/option/gradient/auto_collect", true)) {
            repr->setAttribute("inkscape:collect", "always");
        } else {
            repr->setAttribute("inkscape:collect", "never");
        }

        // set here, but removed when it's edited in the gradient editor
        // to further reduce clutter, we could
        // (1) here, search gradients by color and return what is found without duplication
        // (2) in fill & stroke, show only one copy of each gradient in list

        // Use a gradient prefix for the ID - it's a vector with stop definitions for either linear or radial gradient
        repr->setAttribute("id", document->generate_unique_id("gradient"));
    } else {
        // Use a swatch prefix for the id, for better UX
        repr->setAttribute("id", document->generate_unique_id("swatch"));
    }

    addStop(repr, color, opacity, "0");
    if (!singleStop) {
        auto lightness = Colors::get_perceptual_lightness(color);
        auto contrast = (lightness > 0.95 ? Colors::Color(0x00'00'00'ff) : Colors::Color(0xff'ff'ff'ff)).converted(color.getSpace());
        // second stop without transparency - no more forcing users to fix alpha channel on new gradients
        addStop(repr, /*contrast.value_or*/(color), 1.0, "1");
    }

    Inkscape::GC::release(repr);

    /* fixme: This does not look like nice */
    SPGradient *gr = static_cast<SPGradient *>(document->getObjectByRepr(repr));
    g_assert(gr != nullptr);
    /* fixme: Maybe add extra sanity check here */
    gr->state = SP_GRADIENT_STATE_VECTOR;

    return gr;
}

SPGradient *sp_gradient_vector_for_object( SPDocument *const doc, SPDesktop *const desktop,
                                           SPObject *const o, Inkscape::PaintTarget const fill_or_stroke, bool singleStop )
{
    Colors::Color color(0x000000ff);
    double opacity = 1.0;
    bool for_fill = fill_or_stroke == Inkscape::FOR_FILL;

    // if not o or o doesn't use flat color, then take current color of the desktop.
    if (auto tmp = sp_desktop_get_color(desktop, for_fill))
        color = *tmp;

    if (o && o->style) {
        // take the color of the object
        SPStyle const &style = *(o->style);
        SPIPaint const &paint = *style.getFillOrStroke(for_fill);
        if (paint.isPaintserver()) {
            SPObject *server = for_fill ? o->style->getFillPaintServer() : o->style->getStrokePaintServer();
            if ( is<SPLinearGradient>(server) || is<SPRadialGradient>(server) ) {
                return cast<SPGradient>(server)->getVector(true);
            }
        } else if (paint.isColor()) {
            color = paint.getColor();
            opacity = for_fill ? style.fill_opacity.as_double() : style.stroke_opacity.as_double();
        }
    }

    return sp_document_default_gradient_vector(doc, color, opacity, singleStop);
}

void sp_gradient_invert_selected_gradients(SPDesktop *desktop, Inkscape::PaintTarget fill_or_stroke)
{
    Inkscape::Selection *selection = desktop->getSelection();

    auto list= selection->items();
    for (auto i : list) {
        sp_item_gradient_invert_vector_color(i, fill_or_stroke);
    }

    // we did an undoable action
    DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Invert gradient colors"), INKSCAPE_ICON("color-gradient"));
}

void sp_gradient_reverse_selected_gradients(SPDesktop *desktop)
{
    Inkscape::Selection *selection = desktop->getSelection();
    Inkscape::UI::Tools::ToolBase *ev = desktop->getTool();

    if (!ev) {
        return;
    }

    GrDrag *drag = ev->get_drag();

    // First try selected dragger
    if (drag && !drag->selected.empty()) {
        drag->selected_reverse_vector();
    } else { // If no drag or no dragger selected, act on selection (both fill and stroke gradients)
        auto list= selection->items();
        for (auto i : list) {
            sp_item_gradient_reverse_vector(i, Inkscape::FOR_FILL);
            sp_item_gradient_reverse_vector(i, Inkscape::FOR_STROKE);
        }
    }

    // we did an undoable action
    DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Reverse gradient"), INKSCAPE_ICON("color-gradient"));
}

void sp_gradient_unset_swatch(SPDesktop *desktop, std::string const &id)
{
    SPDocument *doc = desktop ? desktop->doc() : nullptr;

    if (doc) {
        const std::vector<SPObject *> gradients = doc->getResourceList("gradient");
        for (auto gradient : gradients) {
            auto grad = cast<SPGradient>(gradient);
            if ( id == grad->getId() ) {
                grad->setSwatch(false);
                DocumentUndo::done(doc, RC_("Undo", "Delete swatch"), INKSCAPE_ICON("color-gradient"));
                break;
            }
        }
    }
}

/*
 * Return a SPItem's gradient
 */
SPGradient* sp_item_get_gradient(SPItem *item, bool fillorstroke)
{
    SPIPaint *item_paint = item->style->getFillOrStroke(fillorstroke);
    if (item_paint->isPaintserver()) {

        SPPaintServer *item_server = fillorstroke ? item->style->getFillPaintServer() : item->style->getStrokePaintServer();

        if (is<SPLinearGradient>(item_server) || is<SPRadialGradient>(item_server) ||
                (is<SPGradient>(item_server) && cast<SPGradient>(item_server)->getVector()->isSwatch()))  {

            return cast<SPGradient>(item_server)->getVector();
        }
    }

    return nullptr;
}

int sp_get_gradient_refcount(SPDocument* document, SPGradient* gradient) {
    if (!document || !gradient) return 0;

    int count = std::accumulate(object_subtree_iterator(document->getRoot()), object_subtree_iterator(nullptr), 0, [=](auto acc, auto obj) {
        auto item = cast<SPItem>(obj);
        if (!item || !item->getId()) {
            return acc;
        }
        SPGradient* fill = sp_item_get_gradient(item, true); // fill
        if (fill == gradient) {
            ++acc;
        }
        SPGradient* stroke = sp_item_get_gradient(item, false); // stroke
        if (stroke == gradient) {
            ++acc;
        }
        return acc;
    });

    return count;
}

void sp_item_apply_gradient(SPItem* item, SPGradient* vector, SPDesktop* desktop, SPGradientType gradient_type, bool create_swatch, FillOrStroke kind) {
    if (!item || !item->document || !item->style || gradient_type == SP_GRADIENT_TYPE_MESH) return;

    Inkscape::PaintTarget paint_target = kind == FILL ? Inkscape::FOR_FILL : Inkscape::FOR_STROKE;

    if (!vector) {
        // no vector means that we want to create a new one; find initial color
        if (auto paint = item->style->getFillOrStroke(kind == FILL)) {
            Colors::Color common(0xff'ff'ff'ff); // fallback: opaque white
            if (paint->isColor()) {
                common = paint->getColor();
            }
            else if (auto server = cast<SPGradient>(kind == FILL ? item->style->getFillPaintServer() : item->style->getStrokePaintServer())) {
                if (auto vect = server->getVector(); vect && vect->hasStops()) {
                    common = vect->getFirstStop()->getColor();
                }
            }
            else if (desktop) {
                if (auto color = sp_desktop_get_color(desktop, kind == FILL)) {
                    common = *color;
                }
            }
            vector = sp_document_default_gradient_vector(item->document, common, 1.0, create_swatch);
        }
        if (vector) {
            vector->setSwatch(create_swatch);
        }
        // apply
        if (!vector) {
            auto gr = sp_gradient_vector_for_object(item->document, desktop, item, paint_target, create_swatch);
            if (gr) {
                gr->setSwatch(create_swatch);
            }
            sp_item_set_gradient(item, gr, gradient_type, paint_target);
        }
        else {
            sp_item_set_gradient(item, vector, gradient_type, paint_target);
        }
    }
    else {
        // We have changed from another gradient type, or modified spread/units within
        // this gradient type.
        vector = sp_gradient_ensure_vector_normalized(vector);
        sp_item_set_gradient(item, vector, gradient_type, paint_target);
    }
}

void sp_item_apply_mesh(SPItem* item, SPGradient* mesh, SPDocument* document, FillOrStroke kind) {
    if (!item || !item->document || !item->style) return;

    auto style = item->style;
    SPPaintServer* server = kind == FILL ? style->getFillPaintServer() : style->getStrokePaintServer();
    bool has_mesh = false;
    if (server && is<SPMeshGradient>(server)) {
        has_mesh = true;
    }

    auto xml_doc = document->getReprDoc();
    SPDefs* defs = document->getDefs();
    SPMeshGradient* mesh_gradient = nullptr;

    if (!mesh || !has_mesh) {
        // No mesh in document or object does not already have mesh -> create new mesh.

        // Create mesh element
        auto repr = xml_doc->createElement("svg:meshgradient");

        // privates are garbage-collectable
        repr->setAttribute("inkscape:collect", "always");

        // Attach to document
        defs->getRepr()->appendChild(repr);
        Inkscape::GC::release(repr);

        // Get corresponding object
        mesh_gradient = static_cast<SPMeshGradient*>(document->getObjectByRepr(repr));
        mesh_gradient->array.create(mesh_gradient, item, kind == FILL ? item->geometricBounds() : item->visualBounds());
    }
    else {
        // Using found mesh

        // Duplicate
        auto mesh_repr = mesh->getRepr();
        auto copy_repr = mesh_repr->duplicate(xml_doc);

        // privates are garbage-collectable
        copy_repr->setAttribute("inkscape:collect", "always");

        // Attach to document
        defs->getRepr()->appendChild(copy_repr);
        Inkscape::GC::release(copy_repr);

        // Get corresponding object
        mesh_gradient = static_cast<SPMeshGradient*>(document->getObjectByRepr(copy_repr));
        mesh_gradient->array.read(mesh_gradient);

        Geom::OptRect item_bbox = kind == FILL ? item->geometricBounds() : item->visualBounds();
        mesh_gradient->array.fill_box(mesh_gradient, item_bbox);
    }

    sp_style_set_property_url(item, kind == FILL ? "fill" : "stroke", mesh_gradient, is<SPText>(item));
}

void sp_delete_item_swatch(SPItem* item, FillOrStroke kind, SPGradient* to_delete, SPGradient* replacement) {
    if (!item || !item->style || !replacement) return;

    auto swatch = cast<SPGradient>(kind == FILL ? item->style->getFillPaintServer() : item->style->getStrokePaintServer());
    if (!swatch) return;

    swatch = swatch->getVector();

    // apply replacement swatch
    sp_item_apply_gradient(item, replacement, nullptr, SP_GRADIENT_TYPE_LINEAR, true, kind);

    // now we can remove it
    swatch->collectOrphan();
}

bool sp_can_delete_swatch(SPGradient* swatch) {
    if (!swatch || !swatch->document) return false;

    int count = 0;
    auto&& list = swatch->document->getResourceList("gradient");
    for (auto obj : list) {
        auto grad = cast_unsafe<SPGradient>(obj);
        if (grad->isSwatch()) {
            ++count;
            if (count == 2) break;
        }
    }

    // too few swatches
    if (count < 2) return false;

    count = 0;
    for (auto obj : swatch->document->getRoot()) {
        auto item = cast<SPItem>(obj);
        if (!item || !item->getId()) continue;

        auto fill = sp_item_get_gradient(item, true); // fill
        if (fill == swatch) {
            ++count;
        }
        auto stroke = sp_item_get_gradient(item, false); // stroke
        if (stroke == swatch) {
            ++count;
        }

        if (count > 1) break;
    }

    // swatch can be deleted if there's only one reference to it
    return count < 2;
}

SPGradient* sp_find_replacement_swatch(SPDocument* document, SPGradient* swatch) {
    if (!document || !swatch) return nullptr;

    auto&& list = document->getResourceList("gradient");
    for (auto obj : list) {
        auto grad = cast_unsafe<SPGradient>(obj);
        if (grad->isSwatch() && grad != swatch) {
            return grad;
        }
    }

    return nullptr;
}

void sp_change_swatch_color(SPGradient* swatch, const Colors::Color& color) {
    if (!swatch) return;

    // PROOF: a spot swatch's colour is its ink's definition. Redefine the ink, so its other tint
    // swatches follow, keeping the definition's colour space (a Lab Pantone stays Lab when edited
    // in an RGB picker). Editing a tint swatch works back to the full-strength definition.
    if (auto ink = Inkscape::SpotInk::read(swatch->getRepr())) {
        auto edited = color.converted(ink->alternate.getSpace()).value_or(color);
        Inkscape::SpotInk::redefine(swatch->document, *ink, Inkscape::SpotInk::alternate_for(edited, ink->tint));
        return;
    }

    if (swatch->hasStops()) {
        swatch->getFirstStop()->setColor(color);
    }
}

void sp_rename_swatch(SPGradient* swatch, Glib::ustring const& label) {
    if (!swatch || label.empty()) return;

    // PROOF: renaming a spot swatch renames its ink; a tint swatch's "NN%" suffix is not part of it.
    if (auto ink = Inkscape::SpotInk::read(swatch->getRepr())) {
        std::string name = label.raw();
        auto const suffix = " " + std::to_string(static_cast<int>(std::lround(ink->tint * 100))) + "%";
        if (ink->tint < 1.0 && name.size() > suffix.size() && name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
        }
        Inkscape::SpotInk::rename(swatch->document, *ink, name);
        return;
    }
    swatch->setLabel(label.c_str());
}

void sp_create_document_swatches(SPDocument* document, const std::vector<Colors::Color>& colors) {
    if (!document || colors.empty()) return;

    for (auto& color : colors) {
        auto swatch = sp_document_default_gradient_vector(document, color, 1, true);
        swatch->setSwatch();
        auto name = color.getName();
        if (!name.empty()) {
            swatch->setLabel(name.c_str());
        }
    }
}

std::vector<SPGradient*> sp_collect_all_swatches(SPDocument* document) {
    std::vector<SPGradient*> swatches;
    if (!document) return swatches;

    auto gradients = document->getResourceList("gradient");
    swatches.reserve(gradients.size() / 2);

    for (auto grad : gradients) {
        auto g = static_cast<SPGradient*>(grad);
        if (g->isSwatch()) swatches.push_back(g);
    }

    return swatches;
}

int sp_cleanup_document_swatches(SPDocument* document) {
    if (!document) return false;

    std::unordered_map<SPGradient*, int> swatches;
    auto gradients = document->getResourceList("gradient");

    for (auto grad : gradients) {
        auto g = static_cast<SPGradient*>(grad);
        if (g->isSwatch()) swatches[g] = 0;
    }

    for (auto obj : document->getRoot()) {
        auto item = cast<SPItem>(obj);
        if (!item || !item->getId()) {
            continue;
        }
        if (auto fill = sp_item_get_gradient(item, true)) {
            swatches[fill]++;
        }
        if (auto stroke = sp_item_get_gradient(item, false)) {
            swatches[stroke]++;
        }
    }

    int removed = 0;
    for (auto kv : swatches) {
        if (kv.first && kv.second == 0) {
            // unused swatch, mark for collection
            // kv.first->setSwatch(false);
            // kv.first->removeAttribute("inkscape:swatch");
            // kv.first->setCollectionPolicy(SPObject::ALWAYS_COLLECT);
            kv.first->collectOrphan();
            removed++;
        }
    }

    return removed;
}

SPGradient* sp_find_matching_swatch(SPDocument* document, const Colors::Color& color) {
    if (!document) return nullptr;

    auto rgb = color.converted(Colors::Space::Type::RGB);
    if (!rgb) return nullptr;
    rgb->enableOpacity(false);

    auto gradients = document->getResourceList("gradient");
    for (auto grad : gradients) {
        auto g = static_cast<SPGradient*>(grad);
        if (g->isSwatch()) {
            if (auto c = g->getFirstStop()->getColor().converted(Colors::Space::Type::RGB)) {
                c->enableOpacity(false);
                if (*c == *rgb) return g;
            }
        }
    }

    return nullptr;
}

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
