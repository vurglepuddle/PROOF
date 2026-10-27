// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * <sodipodi:star> implementation
 *
 * Authors:
 *   Mitsuru Oka <oka326@parkcity.ne.jp>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-star.h"

#include <cstring>

#include <glib.h>
#include <glibmm/i18n.h>

#include "attributes.h"
#include "snap-candidate.h"       // for SnapCandidatePoint
#include "snap-enums.h"           // for SnapTargetType, SnapSourceType
#include "snap-preferences.h"     // for SnapPreferences

#include "path/path-curve.h"
#include "svg/svg.h"
#include "xml/document.h"         // for Document
#include "xml/node.h"             // for Node

class SPDocument;

SPStar::SPStar() : SPShape() ,
	sides(5),
	center(0, 0),
	flatsided(false),
	rounded(0.0),
	randomized(0.0)
{
	this->r[0] = 1.0;
	this->r[1] = 0.001;
	this->arg[0] = this->arg[1] = 0.0;
}

SPStar::~SPStar() = default;

void SPStar::build(SPDocument * document, Inkscape::XML::Node * repr) {
	// CPPIFY: see header file
    SPShape::build(document, repr);

    this->readAttr(SPAttr::SODIPODI_CX);
    this->readAttr(SPAttr::SODIPODI_CY);
    this->readAttr(SPAttr::INKSCAPE_FLATSIDED);
    this->readAttr(SPAttr::SODIPODI_SIDES);
    this->readAttr(SPAttr::SODIPODI_R1);
    this->readAttr(SPAttr::SODIPODI_R2);
    this->readAttr(SPAttr::SODIPODI_ARG1);
    this->readAttr(SPAttr::SODIPODI_ARG2);
    this->readAttr(SPAttr::INKSCAPE_ROUNDED);
    this->readAttr(SPAttr::INKSCAPE_RANDOMIZED);
}

Inkscape::XML::Node* SPStar::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = xml_doc->createElement("svg:path");
    }

    if (flags & SP_OBJECT_WRITE_EXT) {
        repr->setAttribute("sodipodi:type", "star");
        repr->setAttributeBoolean("inkscape:flatsided", this->flatsided);
        repr->setAttributeInt("sodipodi:sides", this->sides);
        repr->setAttributeSvgDouble("sodipodi:cx", this->center[Geom::X]);
        repr->setAttributeSvgDouble("sodipodi:cy", this->center[Geom::Y]);
        repr->setAttributeSvgDouble("sodipodi:r1", this->r[0]);
        repr->setAttributeSvgDouble("sodipodi:r2", this->r[1]);
        repr->setAttributeSvgDouble("sodipodi:arg1", this->arg[0]);
        repr->setAttributeSvgDouble("sodipodi:arg2", this->arg[1]);
        repr->setAttributeSvgDouble("inkscape:rounded", this->rounded);
        repr->setAttributeSvgDouble("inkscape:randomized", this->randomized);
    }

    this->set_shape();
    if (_curve) {
        repr->setAttribute("d", sp_svg_write_path(*_curve));
    } else {
        repr->removeAttribute("d");
    }
    // CPPIFY: see header file
    SPShape::write(xml_doc, repr, flags);

    return repr;
}

void SPStar::set(SPAttr key, const gchar* value) {
    SVGLength::Unit unit;
    double temp;

    /* fixme: we should really collect updates */
    switch (key) {
    case SPAttr::SODIPODI_SIDES:
        if (value) {
            this->sides = atoi (value);
            this->sides = CLAMP(this->sides, this->flatsided ? 3 : 2, 1024);
        } else {
            this->sides = 5;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_CX:
        if (!parse_number_with_unit(value, unit, temp, this->center[Geom::X], true)) {
            this->center[Geom::X] = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_CY:
        if (!parse_number_with_unit(value, unit, temp, this->center[Geom::Y], true)) {
            this->center[Geom::Y] = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_R1:
        if (!parse_number_with_unit(value, unit, temp, this->r[0], true)) {
            this->r[0] = 1.0;
        }

        /* fixme: Need CLAMP (Lauris) */
        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_R2:
        if (!parse_number_with_unit(value, unit, temp, this->r[1], true)) {
            this->r[1] = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        return;

    case SPAttr::SODIPODI_ARG1:
        if (value) {
            this->arg[0] = g_ascii_strtod (value, nullptr);
        } else {
            this->arg[0] = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::SODIPODI_ARG2:
        if (value) {
            this->arg[1] = g_ascii_strtod (value, nullptr);
        } else {
            this->arg[1] = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::INKSCAPE_FLATSIDED:
        if (value && !strcmp(value, "true")) {
            this->flatsided = true;
            this->sides = MAX(this->sides, 3);
        } else {
        	this->flatsided = false;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::INKSCAPE_ROUNDED:
        if (value) {
            this->rounded = g_ascii_strtod (value, nullptr);
        } else {
            this->rounded = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    case SPAttr::INKSCAPE_RANDOMIZED:
        if (value) {
            this->randomized = g_ascii_strtod (value, nullptr);
        } else {
            this->randomized = 0.0;
        }

        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        break;

    default:
    	// CPPIFY: see header file
        SPShape::set(key, value);
        break;
    }
}

void SPStar::update(SPCtx *ctx, guint flags) {
    if (flags & (SP_OBJECT_MODIFIED_FLAG |
             SP_OBJECT_STYLE_MODIFIED_FLAG |
             SP_OBJECT_VIEWPORT_MODIFIED_FLAG)) {

        this->set_shape();
    }

    // CPPIFY: see header file
    SPShape::update(ctx, flags);
}

const char* SPStar::typeName() const {
    if (this->flatsided == false)
        return "star";
    return "polygon";
}

const char* SPStar::displayName() const {
    if (this->flatsided == false)
        return _("Star");
    return _("Polygon");
}

gchar* SPStar::description() const {
    // while there will never be less than 2 or 3 vertices, we still need to
    // make calls to ngettext because the pluralization may be different
    // for various numbers >=3.  The singular form is used as the index.
    return g_strdup_printf (ngettext("with %d vertex", "with %d vertices",
                this->sides), this->sides);
}

/**
Returns a unit-length vector at 90 degrees to the direction from o to n
 */
static Geom::Point
rot90_rel (Geom::Point o, Geom::Point n)
{
    return (n-o).ccw().normalized();
}

/**
Returns a unique 32 bit int for a given point.
Obvious (but acceptable for my purposes) limits to uniqueness:
- returned value for x,y repeats for x+n*1024,y+n*1024
- returned value is unchanged when the point is moved by less than 1/1024 of px
*/
static guint32
point_unique_int (Geom::Point o)
{
    return ((guint32)
    65536 *
        (((int) floor (o[Geom::X] * 64)) % 1024 + ((int) floor (o[Geom::X] * 1024)) % 64)
    +
             (((int) floor (o[Geom::Y] * 64)) % 1024 + ((int) floor (o[Geom::Y] * 1024)) % 64)
    );
}

/**
Returns the next pseudorandom value using the Linear Congruential Generator algorithm (LCG)
with the parameters (m = 2^32, a = 69069, b = 1). These parameters give a full-period generator,
i.e. it is guaranteed to go through all integers < 2^32 (see http://random.mat.sbg.ac.at/~charly/server/server.html)
*/
static inline guint32
lcg_next(guint32 const prev)
{
    return (guint32) ( 69069 * prev + 1 );
}

/**
Returns a random number in the range [-0.5, 0.5) from the given seed, stepping the given number of steps from the seed.
*/
static double
rnd (guint32 const seed, unsigned steps) {
    guint32 lcg = seed;
    for (; steps > 0; steps --)
        lcg = lcg_next (lcg);

    return ( lcg / 4294967296. ) - 0.5;
}

static Geom::Point
sp_star_get_curvepoint (SPStar *star, SPStarPoint point, gint index, bool previ)
{
    // the point whose neighboring curve handle we're calculating
    Geom::Point o = sp_star_get_xy (star, point, index);

    // indices of previous and next points
    gint pi = (index > 0)? (index - 1) : (star->sides - 1);
    gint ni = (index < star->sides - 1)? (index + 1) : 0;

    // the other point type
    SPStarPoint other = (point == SP_STAR_POINT_KNOT2? SP_STAR_POINT_KNOT1 : SP_STAR_POINT_KNOT2);

    // the neighbors of o; depending on flatsided, they're either the same type (polygon) or the other type (star)
    Geom::Point prev = (star->flatsided? sp_star_get_xy (star, point, pi) : sp_star_get_xy (star, other, point == SP_STAR_POINT_KNOT2? index : pi));
    Geom::Point next = (star->flatsided? sp_star_get_xy (star, point, ni) : sp_star_get_xy (star, other, point == SP_STAR_POINT_KNOT1? index : ni));

    // prev-next midpoint
    Geom::Point mid =  0.5 * (prev + next);

    // point to which we direct the bissector of the curve handles;
    // it's far enough outside the star on the perpendicular to prev-next through mid
    Geom::Point biss =  mid + 100000 * rot90_rel (mid, next);

    // lengths of vectors to prev and next
    gdouble prev_len = Geom::L2 (prev - o);
    gdouble next_len = Geom::L2 (next - o);

    // unit-length vector perpendicular to o-biss
    Geom::Point rot = rot90_rel (o, biss);

    // multiply rot by star->rounded coefficient and the distance to the star point; flip for next
    Geom::Point ret;
    if (previ) {
        ret = (star->rounded * prev_len) * rot;
    } else {
        ret = (star->rounded * next_len * -1) * rot;
    }

    if (star->randomized == 0) {
        // add the vector to o to get the final curvepoint
        return o + ret;
    } else {
        // the seed corresponding to the exact point
        guint32 seed = point_unique_int (o);

        // randomly rotate (by step 3 from the seed) and scale (by step 4) the vector
        ret = ret * Geom::Affine (Geom::Rotate (star->randomized * M_PI * rnd (seed, 3)));
        ret *= ( 1 + star->randomized * rnd (seed, 4));

        // the randomized corner point
        Geom::Point o_randomized = sp_star_get_xy (star, point, index, true);

        return o_randomized + ret;
    }
}

#define NEXT false
#define PREV true

void SPStar::set_shape() {
    // perhaps we should convert all our shapes into LPEs without source path
    // and with knotholders for parameters, then this situation will be handled automatically
    // by disabling the entire stack (including the shape LPE)
    if (checkBrokenPathEffect()) {
        return;
    }

    Geom::Path c;

    bool not_rounded = (fabs (this->rounded) < 1e-4);

    // note that we pass randomized=true to sp_star_get_xy, because the curve must be randomized;
    // other places that call that function (e.g. the knotholder) need the exact point

    // draw 1st segment
    c.start(sp_star_get_xy (this, SP_STAR_POINT_KNOT1, 0, true));

    if (this->flatsided == false) {
        if (not_rounded) {
            c.appendNew<Geom::LineSegment>(sp_star_get_xy (this, SP_STAR_POINT_KNOT2, 0, true));
        } else {
            c.appendNew<Geom::CubicBezier>(sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, 0, NEXT),
                sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT2, 0, PREV),
                sp_star_get_xy (this, SP_STAR_POINT_KNOT2, 0, true));
        }
    }

    // draw all middle segments
    for (gint i = 1; i < sides; i++) {
        if (not_rounded) {
            c.appendNew<Geom::LineSegment>(sp_star_get_xy (this, SP_STAR_POINT_KNOT1, i, true));
        } else {
            if (this->flatsided == false) {
                c.appendNew<Geom::CubicBezier>(sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT2, i - 1, NEXT),
                        sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, i, PREV),
                        sp_star_get_xy (this, SP_STAR_POINT_KNOT1, i, true));
            } else {
                c.appendNew<Geom::CubicBezier>(sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, i - 1, NEXT),
                        sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, i, PREV),
                        sp_star_get_xy (this, SP_STAR_POINT_KNOT1, i, true));
            }
        }

        if (this->flatsided == false) {
            if (not_rounded) {
                c.appendNew<Geom::LineSegment>(sp_star_get_xy (this, SP_STAR_POINT_KNOT2, i, true));
            } else {
                c.appendNew<Geom::CubicBezier>(sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, i, NEXT),
                    sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT2, i, PREV),
                    sp_star_get_xy (this, SP_STAR_POINT_KNOT2, i, true));
            }
        }
    }

    // draw last segment
	if (!not_rounded) {
		if (this->flatsided == false) {
            c.appendNew<Geom::CubicBezier>(sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT2, sides - 1, NEXT),
				sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, 0, PREV),
				sp_star_get_xy (this, SP_STAR_POINT_KNOT1, 0, true));
		} else {
            c.appendNew<Geom::CubicBezier>(sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, sides - 1, NEXT),
				sp_star_get_curvepoint (this, SP_STAR_POINT_KNOT1, 0, PREV),
				sp_star_get_xy (this, SP_STAR_POINT_KNOT1, 0, true));
		}
	}

    c.close();

    prepareShapeForLPE(std::move(c));
}

void
sp_star_position_set (SPStar *star, gint sides, Geom::Point center, gdouble r1, gdouble r2, gdouble arg1, gdouble arg2, bool isflat, double rounded, double randomized)
{
    g_return_if_fail (star != nullptr);

    star->flatsided = isflat;
    star->center = center;
    star->r[0] = MAX (r1, 0.001);

    if (isflat == false) {
        star->sides = CLAMP(sides, 2, 1024);
        star->r[1] = CLAMP(r2, 0.0, star->r[0]);
    } else {
        star->sides = CLAMP(sides, 3, 1024);
        star->r[1] = CLAMP( r1*cos(M_PI/sides) ,0.0, star->r[0] );
    }

    star->arg[0] = arg1;
    star->arg[1] = arg2;
    star->rounded = rounded;
    star->randomized = randomized;
    star->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
}

void SPStar::snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const {
    // We will determine the star's midpoint ourselves, instead of trusting on the base class
    // Therefore snapping to object midpoints is temporarily disabled
    Inkscape::SnapPreferences local_snapprefs = *snapprefs;
    local_snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_OBJECT_MIDPOINT, false);

    // CPPIFY: see header file
    SPShape::snappoints(p, &local_snapprefs);

    if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_OBJECT_MIDPOINT)) {
        Geom::Affine const i2dt (this->i2dt_affine ());
        p.emplace_back(this->center * i2dt,Inkscape::SNAPSOURCE_OBJECT_MIDPOINT, Inkscape::SNAPTARGET_OBJECT_MIDPOINT);
    }
}

Geom::Affine SPStar::set_transform(Geom::Affine const &xform, Inkscape::StrokeTransform *preview)
{
    bool opt_trans = (randomized == 0);
    if (pathEffectsEnabled() && !optimizeTransforms()) {
        return xform;
    }
    // Only set transform with proportional scaling
    if (!xform.withoutTranslation().isUniformScale()) {
        return xform;
    }

    /* Calculate star start in parent coords. */
    Geom::Point pos( this->center * xform );

    /* This function takes care of translation and scaling, we return whatever parts we can't
       handle. */
    Geom::Affine ret(opt_trans ? xform.withoutTranslation() : xform);
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

    this->r[0] *= s;
    this->r[1] *= s;

    /* Find start in item coords */
    pos = pos * ret.inverse();
    this->center = pos;

    this->set_shape();

    // Adjust stroke width
    this->adjust_stroke(s);

    // Adjust pattern fill
    this->adjust_pattern(xform * ret.inverse());

    // Adjust gradient fill
    this->adjust_gradient(xform * ret.inverse());

    return ret;
}

void SPStar::update_patheffect(bool write) {
    SPShape::update_patheffect(write);
}

/**
 * Calculate the average side length of the polygon.
 *
 * For spoked polygons (stars) this is the radius delta; for non-spoked
 * polygons this is the regular side length directly.
 *
 * @returns the average length of the polygon sides.
 */
double SPStar::getSideLength() const
{
    if (!flatsided) {
        // Pointy star
        double totalLength = 0.0;
        auto tr = i2doc_affine();
        
        for (gint i = 0; i < sides; i++) {
            Geom::Point outer1 = sp_star_get_xy(this, SP_STAR_POINT_KNOT1, i, false) * tr;
            Geom::Point inner1 = sp_star_get_xy(this, SP_STAR_POINT_KNOT2, i, false) * tr;
            
            totalLength += Geom::distance(outer1, inner1);

            Geom::Point outer2 = sp_star_get_xy(this, SP_STAR_POINT_KNOT1, (i + 1) % sides, false) * tr;
            totalLength += Geom::distance(inner1, outer2);
        }
        
        // Return the average side length (since we have 2 * sides distances, divide by 2 * sides)
        return totalLength / (2 * sides);
    }
    
    double diameter = 0.0;
    auto tr = i2doc_affine();
    for (gint i = 0; i < sides; i++) {
        diameter += Geom::distance(sp_star_get_xy(this, SP_STAR_POINT_KNOT1, i, false) * tr,
                                   sp_star_get_xy(this, SP_STAR_POINT_KNOT1, (i + 1) % sides, false) * tr);
    }
    return diameter / sides;
}

/**
 * Set the average side length of the polygon.
 *
 * For spoked polygons (stars) this is the radius delta; for non-spoked
 * polygons this is the regular side length directly.
 *
 * @param length the new average length of the polygon sides.
 */
void SPStar::setSideLength(double length) {
    double currentLength = getSideLength();
    if (currentLength <= 0 || length <= 0) {
        return; // Prevent division by zero or invalid scaling
    }

    double scale = length / currentLength;

    if (!flatsided) {
        // Pointy star
        r[0] *= scale;
        r[1] *= scale;
    } else {
        // Flat star
        r[0] *= scale;
    }

    this->updateRepr();

    requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
}

/**
 * sp_star_get_xy: Get X-Y value as item coordinate system
 * @star: star item
 * @point: point type to obtain X-Y value
 * @index: index of vertex
 * @p: pointer to store X-Y value
 * @randomized: false (default) if you want to get exact, not randomized point
 *
 * Initial item coordinate system is same as document coordinate system.
 */
Geom::Point
sp_star_get_xy (SPStar const *star, SPStarPoint point, gint index, bool randomized)
{
    gdouble darg = 2.0 * M_PI / (double) star->sides;

    double arg = star->arg[point];
    arg += index * darg;

    Geom::Point xy = star->r[point] * Geom::Point(cos(arg), sin(arg)) + star->center;

    if (!randomized || star->randomized == 0) {
        // return the exact point
        return xy;
    } else { // randomize the point
        // find out the seed, unique for this point so that randomization is the same so long as the original point is stationary
        guint32 seed = point_unique_int (xy);
        // the full range (corresponding to star->randomized == 1.0) is equal to the star's diameter
        double range = 2 * MAX (star->r[0], star->r[1]);
        // find out the random displacement; x is controlled by step 1 from the seed, y by the step 2
        Geom::Point shift (star->randomized * range * rnd (seed, 1), star->randomized * range * rnd (seed, 2));
        // add the shift to the exact point
        return xy + shift;
    }
}

/**
 * Turn the star upright.
 */
void SPStar::turn_upright()
{
    if (sides) {
        auto arg1 = arg[0];
        auto arg2 = arg[1];
        auto delta = arg2 - arg1;
        auto top = -M_PI / 2;
        auto odd = sides & 1;
        if (odd) {
            arg1 = top;
        } else {
            arg1 = top - M_PI / sides;
        }
        arg2 = arg1 + delta;

        setAttributeDouble("sodipodi:arg1", arg1);
        setAttributeDouble("sodipodi:arg2", arg2);

        // also revert rotation in transform
        double angle = std::atan2(transform[1], transform[0]) * (180 / M_PI);
        rotate_rel(Geom::Rotate::from_degrees(-angle));

        updateRepr();
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
