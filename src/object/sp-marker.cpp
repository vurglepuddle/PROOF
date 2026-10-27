// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SVG <marker> implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Bryce Harrington <bryce@bryceharrington.org>
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 1999-2003 Lauris Kaplinski
 *               2004-2006 Bryce Harrington
 *               2008      Johan Engelen
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-marker.h"

#include <cassert>
#include <cstring>

#include <glib/gi18n.h>

#include <2geom/affine.h>
#include <2geom/transforms.h>

#include "attributes.h"
#include "document-undo.h"
#include "document.h"
#include "preferences.h"
#include "sp-defs.h"
#include "marker-transform-preview.h"

#include "display/drawing-group.h"
#include "display/drawing-item-ptr.h"
#include "object/object-set.h"
#include "svg/css-ostringstream.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "xml/document.h"              // for Document

using Inkscape::DocumentUndo;
using Inkscape::ObjectSet;

struct SPMarkerView
{
    std::vector<DrawingItemPtr<Inkscape::DrawingItem>> items;
};

void sp_marker_set_transform_preview(SPMarker &marker, unsigned key,
                                   std::optional<Geom::Affine> const &transform,
                                   std::optional<unsigned> position)
{
    auto view = marker.views_map.find(key);
    if (view == marker.views_map.end()) return;
    if (position) {
        if (*position < view->second.items.size() && view->second.items[*position]) {
            view->second.items[*position]->setTransformPreview(transform);
        }
    } else {
        for (auto &item : view->second.items) if (item) item->setTransformPreview(transform);
    }
}

SPMarker::SPMarker() : SPGroup(), SPViewBox(),
    markerUnits_set(0),
    markerUnits(0),
    refX(),
    refY(),
    markerWidth(),
    markerHeight(),
    orient_set(0),
    orient_mode(MARKER_ORIENT_ANGLE)
{
    // cppcheck-suppress useInitializationList
	orient = 0;
}

/**
 * Initializes an SPMarker object.  This notes the marker's viewBox is
 * not set and initializes the marker's c2p identity matrix.
 */

SPMarker::~SPMarker() = default;

/**
 * Virtual build callback for SPMarker.
 *
 * This is to be invoked immediately after creation of an SPMarker.  This
 * method fills an SPMarker object with its SVG attributes, and calls the
 * parent class' build routine to attach the object to its document and
 * repr.  The result will be creation of the whole document tree.
 *
 * \see SPObject::build()
 */
void SPMarker::build(SPDocument *document, Inkscape::XML::Node *repr) {
    this->readAttr(SPAttr::MARKERUNITS);
    this->readAttr(SPAttr::REFX);
    this->readAttr(SPAttr::REFY);
    this->readAttr(SPAttr::MARKERWIDTH);
    this->readAttr(SPAttr::MARKERHEIGHT);
    this->readAttr(SPAttr::ORIENT);
    this->readAttr(SPAttr::VIEWBOX);
    this->readAttr(SPAttr::PRESERVEASPECTRATIO);
    this->readAttr(SPAttr::STYLE);

    SPGroup::build(document, repr);
}


/**
 * Removes, releases and unrefs all children of object
 *
 * This is the inverse of sp_marker_build().  It must be invoked as soon
 * as the marker is removed from the tree, even if it is still referenced
 * by other objects.  It hides and removes any views of the marker, then
 * calls the parent classes' release function to deregister the object
 * and release its SPRepr bindings.  The result will be the destruction
 * of the entire document tree.
 *
 * \see SPObject::release()
 */
void SPMarker::release() {

    for (auto &it : views_map) {
        SPGroup::hide(it.first);
    }
    views_map.clear();

    SPGroup::release();
}

void SPMarker::set(SPAttr key, const gchar* value) {
	switch (key) {
	case SPAttr::MARKERUNITS:
		this->markerUnits_set = FALSE;
		this->markerUnits = SP_MARKER_UNITS_STROKEWIDTH;

		if (value) {
			if (!strcmp (value, "strokeWidth")) {
				this->markerUnits_set = TRUE;
			} else if (!strcmp (value, "userSpaceOnUse")) {
				this->markerUnits = SP_MARKER_UNITS_USERSPACEONUSE;
				this->markerUnits_set = TRUE;
			}
		}

		this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG);
		break;

	case SPAttr::REFX:
	    this->refX.readOrUnset(value);
		this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
		break;

	case SPAttr::REFY:
	    this->refY.readOrUnset(value);
		this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
		break;

	case SPAttr::MARKERWIDTH:
	    this->markerWidth.readOrUnset(value, SVGLength::NONE, 3.0, 3.0);
		this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
		break;

	case SPAttr::MARKERHEIGHT:
	    this->markerHeight.readOrUnset(value, SVGLength::NONE, 3.0, 3.0);
		this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
		break;

	case SPAttr::ORIENT:
		this->orient_set = FALSE;
		this->orient_mode = MARKER_ORIENT_ANGLE;
		this->orient = 0.0;

		if (value) {
                    if (!strcmp (value, "auto")) {
                        this->orient_mode = MARKER_ORIENT_AUTO;
                        this->orient_set = TRUE;
                    } else if (!strcmp (value, "auto-start-reverse")) {
                        this->orient_mode = MARKER_ORIENT_AUTO_START_REVERSE;
                        this->orient_set = TRUE;
                    } else {
                        orient.readOrUnset(value);
                        if (orient._set) {
                            this->orient_mode = MARKER_ORIENT_ANGLE;
                            this->orient_set = orient._set;
                        }
                    }
		}
		this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
		break;

	case SPAttr::VIEWBOX:
            set_viewBox( value );
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG);
            break;

	case SPAttr::PRESERVEASPECTRATIO:
            set_preserveAspectRatio( value );
            this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_VIEWPORT_MODIFIED_FLAG);
            break;

	default:
		SPGroup::set(key, value);
		break;
	}
}

void SPMarker::update(SPCtx *ctx, guint flags) {

    SPItemCtx ictx;

    // Copy parent context
    ictx.flags = ctx->flags;

    // Initialize transformations
    ictx.i2doc = Geom::identity();
    ictx.i2vp = Geom::identity();

    // Set up viewport
    ictx.viewport = Geom::Rect::from_xywh(0, 0, this->markerWidth.computed, this->markerHeight.computed);

    SPItemCtx rctx = get_rctx( &ictx );

    // Shift according to refX, refY
    Geom::Point ref( this->refX.computed, this->refY.computed );
    ref *= c2p;
    this->c2p = this->c2p * Geom::Translate( -ref );

    // And invoke parent method
    SPGroup::update((SPCtx *) &rctx, flags);

    // As last step set additional transform of drawing group
    for (auto &it : views_map) {
        for (auto &item : it.second.items) {
            if (item) {
                auto g = cast<Inkscape::DrawingGroup>(item.get());
                g->setChildTransform(c2p);
            }
        }
    }
}

Inkscape::XML::Node* SPMarker::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
	if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
		repr = xml_doc->createElement("svg:marker");
	}

	if (this->markerUnits_set) {
		if (this->markerUnits == SP_MARKER_UNITS_STROKEWIDTH) {
			repr->setAttribute("markerUnits", "strokeWidth");
		} else {
			repr->setAttribute("markerUnits", "userSpaceOnUse");
		}
	} else {
		repr->removeAttribute("markerUnits");
	}

	if (this->refX._set) {
		repr->setAttributeSvgDouble("refX", this->refX.computed);
	} else {
		repr->removeAttribute("refX");
	}

	if (this->refY._set) {
		repr->setAttributeSvgDouble("refY", this->refY.computed);
	} else {
		repr->removeAttribute("refY");
	}

	if (this->markerWidth._set) {
		repr->setAttributeSvgDouble("markerWidth", this->markerWidth.computed);
	} else {
		repr->removeAttribute("markerWidth");
	}

	if (this->markerHeight._set) {
		repr->setAttributeSvgDouble("markerHeight", this->markerHeight.computed);
	} else {
		repr->removeAttribute("markerHeight");
	}

	if (this->orient_set) {
            if (this->orient_mode == MARKER_ORIENT_AUTO) {
                repr->setAttribute("orient", "auto");
            } else if (this->orient_mode == MARKER_ORIENT_AUTO_START_REVERSE) {
                repr->setAttribute("orient", "auto-start-reverse");
            } else {
                repr->setAttributeCssDouble("orient", this->orient.computed);
            }
	} else {
            repr->removeAttribute("orient");
	}

    this->write_viewBox(repr);
    this->write_preserveAspectRatio(repr);

	SPGroup::write(xml_doc, repr, flags);

	return repr;
}

Inkscape::DrawingItem* SPMarker::show(Inkscape::Drawing &/*drawing*/, unsigned int /*key*/, unsigned int /*flags*/) {
    // Markers in tree are never shown directly even if outside of <defs>.
    return  nullptr;
}

Inkscape::DrawingItem* SPMarker::private_show(Inkscape::Drawing &drawing, unsigned int key, unsigned int flags) {
    return SPGroup::show(drawing, key, flags);
}

void SPMarker::hide(unsigned int key) {
	// CPPIFY: correct?
	SPGroup::hide(key);
}

/**
 * Calculate the transformation for this marker.
 */
Geom::Affine SPMarker::get_marker_transform(Geom::Affine const &base, double linewidth, bool start_marker) const
{
    // Default is MARKER_ORIENT_AUTO
    Geom::Affine result = base;

    if (this->orient_mode == MARKER_ORIENT_AUTO_START_REVERSE) {
        if (start_marker) {
            result = Geom::Rotate::from_degrees( 180.0 ) * base;
        }
    } else if (this->orient_mode != MARKER_ORIENT_AUTO) {
        /* fixme: Orient units (Lauris) */
        result = Geom::Rotate::from_degrees(this->orient.computed);
        result *= Geom::Translate(base.translation());
    }

    if (this->markerUnits == SP_MARKER_UNITS_STROKEWIDTH) {
        result = Geom::Scale(linewidth) * result;
    }
    return result;
}

/* 
- used to validate the marker item before passing it into the shape editor from the marker-tool. 
- sets any missing properties that are needed before editing starts.
*/
void sp_validate_marker(SPMarker *sp_marker, SPDocument *doc) {

    if (!doc || !sp_marker) return;

    doc->ensureUpToDate();

    // calculate the marker bounds to set any missing viewBox information
    std::vector<SPObject*> items = const_cast<SPMarker*>(sp_marker)->childList(false, SPObject::ActionBBox);

    Geom::OptRect r;
    for (auto *i : items) {
        auto item = cast<SPItem>(i);
        r.unionWith(item->desktopVisualBounds());
    }

    Geom::Rect bounds(r->min() * doc->dt2doc(), r->max() * doc->dt2doc());

    if(!sp_marker->refX._set) {
        sp_marker->setAttribute("refX", "0.0");
    }

    if(!sp_marker->refY._set) {
        sp_marker->setAttribute("refY", "0.0");
    }

    if(!sp_marker->orient._set) {
        sp_marker->setAttribute("orient", "0.0");
    }

    double xScale = 1;
    double yScale = 1;

    if(sp_marker->viewBox_set) {
        // check if the X direction has any existing scale factor
        if(sp_marker->viewBox.width() > 0) {
            double existingXScale = sp_marker->markerWidth.computed/sp_marker->viewBox.width();
            xScale = (existingXScale >= 0? existingXScale: 1);
        }

        // check if the Y direction has any existing scale factor
        if(sp_marker->viewBox.height() > 0) {
            double existingYScale = sp_marker->markerHeight.computed/sp_marker->viewBox.height();
            yScale = (existingYScale >= 0? existingYScale: 1);
        }

        // only enforce uniform scale if the preserveAspectRatio is not set yet or if it does not equal "none"
        if((!sp_marker->aspect_set) || (sp_marker->aspect_align != SP_ASPECT_NONE)) {
            // set the scale to the smaller option if both xScale and yScale exist
            if(xScale > yScale) {
                xScale = yScale;
            } else {
                yScale = xScale;
            }
        }
    } else {
        Inkscape::CSSOStringStream os;
        os << "0 0 " << bounds.dimensions()[Geom::X] << " " << bounds.dimensions()[Geom::Y];
        sp_marker->setAttribute("viewBox", os.str().c_str());
    }

    sp_marker->setAttributeDouble("markerWidth", sp_marker->viewBox.width() * xScale);
    sp_marker->setAttributeDouble("markerHeight", sp_marker->viewBox.height() * yScale);

    if(!sp_marker->aspect_set) {
        // feedback from UX expert indicates that uniform scaling should be used by default;
        // marker tool should respect aspect ratio setting too (without Ctrl key modifier?)
        sp_marker->setAttribute("preserveAspectRatio", "xMidYMid");
    }
}

void SPMarker::print(SPPrintContext* /*ctx*/) {
}

/* fixme: Remove link if zero-sized (Lauris) */

/**
 * Removes any SPMarkerViews that a marker has with a specific key.
 * Set up the DrawingItem array's size in the specified SPMarker's SPMarkerView.
 * This is called from sp_shape_update() for shapes that have markers.  It
 * removes the old view of the marker and establishes a new one, registering
 * it with the marker's list of views for future updates.
 *
 * \param marker Marker to create views in.
 * \param key Key to give each SPMarkerView.
 * \param size Number of DrawingItems to put in the SPMarkerView.
 */
// If marker views are always created in order, then this function could be eliminated
// by doing the push_back in sp_marker_show_instance.
void
sp_marker_show_dimension (SPMarker *marker, unsigned int key, unsigned int size)
{
    auto it = marker->views_map.find(key);
    if (it != marker->views_map.end()) {
        if (it->second.items.size() != size ) {
            // Need to change size of vector! (We should not really need to do this.)
            marker->hide(key);
            it->second.items.clear();
            for (unsigned int i = 0; i < size; ++i) {
                it->second.items.push_back(nullptr);
            }
        }
    } else {
        marker->views_map[key] = SPMarkerView();
        for (unsigned int i = 0; i < size; ++i) {
            marker->views_map[key].items.push_back(nullptr);
        }
    }
}

/**
 * Shows an instance of a marker.  This is called during sp_shape_update_marker_view()
 * show and transform a child item in the drawing for all views with the given key.
 */
Inkscape::DrawingItem *
sp_marker_show_instance ( SPMarker *marker, Inkscape::DrawingItem *parent,
                          unsigned int loc, unsigned int pos, unsigned int z_order,
                          Geom::Affine const &marker_transform, float linewidth)
{
    // Do not show marker if linewidth == 0 and markerUnits == strokeWidth
    // otherwise Cairo will fail to render anything on the tile
    // that contains the "degenerate" marker.
    if (marker->markerUnits == SP_MARKER_UNITS_STROKEWIDTH && linewidth == 0) {
        return nullptr;
    }

    auto key = parent->key() + ITEM_KEY_MARKERS + loc;
    auto it = marker->views_map.find(key);
    if (it == marker->views_map.end()) {
        // Key not found
        return nullptr;
    }

    SPMarkerView *view = &it->second;
    if (pos >= view->items.size() ) {
        // Position index too large, doesn't exist.
        return nullptr;
    }

    // If not already created
    if (!view->items[pos]) {

        /* Parent class ::show method */
        view->items[pos].reset(marker->private_show(parent->drawing(), key, SP_ITEM_REFERENCE_FLAGS));

        if (view->items[pos]) {
            parent->appendChild(view->items[pos].get());
            if (auto g = cast<Inkscape::DrawingGroup>(view->items[pos].get())) {
                g->setChildTransform(marker->c2p);
            }
        }
    }

    if (view->items[pos]) {
        view->items[pos]->setTransform(marker_transform);
        view->items[pos]->setZOrder(z_order);
    }

    return view->items[pos].get();
}

/**
 * Hides/removes all views of the given marker that have key 'key'.
 * This replaces SPItem implementation because we have our own views
 * \param key SPMarkerView key to hide.
 */
void
sp_marker_hide (SPMarker *marker, unsigned int key)
{
    marker->hide(key);
    marker->views_map.erase(key);
}


const gchar *generate_marker(std::vector<Inkscape::XML::Node*> &reprs, Geom::Rect bounds, SPDocument *document, Geom::Point center, Geom::Affine move)
{
    Inkscape::XML::Document *xml_doc = document->getReprDoc();
    Inkscape::XML::Node *defsrepr = document->getDefs()->getRepr();

    Inkscape::XML::Node *repr = xml_doc->createElement("svg:marker");

    // Uncommenting this will make the marker fixed-size independent of stroke width.
    // Commented out for consistency with standard markers which scale when you change
    // stroke width:
    //repr->setAttribute("markerUnits", "userSpaceOnUse");

    repr->setAttributeSvgDouble("markerWidth", bounds.dimensions()[Geom::X]);
    repr->setAttributeSvgDouble("markerHeight", bounds.dimensions()[Geom::Y]);
    repr->setAttributeSvgDouble("refX", center[Geom::X]);
    repr->setAttributeSvgDouble("refY", center[Geom::Y]);

    repr->setAttribute("orient", "auto");

    defsrepr->appendChild(repr);
    const gchar *mark_id = repr->attribute("id");
    SPObject *mark_object = document->getObjectById(mark_id);

    for (auto node : reprs){
        auto copy = cast<SPItem>(mark_object->appendChildRepr(node));

        Geom::Affine dup_transform;
        if (!sp_svg_transform_read (node->attribute("transform"), &dup_transform))
            dup_transform = Geom::identity();
        dup_transform *= move;

        copy->doWriteTransform(dup_transform);
    }

    Inkscape::GC::release(repr);
    return mark_id;
}

SPObject *sp_marker_fork_if_necessary(SPObject *marker)
{
    if (marker->hrefcount < 2) {
        return marker;
    }

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    gboolean colorStock = prefs->getBool("/options/markers/colorStockMarkers", true);
    gboolean colorCustom = prefs->getBool("/options/markers/colorCustomMarkers", false);
    const gchar *stock = marker->getRepr()->attribute("inkscape:isstock");
    gboolean isStock = (!stock || !strcmp(stock,"true"));

    if (isStock ? !colorStock : !colorCustom) {
        return marker;
    }

    SPDocument *doc = marker->document;
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();
    // Turn off garbage-collectable or it might be collected before we can use it
    marker->removeAttribute("inkscape:collect");
    Inkscape::XML::Node *mark_repr = marker->getRepr()->duplicate(xml_doc);
    doc->getDefs()->getRepr()->appendChild(mark_repr);
    if (!mark_repr->attribute("inkscape:stockid")) {
        mark_repr->setAttribute("inkscape:stockid", mark_repr->attribute("id"));
    }
    marker->setAttribute("inkscape:collect", "always");

    SPObject *marker_new = static_cast<SPObject *>(doc->getObjectByRepr(mark_repr));
    Inkscape::GC::release(mark_repr);
    return marker_new;
}

void sp_marker_set_orient(SPMarker* marker, const char* value) {
    if (!marker || !value) return;

    marker->setAttribute("orient", value);

    if (marker->document) {
        DocumentUndo::maybeDone(marker->document, "marker", RC_("Undo", "Set marker orientation"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

void sp_marker_set_size(SPMarker* marker, double sx, double sy) {
    if (!marker) return;

    marker->setAttributeDouble("markerWidth", sx);
    marker->setAttributeDouble("markerHeight", sy);

    if (marker->document) {
        DocumentUndo::maybeDone(marker->document, "marker", RC_("Undo", "Set marker size"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

void sp_marker_scale_with_stroke(SPMarker* marker, bool scale_with_stroke) {
    if (!marker) return;

    marker->setAttribute("markerUnits", scale_with_stroke ? "strokeWidth" : "userSpaceOnUse");

    if (marker->document) {
        DocumentUndo::maybeDone(marker->document, "marker", RC_("Undo", "Set marker scale with stroke"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

void sp_marker_set_offset(SPMarker* marker, double dx, double dy) {
    if (!marker) return;

    marker->setAttributeDouble("refX", dx);
    marker->setAttributeDouble("refY", dy);

    if (marker->document) {
        DocumentUndo::maybeDone(marker->document, "marker", RC_("Undo", "Set marker offset"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

void sp_marker_set_uniform_scale(SPMarker* marker, bool uniform) {
    if (!marker) return;

    marker->setAttribute("preserveAspectRatio", uniform ? "xMidYMid" : "none");

    if (marker->document) {
        DocumentUndo::maybeDone(marker->document, "marker", RC_("Undo", "Set marker uniform scaling"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

void sp_marker_flip_horizontally(SPMarker* marker) {
    if (!marker) return;

    ObjectSet set(marker->document);
    set.addList(marker->item_list());
    Geom::OptRect bbox = set.visualBounds();
    if (bbox) {
        set.scaleRelative(bbox->midpoint(), Geom::Scale(-1.0, 1.0));
        if (marker->document) {
            DocumentUndo::maybeDone(marker->document, "marker", RC_("Undo", "Flip marker horizontally"), INKSCAPE_ICON("dialog-fill-and-stroke"));
        }
    }
}

void sp_marker_set_opacity(SPMarker* marker, double alpha) {
    assert(marker);
    assert(marker->document);

    if (!marker) return;

    // "opacity" alone doesn't have any impact; using fill and stroke opacity instead
    marker->setAttributeDouble("fill-opacity", alpha);
    marker->setAttributeDouble("stroke-opacity", alpha);

    if (marker->document) {
        DocumentUndo::maybeDone(marker->document, "marker", RC_("Undo", "Set marker opacity"), INKSCAPE_ICON("dialog-fill-and-stroke"));
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
