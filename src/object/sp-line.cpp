// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SVG <line> implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-line.h"

#include <glibmm/i18n.h>

#include "attributes.h"
#include "style.h"
#include "sp-guide.h"
#include "path/path-curve.h"

SPLine::SPLine() : SPShape() {
    this->x1.unset();
    this->y1.unset();
    this->x2.unset();
    this->y2.unset();
}

SPLine::~SPLine() = default;

void SPLine::build(SPDocument * document, Inkscape::XML::Node * repr) {
    SPShape::build(document, repr);

    this->readAttr(SPAttr::X1);
    this->readAttr(SPAttr::Y1);
    this->readAttr(SPAttr::X2);
    this->readAttr(SPAttr::Y2);
}

void SPLine::set(SPAttr key, const gchar* value) {
    /* fixme: we should really collect updates */

    switch (key) {
        case SPAttr::X1:
            this->x1.readOrUnset(value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::Y1:
            this->y1.readOrUnset(value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::X2:
            this->x2.readOrUnset(value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::Y2:
            this->y2.readOrUnset(value);
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;

        default:
            SPShape::set(key, value);
            break;
    }
}

void SPLine::update(SPCtx *ctx, guint flags) {
    if (flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG)) {
        SPStyle const *style = this->style;
        SPItemCtx const *ictx = (SPItemCtx const *) ctx;
        double const w = ictx->viewport.width();
        double const h = ictx->viewport.height();
        double const em = style->font_size.computed;
        double const ex = em * 0.5;  // fixme: get from pango or libnrtype.

        this->x1.update(em, ex, w);
        this->x2.update(em, ex, w);
        this->y1.update(em, ex, h);
        this->y2.update(em, ex, h);

        this->set_shape();
    }

    SPShape::update(ctx, flags);
}

Inkscape::XML::Node* SPLine::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = xml_doc->createElement("svg:line");
    }

    if (repr != this->getRepr()) {
        repr->mergeFrom(this->getRepr(), "id");
    }

    repr->setAttributeSvgDouble("x1", this->x1.computed);
    repr->setAttributeSvgDouble("y1", this->y1.computed);
    repr->setAttributeSvgDouble("x2", this->x2.computed);
    repr->setAttributeSvgDouble("y2", this->y2.computed);

    SPShape::write(xml_doc, repr, flags);

    return repr;
}

const char* SPLine::typeName() const {
    return "path";
}

const char* SPLine::displayName() const {
    return _("Line");
}

void SPLine::convert_to_guides() const {
    Geom::Point points[2];
    Geom::Affine const i2dt(this->i2dt_affine());

    points[0] = Geom::Point(this->x1.computed, this->y1.computed)*i2dt;
    points[1] = Geom::Point(this->x2.computed, this->y2.computed)*i2dt;

    SPGuide::createSPGuide(this->document, points[0], points[1]);
}


Geom::Affine SPLine::set_transform(Geom::Affine const &transform, Inkscape::StrokeTransform *preview) {
    if (preview) {
        preview->expansion = transform.descrim();
        return Geom::identity();
    }
    Geom::Point points[2];

    points[0] = Geom::Point(this->x1.computed, this->y1.computed);
    points[1] = Geom::Point(this->x2.computed, this->y2.computed);

    points[0] *= transform;
    points[1] *= transform;

    this->x1.computed = points[0][Geom::X];
    this->y1.computed = points[0][Geom::Y];
    this->x2.computed = points[1][Geom::X];
    this->y2.computed = points[1][Geom::Y];

    this->adjust_stroke(transform.descrim());

    this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);

    return Geom::identity();
}

void SPLine::set_shape()
{
    auto c = Geom::LineSegment{Geom::Point{x1.computed, y1.computed}, Geom::Point{x2.computed, y2.computed}};

    // *_insync does not call update, avoiding infinite recursion when set_shape is called by update
    setCurveInsync(path_from_curve(c));
    setCurveBeforeLPE(curve());

    // LPEs cannot be applied to lines. (the result can (generally) not be represented as SPLine)
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
