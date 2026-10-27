// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * <sodipodi:spiral> implementation
 */
/*
 * Authors:
 *   Mitsuru Oka <oka326@parkcity.ne.jp>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-spiral.h"

#include <glibmm/i18n.h>

#include <2geom/bezier-utils.h>

#include "attributes.h"

#include "path/path-curve.h"
#include "snap-candidate.h"      // for SnapCandidatePoint
#include "snap-enums.h"          // for SnapTargetType, SnapSourceType
#include "snap-preferences.h"    // for SnapPreferences
#include "svg/svg.h"
#include "xml/document.h"        // for Document
#include "xml/node.h"            // for Node

class SPDocument;

SPSpiral::SPSpiral()
    : SPShape()
    , cx(0)
    , cy(0)
    , exp(1)
    , revo(3)
    , rad(1)
    , arg(0)
    , t0(0)
{
}

SPSpiral::~SPSpiral() = default;

void SPSpiral::build(SPDocument * document, Inkscape::XML::Node * repr) {
    SPShape::build(document, repr);

    this->readAttr(SPAttr::SODIPODI_CX);
    this->readAttr(SPAttr::SODIPODI_CY);
    this->readAttr(SPAttr::SODIPODI_EXPANSION);
    this->readAttr(SPAttr::SODIPODI_REVOLUTION);
    this->readAttr(SPAttr::SODIPODI_RADIUS);
    this->readAttr(SPAttr::SODIPODI_ARGUMENT);
    this->readAttr(SPAttr::SODIPODI_T0);
}

Inkscape::XML::Node* SPSpiral::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = xml_doc->createElement("svg:path");
    }

    if (flags & SP_OBJECT_WRITE_EXT) {
        /* Fixme: we may replace these attributes by
         * sodipodi:spiral="cx cy exp revo rad arg t0"
         */
        repr->setAttribute("sodipodi:type", "spiral");
        repr->setAttributeSvgDouble("sodipodi:cx", this->cx);
        repr->setAttributeSvgDouble("sodipodi:cy", this->cy);
        repr->setAttributeSvgDouble("sodipodi:expansion", this->exp);
        repr->setAttributeSvgDouble("sodipodi:revolution", this->revo);
        repr->setAttributeSvgDouble("sodipodi:radius", this->rad);
        repr->setAttributeSvgDouble("sodipodi:argument", this->arg);
        repr->setAttributeSvgDouble("sodipodi:t0", this->t0);
    }

     // make sure the curve is rebuilt with all up-to-date parameters
     this->set_shape();

    // Nulls might be possible if this called iteratively
    if (!this->_curve) {
            //g_warning("sp_spiral_write(): No path to copy");
            return nullptr;
    }

    repr->setAttribute("d", sp_svg_write_path(*_curve));

    SPShape::write(xml_doc, repr, flags | SP_SHAPE_WRITE_PATH);

    return repr;
}

void SPSpiral::set(SPAttr key, gchar const* value) {
    /// \todo fixme: we should really collect updates
    SVGLength::Unit unit;
    double temp;
    switch (key) {
    case SPAttr::SODIPODI_CX:
        if (!parse_number_with_unit(value, unit, temp, this->cx, false)) {
            this->cx = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_CY:
        if (!parse_number_with_unit(value, unit, temp, this->cy, false)) {
            this->cy = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_EXPANSION:
        if (sp_svg_number_read_d(value, &this->exp)) {
            this->exp = CLAMP (this->exp, 0.0, 1000.0);
        } else {
            this->exp = 1.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_REVOLUTION:
        if (sp_svg_number_read_d(value, &this->revo)) {
            this->revo = CLAMP (this->revo, 0.05, 1024.0);
        } else {
            this->revo = 3.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_RADIUS:
        if (!parse_number_with_unit(value, unit, temp, this->rad, false)) {
            this->rad = MAX (this->rad, 0.001);
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_ARGUMENT:
        if (!sp_svg_number_read_d(value, &this->arg)) {
            this->arg = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_T0:
        if (sp_svg_number_read_d(value, &this->t0)) {
            this->t0 = CLAMP (this->t0, 0.0, 0.999);
            /** \todo
                         * Have shared constants for the allowable bounds for
                         * attributes. There was a bug here where we used -1.0
                         * as the minimum (which leads to NaN via, e.g.,
                         * pow(-1.0, 0.5); see sp_spiral_get_xy for
                         * requirements.
                         */
        } else {
        	this->t0 = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    default:
        SPShape::set(key, value);
        break;
    }
}

void SPSpiral::update(SPCtx *ctx, guint flags) {
    if (flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG)) {
        this->set_shape();
    }

    SPShape::update(ctx, flags);
}

const char* SPSpiral::typeName() const {
    return "spiral";
}

const char* SPSpiral::displayName() const {
    return _("Spiral");
}

gchar* SPSpiral::description() const {
    // TRANSLATORS: since turn count isn't an integer, please adjust the
    // string as needed to deal with an localized plural forms.
    return g_strdup_printf (_("with %3f turns"), this->revo);
}

/**
 * Fit beziers together to spiral and draw it.
 *
 * \pre dstep \> 0.
 * \pre is_unit_vector(*hat1).
 * \post is_unit_vector(*hat2).
 **/
void SPSpiral::fitAndDraw(Geom::Path &c, double dstep, Geom::Point darray[], Geom::Point const& hat1, Geom::Point& hat2, double* t) const
{
#define BEZIER_SIZE   4
#define FITTING_MAX_BEZIERS 4
#define BEZIER_LENGTH (BEZIER_SIZE * FITTING_MAX_BEZIERS)

    g_assert (dstep > 0);
    g_assert (is_unit_vector (hat1));

    Geom::Point bezier[BEZIER_LENGTH];
    double d;
    int depth, i;

    for (d = *t, i = 0; i <= SAMPLE_SIZE; d += dstep, i++) {
        darray[i] = this->getXY(d);

        /* Avoid useless adjacent dups.  (Otherwise we can have all of darray filled with
           the same value, which upsets chord_length_parameterize.) */
        if ((i != 0) && (darray[i] == darray[i - 1]) && (d < 1.0)) {
            i--;
            d += dstep;
            /** We mustn't increase dstep for subsequent values of
                         * i: for large spiral.exp values, rate of growth
                         * increases very rapidly.
                         */
                        /** \todo
                         * Get the function itself to decide what value of d
                         * to use next: ensure that we move at least 0.25 *
                         * stroke width, for example.  The derivative (as used
                         * for get_tangent before normalization) would be
                         * useful for estimating the appropriate d value.  Or
                         * perhaps just start with a small dstep and scale by
                         * some small number until we move >= 0.25 *
                         * stroke_width.  Must revert to the original dstep
                         * value for next iteration to avoid the problem
                         * mentioned above.
                         */
        }
    }

    double const next_t = d - 2 * dstep;
    /* == t + (SAMPLE_SIZE - 1) * dstep, in absence of dups. */

    hat2 = -this->getTangent(next_t);

    /** \todo
         * We should use better algorithm to specify maximum error.
         */
    depth = Geom::bezier_fit_cubic_full (bezier, nullptr, darray, SAMPLE_SIZE,
                      hat1, hat2,
                      SPIRAL_TOLERANCE*SPIRAL_TOLERANCE,
                      FITTING_MAX_BEZIERS);

    g_assert(depth * BEZIER_SIZE <= gint(G_N_ELEMENTS(bezier)));

#ifdef SPIRAL_DEBUG
    if (*t == spiral->t0 || *t == 1.0)
        g_print ("[%s] depth=%d, dstep=%g, t0=%g, t=%g, arg=%g\n",
             debug_state, depth, dstep, spiral->t0, *t, spiral->arg);
#endif

    if (depth != -1) {
        for (i = 0; i < 4*depth; i += 4) {
            c.appendNew<Geom::CubicBezier>(bezier[i + 1],
                      bezier[i + 2],
                      bezier[i + 3]);
        }
    } else {
#ifdef SPIRAL_VERBOSE
        g_print ("cant_fit_cubic: t=%g\n", *t);
#endif
        for (i = 1; i < SAMPLE_SIZE; i++)
            c.appendNew<Geom::LineSegment>(darray[i]);
    }

    *t = next_t;

    g_assert (is_unit_vector (hat2));
}

void SPSpiral::set_shape() {
    if (checkBrokenPathEffect()) {
        return;
    }

    Geom::Point darray[SAMPLE_SIZE + 1];

    this->requestModified(SP_OBJECT_MODIFIED_FLAG);

    Geom::Path c;

#ifdef SPIRAL_VERBOSE
    g_print ("cx=%g, cy=%g, exp=%g, revo=%g, rad=%g, arg=%g, t0=%g\n",
            this->cx,
            this->cy,
            this->exp,
            this->revo,
            this->rad,
            this->arg,
            this->t0);
#endif

    /* Initial moveto. */
    c.start(getXY(t0));

    double const tstep = SAMPLE_STEP / this->revo;
    double const dstep = tstep / (SAMPLE_SIZE - 1);

    Geom::Point hat1 = this->getTangent(this->t0);
    Geom::Point hat2;

    double t;
    for (t = this->t0; t < (1.0 - tstep);) {
        fitAndDraw(c, dstep, darray, hat1, hat2, &t);

        hat1 = -hat2;
    }

    if (1.0 - t > SP_EPSILON) {
        fitAndDraw(c, (1.0 - t) / (SAMPLE_SIZE - 1.0), darray, hat1, hat2, &t);
    }

    prepareShapeForLPE(std::move(c));
}

/**
 * Set spiral properties and update display.
 */
void SPSpiral::setPosition(gdouble cx, gdouble cy, gdouble exp, gdouble revo, gdouble rad, gdouble arg, gdouble t0) {
    /** \todo
         * Consider applying CLAMP or adding in-bounds assertions for
         * some of these parameters.
         */
	this->cx         = cx;
	this->cy         = cy;
	this->exp        = exp;
    this->revo       = revo;
    this->rad        = MAX (rad, 0.0);
    this->arg        = arg;
    this->t0         = CLAMP(t0, 0.0, 0.999);

    this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
}

void SPSpiral::snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const {
    // We will determine the spiral's midpoint ourselves, instead of trusting on the base class
    // Therefore snapping to object midpoints is temporarily disabled
    Inkscape::SnapPreferences local_snapprefs = *snapprefs;
    local_snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_OBJECT_MIDPOINT, false);

    SPShape::snappoints(p, &local_snapprefs);

    if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_OBJECT_MIDPOINT)) {
        Geom::Affine const i2dt (this->i2dt_affine ());

        p.emplace_back(Geom::Point(this->cx, this->cy) * i2dt, Inkscape::SNAPSOURCE_OBJECT_MIDPOINT, Inkscape::SNAPTARGET_OBJECT_MIDPOINT);
        // This point is the start-point of the spiral, which is also returned when _snap_to_itemnode has been set
        // in the object snapper. In that case we will get a duplicate!
    }
}

/**
 * Set spiral transform
 */
Geom::Affine SPSpiral::set_transform(Geom::Affine const &xform, Inkscape::StrokeTransform *preview)
{
    if (pathEffectsEnabled() && !optimizeTransforms()) {
        return xform;
    }
    // Only set transform with proportional scaling
    if (!xform.withoutTranslation().isUniformScale()) {
        return xform;
    }
    /* Calculate spiral start in parent coords. */
    Geom::Point pos( Geom::Point(this->cx, this->cy) * xform );

    /* This function takes care of translation and scaling, we return whatever parts we can't
       handle. */
    Geom::Affine ret(Geom::Affine(xform).withoutTranslation());
    gdouble const s = hypot(ret[0], ret[1]);
    if (s > 1e-9) {
        ret[0] /= s;
        ret[1] /= s;
        ret[2] /= s;
        ret[3] /= s;
    } else {
        ret[0] = 1.0;
        ret[1] = 0.0;
        ret[2] = 0.0;
        ret[3] = 1.0;
    }

    if (preview) {
        preview->expansion = s;
        return ret;
    }

    this->rad *= s;

    /* Find start in item coords */
    pos = pos * ret.inverse();
    this->cx = pos[Geom::X];
    this->cy = pos[Geom::Y];

    this->set_shape();

    // Adjust stroke width
    this->adjust_stroke(s);

    // Adjust pattern fill
    this->adjust_pattern(xform * ret.inverse());

    // Adjust gradient fill
    this->adjust_gradient(xform * ret.inverse());

    return ret;
}

void SPSpiral::update_patheffect(bool write) {
    SPShape::update_patheffect(write);
}

/**
 * Return one of the points on the spiral.
 *
 * \param t specifies how far along the spiral.
 * \pre \a t in [0.0, 2.03].  (It doesn't make sense for t to be much more
 * than 1.0, though some callers go slightly beyond 1.0 for curve-fitting
 * purposes.)
 */
Geom::Point SPSpiral::getXY(gdouble t) const {
    g_assert (this->exp >= 0.0);
    /* Otherwise we get NaN for t==0. */
    g_assert (this->exp <= 1000.0);
    /* Anything much more results in infinities.  Even allowing 1000 is somewhat overkill. */
    g_assert (t >= 0.0);
    /* Any callers passing -ve t will have a bug for non-integral values of exp. */

    double const rad = this->rad * pow(t, (double)this->exp);
    double const arg = 2.0 * M_PI * this->revo * t + this->arg;

    return Geom::Point(rad * cos(arg) + this->cx, rad * sin(arg) + this->cy);
}


/**
 * Returns the derivative of sp_spiral_get_xy with respect to t,
 *  scaled to a unit vector.
 *
 *  \pre spiral != 0.
 *  \pre 0 \<= t.
 *  \pre p != NULL.
 *  \post is_unit_vector(*p).
 */
Geom::Point SPSpiral::getTangent(gdouble t) const {
    Geom::Point ret(1.0, 0.0);

    g_assert (t >= 0.0);
    g_assert (this->exp >= 0.0);
    /* See above for comments on these assertions. */

    double const t_scaled = 2.0 * M_PI * this->revo * t;
    double const arg = t_scaled + this->arg;
    double const s = sin(arg);
    double const c = cos(arg);

    if (this->exp == 0.0) {
        ret = Geom::Point(-s, c);
    } else if (t_scaled == 0.0) {
        ret = Geom::Point(c, s);
    } else {
        Geom::Point unrotated(this->exp, t_scaled);
        double const s_len = L2 (unrotated);
        g_assert (s_len != 0);
        /** \todo
                 * Check that this isn't being too hopeful of the hypot
                 * function.  E.g. test with numbers around 2**-1070
                 * (denormalized numbers), preferably on a few different
                 * platforms.  However, njh says that the usual implementation
                 * does handle both very big and very small numbers.
                 */
        unrotated /= s_len;

        /* ret = spiral->exp * (c, s) + t_scaled * (-s, c);
           alternatively ret = (spiral->exp, t_scaled) * (( c, s),
                                  (-s, c)).*/
        ret = Geom::Point(dot(unrotated, Geom::Point(c, -s)),
                                  dot(unrotated, Geom::Point(s, c)));
        /* ret should already be approximately normalized: the
           matrix ((c, -s), (s, c)) is orthogonal (it just
           rotates by arg), and unrotated has been normalized,
           so ret is already of unit length other than numerical
           error in the above matrix multiplication. */

        /** \todo
                 * I haven't checked how important it is for ret to be very
                 * near unit length; we could get rid of the below.
                 */

        ret.normalize();
        /* Proof that ret length is non-zero: see above.  (Should be near 1.) */
    }

    g_assert (is_unit_vector(ret));
    return ret;
}

/**
 * Compute rad and/or arg for point on spiral.
 */
void SPSpiral::getPolar(gdouble t, gdouble* rad, gdouble* arg) const {
    if (rad) {
        *rad = this->rad * pow(t, (double)this->exp);
    }

    if (arg) {
        *arg = 2.0 * M_PI * this->revo * t + this->arg;
    }
}

/**
 * Return true if spiral has properties that make it invalid.
 */
bool SPSpiral::isInvalid() const {
    gdouble rad;

    this->getPolar(0.0, &rad, nullptr);

    if (rad < 0.0 || rad > SP_HUGE) {
        g_warning("rad(t=0)=%g", rad);
        return true;
    }

    this->getPolar(1.0, &rad, nullptr);

    if (rad < 0.0 || rad > SP_HUGE) {
        g_warning("rad(t=1)=%g", rad);
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
