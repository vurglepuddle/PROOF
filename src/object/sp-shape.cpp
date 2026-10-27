// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Base class for shapes, including <path> element
 *
 * Author:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 * Copyright (C) 2004 John Cliff
 * Copyright (C) 2007-2008 Johan Engelen
 * Copyright (C) 2010      Jon A. Cruz <jon@joncruz.org>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <2geom/rect.h>
#include <2geom/transforms.h>
#include <2geom/pathvector.h>
#include <2geom/path-intersection.h>
#include "helper/geom.h"
#include "helper/geom-nodetype.h"

#include <sigc++/functors/ptr_fun.h>
#include <sigc++/adaptors/bind.h>

#include "display/drawing-shape.h"
#include "print.h"
#include "document.h"
#include "style.h"
#include "sp-marker.h"
#include "marker-transform-preview.h"
#include "sp-root.h"
#include "sp-path.h"
#include "preferences.h"
#include "attributes.h"
#include "path/path-outline.h" // For bound box calculation

#include "svg/svg.h"
#include "svg/path-string.h"
#include "snap-candidate.h"
#include "snap-preferences.h"
#include "live_effects/lpeobject.h"

#define noSHAPE_VERBOSE

static void sp_shape_update_marker_view (SPShape *shape, Inkscape::DrawingItem *ai);

SPShape::SPShape() : SPLPEItem() {
    for (auto & i : this->_marker) {
        i = nullptr;
    }
}

SPShape::~SPShape() {
    for ( int i = 0 ; i < SP_MARKER_LOC_QTY ; i++ ) {
        this->_release_connect[i].disconnect();
        this->_modified_connect[i].disconnect();
    }
}

void SPShape::build(SPDocument *document, Inkscape::XML::Node *repr)
{
    SPLPEItem::build(document, repr);

    for (int i = 0 ; i < SP_MARKER_LOC_QTY ; i++) {
        set_marker(i, style->marker_ptrs[i]->value());
    }
    if (!hasPathEffectOnClipOrMaskRecursive(this)) {
        if (is<SPPath>(this)) {
            if (auto originald = getAttribute("inkscape:original-d")) {
                if (isOnClipboard()) {
                    setAttribute("d", originald);
                }
                setAttribute("inkscape:original-d", nullptr);
            }
        }
    }
}

/**
 * Removes, releases and unrefs all children of object
 *
 * This is the inverse of sp_shape_build().  It must be invoked as soon
 * as the shape is removed from the tree, even if it is still referenced
 * by other objects.  This routine also disconnects/unrefs markers and
 * curves attached to it.
 *
 * \see SPObject::release()
 */
void SPShape::release()
{
    for (int i = 0; i < SP_MARKER_LOC_QTY; i++) {
        if (_marker[i]) {

            for (auto &v : views) {
                sp_marker_hide(_marker[i], v.drawingitem->key() + ITEM_KEY_MARKERS + i);
            }

            _release_connect[i].disconnect();
            _modified_connect[i].disconnect();
            _marker[i]->unhrefObject(this);
            _marker[i] = nullptr;
        }
    }
    
    _curve.reset();
    
    _curve_before_lpe.reset();

    SPLPEItem::release();
}

void SPShape::set(SPAttr key, const gchar* value) {
	SPLPEItem::set(key, value);
}


Inkscape::XML::Node* SPShape::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
	SPLPEItem::write(xml_doc, repr, flags);
	return repr;
}

void SPShape::update(SPCtx* ctx, guint flags) {
    // Any update can change the bounding box,
    // so the cached version can no longer be used.
    // But the idle checker usually is just moving the objects around.
    bbox_vis_cache_is_valid = false;
    bbox_geom_cache_is_valid = false;

    // std::cout << "SPShape::update(): " << (getId()?getId():"null") << std::endl;
    SPLPEItem::update(ctx, flags);

    /* This stanza checks that an object's marker style agrees with
     * the marker objects it has allocated.  sp_shape_set_marker ensures
     * that the appropriate marker objects are present (or absent) to
     * match the style.
     */
    for (int i = 0 ; i < SP_MARKER_LOC_QTY ; i++) {
        set_marker(i, style->marker_ptrs[i]->value());
    }

    if (flags & (SP_OBJECT_STYLE_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG)) {
        if (this->style->stroke_width.unit == SP_CSS_UNIT_PERCENT) {
            SPItemCtx *ictx = (SPItemCtx *) ctx;
            double const aw = 1.0 / ictx->i2vp.descrim();
            this->style->stroke_width.computed = this->style->stroke_width.value * aw;

            for (auto &v : views) {
                auto sh = cast<Inkscape::DrawingShape>(v.drawingitem.get());
                if (hasMarkers()) {
                    context_style = style;
                    sh->setStyle(style, context_style);
                    // Done at end:
                    // sh->setChildrenStyle(this->context_style); //Resolve 'context-xxx' in children.
                } else if (parent) {
                    context_style = parent->context_style;
                    sh->setStyle(style, context_style);
                }
            }
        }
    }

    if (flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_PARENT_MODIFIED_FLAG)) {
        /* This is suboptimal, because changing parent style schedules recalculation */
        /* But on the other hand - how can we know that parent does not tie style and transform */
        for (auto &v : views) {
            if (flags & SP_OBJECT_MODIFIED_FLAG) {
                auto sh = cast_unsafe<Inkscape::DrawingShape>(v.drawingitem.get());
                sh->setPath(_curve);
            }
        }
    }

    if (this->hasMarkers ()) {

        /* Dimension marker views */
        for (auto &v : views) {
            SPItem::ensure_key(v.drawingitem.get());
            for (int i = 0; i < SP_MARKER_LOC_QTY; i++) {
                if (_marker[i]) {
                    sp_marker_show_dimension(_marker[i], v.drawingitem->key() + ITEM_KEY_MARKERS + i, numberOfMarkers(i));
                }
            }
        }

        /* Update marker views */
        for (auto &v : views) {
            sp_shape_update_marker_view (this, v.drawingitem.get());
        }
    
        // Marker selector needs this here or marker previews are not rendered.
        for (auto &v : views) {
            auto sh = static_cast<Inkscape::DrawingShape*>(v.drawingitem.get());
            sh->setChildrenStyle(this->context_style); // Resolve 'context-xxx' in children.
        }
    }

    /* Update stroke/dashes for relative units. */
    if (flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG)) {

        SPItemCtx const *ictx = reinterpret_cast<SPItemCtx const *>(ctx);

        double const w = ictx->viewport.width();
        double const h = ictx->viewport.height();
        double const d = sqrt(w*w + h*h) * M_SQRT1_2; // diagonal per SVG spec
        double const em = style->font_size.computed;
        double const ex = 0.5 * em;  // fixme: get x height from pango or libnrtype.

        if (style->stroke_width.unit == SP_CSS_UNIT_EM) {
            style->stroke_width.computed = style->stroke_width.value * em;
        }
        else if (style->stroke_width.unit == SP_CSS_UNIT_EX) {
            style->stroke_width.computed = style->stroke_width.value * ex;
        }
        else if (style->stroke_width.unit == SP_CSS_UNIT_PERCENT) {
            style->stroke_width.computed = style->stroke_width.value * d;
        }

        if (style->stroke_dasharray.values.size() != 0) {
            for (auto&& i: style->stroke_dasharray.values) {
                if      (i.unit == SP_CSS_UNIT_EM)      i.computed = i.value * em;
                else if (i.unit == SP_CSS_UNIT_EX)      i.computed = i.value * ex;
                else if (i.unit == SP_CSS_UNIT_PERCENT) i.computed = i.value * d;
            }
        }

        if (style->stroke_dashoffset.unit == SP_CSS_UNIT_EM) {
            style->stroke_dashoffset.computed = style->stroke_dashoffset.value * em;
        }
        else if (style->stroke_dashoffset.unit == SP_CSS_UNIT_EX) {
            style->stroke_dashoffset.computed = style->stroke_dashoffset.value * ex;
        }
        else if (style->stroke_dashoffset.unit == SP_CSS_UNIT_PERCENT) {
            style->stroke_dashoffset.computed = style->stroke_dashoffset.value * d;
        }
    }
}

/**
 * @brief Lists every marker on this shape along with its transform and marker type.
 *
 * @note The transform returned is not premultiplied by `marker->c2p`. The caller should
 *       ensure to apply any required premultiplication(s).
 */
std::vector<std::tuple<SPMarkerLoc, SPMarker *, Geom::Affine>> SPShape::get_markers(
    Geom::PathVector const *preview_curve, std::optional<double> preview_width) const
{
    std::vector<std::tuple<SPMarkerLoc, SPMarker *, Geom::Affine>> markers;

    Geom::PathVector const &pathv = preview_curve ? *preview_curve : *_curve;
    if (pathv.empty())
        return markers; // empty list

    auto width = preview_width.value_or(style->stroke_width.computed);
    auto add_marker = [this, &markers, width](SPMarkerLoc marker_type, Geom::Affine const &m, bool start) {
        if (SPMarker *marker = _marker[marker_type]) {
            // TODO: We shouldn't need to return the marker type
            markers.emplace_back(marker_type, marker, marker->get_marker_transform(m, width, start));
        }
    };

    // START marker
    {
        Geom::Affine const m(sp_shape_marker_get_transform_at_start(pathv.front().front()));
        for (auto marker_type : {SP_MARKER_LOC, SP_MARKER_LOC_START}) {
            add_marker(marker_type, m, true);
        }
    }

    // MID marker
    for (Geom::PathVector::const_iterator path_it = pathv.begin(); path_it != pathv.end(); ++path_it) {
        // START position
        // if this is the last path and it is a moveto-only, don't draw mid marker there
        if (path_it != pathv.begin() && !((path_it == (pathv.end() - 1)) && (path_it->size_default() == 0))) {
            Geom::Affine const m(sp_shape_marker_get_transform_at_start(path_it->front()));
            for (auto marker_type : {SP_MARKER_LOC, SP_MARKER_LOC_MID}) {
                add_marker(marker_type, m, false);
            }
        }
        // MID position
        if (path_it->size_default() > 1) {
            Geom::Path::const_iterator curve_it1 = path_it->begin();     // incoming curve
            Geom::Path::const_iterator curve_it2 = ++(path_it->begin()); // outgoing curve
            while (curve_it2 != path_it->end_default()) {
                /* Put marker between curve_it1 and curve_it2.
                 * Loop to end_default (so including closing segment), because when a path is closed,
                 * there should be a midpoint marker between last segment and closing straight line segment
                 */
                Geom::Affine const m(sp_shape_marker_get_transform(*curve_it1, *curve_it2));
                for (auto marker_type : {SP_MARKER_LOC, SP_MARKER_LOC_MID}) {
                    add_marker(marker_type, m, false);
                }
                ++curve_it1;
                ++curve_it2;
            }
        }
        // END position
        if (path_it != (pathv.end() - 1) && !path_it->empty()) {
            Geom::Curve const &lastcurve = path_it->back_default();
            Geom::Affine const m = sp_shape_marker_get_transform_at_end(lastcurve);
            for (auto marker_type : {SP_MARKER_LOC, SP_MARKER_LOC_MID}) {
                add_marker(marker_type, m, false);
            }
        }
    }

    // END marker
    {
        /* Get reference to last curve in the path.
         * For moveto-only path, this returns the "closing line segment". */
        Geom::Path const &path_last = pathv.back();
        auto index = path_last.size_default();
        Geom::Curve const &lastcurve = path_last[index > 0 ? index - 1 : index];
        Geom::Affine const m = sp_shape_marker_get_transform_at_end(lastcurve);

        for (auto marker_type : {SP_MARKER_LOC, SP_MARKER_LOC_END}) {
            add_marker(marker_type, m, false);
        }
    }
    return markers;
}

/**
 * Calculate the transform required to get a marker's path object in the
 * right place for particular path segment on a shape.
 *
 * \see sp_shape_marker_update_marker_view.
 *
 * From SVG spec:
 * The axes of the temporary new user coordinate system are aligned according to the orient attribute on the 'marker'
 * element and the slope of the curve at the given vertex. (Note: if there is a discontinuity at a vertex, the slope
 * is the average of the slopes of the two segments of the curve that join at the given vertex. If a slope cannot be
 * determined, the slope is assumed to be zero.)
 *
 * Reference: http://www.w3.org/TR/SVG11/painting.html#MarkerElement, the `orient' attribute.
 * Reference for behaviour of zero-length segments:
 * http://www.w3.org/TR/SVG11/implnote.html#PathElementImplementationNotes
 */
Geom::Affine sp_shape_marker_get_transform(Geom::Curve const & c1, Geom::Curve const & c2)
{
    Geom::Point p = c1.pointAt(1);
    Geom::Curve * c1_reverse = c1.reverse();
    Geom::Point tang1 = - c1_reverse->unitTangentAt(0);
    delete c1_reverse;
    Geom::Point tang2 = c2.unitTangentAt(0);

    double const angle1 = Geom::atan2(tang1);
    double const angle2 = Geom::atan2(tang2);

    double ret_angle = .5 * (angle1 + angle2);

    if ( fabs( angle2 - angle1 ) > M_PI ) {
        /* ret_angle is in the middle of the larger of the two sectors between angle1 and
         * angle2, so flip it by 180degrees to force it to the middle of the smaller sector.
         *
         * (Imagine a circle with rays drawn at angle1 and angle2 from the centre of the
         * circle.  Those two rays divide the circle into two sectors.)
         */
        ret_angle += M_PI;
    }

    return Geom::Rotate(ret_angle) * Geom::Translate(p);
}

Geom::Affine sp_shape_marker_get_transform_at_start(Geom::Curve const & c)
{
    Geom::Point p = c.pointAt(0);
    Geom::Affine ret = Geom::Translate(p);

    if ( !c.isDegenerate() ) {
        Geom::Point tang = c.unitTangentAt(0);
        double const angle = Geom::atan2(tang);
        ret = Geom::Rotate(angle) * Geom::Translate(p);
    } else {
        /* FIXME: the svg spec says to search for a better alternative than zero angle directionality:
         * http://www.w3.org/TR/SVG11/implnote.html#PathElementImplementationNotes */
    }

    return ret;
}

Geom::Affine sp_shape_marker_get_transform_at_end(Geom::Curve const & c)
{
    Geom::Point p = c.pointAt(1);
    Geom::Affine ret = Geom::Translate(p);

    if ( !c.isDegenerate() ) {
        Geom::Curve * c_reverse = c.reverse();
        Geom::Point tang = - c_reverse->unitTangentAt(0);
        delete c_reverse;
        double const angle = Geom::atan2(tang);
        ret = Geom::Rotate(angle) * Geom::Translate(p);
    } else {
        /* FIXME: the svg spec says to search for a better alternative than zero angle directionality:
         * http://www.w3.org/TR/SVG11/implnote.html#PathElementImplementationNotes */
    }

    return ret;
}

void SPShape::setMarkerTransformPreview(unsigned key, std::optional<Inkscape::StrokeTransform> const &stroke)
{
    auto drawing_item = get_arenaitem(key);
    if (!drawing_item || !_curve) return;
    if (!stroke) {
        for (auto marker : _marker) {
            if (!marker) continue;
            for (unsigned loc = 0; loc < SP_MARKER_LOC_QTY; ++loc) {
                sp_marker_set_transform_preview(*marker, drawing_item->key() + ITEM_KEY_MARKERS + loc, std::nullopt);
            }
        }
        return;
    }
    if (stroke->transform.isSingular() || transform.isSingular()) return;
    auto curve = *_curve * (transform * stroke->transform.inverse());
    auto width = style->stroke_width.computed * stroke->width_expansion;
    unsigned counter[SP_MARKER_LOC_QTY] = {};
    for (auto const &[loc, marker, affine] : get_markers(&curve, width)) {
        auto pos = counter[loc]++;
        sp_marker_set_transform_preview(*marker, drawing_item->key() + ITEM_KEY_MARKERS + loc,
                                        affine * stroke->transform * transform.inverse(), pos);
    }
}

/**
 * Updates the instances (views) of a given marker in a shape.
 * Marker views have to be scaled already.  The transformation
 * is retrieved and then shown by calling sp_marker_show_instance.
 *
 * @todo figure out what to do when both 'marker' and for instance 'marker-end' are set.
 */
static void sp_shape_update_marker_view(SPShape *shape, Inkscape::DrawingItem *ai)
{
    if (!shape->curve())
        return;

    // the first vertex should get a start marker, the last an end marker, and all the others a mid marker
    // see bug 456148

    // Record the number of instances of each marker type and their absolute position
    unsigned int counter[SPMarkerLoc::SP_MARKER_LOC_QTY] = {0};
    unsigned int z_order = 0;

    // C++23: Use std::ranges::enumerate for z_order
    for (auto const &[type, marker, tr] : shape->get_markers()) {
        sp_marker_show_instance(marker, ai, type, counter[type]++, z_order++, tr, shape->style->stroke_width.computed);
    }
}

void SPShape::modified(unsigned int flags) {
    // std::cout << "SPShape::modified(): " << (getId()?getId():"null") << std::endl; 
    SPLPEItem::modified(flags);

    if (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) {
        for (auto &v : views) {
            auto sh = cast<Inkscape::DrawingShape>(v.drawingitem.get());
            if (hasMarkers()) {
                this->context_style = this->style;
                sh->setStyle(this->style, this->context_style);
                // Note: marker selector preview does not trigger SP_OBJECT_STYLE_MODIFIED_FLAG so
                // this is not called when marker previews are generated, however there is code in
                // SPShape::update() that calls this routine so we don't worry about it here.
                sh->setChildrenStyle(this->context_style); // Resolve 'context-xxx' in children.
            } else if (this->parent) {
                this->context_style = this->parent->context_style;
                sh->setStyle(this->style, this->context_style);
            }
        }
    }

    if (flags & SP_OBJECT_MODIFIED_FLAG && style->filter.set) {
        if (auto filter = style->getFilter()) {
            filter->update_filter_all_regions();
        }
    }

    if (!_curve) {
        sp_lpe_item_update_patheffect(this, true, false);
    }
}

bool SPShape::checkBrokenPathEffect()
{
    if (hasBrokenPathEffect()) {
        g_warning("The shape has unknown LPE on it. Convert to path to make it editable preserving the appearance; "
                  "editing it will remove the bad LPE");

        if (this->getRepr()->attribute("d")) {
            // unconditionally read the curve from d, if any, to preserve appearance
            setCurveInsync(sp_svg_read_pathv(getAttribute("d")));
            setCurveBeforeLPE(curve());
        }

        return true;
    }
    return false;
}

/* Reset the shape's curve to the "original_curve"
 *  This is very important for LPEs to work properly! (the bbox might be recalculated depending on the curve in shape)*/

bool SPShape::prepareShapeForLPE(Geom::PathVector &&c)
{
    auto const before = curveBeforeLPE();
    if (before && *before != c) {
        setCurveBeforeLPE(std::move(c));
        sp_lpe_item_update_patheffect(this, true, false);
        return true;
    }

    if (hasPathEffectOnClipOrMaskRecursive(this)) {
        if (!before && getRepr()->attribute("d")) {
            setCurveInsync(sp_svg_read_pathv(getAttribute("d")));
        }
        setCurveBeforeLPE(std::move(c));
        return true;
    }

    setCurveInsync(std::move(c));
    return false;
}

Geom::OptRect SPShape::bbox(Geom::Affine const &transform, SPItem::BBoxType bboxtype) const {
    // If the object is clipped, the update function that invalidates
    // the cache doesn't get called if the object is moved, so we need
    // to compare the transformations as well.

    if (bboxtype == SPItem::VISUAL_BBOX) {
        bbox_vis_cache =
            either_bbox(transform, bboxtype, bbox_vis_cache_is_valid, bbox_vis_cache, bbox_vis_cache_transform);
        if (bbox_vis_cache) {
            bbox_vis_cache_transform = transform;
            bbox_vis_cache_is_valid = true;
        }
        return bbox_vis_cache;
    } else {
        bbox_geom_cache =
            either_bbox(transform, bboxtype, bbox_geom_cache_is_valid, bbox_geom_cache, bbox_geom_cache_transform);
        if (bbox_geom_cache) {
            bbox_geom_cache_transform = transform;
            bbox_geom_cache_is_valid = true;
        }
        return bbox_geom_cache;
    }
}

Geom::OptRect SPShape::either_bbox(Geom::Affine const &transform, SPItem::BBoxType bboxtype, bool cache_is_valid,
                                   Geom::OptRect const &bbox_cache, Geom::Affine const &transform_cache) const
{
    // Return the cache if possible.
    if (cache_is_valid && bbox_cache) {
        auto delta = transform_cache.inverse() * transform;

        if (delta.isTranslation()) {
            // Don't re-adjust the cache if we haven't moved
            if (!delta.isNonzeroTranslation()) {
                return bbox_cache;
            }
            // delta is pure translation so it's safe to use it as is
            return *bbox_cache * delta;
        }
    }

    Geom::OptRect bbox;

    if (!_curve || _curve->empty()) {
    	return bbox;
    }

    bbox = bounds_exact_transformed(*_curve, transform);

    if (!bbox) {
    	return bbox;
    }

    if (bboxtype == SPItem::VISUAL_BBOX) {
        // convert the stroke to a path and calculate that path's geometric bbox

        if (!this->style->stroke.isNone() && !this->style->stroke_extensions.hairline) {
            Geom::PathVector *pathv = item_to_outline(this, true);  // calculate bbox_only

            if (pathv) {
                bbox |= bounds_exact_transformed(*pathv, transform);
                delete pathv;
            }
        }

        if (this->hasMarkers()) {
            for (auto const &[_, marker, tr] : this->get_markers()) {
                if (auto const marker_item = sp_item_first_item_child(marker)) {
                    bbox |= marker_item->visualBounds(marker_item->transform * marker->c2p * tr * transform);
                }
            }
        }
    }

    return bbox;
}

void SPShape::print(SPPrintContext* ctx) {
    if (!this->_curve) {
    	return;
    }

    Geom::PathVector const &pathv = *_curve;
    
    if (pathv.empty()) {
    	return;
    }

    /* fixme: Think (Lauris) */
	Geom::OptRect pbox, dbox, bbox;
    pbox = this->geometricBounds();
    bbox = this->desktopVisualBounds();
    dbox = Geom::Rect::from_xywh(Geom::Point(0,0), this->document->getDimensions());
    
    Geom::Affine const i2dt(this->i2dt_affine());

    // Copy the style for this printable item
    SPStyle *style = this->style;
    SPStyle *new_style = nullptr;

    if (ctx->context_item) {
        new_style = new SPStyle(document, this);
        new_style->merge(style);
        // Set style contexts for print here
        if (style->fill.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE)
            new_style->fill.overwrite(ctx->context_item->style->stroke.upcast());
        if (style->fill.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL)
            new_style->fill.overwrite(ctx->context_item->style->fill.upcast());
        if (style->stroke.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE)
            new_style->stroke.overwrite(ctx->context_item->style->stroke.upcast());
        if (style->stroke.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL)
            new_style->stroke.overwrite(ctx->context_item->style->fill.upcast());
        style = new_style;
    }

    if (!style->fill.isNone()) {
        ctx->fill (pathv, i2dt, style, pbox, dbox, bbox);
    }

    if (!style->stroke.isNone()) {
        ctx->stroke (pathv, i2dt, style, pbox, dbox, bbox);
    }

    if (new_style) {
        // Clean up temporary context style copy
        delete new_style;
    }

    for (auto const &[_, marker, tr] : this->get_markers()) {
        if (auto marker_item = sp_item_first_item_child(marker)) {
            auto const old_tr = marker_item->transform;
            marker_item->transform = old_tr * marker->c2p * tr;
            marker_item->invoke_print(ctx);
            marker_item->transform = old_tr;
        }
    }

    // Clear any context item used in the above markers.
    ctx->context_item = nullptr;
}

std::optional<Geom::PathVector> SPShape::documentExactBounds() const
{
    if (_curve) {
        return *_curve * i2doc_affine();
    }
    return {};
}

void SPShape::update_patheffect(bool write)
{
    if (!curveForEdit()) {
        set_shape();
    }
    if (curveForEdit()) {
        auto c_lpe = *curveForEdit();
        /* if a path has an lpeitem applied, then reset the curve to the _curve_before_lpe.
         * This is very important for LPEs to work properly! (the bbox might be recalculated depending on the curve in shape)*/
        setCurveInsync(&c_lpe);
       

        bool success = false;
        // avoid update lpe in each selection
        // must be set also to non effect items (satellites or parents)
        lpe_initialized = true; 
        if (hasPathEffect() && pathEffectsEnabled()) {
            success = this->performPathEffect(c_lpe, this);
            if (success) {
                if (!document->getRoot()->inkscape_version.isInsideRangeExclusive({0, 1}, {0, 92})) {
                    resetClipPathAndMaskLPE();
                }
                setCurveInsync(c_lpe);
                applyToClipPath(this);
                applyToMask(this);
            }
        } 
        if (write && success) {
            if (auto repr = getRepr()) {
                repr->setAttribute("d", sp_svg_write_path(c_lpe));
            }
        }
        if (success) {
            requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        }
    }
}

Inkscape::DrawingItem* SPShape::show(Inkscape::Drawing &drawing, unsigned int /*key*/, unsigned int /*flags*/) {
    // std::cout << "SPShape::show(): " << (getId()?getId():"null") << std::endl;
    Inkscape::DrawingShape *s = new Inkscape::DrawingShape(drawing);

    bool has_markers = this->hasMarkers();

    s->setPath(_curve);

    /* This stanza checks that an object's marker style agrees with
     * the marker objects it has allocated.  sp_shape_set_marker ensures
     * that the appropriate marker objects are present (or absent) to
     * match the style.
     */
    for (int i = 0; i < SP_MARKER_LOC_QTY; i++) {
        set_marker(i, style->marker_ptrs[i]->value());
    }

    if (has_markers) {
        /* provide key and dimension the marker views */
        SPItem::ensure_key(s);
        for (int i = 0; i < SP_MARKER_LOC_QTY; i++) {
            if (_marker[i]) {
                sp_marker_show_dimension(_marker[i], s->key() + ITEM_KEY_MARKERS + i, numberOfMarkers(i));
            }
        }

        /* Update marker views */
        sp_shape_update_marker_view(this, s);

        this->context_style = this->style;
        s->setStyle(this->style, this->context_style);
        s->setChildrenStyle(this->context_style); // Resolve 'context-xxx' in children.
    } else if (this->parent) {
        this->context_style = this->parent->context_style;
        s->setStyle(this->style, this->context_style);
    }

    // apply 'shape-rendering' presentation attribute
    Inkscape::propagate_antialias(style->shape_rendering.computed, *s);

    return s;
}

/**
 * Sets style, path, and paintbox.  Updates marker views, including dimensions.
 */
void SPShape::hide(unsigned key)
{
    for (int i = 0; i < SP_MARKER_LOC_QTY; ++i) {
        if (_marker[i]) {
            for (auto &v : views) {
                if (key == v.key) {
                    sp_marker_hide(_marker[i], v.drawingitem->key() + ITEM_KEY_MARKERS + i);
                }
            }
        }
    }

    //SPLPEItem::onHide(key);
}

/**
* \param shape Shape.
* \return TRUE if the shape has any markers, or FALSE if not.
*/
int SPShape::hasMarkers() const
{
    /* Note, we're ignoring 'marker' settings, which technically should apply for
       all three settings.  This should be fixed later such that if 'marker' is
       specified, then all three should appear. */

    // Ignore markers for objects which are inside markers themselves.
    for (SPObject *parent = this->parent; parent != nullptr; parent = parent->parent) {
      if (is<SPMarker>(parent)) {
        return 0;
      }
    }

    return (
        this->_curve &&
        (this->_marker[SP_MARKER_LOC] ||
         this->_marker[SP_MARKER_LOC_START] ||
         this->_marker[SP_MARKER_LOC_MID] ||
         this->_marker[SP_MARKER_LOC_END])
        );
}

/**
* \param shape Shape.
* \param type Marker type (e.g. SP_MARKER_LOC_START)
* \return Number of markers that the shape has of this type.
*/
int SPShape::numberOfMarkers(int type) const
{
    Geom::PathVector const &pathv = *_curve;

    if (pathv.empty()) {
        return 0;
    }
    switch (type) {

        case SP_MARKER_LOC:
        {
            if ( this->_marker[SP_MARKER_LOC] ) {
                guint n = 0;
                for(const auto & path_it : pathv) {
                    n += path_it.size_default() + 1;
                }
                return n;
            } else {
                return 0;
            }
        }
        case SP_MARKER_LOC_START:
            // there is only a start marker on the first path of a pathvector
            return this->_marker[SP_MARKER_LOC_START] ? 1 : 0;

        case SP_MARKER_LOC_MID:
        {
            if ( this->_marker[SP_MARKER_LOC_MID] ) {
                guint n = 0;
                for(const auto & path_it : pathv) {
                    n += path_it.size_default() + 1;
                }
                n = (n > 1) ? (n - 2) : 0; // Minus the start and end marker, but never negative.
                                           // A path or polyline may have only one point.
                return n;
            } else {
                return 0;
            }
        }

        case SP_MARKER_LOC_END:
        {
            // there is only an end marker on the last path of a pathvector
            return this->_marker[SP_MARKER_LOC_END] ? 1 : 0;
        }

        default:
            return 0;
    }
}

/**
 * Checks if the given marker is used in the shape, and if so, it
 * releases it by calling sp_marker_hide.  Also detaches signals
 * and unrefs the marker from the shape.
 */
static void
sp_shape_marker_release(SPObject *marker, SPShape *shape)
{
    auto item = shape;
    g_return_if_fail(item != nullptr);

    for (int i = 0; i < SP_MARKER_LOC_QTY; i++) {
        if (marker == shape->_marker[i]) {
            /* Hide marker */
            for (auto &v : item->views) {
                sp_marker_hide(shape->_marker[i], v.drawingitem->key() + ITEM_KEY_MARKERS + i);
            }
            /* Detach marker */
            shape->_release_connect[i].disconnect();
            shape->_modified_connect[i].disconnect();
            shape->_marker[i]->unhrefObject(item);
            shape->_marker[i] = nullptr;
        }
    }
}

/**
 * No-op.  Exists for handling 'modified' messages
 */
static void sp_shape_marker_modified (SPObject* marker, guint flags, SPItem* item) {
    if ((flags & SP_OBJECT_MODIFIED_FLAG) && item && marker) {
        // changing marker can impact object's visual bounding box, so request update on this object itself
        item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    }
}

/**
 * Adds a new marker to shape object at the location indicated by key.  value
 * must be a valid URI reference resolvable from the shape object (i.e., present
 * in the document <defs>).  If the shape object already has a marker
 * registered at the given position, it is removed first.  Then the
 * new marker is hrefed and its signals connected.
 */
void SPShape::set_marker(unsigned key, char const *value)
{
    if (key > SP_MARKER_LOC_END) {
        return;
    }

    auto mrk = sp_css_uri_reference_resolve(document, value);
    auto marker = cast<SPMarker>(mrk);

    if (marker != _marker[key]) {
        if (_marker[key]) {
            /* Detach marker */
            _release_connect[key].disconnect();
            _modified_connect[key].disconnect();

            /* Hide marker */
            for (auto &v : views) {
                sp_marker_hide(_marker[key], v.drawingitem->key() + ITEM_KEY_MARKERS + key);
            }

            /* Unref marker */
            _marker[key]->unhrefObject(this);
            _marker[key] = nullptr;
        }
        if (marker) {
            _marker[key] = marker;
            _marker[key]->hrefObject(this);
            _release_connect[key] = marker->connectRelease(sigc::bind<1>(sigc::ptr_fun(&sp_shape_marker_release), this));
            _modified_connect[key] = marker->connectModified(sigc::bind<2>(sigc::ptr_fun(&sp_shape_marker_modified), this));
        }
    }
}

// CPPIFY: make pure virtual
void SPShape::set_shape() {
	//throw;
}

/* Shape section */

/**
 * Adds a curve to the shape.
 * Any existing curve in the shape will be unreferenced first.
 * This routine also triggers a request to update the display.
 */
void SPShape::setCurve(Geom::PathVector new_curve)
{
    _curve = std::make_shared<Geom::PathVector>(std::move(new_curve));
    if (document) {
        requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    }
}
void SPShape::setCurve(Geom::PathVector const *new_curve)
{
    if (new_curve) {
        setCurve(*new_curve);
    } else {
        _curve.reset();
    }
}

/**
 * Sets _curve_before_lpe to a copy of `new_curve`
 */
void SPShape::setCurveBeforeLPE(Geom::PathVector new_curve)
{
    _curve_before_lpe = std::move(new_curve);
}
void SPShape::setCurveBeforeLPE(Geom::PathVector const *new_curve)
{
    if (new_curve) {
        setCurveBeforeLPE(*new_curve);
    } else {
        _curve_before_lpe.reset();
    }
}

/**
 * Same as setCurve() but without updating the display
 */
void SPShape::setCurveInsync(Geom::PathVector new_curve)
{
    _curve = std::make_shared<Geom::PathVector>(std::move(new_curve));
}
void SPShape::setCurveInsync(Geom::PathVector const *new_curve)
{
    if (new_curve) {
        setCurveInsync(*new_curve);
    } else {
        _curve.reset();
    }
}

/**
 * Return a borrowed pointer to the curve (if any exists) or NULL if there is no curve
 */
Geom::PathVector const *SPShape::curve() const
{
    return _curve.get();
}

/**
 * Return a borrowed pointer of the curve *before* LPE (if any exists) or NULL if there is no curve
 */
Geom::PathVector const *SPShape::curveBeforeLPE() const
{
    return _curve_before_lpe ? &*_curve_before_lpe : nullptr;
}

/**
 * Return a borrowed pointer of the curve for edit
 */
Geom::PathVector const *SPShape::curveForEdit() const
{
    return _curve_before_lpe ? &*_curve_before_lpe : curve();
}

void SPShape::snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const
{
    if (this->_curve == nullptr) {
        return;
    }

    Geom::PathVector const &pathv = *_curve;

    if (pathv.empty()) {
        return;
    }

    Geom::Affine const i2dt (this->i2dt_affine ());

    if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_OBJECT_MIDPOINT)) {
        Geom::OptRect bbox = this->desktopVisualBounds();

        if (bbox) {
            p.emplace_back(bbox->midpoint(), Inkscape::SNAPSOURCE_OBJECT_MIDPOINT, Inkscape::SNAPTARGET_OBJECT_MIDPOINT);
        }
    }

    for(const auto & path_it : pathv) {
        if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_NODE_CUSP)) {
            // Add the first point of the path
            p.emplace_back(path_it.initialPoint() * i2dt, Inkscape::SNAPSOURCE_NODE_CUSP, Inkscape::SNAPTARGET_NODE_CUSP);
        }

        Geom::Path::const_iterator curve_it1 = path_it.begin();      // incoming curve
        Geom::Path::const_iterator curve_it2 = ++(path_it.begin());  // outgoing curve

        while (curve_it1 != path_it.end_default())
        {
            // For each path: consider midpoints of line segments for snapping
            if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_LINE_MIDPOINT)) {
                if (Geom::LineSegment const* line_segment = dynamic_cast<Geom::LineSegment const*>(&(*curve_it1))) {
                    p.emplace_back(Geom::middle_point(*line_segment) * i2dt, Inkscape::SNAPSOURCE_LINE_MIDPOINT, Inkscape::SNAPTARGET_LINE_MIDPOINT);
                }
            }

            if (curve_it2 == path_it.end_default()) { // Test will only pass for the last iteration of the while loop
                if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_NODE_CUSP) && !path_it.closed()) {
                    // Add the last point of the path, but only for open paths
                    // (for closed paths the first and last point will coincide)
                    p.emplace_back((*curve_it1).finalPoint() * i2dt, Inkscape::SNAPSOURCE_NODE_CUSP, Inkscape::SNAPTARGET_NODE_CUSP);
                }
            } else {
                /* Test whether to add the node between curve_it1 and curve_it2.
                 * Loop to end_default (so only iterating through the stroked part); */

                Geom::NodeType nodetype = Geom::get_nodetype(*curve_it1, *curve_it2);

                bool c1 = snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_NODE_CUSP) && (nodetype == Geom::NODE_CUSP || nodetype == Geom::NODE_NONE);
                bool c2 = snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_NODE_SMOOTH) && (nodetype == Geom::NODE_SMOOTH || nodetype == Geom::NODE_SYMM);

                if (c1 || c2) {
                    Inkscape::SnapSourceType sst;
                    Inkscape::SnapTargetType stt;

                    switch (nodetype) {
                    case Geom::NODE_CUSP:
                        sst = Inkscape::SNAPSOURCE_NODE_CUSP;
                        stt = Inkscape::SNAPTARGET_NODE_CUSP;
                        break;
                    case Geom::NODE_SMOOTH:
                    case Geom::NODE_SYMM:
                        sst = Inkscape::SNAPSOURCE_NODE_SMOOTH;
                        stt = Inkscape::SNAPTARGET_NODE_SMOOTH;
                        break;
                    default:
                        sst = Inkscape::SNAPSOURCE_UNDEFINED;
                        stt = Inkscape::SNAPTARGET_UNDEFINED;
                        break;
                    }

                    p.emplace_back(curve_it1->finalPoint() * i2dt, sst, stt);
                }
            }

            ++curve_it1;
            ++curve_it2;
        }

        // Find the internal intersections of each path and consider these for snapping
        // (using "Method 1" as described in Inkscape::ObjectSnapper::_collectNodes())
        if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_PATH_INTERSECTION) || snapprefs->isSourceSnappable(Inkscape::SNAPSOURCE_PATH_INTERSECTION)) {
            Geom::Crossings cs;

            try {
                cs = self_crossings(path_it); // This can be slow!

                if (!cs.empty()) { // There might be multiple intersections...
                    for (const auto & c : cs) {
                        Geom::Point p_ix = path_it.pointAt(c.ta);
                        p.emplace_back(p_ix * i2dt, Inkscape::SNAPSOURCE_PATH_INTERSECTION, Inkscape::SNAPTARGET_PATH_INTERSECTION);
                    }
                }
            } catch (Geom::RangeError &e) {
                // do nothing
                // The exception could be Geom::InfiniteSolutions: then no snappoints should be added
            }

        }
    }
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
