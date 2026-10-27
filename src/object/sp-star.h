// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_STAR_H
#define SEEN_SP_STAR_H

/*
 * <sodipodi:star> implementation
 *
 * Authors:
 *   Mitsuru Oka <oka326@parkcity.ne.jp>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-polygon.h"

enum SPStarPoint {
	SP_STAR_POINT_KNOT1,
	SP_STAR_POINT_KNOT2
};

class SPStar final : public SPShape {
public:
	SPStar();
	~SPStar() override;
    int tag() const override { return tag_of<decltype(*this)>; }

	int sides;

	Geom::Point center;
	double r[2];
	double arg[2];
	bool flatsided;

	double rounded;
	double randomized;

	void build(SPDocument *document, Inkscape::XML::Node *repr) override;
	Inkscape::XML::Node* write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, unsigned int flags) override;
	void set(SPAttr key, char const* value) override;
	void update(SPCtx* ctx, unsigned int flags) override;

        const char* typeName() const override;
        const char* displayName() const override;
	char* description() const override;
	void snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const override;
    void update_patheffect(bool write) override;
	void set_shape() override;
	Geom::Affine set_transform(Geom::Affine const& xform, Inkscape::StrokeTransform *preview = nullptr) override;
	double getSideLength() const;
	void setSideLength(double length);
	void turn_upright();
};

void sp_star_position_set (SPStar *star, int sides, Geom::Point center, double r1, double r2, double arg1, double arg2, bool isflat, double rounded, double randomized);

Geom::Point sp_star_get_xy (SPStar const *star, SPStarPoint point, int index, bool randomized = false);

#endif
