// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_ITEM_H
#define SEEN_SP_ITEM_H

/**
 * @file
 * Some things pertinent to all visible shapes: SPItem, SPItemView, SPItemCtx.
 */

/*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2006 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 * Copyright (C) 2004 Monash University
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstdint>
#include <vector>
#include <2geom/forward.h>
#include <2geom/affine.h>
#include <2geom/rect.h>

#include "colors/color.h"

#include "sp-object.h"
#include "sp-marker-loc.h"
#include "display/drawing-item-ptr.h"
#include "display/stroke-transform.h"
#include "xml/repr.h"

class SPGroup;
class SPClipPath;
class SPClipPathReference;
class SPMask;
class SPMaskReference;
class SPAvoidRef;
class SPPattern;
struct SPPrintContext;

namespace Inkscape {
class Drawing;
class DrawingItem;
class URIReference;
class SnapCandidatePoint;
class SnapPreferences;
} // namespace Inkscape

// TODO make a completely new function that transforms either the fill or
// stroke of any SPItem  without adding an extra parameter to adjust_pattern.
enum PaintServerTransform { TRANSFORM_BOTH, TRANSFORM_FILL, TRANSFORM_STROKE };

struct SPItemView
{
    unsigned flags;
    unsigned key;
    DrawingItemPtr<Inkscape::DrawingItem> drawingitem;
    SPItemView(unsigned flags, unsigned key, DrawingItemPtr<Inkscape::DrawingItem> drawingitem);
};

enum SPItemKey
{
    ITEM_KEY_CLIP,
    ITEM_KEY_MASK,
    ITEM_KEY_FILL,
    ITEM_KEY_STROKE,
    ITEM_KEY_MARKERS,
    ITEM_KEY_SIZE = ITEM_KEY_MARKERS + SP_MARKER_LOC_QTY
};

/* flags */

#define SP_ITEM_BBOX_VISUAL 1

#define SP_ITEM_SHOW_DISPLAY (1 << 0)

/**
 * Flag for referenced views (i.e. markers, clippaths, masks and patterns);
 * currently unused, does the same as DISPLAY
 */
#define SP_ITEM_REFERENCE_FLAGS (1 << 1)

/**
 * Contains transformations to document/viewport and the viewport size.
 */
struct SPItemCtx : SPCtx
{
    /** Item to document transformation */
    Geom::Affine i2doc;

    /** Viewport size */
    Geom::Rect viewport;

    /** Item to viewport transformation */
    Geom::Affine i2vp;
};

/**
 * Base class for visual SVG elements.
 * SPItem is an abstract base class for all graphic (visible) SVG nodes. It
 * is a subclass of SPObject, with great deal of specific functionality.
 */
class SPItem : public SPObject
{
public:
    enum BBoxType
    {
        // legacy behavior: includes crude stroke, markers; excludes long miters, blur margin; is known to be wrong for caps
        APPROXIMATE_BBOX,
        // includes only the bare path bbox, no stroke, no nothing
        GEOMETRIC_BBOX,
        // includes everything: correctly done stroke (with proper miters and caps), markers, filter margins (e.g. blur)
        VISUAL_BBOX
    };

    enum PaintServerType { PATTERN, HATCH, GRADIENT };

    SPItem();
    ~SPItem() override;
    int tag() const override { return tag_of<decltype(*this)>; }

    unsigned int sensitive : 1;
    unsigned int stop_paint: 1;
    mutable unsigned bbox_valid : 1;
    double transform_center_x;
    double transform_center_y;
    bool freeze_stroke_width;

    // Used in the layers/objects dialog, this remembers if this item's
    // children are visible in the expanded state in the tree.
    bool _is_expanded = false;

    Geom::Affine transform;
    mutable Geom::OptRect doc_bbox;
    Geom::Rect viewport;  // Cache viewport information

    SPClipPath *getClipObject() const;
    SPMask *getMaskObject() const;

    SPClipPathReference &getClipRef();
    SPMaskReference &getMaskRef();

    std::optional<Geom::PathVector> getClipPathVector() const;
    std::optional<Geom::PathVector> getClipPathVector(SPItem const *root) const;
    SPText const *getClipTextObject() const;

    SPAvoidRef &getAvoidRef();
    std::vector<std::pair <Glib::ustring, Glib::ustring> > rootsatellites;

private:
    SPClipPathReference *clip_ref;
    SPMaskReference *mask_ref;

    // Used for object-avoiding connectors
    SPAvoidRef *avoidRef;

public:
    std::vector<SPItemView> views;

    sigc::signal<void (Geom::Affine const *, SPItem *)> _transformed_signal;

    bool isLocked() const;
    void setLocked(bool lock, bool recursive = false);

    bool isHidden() const;
    void setHidden(bool hidden);

    // Objects dialogue
    bool isSensitive() const {
        return sensitive;
    };

    void setHighlight(Inkscape::Colors::Color color);
    bool isHighlightSet() const;
    virtual Inkscape::Colors::Color highlight_color() const;

    //====================

    bool isEvaluated() const;
    void setEvaluated(bool visible);
    void resetEvaluated();
    bool unoptimized();
    bool isHidden(unsigned display_key) const;

    /**
     * Returns something suitable for the `Hide' checkbox in the Object Properties dialog box.
     *  Corresponds to setExplicitlyHidden.
     */
    bool isExplicitlyHidden() const;

    /**
     * Sets the display CSS property to `hidden' if \a val is true,
     * otherwise makes it unset.
     */
    void setExplicitlyHidden(bool val);

    /**
     * Sets the transform_center_x and transform_center_y properties to retain the rotation center
     */
    void setCenter(Geom::Point const &object_centre);
    bool updateCenterIfSet(Geom::Point const &center);

    void unsetCenter();
    bool isCenterSet() const;
    Geom::Point getCenter(bool ensure_uptodate = true) const;
    void scaleCenter(Geom::Scale const &sc);

    bool isVisibleAndUnlocked() const;

    bool isVisibleAndUnlocked(unsigned display_key) const;

    Geom::Affine getRelativeTransform(SPObject const *obj) const;

    bool raiseOne();
    bool lowerOne();
    void raiseToTop();
    void lowerToBottom();

    SPGroup *getParentGroup() const;

    /**
     * Move this SPItem into or after another SPItem in the doc.
     *
     * @param target the SPItem to move into or after.
     * @param intoafter move to after the target (false), move inside (sublayer) of the target (true).
     */
    void moveTo(SPItem *target, bool intoafter);

    sigc::connection connectTransformed(sigc::slot<void (Geom::Affine const *, SPItem *)> slot)  {
        return _transformed_signal.connect(slot);
    }

    /**
     * Get item's geometric bounding box in this item's coordinate system.
     *
     * The geometric bounding box includes only the path, disregarding all style attributes.
     */
    Geom::OptRect geometricBounds(Geom::Affine const &transform = Geom::identity()) const;

    /**
     * Get item's visual bounding box in this item's coordinate system.
     *
     * The visual bounding box includes the stroke and the filter region.
     * @param wfilter use filter expand in bbox calculation
     * @param wclip use clip data in bbox calculation
     * @param wmask use mask data in bbox calculation
     */
    Geom::OptRect visualBounds(Geom::Affine const &transform = Geom::identity(), bool wfilter = true, bool wclip = true,
                               bool wmask = true) const;

    Geom::OptRect bounds(BBoxType type, Geom::Affine const &transform = Geom::identity()) const;

    /**
     * Get item's geometric bbox in document coordinate system.
     * Document coordinates are the default coordinates of the root element:
     * the origin is at the top left, X grows to the right and Y grows downwards.
     */
    Geom::OptRect documentGeometricBounds() const;

    /**
     * Get item's visual bbox in document coordinate system.
     */
    Geom::OptRect documentVisualBounds() const;

    Geom::OptRect documentBounds(BBoxType type) const;
    Geom::OptRect documentPreferredBounds() const;

    /**
     * Get an exact geometric shape representing the visual bounds of the item in the document
     * coordinates. This is different than a simple bounding rectangle aligned to the coordinate axes:
     * the returned pathvector may effectively describe any shape and coincides with an appropriately
     * transformed path-vector for paths. Even for rectangular items such as SPImage, the bounds may be
     * a parallelogram resulting from transforming the bounding rectangle by an affine transformation.
     */
    virtual std::optional<Geom::PathVector> documentExactBounds() const;

    /**
     * Get item's geometric bbox in desktop coordinate system.
     * Desktop coordinates should be user defined. Currently they are hardcoded:
     * origin is at bottom left, X grows to the right and Y grows upwards.
     */
    Geom::OptRect desktopGeometricBounds() const;

    /**
     * Get item's visual bbox in desktop coordinate system.
     */
    Geom::OptRect desktopVisualBounds() const;

    Geom::OptRect desktopPreferredBounds() const;
    Geom::OptRect desktopBounds(BBoxType type) const;

    unsigned int pos_in_parent() const;

    /**
     * Returns a string suitable for status bar, formatted in pango markup language.
     *
     * Must be freed by caller.
     */
    char *detailedDescription() const;

    /**
     * Returns true if the item is filtered, false otherwise.
     * Used with groups/lists to determine how many, or if any, are filtered.
     */
    bool isFiltered() const;

    SPObject* isInMask() const;

    SPObject* isInClipPath() const;

    void invoke_print(SPPrintContext *ctx);

    /**
     * Allocates unique integer keys.
     *
     * @param numkeys Number of keys required.
     * @return First allocated key; hence if the returned key is n
     * you can use n, n + 1, ..., n + (numkeys - 1)
     */
    static unsigned int display_key_new(unsigned numkeys);

    /**
     * Ensures that a drawing item's key is the first of a block of ITEM_KEY_SIZE keys,
     * assigning it such a key if necessary.
     *
     * @return The value of di->key() after assignment.
     */
    static unsigned ensure_key(Inkscape::DrawingItem *di);

    Inkscape::DrawingItem *invoke_show(Inkscape::Drawing &drawing, unsigned int key, unsigned int flags);

    // Removed item from display tree.
    void invoke_hide(unsigned int key);
    void invoke_hide_except(unsigned key, const std::vector<SPItem const *> &to_keep);

    void getSnappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs=nullptr) const;
    void adjust_pattern(/* Geom::Affine const &premul, */ Geom::Affine const &postmul, bool set = false,
                        PaintServerTransform = TRANSFORM_BOTH);
    void adjust_hatch(/* Geom::Affine const &premul, */ Geom::Affine const &postmul, bool set = false,
                      PaintServerTransform = TRANSFORM_BOTH);
    void adjust_gradient(/* Geom::Affine const &premul, */ Geom::Affine const &postmul, bool set = false);
    void adjust_clip(/* Geom::Affine const &premul, */ Geom::Affine const &postmul, bool set = false);
    void remove_clip_transforms();
    void adjust_stroke(double ex);

    /**
     * Recursively scale stroke width in \a item and its children by \a expansion.
     */
    void adjust_stroke_width_recursive(double ex);

    void freeze_stroke_width_recursive(bool freeze);

    /**
     * Recursively compensate pattern or gradient transform.
     */
    void adjust_paint_recursive(Geom::Affine advertized_transform, Geom::Affine t_ancestors,
                                PaintServerType type = GRADIENT);

    /**
     * Checks for visual collision with another item
     */
    bool collidesWith(Geom::PathVector const &shape) const;
    bool collidesWith(SPItem const &other) const;

    /**
     * Set a new transform on an object.
     *
     * Compensate for stroke scaling and gradient/pattern fill transform, if
     * necessary. Call the object's set_transform method if transforms are
     * stored optimized. Send _transformed_signal. Invoke _write method so that
     * the repr is updated with the new transform.
     */
    void doWriteTransform(Geom::Affine const &transform, Geom::Affine const *adv = nullptr, bool compensate = true);

    Inkscape::StrokeTransform getStrokeTransform(Geom::Affine const &transform);
    void setTransformPreview(unsigned key, std::optional<Inkscape::StrokeTransform> const &stroke);
    bool canEmbedTransform(Geom::Affine const &transform);

    /**
     * Sets item private transform (not propagated to repr), without compensating stroke widths,
     * gradients, patterns as sp_item_write_transform does.
     */
    void set_item_transform(Geom::Affine const &transform_matrix);

    /**
     * Return the arenaitem corresponding to the given item in the display
     * with the given key
     */
    Inkscape::DrawingItem *get_arenaitem(unsigned key) const;

    /**
     * Returns the accumulated transformation of the item and all its ancestors, including root's viewport.
     */
    Geom::Affine i2doc_affine() const;

    /**
     * Returns the transformation from item to desktop coords.
     */
    Geom::Affine i2dt_affine() const;

    void set_i2d_affine(Geom::Affine const &transform);

    /**
     * Returns the transformation from desktop to item coords.
     */
    Geom::Affine dt2i_affine() const;

    std::optional<Inkscape::Colors::Color> _highlightColor;

    bool isExpanded() const { return _is_expanded; }
    void setExpanded(bool expand) { _is_expanded = expand; }

private:
    enum EvaluatedStatus
    {
        StatusUnknown,
        StatusCalculated,
        StatusSet
    };

    mutable bool _is_evaluated;
    mutable EvaluatedStatus _evaluated_status;

    void clip_ref_changed(SPObject *old_clip, SPObject *clip);
    void mask_ref_changed(SPObject *old_mask, SPObject *mask);
    void fill_ps_ref_changed(SPObject *old_ps, SPObject *ps);
    void stroke_ps_ref_changed(SPObject *old_ps, SPObject *ps);
    void filter_ref_changed(SPObject *old_obj, SPObject *obj);

public:
    void rotate_rel(Geom::Rotate const &rotation);
    void scale_rel(Geom::Scale const &scale);
    void skew_rel(double skewX, double skewY);
    void move_rel( Geom::Translate const &tr);
	void build(SPDocument *document, Inkscape::XML::Node *repr) override;
	void release() override;
	void set(SPAttr key, char const* value) override;
	void update(SPCtx *ctx, unsigned int flags) override;
    void modified(unsigned int flags) override;
	Inkscape::XML::Node* write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, unsigned int flags) override;

	virtual Geom::OptRect bbox(Geom::Affine const &transform, SPItem::BBoxType type) const;
	virtual void print(SPPrintContext *ctx);
    virtual const char* typeName() const;
    virtual const char* displayName() const;
	virtual char* description() const;
	virtual Inkscape::DrawingItem* show(Inkscape::Drawing &drawing, unsigned int key, unsigned int flags);
	virtual void hide(unsigned int key);
    virtual void snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const;
    // With preview, return the residual affine and stroke expansion without changing the item.
    virtual Geom::Affine set_transform(Geom::Affine const &transform, Inkscape::StrokeTransform *preview = nullptr);
    virtual void removeTransformsRecursively(SPObject const *root);

    virtual void convert_to_guides() const;
};

// Utility

/**
 * @pre \a ancestor really is an ancestor (\>=) of \a object, or NULL.
 *   ("Ancestor (\>=)" here includes as far as \a object itself.)
 */
Geom::Affine i2anc_affine(SPObject const *item, SPObject const *ancestor);

Geom::Affine i2i_affine(SPObject const *src, SPObject const *dest);

Geom::Affine sp_item_transform_repr(SPItem *item);

/* fixme: - these are evil, but OK */

int sp_item_repr_compare_position(SPItem const *first, SPItem const *second);

inline bool sp_item_repr_compare_position_bool(SPItem const *first, SPItem const *second)
{
    return sp_item_repr_compare_position(first, second) < 0;
}

SPItem *sp_item_first_item_child(SPObject *obj);
SPItem const *sp_item_first_item_child(SPObject const *obj);

#endif // SEEN_SP_ITEM_H

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
