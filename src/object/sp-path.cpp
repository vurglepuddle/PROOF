// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SVG <path> implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   David Turner <novalis@gnu.org>
 *   Abhishek Sharma
 *   Johan Engelen
 *
 * Copyright (C) 2004 David Turner
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 * Copyright (C) 1999-2012 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-path.h"

#include <glibmm/i18n.h>
#include <glibmm/regex.h>

#include <2geom/curves.h>

#include "attributes.h"
#include "sp-guide.h"
#include "sp-lpe-item.h"
#include "style.h"

#include "path/path-curve.h"
#include "helper/geom-curves.h"
#include "live_effects/effect.h"
#include "live_effects/lpeobject-reference.h"
#include "live_effects/lpeobject.h"
#include "svg/svg.h"
#include "xml/repr.h"


#define noPATH_VERBOSE

gint SPPath::nodesInPath() const
{
    return _curve ? node_count(*_curve) : 0;
}

const char* SPPath::typeName() const {
    return "path";
}

const char* SPPath::displayName() const {
    return _("Path");
}

gchar* SPPath::description() const {
    int count = this->nodesInPath();
    char *lpe_desc = g_strdup("");
    
    if (hasPathEffect()) {
        Glib::ustring s;
        PathEffectList effect_list =  this->getEffectList();
        
        for (auto & it : effect_list)
        {
            LivePathEffectObject *lpeobj = it->lpeobject;
            
            if (!lpeobj || !lpeobj->get_lpe()) {
                break;
            }
            
            if (s.empty()) {
                s = lpeobj->get_lpe()->getName();
            } else {
                s = s + ", " + lpeobj->get_lpe()->getName();
            }
        }
        lpe_desc = g_strdup_printf(_(", path effect: %s"), s.c_str());
    }
    char *ret = g_strdup_printf(ngettext(
                "%i node%s", "%i nodes%s", count), count, lpe_desc);
    g_free(lpe_desc);
    return ret;
}

void SPPath::convert_to_guides() const {
    if (!this->_curve) {
        return;
    }

    std::list<std::pair<Geom::Point, Geom::Point> > pts;

    Geom::Affine const i2dt(this->i2dt_affine());
    Geom::PathVector const &pv = *_curve;
    
    for(const auto & pit : pv) {
        for(Geom::Path::const_iterator cit = pit.begin(); cit != pit.end_default(); ++cit) {
            // only add curves for straight line segments
            if( is_straight_curve(*cit) )
            {
                pts.emplace_back(cit->initialPoint() * i2dt, cit->finalPoint() * i2dt);
            }
        }
    }

    sp_guide_pt_pairs_to_guides(this->document, pts);
}

SPPath::SPPath() : SPShape(), connEndPair(this) {
}

SPPath::~SPPath() = default;

void SPPath::build(SPDocument *document, Inkscape::XML::Node *repr) {
    /* Are these calls actually necessary? */
    this->readAttr(SPAttr::MARKER);
    this->readAttr(SPAttr::MARKER_START);
    this->readAttr(SPAttr::MARKER_MID);
    this->readAttr(SPAttr::MARKER_END);

    sp_conn_end_pair_build(this);

    SPShape::build(document, repr);
    // Our code depends on 'd' being an attribute (LPE's, etc.). To support 'd' as a property, we
    // check it here (after the style property has been evaluated, this allows us to properly
    // handled precedence of property vs attribute). If we read in a 'd' set by styling, convert it
    // to an attribute. We'll convert it back on output.

    d_source = style->d.style_src;

    if (style->d.set &&

        (d_source == SPStyleSrc::STYLE_PROP || d_source == SPStyleSrc::STYLE_SHEET) ) {

        if (char const *d_val = style->d.value()) {
            // Chrome shipped with a different syntax for property vs attribute.
            // The SVG Working group decided to follow the Chrome syntax (which may
            // allow future extensions of the 'd' property). The property syntax
            // wraps the path data with "path(...)". We must strip that!

            // Must be Glib::ustring or we get conversion errors!
            Glib::ustring input = d_val;
            Glib::ustring expression = R"A(path\("(.*)"\))A";
            Glib::RefPtr<Glib::Regex> regex = Glib::Regex::create(expression);
            Glib::MatchInfo matchInfo;
            regex->match(input, matchInfo);

            if (matchInfo.matches()) {
                Glib::ustring value = matchInfo.fetch(1);

                // Update curve
                setCurveInsync(sp_svg_read_pathv(value.c_str()));

                // Convert from property to attribute (convert back on write)
                setAttributeOrRemoveIfEmpty("d", value);

                SPCSSAttr *css = sp_repr_css_attr( getRepr(), "style");
                sp_repr_css_unset_property ( css, "d");
                sp_repr_css_set ( getRepr(), css, "style" );
                sp_repr_css_attr_unref ( css );

                style->d.style_src = SPStyleSrc::ATTRIBUTE;
            }
        }
        // If any if statement is false, do nothing... don't overwrite 'd' from attribute
    }

    this->readAttr(SPAttr::INKSCAPE_ORIGINAL_D);
    this->readAttr(SPAttr::D);

    /* d is a required attribute */
    char const *d = this->getAttribute("d");

    if (d == nullptr) {
        // First see if calculating the path effect will generate "d":
        this->update_patheffect(true);
        d = this->getAttribute("d");

        // I guess that didn't work, now we have nothing useful to write ("")
        if (d == nullptr) {
            this->setKeyValue( sp_attribute_lookup("d"), "");
        }
    }
}

void SPPath::release() {
    this->connEndPair.release();

    SPShape::release();
}

void SPPath::set(SPAttr key, const gchar* value) {
    switch (key) {
        case SPAttr::INKSCAPE_ORIGINAL_D:
            if (value) {
                setCurveBeforeLPE(sp_svg_read_pathv(value));
            } else {
                setCurveBeforeLPE(nullptr);
            }
            break;

       case SPAttr::D:
            if (value) {
                setCurve(sp_svg_read_pathv(value));
            } else {
                setCurve(nullptr);
            }
            break;

        case SPAttr::MARKER:
            set_marker(SP_MARKER_LOC, value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;
        case SPAttr::MARKER_START:
            set_marker(SP_MARKER_LOC_START, value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;
        case SPAttr::MARKER_MID:
            set_marker(SP_MARKER_LOC_MID, value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;
        case SPAttr::MARKER_END:
            set_marker(SP_MARKER_LOC_END, value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::CONNECTOR_TYPE:
        case SPAttr::CONNECTOR_CURVATURE:
        case SPAttr::CONNECTION_START:
        case SPAttr::CONNECTION_END:
        case SPAttr::CONNECTION_START_POINT:
        case SPAttr::CONNECTION_END_POINT:
            this->connEndPair.setAttr(key, value);
            break;

        default:
            SPShape::set(key, value);
            break;
    }
}

Inkscape::XML::Node* SPPath::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = xml_doc->createElement("svg:path");
    }

#ifdef PATH_VERBOSE
g_message("sp_path_write writes 'd' attribute");
#endif

    if (_curve) {
        repr->setAttribute("d", sp_svg_write_path(*_curve));
    } else {
        repr->removeAttribute("d");
    }

    if (flags & SP_OBJECT_WRITE_EXT) {
        if (_curve_before_lpe) {
            repr->setAttribute("inkscape:original-d", sp_svg_write_path(*_curve_before_lpe));
        } else {
            repr->removeAttribute("inkscape:original-d");
        }
    }

    this->connEndPair.writeRepr(repr);

    SPShape::write(xml_doc, repr, flags);

    return repr;
}

void SPPath::update_patheffect(bool write) {
    SPShape::update_patheffect(write);
}

void SPPath::update(SPCtx *ctx, guint flags) {
    if (flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG)) {
        flags &= ~SP_OBJECT_USER_MODIFIED_FLAG_B; // since we change the description, it's not a "just translation" anymore
    }

    SPShape::update(ctx, flags);
    this->connEndPair.update();
}

Geom::Affine SPPath::set_transform(Geom::Affine const &transform, Inkscape::StrokeTransform *preview) {
    if (!_curve) { // 0 nodes, nothing to transform
        return Geom::identity();
    }
    if (pathEffectsEnabled() && !optimizeTransforms()) {
        return transform;
    }
    if (preview) {
        preview->expansion = transform.descrim();
        return Geom::identity();
    }
    if (hasPathEffectRecursive() && pathEffectsEnabled()) {
        if (!_curve_before_lpe) {
            // we are inside a LPE group creating a new element 
            // and the original-d curve is not defined, 
            // This fix a issue with calligrapic tool that make a transform just when draw
            setCurveBeforeLPE(_curve.get());
        }
        *_curve_before_lpe *= transform;
        // fix issue https://gitlab.com/inkscape/inbox/-/issues/5460
        sp_lpe_item_update_patheffect(this, false, false);
    } else {
        setCurve(*_curve * transform);
    }
    // Adjust stroke
    this->adjust_stroke(transform.descrim());

    // Adjust pattern fill
    this->adjust_pattern(transform);

    // Adjust gradient fill
    this->adjust_gradient(transform);

    // nothing remains - we've written all of the transform, so return identity
    return Geom::identity();
}

void SPPath::removeTransformsRecursively(SPObject const *root)
{
    if (!_curve)
        return;

    auto transform = i2i_affine(root, this).inverse();

    if (hasPathEffectRecursive() && pathEffectsEnabled()) {
        *_curve_before_lpe *= transform;
        sp_lpe_item_update_patheffect(this, false, false);
    } else {
        setCurve(*_curve * transform);
    }
    setAttribute("d", sp_svg_write_path(*_curve));
    adjust_stroke(transform.descrim());
    adjust_pattern(transform);
    adjust_gradient(transform);
    adjust_clip(transform, true);
    removeAttribute("transform");
    remove_clip_transforms();
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
