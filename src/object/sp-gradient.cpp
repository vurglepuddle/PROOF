// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * SPGradient, SPStop, SPLinearGradient, SPRadialGradient,
 * SPMeshGradient, SPMeshRow, SPMeshPatch
 */
/*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jasper van de Gronde <th.v.d.gronde@hccnet.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Tavmjong Bah <tavmjong@free.fr>
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2009 Jasper van de Gronde
 * Copyright (C) 2011 Tavmjong Bah
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 */

#define noSP_GRADIENT_VERBOSE

#include "sp-gradient.h"
#include "gradient-rendering.h"

#include <cstring>
#include <cairo.h>
#include <sigc++/adaptors/bind.h>
#include <sigc++/functors/ptr_fun.h>
#include <2geom/transforms.h>

#include "attributes.h"
#include "bad-uri-exception.h"
#include "colors/gradient-averager.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "gradient-chemistry.h"
#include "object/uri.h"
#include "sp-gradient-reference.h"
#include "sp-linear-gradient.h"
#include "sp-mesh-gradient.h"
#include "sp-mesh-patch.h"
#include "sp-mesh-row.h"
#include "sp-radial-gradient.h"
#include "sp-stop.h"
#include "svg/css-ostringstream.h"
#include "svg/svg.h"
#include "xml/href-attribute-helper.h"

using namespace Inkscape::Colors;

bool SPGradient::hasStops() const
{
    return has_stops;
}

bool SPGradient::hasPatches() const
{
    return has_patches;
}

bool SPGradient::isUnitsSet() const
{
    return units_set;
}

SPGradientUnits SPGradient::getUnits() const
{
    return units;
}

bool SPGradient::isSpreadSet() const
{
    return spread_set;
}

SPGradientSpread SPGradient::getSpread() const
{
    return spread;
}

void SPGradient::setSwatch( bool swatch )
{
    if ( swatch != isSwatch() ) {
        this->swatch = swatch; // to make isSolid() work, this happens first
        gchar const* paintVal = swatch ? (isSolid() ? "solid" : "gradient") : nullptr;
        setAttribute( "inkscape:swatch", paintVal);

        requestModified( SP_OBJECT_MODIFIED_FLAG );
    }
}

void SPGradient::setPinned(bool pinned)
{
    if (pinned != isPinned()) {
        setAttribute("inkscape:pinned", pinned ? "true" : "false");
        requestModified(SP_OBJECT_MODIFIED_FLAG);
    }
}


/**
 * return true if this gradient is "equivalent" to that gradient.
 * Equivalent meaning they have the same stop count, same stop colors and same stop opacity
 * @param that - A gradient to compare this to
 */
bool SPGradient::isEquivalent(SPGradient *that)
{
    //TODO Make this work for mesh gradients

    bool status = false;

    while(true){ // not really a loop, used to avoid deep nesting or multiple exit points from function
        if (this->getStopCount() != that->getStopCount()) { break; }
        if (this->hasStops() != that->hasStops()) { break; }
        if (!this->getVector() || !that->getVector()) { break; }
        if (this->isSwatch() != that->isSwatch()) {  break; }
        if ( this->isSwatch() ){
           // drop down to check stops.
        }
        else if (
            (is<SPLinearGradient>(this) && is<SPLinearGradient>(that)) ||
            (is<SPRadialGradient>(this) && is<SPRadialGradient>(that)) ||
            (is<SPMeshGradient>(this)   && is<SPMeshGradient>(that))) {
            if(!this->isAligned(that))break;
        }
        else { break; }  // this should never happen, some unhandled type of gradient

        SPStop *as = this->getVector()->getFirstStop();
        SPStop *bs = that->getVector()->getFirstStop();

        bool effective = true;
        while (effective && (as && bs)) {
            if (!as->getColor().isClose(bs->getColor(), 0.001) || as->offset != bs->offset) {
                effective = false;
                break;
            } else {
                as = as->getNextStop();
                bs = bs->getNextStop();
            }
        }
        if (!effective) break;

        status = true;
        break;
    }
    return status;
}

/**
 * return true if this gradient is "aligned" to that gradient.
 * Aligned means that they have exactly the same coordinates and transform.
 * @param that - A gradient to compare this to
 */
bool SPGradient::isAligned(SPGradient *that)
{
    bool status = false;

    /*  Some gradients have coordinates/other values specified, some don't.
            yes/yes check the coordinates/other values
            no/no   aligned (because both have all default values)
            yes/no  not aligned
            no/yes  not aligned
        It is NOT safe to just compare the computed values because if that field has
        not been set the computed value could be full of garbage.

        In theory the yes/no and no/yes cases could be aligned if the specified value
        matches the default value.
        */

    while (true) { // not really a loop, used to avoid deep nesting or multiple exit points from function
        if (this->gradientTransform_set != that->gradientTransform_set) {
            break;
        }
        if (this->gradientTransform_set && this->gradientTransform != that->gradientTransform) {
            break;
        }
        if (is<SPLinearGradient>(this) && is<SPLinearGradient>(that)) {
            auto sg = cast<SPLinearGradient>(this);
            auto tg = cast<SPLinearGradient>(that);

            if (sg->x1._set != tg->x1._set) {
                break;
            }
            if (sg->y1._set != tg->y1._set) {
                break;
            }
            if (sg->x2._set != tg->x2._set) {
                break;
            }
            if (sg->y2._set != tg->y2._set) {
                break;
            }
            if (sg->x1._set && sg->y1._set && sg->x2._set && sg->y2._set) {
                if (sg->x1.computed != tg->x1.computed || sg->y1.computed != tg->y1.computed ||
                    sg->x2.computed != tg->x2.computed || sg->y2.computed != tg->y2.computed) {
                    break;
                }
            } else if (sg->x1._set || sg->y1._set || sg->x2._set || sg->y2._set) {
                // some mix of set and not set
                break;
            }
            // none set? assume aligned and fall through
        } else if (is<SPRadialGradient>(this) && is<SPLinearGradient>(that)) {
            auto sg = cast<SPRadialGradient>(this);
            auto tg = cast<SPRadialGradient>(that);

            if (sg->cx._set != tg->cx._set) {
                break;
            }
            if (sg->cy._set != tg->cy._set) {
                break;
            }
            if (sg->r._set != tg->r._set) {
                break;
            }
            if (sg->fx._set != tg->fx._set) {
                break;
            }
            if (sg->fy._set != tg->fy._set) {
                break;
            }
            if (sg->cx._set && sg->cy._set && sg->fx._set && sg->fy._set && sg->r._set) {
                if (sg->cx.computed != tg->cx.computed || sg->cy.computed != tg->cy.computed ||
                    sg->r.computed != tg->r.computed || sg->fx.computed != tg->fx.computed ||
                    sg->fy.computed != tg->fy.computed) {
                    break;
                }
            } else if (sg->cx._set || sg->cy._set || sg->fx._set || sg->fy._set || sg->r._set) {
                // some mix of set and not set
                break;
            }
            // none set? assume aligned and fall through
        } else if (is<SPMeshGradient>(this) && is<SPMeshGradient>(that)) {
            auto sg = cast<SPMeshGradient>(this);
            auto tg = cast<SPMeshGradient>(that);

            if (sg->x._set != !tg->x._set) {
                break;
            }
            if (sg->y._set != !tg->y._set) {
                break;
            }
            if (sg->x._set && sg->y._set) {
                if (sg->x.computed != tg->x.computed || sg->y.computed != tg->y.computed) {
                    break;
                }
            } else if (sg->x._set || sg->y._set) {
                // some mix of set and not set
                break;
            }
            // none set? assume aligned and fall through
        } else {
            break;
        }
        status = true;
        break;
    }
    return status;
}

/*
 * Gradient
 */
SPGradient::SPGradient()
    : SPPaintServer()
    , units()
    , spread()
    , ref(nullptr)
    , state(2)
    , vector()
    , mesh()
    , array()
    , array_smoothed()
{
    this->ref = new SPGradientReference(this);
    this->ref->changedSignal().connect(sigc::bind(sigc::ptr_fun(SPGradient::gradientRefChanged), this));

    /** \todo
     * Fixme: reprs being rearranged (e.g. via the XML editor)
     * may require us to clear the state.
     */
    this->state = SP_GRADIENT_STATE_UNKNOWN;

    this->units = SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX;
    this->units_set = FALSE;

    this->gradientTransform = Geom::identity();
    this->gradientTransform_set = FALSE;

    this->spread = SP_GRADIENT_SPREAD_PAD;
    this->spread_set = FALSE;

    this->has_stops = FALSE;
    this->has_patches = FALSE;

    this->vector.built = false;
    this->vector.stops.clear();
}

SPGradient::~SPGradient() = default;

/**
 * Virtual build: set gradient attributes from its associated repr.
 */
void SPGradient::build(SPDocument *document, Inkscape::XML::Node *repr)
{
    // Work-around in case a swatch had been marked for immediate collection:
    if ( repr->attribute("inkscape:swatch") && repr->attribute("inkscape:collect") ) {
        repr->removeAttribute("inkscape:collect");
    }

    this->readAttr(SPAttr::STYLE);

    SPPaintServer::build(document, repr);

    for (auto& ochild: children) {
        if (is<SPStop>(&ochild)) {
            this->has_stops = TRUE;
            break;
        }
        if (is<SPMeshrow>(&ochild)) {
            for (auto& ochild2: ochild.children) {
                if (is<SPMeshpatch>(&ochild2)) {
                    this->has_patches = TRUE;
                    break;
                }
            }
            if (this->has_patches == TRUE) {
                break;
            }
        }
    }

    this->readAttr(SPAttr::GRADIENTUNITS);
    this->readAttr(SPAttr::GRADIENTTRANSFORM);
    this->readAttr(SPAttr::SPREADMETHOD);
    this->readAttr(SPAttr::XLINK_HREF);
    this->readAttr(SPAttr::INKSCAPE_SWATCH);
    this->readAttr(SPAttr::INKSCAPE_PINNED);

    // Register ourselves
    document->addResource("gradient", this);
}

/**
 * Virtual release of SPGradient members before destruction.
 */
void SPGradient::release()
{

#ifdef SP_GRADIENT_VERBOSE
    g_print("Releasing this %s\n", this->getId());
#endif

    if (this->document) {
        // Unregister ourselves
        this->document->removeResource("gradient", this);
    }

    if (this->ref) {
        this->modified_connection.disconnect();
        this->ref->detach();
        delete this->ref;
        this->ref = nullptr;
    }

    SPPaintServer::release();
}

/**
 * Set gradient attribute to value.
 */
void SPGradient::set(SPAttr key, gchar const *value)
{
#ifdef OBJECT_TRACE
    std::stringstream temp;
    temp << "SPGradient::set: " << sp_attribute_name(key)  << " " << (value?value:"null");
    objectTrace( temp.str() );
#endif

    switch (key) {
        case SPAttr::GRADIENTUNITS:
            if (value) {
                if (!strcmp(value, "userSpaceOnUse")) {
                    this->units = SP_GRADIENT_UNITS_USERSPACEONUSE;
                } else {
                    this->units = SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX;
                }

                this->units_set = TRUE;
            } else {
                this->units = SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX;
                this->units_set = FALSE;
            }

            this->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::GRADIENTTRANSFORM: {
            Geom::Affine t;
            if (value && sp_svg_transform_read(value, &t)) {
                this->gradientTransform = t;
                this->gradientTransform_set = TRUE;
            } else {
                this->gradientTransform = Geom::identity();
                this->gradientTransform_set = FALSE;
            }

            this->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
        }
        case SPAttr::SPREADMETHOD:
            if (value) {
                if (!strcmp(value, "reflect")) {
                    this->spread = SP_GRADIENT_SPREAD_REFLECT;
                } else if (!strcmp(value, "repeat")) {
                    this->spread = SP_GRADIENT_SPREAD_REPEAT;
                } else {
                    this->spread = SP_GRADIENT_SPREAD_PAD;
                }

                this->spread_set = TRUE;
            } else {
                this->spread_set = FALSE;
            }

            this->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::XLINK_HREF:
            if (value) {
                try {
                    this->ref->attach(Inkscape::URI(value));
                } catch (Inkscape::BadURIException &e) {
                    g_warning("%s", e.what());
                    this->ref->detach();
                }
            } else {
                this->ref->detach();
            }
            break;

        case SPAttr::INKSCAPE_PINNED:
        {
            if (value) {
                this->_pinned = !strcmp(value, "true");
            }
            break;
        }
        case SPAttr::INKSCAPE_SWATCH:
        {
            bool newVal = (value != nullptr);
            bool modified = false;

            if (newVal != this->swatch) {
                this->swatch = newVal;
                modified = true;
            }

            if (newVal) {
                // Might need to flip solid/gradient
                Glib::ustring paintVal = ( this->hasStops() && (this->getStopCount() <= 1) ) ? "solid" : "gradient";

                if ( paintVal != value ) {
                    this->setAttribute( "inkscape:swatch", paintVal);
                    modified = true;
                }
            }

            if (modified) {
                this->requestModified(SP_OBJECT_MODIFIED_FLAG);
            }
        }
            break;
        default:
            SPPaintServer::set(key, value);
            break;
    }

#ifdef OBJECT_TRACE
    objectTrace( "SPGradient::set", false );
#endif
}

/**
 * Gets called when the gradient is (re)attached to another gradient.
 */
void SPGradient::gradientRefChanged(SPObject *old_ref, SPObject *ref, SPGradient *gr)
{
    if (old_ref) {
        gr->modified_connection.disconnect();
    }
    if ( is<SPGradient>(ref)
         && ref != gr )
    {
        gr->modified_connection = ref->connectModified(sigc::bind<2>(sigc::ptr_fun(&SPGradient::gradientRefModified), gr));
    }

    // Per SVG, all unset attributes must be inherited from linked gradient.
    // So, as we're now (re)linked, we assign linkee's values to this gradient if they are not yet set -
    // but without setting the _set flags.
    // FIXME: do the same for gradientTransform too
    if (!gr->units_set) {
        gr->units = gr->fetchUnits();
    }
    if (!gr->spread_set) {
        gr->spread = gr->fetchSpread();
    }

    /// \todo Fixme: what should the flags (second) argument be? */
    gradientRefModified(ref, 0, gr);
}

/**
 * Callback for child_added event.
 */
void SPGradient::child_added(Inkscape::XML::Node *child, Inkscape::XML::Node *ref)
{
    this->invalidateVector();

    SPPaintServer::child_added(child, ref);

    SPObject *ochild = this->get_child_by_repr(child);
    if ( ochild && is<SPStop>(ochild) ) {
        this->has_stops = TRUE;
        if ( this->getStopCount() > 1 ) {
            gchar const * attr = this->getAttribute("inkscape:swatch");
            if ( attr && strcmp(attr, "gradient") ) {
               this->setAttribute( "inkscape:swatch", "gradient" );
            }
        }
    }
    if ( ochild && is<SPMeshrow>(ochild) ) {
        this->has_patches = TRUE;
    }

    /// \todo Fixme: should we schedule "modified" here?
    this->requestModified(SP_OBJECT_MODIFIED_FLAG);
}

/**
 * Callback for remove_child event.
 */
void SPGradient::remove_child(Inkscape::XML::Node *child)
{
    this->invalidateVector();

    SPPaintServer::remove_child(child);

    this->has_stops = FALSE;
    this->has_patches = FALSE;
    for (auto& ochild: children) {
        if (is<SPStop>(&ochild)) {
            this->has_stops = TRUE;
            break;
        }
        if (is<SPMeshrow>(&ochild)) {
            for (auto& ochild2: ochild.children) {
                if (is<SPMeshpatch>(&ochild2)) {
                    this->has_patches = TRUE;
                    break;
                }
            }
            if (this->has_patches == TRUE) {
                break;
            }
        }
    }

    if ( this->getStopCount() <= 1 ) {
        gchar const * attr = this->getAttribute("inkscape:swatch");

        if ( attr && strcmp(attr, "solid") ) {
            this->setAttribute( "inkscape:swatch", "solid" );
        }
    }

    /* Fixme: should we schedule "modified" here? */
    this->requestModified(SP_OBJECT_MODIFIED_FLAG);
}

/**
 * Callback for modified event.
 */
void SPGradient::modified(guint flags)
{
#ifdef OBJECT_TRACE
    objectTrace( "SPGradient::modified" );
#endif
    if (flags & SP_OBJECT_CHILD_MODIFIED_FLAG) {
        if (is<SPMeshGradient>(this)) {
            this->invalidateArray();
        } else {
            this->invalidateVector();
        }
    }

    if (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) {
        if (is<SPMeshGradient>(this)) {
            this->ensureArray();
        } else {
            this->ensureVector();
        }
    }

    if (flags & SP_OBJECT_MODIFIED_FLAG) flags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    flags &= SP_OBJECT_MODIFIED_CASCADE;

    // FIXME: climb up the ladder of hrefs
    std::vector<SPObject *> l;
    for (auto& child: children) {
        sp_object_ref(&child);
        l.push_back(&child);
    }

    for (auto child:l) {
        if ((flags & SP_OBJECT_FLAGS_ALL) || (child->mflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
            child->emitModified(flags);
        }
        sp_object_unref(child);
    }

#ifdef OBJECT_TRACE
    objectTrace( "SPGradient::modified", false );
#endif
}

SPStop* SPGradient::getFirstStop()
{
    SPStop* first = nullptr;
    for (auto& ochild: children) {
        if (is<SPStop>(&ochild)) {
            first = cast<SPStop>(&ochild);
            break;
        }
    }
    return first;
}

int SPGradient::getStopCount() const
{
    int count = 0;
    // fixed off-by one count
    SPStop *stop = const_cast<SPGradient*>(this)->getFirstStop();
    while (stop) {
       count++;
       stop = stop->getNextStop();
    }

    return count;
}

/**
 * Write gradient attributes to repr.
 */
Inkscape::XML::Node *SPGradient::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags)
{
#ifdef OBJECT_TRACE
    objectTrace( "SPGradient::write" );
#endif

    SPPaintServer::write(xml_doc, repr, flags);

    if (flags & SP_OBJECT_WRITE_BUILD) {
        std::vector<Inkscape::XML::Node *> l;

        for (auto& child: children) {
            Inkscape::XML::Node *crepr = child.updateRepr(xml_doc, nullptr, flags);

            if (crepr) {
                l.push_back(crepr);
            }
        }

        for (auto i=l.rbegin();i!=l.rend();++i) {
            repr->addChild(*i, nullptr);
            Inkscape::GC::release(*i);
        }
    }

    if (this->ref->getURI()) {
        auto uri_string = this->ref->getURI()->str();
        auto href_key = Inkscape::getHrefAttribute(*repr).first;
        repr->setAttributeOrRemoveIfEmpty(href_key, uri_string);
    }

    if ((flags & SP_OBJECT_WRITE_ALL) || this->units_set) {
        switch (this->units) {
            case SP_GRADIENT_UNITS_USERSPACEONUSE:
                repr->setAttribute("gradientUnits", "userSpaceOnUse");
                break;
            default:
                repr->setAttribute("gradientUnits", "objectBoundingBox");
                break;
        }
    }

    if ((flags & SP_OBJECT_WRITE_ALL) || this->gradientTransform_set) {
        auto c = sp_svg_transform_write(this->gradientTransform);
        repr->setAttributeOrRemoveIfEmpty("gradientTransform", c);
    }

    if ((flags & SP_OBJECT_WRITE_ALL) || this->spread_set) {
        /* FIXME: Ensure that this->spread is the inherited value
         * if !this->spread_set.  Not currently happening: see SPGradient::modified.
         */
        switch (this->spread) {
            case SP_GRADIENT_SPREAD_REFLECT:
                repr->setAttribute("spreadMethod", "reflect");
                break;
            case SP_GRADIENT_SPREAD_REPEAT:
                repr->setAttribute("spreadMethod", "repeat");
                break;
            default:
                repr->setAttribute("spreadMethod", "pad");
                break;
        }
    }

    if ( (flags & SP_OBJECT_WRITE_EXT) && this->isSwatch() ) {
        if ( this->isSolid() ) {
            repr->setAttribute( "inkscape:swatch", "solid" );
        } else {
            repr->setAttribute( "inkscape:swatch", "gradient" );
        }
    } else {
        repr->removeAttribute("inkscape:swatch");
    }

#ifdef OBJECT_TRACE
    objectTrace( "SPGradient::write", false );
#endif
    return repr;
}

/**
 * Forces the vector to be built, if not present (i.e., changed).
 *
 * \pre is<SPGradient>(gradient).
 */
void SPGradient::ensureVector()
{
    if ( !vector.built ) {
        rebuildVector();
    }
}

SPGradientVector const *SPGradient::getGradientVector() const
{
    if (!vector.built) {
        rebuildVector();
    }
    // NOTE: This is to maintain previous logic when geometry was outside of the vector
    // and so wasn't included in the cache invalidation. Move to rebuildVector() if pos.
    vector.geom = getGradientGeom();
    return &vector;
}

/**
 * Forces the array to be built, if not present (i.e., changed).
 *
 * \pre is<SPGradient>(gradient).
 */
void SPGradient::ensureArray() const
{
    //std::cout << "SPGradient::ensureArray()" << std::endl;
    if ( !array.built ) {
        rebuildArray();
    }
}

SPGradientMesh const *SPGradient::getGradientMesh() const
{
    if (!mesh.built) {
        ensureArray();
        mesh.patches = getGradientPatches();
        mesh.built = true;
    }
    return &mesh;
}

/**
 * Set units property of gradient and emit modified.
 */
void SPGradient::setUnits(SPGradientUnits units)
{
    if (units != this->units) {
        this->units = units;
        units_set = TRUE;
        requestModified(SP_OBJECT_MODIFIED_FLAG);
    }
}

/**
 * Set spread property of gradient and emit modified.
 */
void SPGradient::setSpread(SPGradientSpread spread)
{
    if (spread != this->spread) {
        this->spread = spread;
        spread_set = TRUE;
        requestModified(SP_OBJECT_MODIFIED_FLAG);
    }
}

/**
 * Returns the first of {src, src-\>ref-\>getObject(),
 * src-\>ref-\>getObject()-\>ref-\>getObject(),...}
 * for which \a match is true, or NULL if none found.
 *
 * The raison d'être of this routine is that it correctly handles cycles in the href chain (e.g., if
 * a gradient gives itself as its href, or if each of two gradients gives the other as its href).
 *
 * \pre is<SPGradient>(src).
 */
static SPGradient *
chase_hrefs(SPGradient *const src, bool (*match)(SPGradient const *))
{
    g_return_val_if_fail(src, NULL);

    /* Use a pair of pointers for detecting loops: p1 advances half as fast as p2.  If there is a
       loop, then once p1 has entered the loop, we'll detect it the next time the distance between
       p1 and p2 is a multiple of the loop size. */
    SPGradient *p1 = src, *p2 = src;
    bool do1 = false;
    for (;;) {
        if (match(p2)) {
            return p2;
        }

        p2 = p2->ref->getObject();
        if (!p2) {
            return p2;
        }
        if (do1) {
            p1 = p1->ref->getObject();
        }
        do1 = !do1;

        if ( p2 == p1 ) {
            /* We've been here before, so return NULL to indicate that no matching gradient found
             * in the chain. */
            return nullptr;
        }
    }
}

/**
 * True if gradient has stops.
 */
static bool has_stopsFN(SPGradient const *gr)
{
    return gr->hasStops();
}

/**
 * True if gradient has patches (i.e. a mesh).
 */
static bool has_patchesFN(SPGradient const *gr)
{
    return gr->hasPatches();
}

/**
 * True if gradient has spread set.
 */
static bool has_spread_set(SPGradient const *gr)
{
    return gr->isSpreadSet();
}

/**
 * True if gradient has units set.
 */
static bool
has_units_set(SPGradient const *gr)
{
    return gr->isUnitsSet();
}


SPGradient *SPGradient::getVector(bool force_vector)
{
    SPGradient * src = chase_hrefs(this, has_stopsFN);
    if (src == nullptr) {
        src = this;
    }

    if (force_vector) {
        src = sp_gradient_ensure_vector_normalized(src);
    }
    return src;
}

SPGradient *SPGradient::getArray(bool force_vector) const
{
    SPGradient const * src = chase_hrefs(const_cast<SPGradient *>(this), has_patchesFN);
    if (src == nullptr) {
        src = this;
    }
    return const_cast<SPGradient *>(src);
}

/**
 * Returns the effective spread of given gradient (climbing up the refs chain if needed).
 *
 * \pre is<SPGradient>(gradient).
 */
SPGradientSpread SPGradient::fetchSpread() const
{
    // TODO: chase_hrefs should allow for const
    SPGradient const *src = chase_hrefs(const_cast<SPGradient *>(this), has_spread_set);
    return ( src
             ? src->spread
             : SP_GRADIENT_SPREAD_PAD ); // pad is the default
}

/**
 * Returns the effective units of given gradient (climbing up the refs chain if needed).
 *
 * \pre is<SPGradient>(gradient).
 */
SPGradientUnits SPGradient::fetchUnits()
{
    SPGradient const *src = chase_hrefs(this, has_units_set);
    return ( src
             ? src->units
             : SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX ); // bbox is the default
}


/**
 * Clears the gradient's svg:stop children from its repr.
 */
void
SPGradient::repr_clear_vector()
{
    Inkscape::XML::Node *repr = getRepr();

    /* Collect stops from original repr */
    std::vector<Inkscape::XML::Node *> l;
    for (Inkscape::XML::Node *child = repr->firstChild() ; child != nullptr; child = child->next() ) {
        if (!strcmp(child->name(), "svg:stop")) {
            l.push_back(child);
        }
    }
    /* Remove all stops */
    for (auto i=l.rbegin();i!=l.rend();++i) {
        /** \todo
         * fixme: This should work, unless we make gradient
         * into generic group.
         */
        sp_repr_unparent(*i);
    }
}

/**
 * Writes the gradient's internal vector (whether from its own stops, or
 * inherited from refs) into the gradient repr as svg:stop elements.
 */
void
SPGradient::repr_write_vector()
{
    Inkscape::XML::Document *xml_doc = document->getReprDoc();
    Inkscape::XML::Node *repr = getRepr();

    /* We have to be careful, as vector may be our own, so construct repr list at first */
    std::vector<Inkscape::XML::Node *> l;

    for (auto & stop : vector.stops) {
        Inkscape::CSSOStringStream os;
        Inkscape::XML::Node *child = xml_doc->createElement("svg:stop");
        child->setAttributeCssDouble("offset", stop.offset);
        /* strictly speaking, offset an SVG <number> rather than a CSS one, but exponents make no
         * sense for offset proportions. */
        auto obj = cast<SPStop>(document->getObjectByRepr(child));
        if (auto color = stop.color)
            obj->setColor(*color);
        /* Order will be reversed here */
        l.push_back(child);
    }

    repr_clear_vector();

    /* And insert new children from list */
    for (auto i=l.rbegin();i!=l.rend();++i) {
        Inkscape::XML::Node *child = *i;
        repr->addChild(child, nullptr);
        Inkscape::GC::release(child);
    }
}


void SPGradient::gradientRefModified(SPObject */*href*/, guint /*flags*/, SPGradient *gradient)
{
    if ( gradient->invalidateVector() ) {
        gradient->requestModified(SP_OBJECT_MODIFIED_FLAG);
        // Conditional to avoid causing infinite loop if there's a cycle in the href chain.
    }
}

/** Return true if change made. */
bool SPGradient::invalidateVector()
{
    bool ret = false;

    if (vector.built) {
        vector.built = false;
        vector.stops.clear();
        ret = true;
    }

    return ret;
}

/** Return true if change made. */
bool SPGradient::invalidateArray()
{
    bool ret = false;

    if (array.built) {
        array.built = false;
        mesh.built = false; // the mesh depends on the array
        // array.clear();
        ret = true;
    }

    return ret;
}

/** Creates normalized color vector */
void SPGradient::rebuildVector() const
{
    gint len = 0;
    for (auto& child: children) {
        if (is<SPStop>(&child)) {
            len ++;
        }
    }

    vector.stops.clear();

    SPGradient *reffed = ref ? ref->getObject() : nullptr;
    if ( !hasStops() && reffed ) {
        /* Copy vector from referenced gradient */
        vector.built = true;   // Prevent infinite recursion.
        reffed->ensureVector();
        if (!reffed->vector.stops.empty()) {
            vector.built = reffed->vector.built;
            vector.stops.assign(reffed->vector.stops.begin(), reffed->vector.stops.end());
            return;
        }
    }

    for (auto& child: children) {
        if (is<SPStop>(&child)) {
            auto stop = cast<SPStop>(&child);

            SPGradientStop gstop;
            if (!vector.stops.empty()) {
                // "Each gradient offset value is required to be equal to or greater than the
                // previous gradient stop's offset value. If a given gradient stop's offset
                // value is not equal to or greater than all previous offset values, then the
                // offset value is adjusted to be equal to the largest of all previous offset
                // values."
                gstop.offset = MAX(stop->offset, vector.stops.back().offset);
            } else {
                gstop.offset = stop->offset;
            }

            // "Gradient offset values less than 0 (or less than 0%) are rounded up to
            // 0%. Gradient offset values greater than 1 (or greater than 100%) are rounded
            // down to 100%."
            gstop.offset = CLAMP(gstop.offset, 0, 1);
            gstop.color = stop->getColor();
            vector.stops.push_back(gstop);
        }
    }

    // Normalize per section 13.2.4 of SVG 1.1.
    if (vector.stops.empty()) {
        /* "If no stops are defined, then painting shall occur as if 'none' were specified as the
         * paint style."
         */
        {
            SPGradientStop gstop;
            gstop.offset = 0.0;
            vector.stops.push_back(gstop);
        }
        {
            SPGradientStop gstop;
            gstop.offset = 1.0;
            vector.stops.push_back(gstop);
        }
    } else {
        /* "If one stop is defined, then paint with the solid color fill using the color defined
         * for that gradient stop."
         */
        if (vector.stops.front().offset > 0.0) {
            // If the first one is not at 0, then insert a copy of the first at 0.
            SPGradientStop gstop;
            gstop.offset = 0.0;
            gstop.color = vector.stops.front().color;
            vector.stops.insert(vector.stops.begin(), gstop);
        }
        if (vector.stops.back().offset < 1.0) {
            // If the last one is not at 1, then insert a copy of the last at 1.
            SPGradientStop gstop;
            gstop.offset = 1.0;
            gstop.color = vector.stops.back().color;
            vector.stops.push_back(gstop);
        }
    }

    vector.built = true;
}

/** Creates normalized color mesh patch array */
void SPGradient::rebuildArray() const
{
    // std::cout << "SPGradient::rebuildArray()" << std::endl;

    if( !is<SPMeshGradient>(this) ) {
        g_warning( "SPGradient::rebuildArray() called for non-mesh gradient" );
        return;
    }

    array.read( cast<SPMeshGradient>( this ) );
    has_patches = array.patch_columns() > 0;
}

Geom::Affine
SPGradient::get_g2d_matrix(Geom::Affine const &ctm, Geom::Rect const &bbox) const
{
    if (getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {
        return ( Geom::Scale(bbox.dimensions())
                 * Geom::Translate(bbox.min())
                 * Geom::Affine(ctm) );
    } else {
        return ctm;
    }
}

Geom::Affine
SPGradient::get_gs2d_matrix(Geom::Affine const &ctm, Geom::Rect const &bbox) const
{
    if (getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX) {
        return ( gradientTransform
                 * Geom::Scale(bbox.dimensions())
                 * Geom::Translate(bbox.min())
                 * Geom::Affine(ctm) );
    } else {
        return gradientTransform * ctm;
    }
}

void
SPGradient::set_gs2d_matrix(Geom::Affine const &ctm,
                            Geom::Rect const &bbox, Geom::Affine const &gs2d)
{
    gradientTransform = gs2d * ctm.inverse();
    if (getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX ) {
        gradientTransform = ( gradientTransform
                                  * Geom::Translate(-bbox.min())
                                  * Geom::Scale(bbox.dimensions()).inverse() );
    }
    gradientTransform_set = TRUE;

    requestModified(SP_OBJECT_MODIFIED_FLAG);
}

/**
 * Return a visual bounding box that covers every item this gradient
 * would paint added together.
 */
Geom::OptRect SPGradient::getAllItemsBox() const
{
    std::vector<SPObject *> links;
    getLinkedRecursive(links, SPObject::LinkedObjectNature::DEPENDENT);

    Geom::OptRect bbox;
    for (auto obj : links) {
        if (auto item = cast<SPItem>(obj)) {
            bbox.unionWith(item->visualBounds(Geom::identity(), true, false, true));
        }
    }
    return bbox;
}

/* CAIRO RENDERING STUFF */

void
sp_gradient_pattern_common_setup(cairo_pattern_t *cp,
                                 SPGradient *gr,
                                 Geom::OptRect const &bbox,
                                 double opacity)
{
    // set spread type
    switch (gr->getSpread()) {
    case SP_GRADIENT_SPREAD_REFLECT:
        cairo_pattern_set_extend(cp, CAIRO_EXTEND_REFLECT);
        break;
    case SP_GRADIENT_SPREAD_REPEAT:
        cairo_pattern_set_extend(cp, CAIRO_EXTEND_REPEAT);
        break;
    case SP_GRADIENT_SPREAD_PAD:
    default:
        cairo_pattern_set_extend(cp, CAIRO_EXTEND_PAD);
        break;
    }

    // add stops
    if (!is<SPMeshGradient>(gr)) {
        for (auto const &stop : Inkscape::GradientRendering::canvas_vector(gr).stops) {
            // multiply stop opacity by paint opacity
            ink_cairo_pattern_add_color_stop(cp, stop.offset, stop.color->withOpacity(opacity));
        }
    }

    // set pattern transform matrix
    Geom::Affine gs2user = gr->gradientTransform;
    if (gr->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX && bbox) {
        Geom::Affine bbox2user(bbox->width(), 0, 0, bbox->height(), bbox->left(), bbox->top());
        gs2user *= bbox2user;
    }
    ink_cairo_pattern_set_matrix(cp, gs2user.inverse());
}

/// Iterate over the stops of the preview linear gradient.
void SPGradient::forEachPreviewPatternStop(std::function<void (double, Color const &)> const &callback)
{
    if (!is<SPMeshGradient>(this)) {
        ensureVector();

        for (auto const &stop : Inkscape::GradientRendering::canvas_vector(this).stops) {
            if (stop.color) {
                callback(stop.offset, *stop.color);
            }
        }
    } else if (int const num_columns = array.patch_columns()) {
        // For the moment, use the top row of nodes for preview.
        double offset = 1.0 / num_columns;

        for (int i = 0; i <= num_columns; ++i) {
            auto node = array.node(0, i * 3);
            if (node->color) {
                callback(i * offset, *node->color);
            }
        }
    }
}

/// Returns the average colour of the preview linear gradient.
Color SPGradient::getPreviewAverageColor()
{
    LinearGradientAverager grad;

    forEachPreviewPatternStop([&] (double offset, Color const &col) {
        grad.addStop(offset, col);
    });

    try {
        return grad.finish();
    } catch (std::logic_error const &) {
        return Color{0x0};
    }
}

/// Deprecated, use GtkSnapshot.
cairo_pattern_t *SPGradient::create_preview_pattern(double width)
{
    auto result = cairo_pattern_create_linear(0, 0, width, 0);

    forEachPreviewPatternStop([&] (double offset, Color const &col) {
        ink_cairo_pattern_add_color_stop(result, offset, col);
    });

    return result;
}

bool SPGradient::isSolid() const
{
    if (swatch && hasStops() && getStopCount() == 1) {
        return true;
    }
    return false;
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
