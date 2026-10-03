// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Abhishek Sharma
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2001-2006 authors
 * Copyright (C) 2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-item.h"

#include <algorithm>
#include <glibmm/i18n.h>

#include "attributes.h"
#include "box-frame.h"
#include "colors/manager.h"
#include "conditions.h"
#include "conn-avoid-ref.h"
#include "desktop.h"
#include "display/drawing-item.h"
#include "display/nr-filter.h"
#include "document.h"
#include "enums.h"
#include "filter-chemistry.h"
#include "gradient-chemistry.h"
#include "helper/geom.h"
#include "inkscape.h"
#include "live_effects/effect.h"
#include "live_effects/lpeobject-reference.h"
#include "live_effects/lpeobject.h"
#include "object/uri.h"
#include "path/path-util.h"
#include "preferences.h"
#include "print.h"
#include "snap-candidate.h"
#include "snap-preferences.h"
#include "sp-clippath.h"
#include "sp-desc.h"
#include "sp-guide.h"
#include "sp-hatch.h"
#include "sp-mask.h"
#include "sp-pattern.h"
#include "sp-rect.h"
#include "sp-root.h"
#include "sp-switch.h"
#include "sp-text.h"
#include "sp-textpath.h"
#include "sp-title.h"
#include "sp-use.h"
#include "style.h"
#include "svg/svg.h"
#include "ui/util.h"
#include "util/units.h"
#include "util/uri.h"

#define noSP_ITEM_DEBUG_IDLE

//#define OBJECT_TRACE

SPItemView::SPItemView(unsigned flags, unsigned key, DrawingItemPtr<Inkscape::DrawingItem> drawingitem)
    : flags(flags)
    , key(key)
    , drawingitem(std::move(drawingitem)) {}

SPItem::SPItem()
{
    sensitive = TRUE;
    bbox_valid = FALSE;

    transform_center_x = 0;
    transform_center_y = 0;

    freeze_stroke_width = false;
    _is_evaluated = true;
    _evaluated_status = StatusUnknown;

    transform = Geom::identity();
    // doc_bbox = Geom::OptRect();

    clip_ref = nullptr;
    mask_ref = nullptr;

    style->signal_fill_ps_changed.connect  ([this] (auto old_obj, auto obj) { fill_ps_ref_changed  (old_obj, obj); });
    style->signal_stroke_ps_changed.connect([this] (auto old_obj, auto obj) { stroke_ps_ref_changed(old_obj, obj); });
    style->signal_filter_changed.connect   ([this] (auto old_obj, auto obj) { filter_ref_changed   (old_obj, obj); });

    avoidRef = nullptr;
}

SPItem::~SPItem() = default;

SPClipPath *SPItem::getClipObject() const
{
    return clip_ref ? clip_ref->getObject() : nullptr;
}

/**
 * Return the path vector of the clipping region.
 *
 * @return The clip, or none if no clip.
 */
std::optional<Geom::PathVector> SPItem::getClipPathVector() const
{
    if (auto clip = getClipObject()) {
        return clip->getPathVector(Geom::identity());
    }
    return {};
}

/**
 * Return the text object, IF and only if this item is clipped
 * by a single SPText object.
 *
 * @return The text object, or nulptr if no clip or not a text clip.
 */
SPText const *SPItem::getClipTextObject() const
{
    if (auto clip = getClipObject()) {
        return clip->getTextObject();
    }
    return nullptr;
}

/**
 * Return the path vector of the clipping region, combined with the clipping
 * regions of all groups containing this object up to and including \a root.
 *
 * @arg root The root to combine clipping regions from.
 * @return The clip, or none if no clip.
*/
std::optional<Geom::PathVector> SPItem::getClipPathVector(SPItem const *root) const
{
    auto intersection = getClipPathVector();

    auto cumulative_transform = transform.inverse();

    // Traverse ancestors from parent to root inclusive.
    for (auto anc = parent; ; anc = anc->parent) {
        if (!anc) {
            std::cerr << "SPItem::getClipPathVector: precondition violation: root specified but not an ancestor" << std::endl;
            break;
        }

        // Bail out if fully clipped.
        if (intersection && intersection->empty()) {
            return intersection;
        }

        auto const anc_item = cast<SPItem>(anc);
        assert(anc_item);

        // Combine with clip of ancestor.
        if (auto clip = anc_item->getClipObject()) {
            intersection = intersect_clips(std::move(intersection), clip->getPathVector(cumulative_transform));
        }

        if (anc == root) {
            break;
        }

        cumulative_transform = anc_item->transform.inverse() * cumulative_transform;
    }

    return intersection;
}

SPMask *SPItem::getMaskObject() const
{
    return mask_ref ? mask_ref->getObject() : nullptr;
}

SPMaskReference &SPItem::getMaskRef()
{
    if (!mask_ref) {
        mask_ref = new SPMaskReference(this);
        mask_ref->changedSignal().connect([this] (auto old_obj, auto obj) { mask_ref_changed(old_obj, obj); });
    }

    return *mask_ref;
}

SPClipPathReference &SPItem::getClipRef()
{
    if (!clip_ref) {
        clip_ref = new SPClipPathReference(this);
        clip_ref->changedSignal().connect([this] (auto old_obj, auto obj) { clip_ref_changed(old_obj, obj); });
    }

    return *clip_ref;
}

SPAvoidRef &SPItem::getAvoidRef()
{
    if (!avoidRef) {
        avoidRef = new SPAvoidRef(this);
    }
    return *avoidRef;
}

bool SPItem::isVisibleAndUnlocked() const {
    return !isHidden() && !isLocked();
}

bool SPItem::isVisibleAndUnlocked(unsigned display_key) const {
    return !isHidden(display_key) && !isLocked();
}

bool SPItem::isLocked() const {
    for (SPObject const *o = this; o != nullptr; o = o->parent) {
        SPItem const *item = cast<SPItem>(o);
        if (item && !(item->sensitive)) {
            return true;
        }
    }
    return false;
}

void SPItem::setLocked(bool locked, bool recursive) {
    setAttribute("sodipodi:insensitive",
                 ( locked ? "1" : nullptr ));
    updateRepr();
    document->_emitModified();

    // Recursively unlock parents too
    if (recursive) {
        if (SPItem *p_item = cast<SPItem>(parent)) {
            p_item->setLocked(locked, recursive);
        }
    }
}

bool SPItem::isHidden() const {
    if (!isEvaluated())
        return true;
    return style->display.computed == SP_CSS_DISPLAY_NONE;
}

void SPItem::setHidden(bool hide) {
    style->display.set = TRUE;
    style->display.value = ( hide ? SP_CSS_DISPLAY_NONE : SP_CSS_DISPLAY_INLINE );
    style->display.computed = style->display.value;
    style->display.inherit = FALSE;
    updateRepr();
}

bool SPItem::isHidden(unsigned display_key) const
{
    if (!isEvaluated()) {
        return true;
    }
    for (auto &v : views) {
        if (v.key == display_key) {
            g_assert(v.drawingitem);
            for (auto di = v.drawingitem.get(); di; di = di->parent()) {
                if (!di->visible()) {
                    return true;
                }
            }
            return false;
        }
    }
    return true;
}

void SPItem::setHighlight(Inkscape::Colors::Color color) {
    _highlightColor = std::move(color);
    updateRepr();
}

bool SPItem::isHighlightSet() const {
    return _highlightColor.has_value();
}

Inkscape::Colors::Color SPItem::highlight_color() const {
    if (isHighlightSet()) {
        return *_highlightColor;
    }

    SPItem const *item = cast<SPItem>(parent);
    if (parent && (parent != this) && item) {
        return item->highlight_color();
    }
    auto prefs = Inkscape::Preferences::get();
    return prefs->getColor("/tools/nodes/highlight_color", "#aaaaaaff");
}

void SPItem::setEvaluated(bool evaluated) {
    _is_evaluated = evaluated;
    _evaluated_status = StatusSet;
}

void SPItem::resetEvaluated() {
    if ( StatusCalculated == _evaluated_status ) {
        _evaluated_status = StatusUnknown;
        bool oldValue = _is_evaluated;
        if ( oldValue != isEvaluated() ) {
            requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
        }
    } if ( StatusSet == _evaluated_status ) {
        auto switchItem = cast<SPSwitch>(parent);
        if (switchItem) {
            switchItem->resetChildEvaluated();
        }
    }
}

bool SPItem::isEvaluated() const {
    if ( StatusUnknown == _evaluated_status ) {
        _is_evaluated = sp_item_evaluate(this);
        _evaluated_status = StatusCalculated;
    }
    return _is_evaluated;
}

bool SPItem::isExplicitlyHidden() const
{
    return (style->display.set
            && style->display.value == SP_CSS_DISPLAY_NONE);
}

void SPItem::setExplicitlyHidden(bool val) {
    style->display.set = val;
    style->display.value = ( val ? SP_CSS_DISPLAY_NONE : SP_CSS_DISPLAY_INLINE );
    style->display.computed = style->display.value;
    updateRepr();
}

void SPItem::setCenter(Geom::Point const &object_centre) {
    document->ensureUpToDate();

    // Copied from DocumentProperties::onDocUnitChange()
    gdouble viewscale = 1.0;
    Geom::Rect vb = this->document->getRoot()->viewBox;
    if ( !vb.hasZeroArea() ) {
        gdouble viewscale_w = this->document->getWidth().value("px") / vb.width();
        gdouble viewscale_h = this->document->getHeight().value("px")/ vb.height();
        viewscale = std::min(viewscale_h, viewscale_w);
    }

    // FIXME this is seriously wrong
    Geom::OptRect bbox = desktopGeometricBounds();
    if (bbox) {
        // object centre is document coordinates (i.e. in pixels), so we need to consider the viewbox
        // to translate to user units; transform_center_x/y is in user units
        transform_center_x = (object_centre[Geom::X] - bbox->midpoint()[Geom::X])/viewscale;
        if (Geom::are_near(transform_center_x, 0)) // rounding error
            transform_center_x = 0;
        transform_center_y = (object_centre[Geom::Y] - bbox->midpoint()[Geom::Y])/viewscale;
        if (Geom::are_near(transform_center_y, 0)) // rounding error
            transform_center_y = 0;
    }
}

bool SPItem::updateCenterIfSet(Geom::Point const &center)
{
    if (isCenterSet()) {
        setCenter(center);
        return true;
    }
    return false;
}

void
SPItem::unsetCenter() {
    transform_center_x = 0;
    transform_center_y = 0;
}

bool SPItem::isCenterSet() const {
    return (transform_center_x != 0 || transform_center_y != 0);
}

// Get the item's transformation center in desktop coordinates (i.e. in pixels)
Geom::Point SPItem::getCenter(bool ensure_uptodate) const {
    // This call is very bad as it can cascade to thousands of calls
    // it should be removed in future refactoring as document scale should be
    // cached and controlled properly elsewhere.
    if (ensure_uptodate)
        document->ensureUpToDate();

    // Copied from DocumentProperties::onDocUnitChange()
    gdouble viewscale = 1.0;
    Geom::Rect vb = this->document->getRoot()->viewBox;
    if ( !vb.hasZeroArea() ) {
        gdouble viewscale_w = this->document->getWidth().value("px") / vb.width();
        gdouble viewscale_h = this->document->getHeight().value("px")/ vb.height();
        viewscale = std::min(viewscale_h, viewscale_w);
    }

    // FIXME this is seriously wrong
    Geom::OptRect bbox = desktopGeometricBounds();
    if (bbox) {
        // transform_center_x/y are stored in user units, so we have to take the viewbox into account to translate to document coordinates
        return bbox->midpoint() + Geom::Point (transform_center_x*viewscale, transform_center_y*viewscale);

    } else {
        return Geom::Point(0, 0); // something's wrong!
    }

}

void
SPItem::scaleCenter(Geom::Scale const &sc) {
    transform_center_x *= sc[Geom::X];
    transform_center_y *= sc[Geom::Y];
}

void SPItem::raiseToTop() {
    auto& list = parent->children;
    auto end = SPObject::ChildrenList::reverse_iterator(list.iterator_to(*this));
    auto topmost = std::find_if(list.rbegin(), end, is<SPItem>);
    // auto topmost = find_last_if(++parent->children.iterator_to(*this), parent->children.end(), &is_item);
    if (topmost != list.rend()) {
        getRepr()->parent()->changeOrder(getRepr(), topmost->getRepr());
    }
}

bool SPItem::raiseOne() {
    auto next_higher = std::find_if(++parent->children.iterator_to(*this), parent->children.end(), is<SPItem>);
    if (next_higher != parent->children.end()) {
        Inkscape::XML::Node *ref = next_higher->getRepr();
        getRepr()->parent()->changeOrder(getRepr(), ref);
        return true;
    }
    return false;
}

bool SPItem::lowerOne() {
    auto& list = parent->children;
    auto self = list.iterator_to(*this);
    auto start = SPObject::ChildrenList::reverse_iterator(self);
    auto next_lower = std::find_if(start, list.rend(), is<SPItem>);
    if (next_lower != list.rend()) {
        auto next = list.iterator_to(*next_lower);
        if (next == list.begin()) {
            getRepr()->parent()->changeOrder(getRepr(), nullptr);
        } else {
            --next;
            auto ref = next->getRepr();
            getRepr()->parent()->changeOrder(getRepr(), ref);
        }
        return true;
    }
    return false;
}

void SPItem::lowerToBottom() {
    auto bottom = std::find_if(parent->children.begin(), parent->children.iterator_to(*this), is<SPItem>);
    if (bottom != parent->children.iterator_to(*this)) {
        Inkscape::XML::Node *ref = nullptr;
        if (bottom != parent->children.begin()) {
            bottom--;
            ref = bottom->getRepr();
        }
        parent->getRepr()->changeOrder(getRepr(), ref);
    }
}

/**
 * Return the parent, only if it's a group object.
 */
SPGroup *SPItem::getParentGroup() const
{
    return cast<SPGroup>(parent);
}

void SPItem::moveTo(SPItem *target, bool intoafter) {

    Inkscape::XML::Node *target_ref = ( target ? target->getRepr() : nullptr );
    Inkscape::XML::Node *our_ref = getRepr();

    if (!target_ref) {
        // Assume move to the "first" in the top node, find the top node
        intoafter = false;
        SPObject* bottom = this->document->getObjectByRepr(our_ref->root())->firstChild();
        while (!is<SPItem>(bottom->getNext())) {
            bottom = bottom->getNext();
        }
        target_ref = bottom->getRepr();
    }

    if (target_ref == our_ref) {
        // Move to ourself ignore
        return;
    }

    if (intoafter) {
        // Move this inside of the target at the end
        our_ref->parent()->removeChild(our_ref);
        target_ref->addChild(our_ref, nullptr);
    } else if (target_ref->parent() != our_ref->parent()) {
        // Change in parent, need to remove and add
        our_ref->parent()->removeChild(our_ref);
        target_ref->parent()->addChild(our_ref, target_ref);
    } else {
        // Same parent, just move
        our_ref->parent()->changeOrder(our_ref, target_ref);
    }
}

void SPItem::build(SPDocument *document, Inkscape::XML::Node *repr) {
#ifdef OBJECT_TRACE
    objectTrace( "SPItem::build");
#endif

    SPItem* object = this;
    object->readAttr(SPAttr::STYLE);
    object->readAttr(SPAttr::TRANSFORM);
    object->readAttr(SPAttr::CLIP_PATH);
    object->readAttr(SPAttr::MASK);
    object->readAttr(SPAttr::SODIPODI_INSENSITIVE);
    object->readAttr(SPAttr::TRANSFORM_CENTER_X);
    object->readAttr(SPAttr::TRANSFORM_CENTER_Y);
    object->readAttr(SPAttr::CONNECTOR_AVOID);
    object->readAttr(SPAttr::CONNECTION_POINTS);
    object->readAttr(SPAttr::INKSCAPE_HIGHLIGHT_COLOR);

    SPObject::build(document, repr);
#ifdef OBJECT_TRACE
    objectTrace( "SPItem::build", false);
#endif
}

void SPItem::release()
{
    // Note: do this here before the clip_ref is deleted, since calling
    // ensureUpToDate() for triggered routing may reference
    // the deleted clip_ref.
    delete avoidRef;
    avoidRef = nullptr;

    // we do NOT disconnect from the changed signal of those before deletion.
    // The destructor will call *_ref_changed with NULL as the new value,
    // which will cause the set_visible(false) function to be called.
    delete clip_ref;
    clip_ref = nullptr;
    delete mask_ref;
    mask_ref = nullptr;

    // the first thing SPObject::release() does is destroy the fill/stroke/filter references.
    // as above, this calls *_ref_changed() which performs the set_visible(false).
    // it is important this happens before the views are cleared.
    SPObject::release();

    views.clear();
}

void SPItem::set(SPAttr key, gchar const* value) {
#ifdef OBJECT_TRACE
    std::stringstream temp;
    temp << "SPItem::set: " << sp_attribute_name(key)  << " " << (value?value:"null");
    objectTrace( temp.str() );
#endif
    SPItem *item = this;
    SPItem* object = item;

    switch (key) {
        case SPAttr::TRANSFORM: {
            Geom::Affine t;
            if (value && sp_svg_transform_read(value, &t)) {
                item->set_item_transform(t);
            } else {
                item->set_item_transform(Geom::identity());
            }
            break;
        }
        case SPAttr::CLIP_PATH: {
            auto uri = extract_uri(value);
            if (!uri.empty() || item->clip_ref) {
                item->getClipRef().try_attach(uri.c_str());
            }
            break;
        }
        case SPAttr::MASK: {
            auto uri = extract_uri(value);
            if (!uri.empty() || item->mask_ref) {
                item->getMaskRef().try_attach(uri.c_str());
            }
            break;
        }
        case SPAttr::SODIPODI_INSENSITIVE:
        {
            item->sensitive = !value;
            for (auto &v : item->views) {
                v.drawingitem->setSensitive(item->sensitive);
            }
            break;
        }
        case SPAttr::INKSCAPE_HIGHLIGHT_COLOR:
        {
            item->_highlightColor = Inkscape::Colors::Color::parse(value);
            break;
        }
        case SPAttr::CONNECTOR_AVOID:
            if (value || item->avoidRef) {
                item->getAvoidRef().setAvoid(value);
            }
            break;
        case SPAttr::TRANSFORM_CENTER_X:
            if (value) {
                item->transform_center_x = g_strtod(value, nullptr);
            } else {
                item->transform_center_x = 0;
            }
            object->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;
        case SPAttr::TRANSFORM_CENTER_Y:
            if (value) {
                item->transform_center_y = g_strtod(value, nullptr);
                item->transform_center_y *= -document->yaxisdir();
            } else {
                item->transform_center_y = 0;
            }
            object->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            break;
        case SPAttr::SYSTEM_LANGUAGE:
        case SPAttr::REQUIRED_FEATURES:
        case SPAttr::REQUIRED_EXTENSIONS:
            {
                item->resetEvaluated();
                // pass to default handler
            }
        default:
            if (SP_ATTRIBUTE_IS_CSS(key)) {
                // FIXME: See if this is really necessary. Also, check after modifying SPIPaint to preserve
                // non-#abcdef color formats.

                // Propagate the property change to all clones
                style->readFromObject(object);
                object->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            } else {
                SPObject::set(key, value);
            }
            break;
    }
#ifdef OBJECT_TRACE
    objectTrace( "SPItem::set", false);
#endif
}

template <typename F>
class lazy
{
public:
    explicit lazy(F &&f): f(std::move(f)) {}

    auto operator()()
    {
        if (!result) result = f();
        return *result;
    }

private:
    F f;
    std::optional<typename std::invoke_result<F>::type> result;
};

void SPItem::clip_ref_changed(SPObject *old_clip, SPObject *clip)
{
    if (old_clip) {
        clip_ref->modified_connection.disconnect();
        for (auto &v : views) {
            auto oldPath = cast<SPClipPath>(old_clip);
            g_assert(oldPath);
            oldPath->hide(v.drawingitem->key() + ITEM_KEY_CLIP);
        }
    }
    auto clipPath = cast<SPClipPath>(clip);
    if (clipPath) {
        Geom::OptRect bbox = geometricBounds();
        for (auto &v : views) {
            auto clip_key = SPItem::ensure_key(v.drawingitem.get()) + ITEM_KEY_CLIP;
            auto ai = clipPath->show(v.drawingitem->drawing(), clip_key, bbox);
            v.drawingitem->setClip(ai);
        }
        clip_ref->modified_connection = clipPath->connectModified([this] (auto, unsigned flags) {
            if (flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG)) {
                requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            }
        });
    }
    requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG); // To update bbox.
}

void SPItem::mask_ref_changed(SPObject *old_mask, SPObject *mask)
{
    if (old_mask) {
        mask_ref->modified_connection.disconnect();
        for (auto &v : views) {
            auto maskItem = cast<SPMask>(old_mask);
            g_assert(maskItem);
            maskItem->hide(v.drawingitem->key() + ITEM_KEY_MASK);
        }
    }
    auto maskItem = cast<SPMask>(mask);
    if (maskItem) {
        Geom::OptRect bbox = geometricBounds();
        for (auto &v : views) {
            auto mask_key = SPItem::ensure_key(v.drawingitem.get()) + ITEM_KEY_MASK;
            auto ai = maskItem->show(v.drawingitem->drawing(), mask_key, bbox);
            v.drawingitem->setMask(ai);
        }
        mask_ref->modified_connection = maskItem->connectModified([this] (auto, unsigned flags) {
            if (flags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG)) {
                requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            }
        });
    }
    requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG); // To update bbox.
}

void SPItem::fill_ps_ref_changed(SPObject *old_ps, SPObject *ps)
{
    auto old_fill_ps = cast<SPPaintServer>(old_ps);
    if (old_fill_ps) {
        for (auto &v : views) {
            old_fill_ps->hide(v.drawingitem->key() + ITEM_KEY_FILL);
        }
    }

    auto new_fill_ps = cast<SPPaintServer>(ps);
    if (new_fill_ps) {
        Geom::OptRect bbox = geometricBounds();
        for (auto &v : views) {
            auto fill_key = SPItem::ensure_key(v.drawingitem.get()) + ITEM_KEY_FILL;
            auto pi = new_fill_ps->show(v.drawingitem->drawing(), fill_key, bbox);
            v.drawingitem->setFillPattern(pi);
        }
    }
}

void SPItem::stroke_ps_ref_changed(SPObject *old_ps, SPObject *ps)
{
    auto old_stroke_ps = cast<SPPaintServer>(old_ps);
    if (old_stroke_ps) {
        for (auto &v : views) {
            old_stroke_ps->hide(v.drawingitem->key() + ITEM_KEY_STROKE);
        }
    }

    auto new_stroke_ps = cast<SPPaintServer>(ps);
    if (new_stroke_ps) {
        Geom::OptRect bbox = geometricBounds();
        for (auto &v : views) {
            auto stroke_key = SPItem::ensure_key(v.drawingitem.get()) + ITEM_KEY_STROKE;
            auto pi = new_stroke_ps->show(v.drawingitem->drawing(), stroke_key, bbox);
            v.drawingitem->setStrokePattern(pi);
        }
    }
}

void SPItem::filter_ref_changed(SPObject *old_obj, SPObject *obj)
{
    auto old_filter = cast<SPFilter>(old_obj);
    if (old_filter) {
        for (auto &v : views) {
            old_filter->hide(v.drawingitem.get());
        }
    }

    auto new_filter = cast<SPFilter>(obj);
    if (new_filter) {
        for (auto &v : views) {
            new_filter->show(v.drawingitem.get());
        }
    }
}

void SPItem::update(SPCtx *ctx, unsigned flags)
{
    auto ictx = static_cast<SPItemCtx const*>(ctx);

    // Any of the modifications defined in sp-object.h might change bbox,
    // so we invalidate it unconditionally
    bbox_valid = false;

    viewport = ictx->viewport; // Cache viewport

    auto bbox = lazy([this] {
        return geometricBounds();
    });

    if (flags & (SP_OBJECT_CHILD_MODIFIED_FLAG |
                 SP_OBJECT_MODIFIED_FLAG |
                 SP_OBJECT_STYLE_MODIFIED_FLAG))
    {
        if (flags & SP_OBJECT_MODIFIED_FLAG) {
            for (auto &v : views) {
                v.drawingitem->setTransform(transform);
            }
        }

        auto set_bboxes = [&, this] (auto obj, int type) {
            if (obj) {
                for (auto &v : views) {
                    obj->setBBox(v.drawingitem->key() + type, bbox());
                }
            }
        };

        set_bboxes(getClipObject(), ITEM_KEY_CLIP);
        set_bboxes(getMaskObject(), ITEM_KEY_MASK);
        set_bboxes(style->getFillPaintServer(), ITEM_KEY_FILL);
        set_bboxes(style->getStrokePaintServer(), ITEM_KEY_STROKE);

        if (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) {
            for (auto &v : views) {
                v.drawingitem->setOpacity(style->opacity.as_double());
                v.drawingitem->setAntialiasing(style->shape_rendering.computed == SP_CSS_SHAPE_RENDERING_CRISPEDGES ? Inkscape::Antialiasing::None : Inkscape::Antialiasing::Good);
                v.drawingitem->setIsolation(style->isolation.value);
                v.drawingitem->setBlendMode(style->mix_blend_mode.value);
                v.drawingitem->setVisible(!isHidden());
            }
        }
    }

    // Update bounding box in user space, used for filter and objectBoundingBox units.
    if (style->filter.set) {
        for (auto &v : views) {
            if (v.drawingitem) {
                v.drawingitem->setItemBounds(bbox());
            }
        }
    }

    // Update libavoid with item geometry (for connector routing).
    if (avoidRef && document) {
        avoidRef->handleSettingChange();
    }
}

void SPItem::modified(unsigned int /*flags*/)
{
#ifdef OBJECT_TRACE
    objectTrace( "SPItem::modified" );
    objectTrace( "SPItem::modified", false );
#endif
}

Inkscape::XML::Node* SPItem::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    SPItem *item = this;
    SPItem* object = item;

    // in the case of SP_OBJECT_WRITE_BUILD, the item should always be newly created,
    // so we need to add any children from the underlying object to the new repr
    if (flags & SP_OBJECT_WRITE_BUILD) {
        std::vector<Inkscape::XML::Node *>l;
        for (auto& child: object->children) {
            if (is<SPTitle>(&child) || is<SPDesc>(&child)) {
                Inkscape::XML::Node *crepr = child.updateRepr(xml_doc, nullptr, flags);
                if (crepr) {
                    l.push_back(crepr);
                }
            }
        }
        for (auto i = l.rbegin(); i!= l.rend(); ++i) {
            repr->addChild(*i, nullptr);
            Inkscape::GC::release(*i);
        }
    } else {
        for (auto& child: object->children) {
            if (is<SPTitle>(&child) || is<SPDesc>(&child)) {
                child.updateRepr(flags);
            }
        }
    }

    repr->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(item->transform));

    if (flags & SP_OBJECT_WRITE_EXT) {
        repr->setAttribute("sodipodi:insensitive", ( item->sensitive ? nullptr : "true" ));
        if (item->transform_center_x != 0)
            repr->setAttributeSvgDouble("inkscape:transform-center-x", item->transform_center_x);
        else
            repr->removeAttribute("inkscape:transform-center-x");
        if (item->transform_center_y != 0) {
            auto y = item->transform_center_y;
            y *= -document->yaxisdir();
            repr->setAttributeSvgDouble("inkscape:transform-center-y", y);
        } else
            repr->removeAttribute("inkscape:transform-center-y");
    }

    if (getClipObject()) {
        auto value = item->clip_ref->getURI()->cssStr();
        repr->setAttributeOrRemoveIfEmpty("clip-path", value);
    }
    if (getMaskObject()) {
        auto value = item->mask_ref->getURI()->cssStr();
        repr->setAttributeOrRemoveIfEmpty("mask", value);
    }
    if (item->isHighlightSet()) {
        repr->setAttribute("inkscape:highlight-color", item->_highlightColor->toString());
    } else {
        repr->removeAttribute("inkscape:highlight-color");
    }

    SPObject::write(xml_doc, repr, flags);

    return repr;
}

// CPPIFY: make pure virtual
Geom::OptRect SPItem::bbox(Geom::Affine const & /*transform*/, SPItem::BBoxType /*type*/) const {
    //throw;
    return Geom::OptRect();
}

Geom::OptRect SPItem::geometricBounds(Geom::Affine const &transform) const
{
    return bbox(transform, SPItem::GEOMETRIC_BBOX);
}

Geom::OptRect SPItem::visualBounds(Geom::Affine const &transform, bool wfilter, bool wclip, bool wmask) const
{
    Geom::OptRect bbox;

    auto gbox = lazy([this] {
        return geometricBounds();
    });

    if (auto filter = style ? style->getFilter() : nullptr; filter && wfilter) {
        // call the subclass method
        bbox = gbox(); // see LP Bug 1229971

        if (!bbox) {
            // If we don't have a valid bbox, don't go on and create one.
            return bbox;
        }

        // default filter area per the SVG spec:
        SVGLength x, y, w, h;
        x.set(SVGLength::PERCENT, -0.10, 0);
        y.set(SVGLength::PERCENT, -0.10, 0);
        w.set(SVGLength::PERCENT, 1.20, 0);
        h.set(SVGLength::PERCENT, 1.20, 0);

        // if area is explicitly set, override:
        if (filter->x._set) x = filter->x;
        if (filter->y._set) y = filter->y;
        if (filter->width._set) w = filter->width;
        if (filter->height._set) h = filter->height;

        auto const len = bbox ? bbox->dimensions() : Geom::Point();
        x.update(12, 6, len.x());
        y.update(12, 6, len.y());
        w.update(12, 6, len.x());
        h.update(12, 6, len.y());

        if (filter->filterUnits == SP_FILTER_UNITS_OBJECTBOUNDINGBOX && bbox) {
            bbox = Geom::Rect::from_xywh(
                       bbox->left() + x.computed * (x.unit == SVGLength::PERCENT ? 1.0 : len.x()),
                       bbox->top()  + y.computed * (y.unit == SVGLength::PERCENT ? 1.0 : len.y()),
                                      w.computed * (w.unit == SVGLength::PERCENT ? 1.0 : len.x()),
                                      h.computed * (h.unit == SVGLength::PERCENT ? 1.0 : len.y())
                   );
        } else {
            bbox = Geom::Rect::from_xywh(x.computed, y.computed, w.computed, h.computed);
        }

        *bbox *= transform;
    } else {
        // call the subclass method
        bbox = this->bbox(transform, SPItem::VISUAL_BBOX);

        if (!bbox) {
            // If we don't have a valid bbox, don't do clipping/masking etc.
            return bbox;
        }
    }

    auto transform_with_units = [&] (bool contentunits) {
        return contentunits == SP_CONTENT_UNITS_OBJECTBOUNDINGBOX && gbox()
             ? Geom::Scale(gbox()->dimensions()) * Geom::Translate(gbox()->min()) * transform
             : transform;
    };

    auto apply_clip_or_mask_bbox = [&] (auto const *obj, bool contentunits) {
        bbox.intersectWith(obj->visualBounds(transform_with_units(contentunits)));
    };

    if (auto clip = getClipObject(); clip && wclip) {
        apply_clip_or_mask_bbox(clip, clip->clippath_units());
    }

    if (auto mask = getMaskObject(); mask && wmask) {
        apply_clip_or_mask_bbox(mask, mask->mask_content_units());
    }

    return bbox;
}

Geom::OptRect SPItem::bounds(BBoxType type, Geom::Affine const &transform) const
{
    if (type == GEOMETRIC_BBOX) {
        return geometricBounds(transform);
    } else {
        return visualBounds(transform);
    }
}

Geom::OptRect SPItem::documentPreferredBounds() const
{
    if (Inkscape::Preferences::get()->getInt("/tools/bounding_box") == 0) {
        return documentBounds(SPItem::VISUAL_BBOX);
    } else {
        return documentBounds(SPItem::GEOMETRIC_BBOX);
    }
}

Geom::OptRect SPItem::documentGeometricBounds() const
{
    return geometricBounds(i2doc_affine());
}

Geom::OptRect SPItem::documentVisualBounds() const
{
    if (!bbox_valid) {
        doc_bbox = visualBounds(i2doc_affine());
        bbox_valid = true;
    }
    return doc_bbox;
}
Geom::OptRect SPItem::documentBounds(BBoxType type) const
{
    if (type == GEOMETRIC_BBOX) {
        return documentGeometricBounds();
    } else {
        return documentVisualBounds();
    }
}

std::optional<Geom::PathVector> SPItem::documentExactBounds() const
{
    std::optional<Geom::PathVector> result;
    if (auto bounding_rect = visualBounds()) {
        result = Geom::Path(*bounding_rect) * i2doc_affine();
    }
    return result;
}

Geom::OptRect SPItem::desktopGeometricBounds() const
{
    return geometricBounds(i2dt_affine());
}

Geom::OptRect SPItem::desktopVisualBounds() const
{
    Geom::OptRect ret = documentVisualBounds();
    if (ret) {
        *ret *= document->doc2dt();
    }
    return ret;
}

Geom::OptRect SPItem::desktopPreferredBounds() const
{
    if (Inkscape::Preferences::get()->getInt("/tools/bounding_box") == 0) {
        return desktopBounds(SPItem::VISUAL_BBOX);
    } else {
        return desktopBounds(SPItem::GEOMETRIC_BBOX);
    }
}

Geom::OptRect SPItem::desktopBounds(BBoxType type) const
{
    if (type == GEOMETRIC_BBOX) {
        return desktopGeometricBounds();
    } else {
        return desktopVisualBounds();
    }
}

unsigned int SPItem::pos_in_parent() const {
    g_assert(parent != nullptr);

    unsigned int pos = 0;

    for (auto& iter: parent->children) {
        if (&iter == this) {
            return pos;
        }

        if (is<SPItem>(&iter)) {
            pos++;
        }
    }

    g_assert_not_reached();
    return 0;
}

// CPPIFY: make pure virtual, see below!
void SPItem::snappoints(std::vector<Inkscape::SnapCandidatePoint> & /*p*/, Inkscape::SnapPreferences const */*snapprefs*/) const {
    //throw;
}
    /* This will only be called if the derived class doesn't override this.
     * see for example sp_genericellipse_snappoints in sp-ellipse.cpp
     * We don't know what shape we could be dealing with here, so we'll just
     * do nothing
     */

void SPItem::getSnappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const
{
    // Get the snappoints of the item
    snappoints(p, snapprefs);

    // Get the snappoints at the item's center
    if (snapprefs && snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_ROTATION_CENTER)) {
        p.emplace_back(getCenter(), Inkscape::SNAPSOURCE_ROTATION_CENTER, Inkscape::SNAPTARGET_ROTATION_CENTER);
    }

    // Get the snappoints of clipping paths and mask, if any
    auto desktop = SP_ACTIVE_DESKTOP;

    auto gbox = lazy([this] {
        return geometricBounds();
    });

    auto add_clip_or_mask_points = [&, this] (SPObject const *obj, bool contentunits) {
        // obj is a group object, the children are the actual clippers
        for (auto &child: obj->children) {
            if (auto item = cast<SPItem>(&child)) {
                std::vector<Inkscape::SnapCandidatePoint> p_clip_or_mask;
                // Please note the recursive call here!
                item->getSnappoints(p_clip_or_mask, snapprefs);
                // Take into account the transformation of the item being clipped or masked
                for (auto const &p_orig : p_clip_or_mask) {
                    // All snappoints are in desktop coordinates, but the item's transformation is
                    // in document coordinates. Hence the awkward construction below
                    auto pt = p_orig.getPoint();
                    if (contentunits == SP_CONTENT_UNITS_OBJECTBOUNDINGBOX && gbox()) {
                        pt *= Geom::Scale(gbox()->dimensions()) * Geom::Translate(gbox()->min());
                    }
                    pt = desktop->dt2doc(pt) * i2dt_affine();
                    p.emplace_back(pt, p_orig.getSourceType(), p_orig.getTargetType());
                }
            }
        }
    };

    if (auto clip = getClipObject()) {
        add_clip_or_mask_points(clip, clip->clippath_units());
    }

    if (auto mask = getMaskObject()) {
        add_clip_or_mask_points(mask, mask->mask_content_units());
    }
}

// CPPIFY: make pure virtual
void SPItem::print(SPPrintContext* /*ctx*/) {
    //throw;
}

void SPItem::invoke_print(SPPrintContext *ctx)
{
    if (!isHidden()) {
        if (!transform.isIdentity() || style->opacity.value != SP_SCALE24_MAX) {
            ctx->bind(transform, style->opacity.as_double());
            print(ctx);
            ctx->release();
        } else {
            print(ctx);
        }
    }
}

/**
 * The item's type name, not node tag name. NOT translated.
 *
 * @return The item's type name (default: 'item')
 */
const char* SPItem::typeName() const {
    return "item";
}

/**
 * The item's type name as a translated human string.
 *
 * Translated string for UI display.
 */
const char* SPItem::displayName() const {
    return _("Object");
}

gchar* SPItem::description() const {
    return g_strdup("");
}

gchar *SPItem::detailedDescription() const {
        gchar* s = g_strdup_printf("<b>%s</b> %s",
                    this->displayName(), this->description());

    if (s && getClipObject()) {
        char *snew = g_strdup_printf(_("%s; <i>clipped</i>"), s);
        g_free(s);
        s = snew;
    }

    if (s && getMaskObject()) {
        char *snew = g_strdup_printf(_("%s; <i>masked</i>"), s);
        g_free(s);
        s = snew;
    }

    if (style && style->filter.href && style->filter.href->getObject()) {
        char const *label = style->filter.href->getObject()->label();
        char *snew = nullptr;

        if (label) {
            snew = g_strdup_printf(_("%s; <i>filtered (%s)</i>"), s, _(label));
        } else {
            snew = g_strdup_printf(_("%s; <i>filtered</i>"), s);
        }

        g_free(s);
        s = snew;
    }

    return s;
}

bool SPItem::isFiltered() const {
    return style && style->filter.href && style->filter.href->getObject();
}

SPObject* SPItem::isInMask() const {
    SPObject* parent = this->parent;
    while (parent && !is<SPMask>(parent)) {
        parent = parent->parent;
    }
    return parent;
}

SPObject* SPItem::isInClipPath() const {
    SPObject* parent = this->parent;
    while (parent && !is<SPClipPath>(parent)) {
        parent = parent->parent;
    }
    return parent;
}

unsigned SPItem::display_key_new(unsigned numkeys)
{
    static unsigned dkey = 1;

    dkey += numkeys;

    return dkey - numkeys;
}

unsigned SPItem::ensure_key(Inkscape::DrawingItem *di)
{
    if (!di->key()) {
        di->setKey(SPItem::display_key_new(ITEM_KEY_SIZE));
    }
    return di->key();
}

// CPPIFY: make pure virtual
Inkscape::DrawingItem* SPItem::show(Inkscape::Drawing& /*drawing*/, unsigned int /*key*/, unsigned int /*flags*/) {
    //throw;
    return nullptr;
}

Inkscape::DrawingItem *SPItem::invoke_show(Inkscape::Drawing &drawing, unsigned key, unsigned flags)
{
    auto ai = show(drawing, key, flags);
    if (!ai) {
        return nullptr;
    }

    auto const bbox = geometricBounds();

    ai->setItem(this);
    ai->setItemBounds(bbox);
    ai->setTransform(transform);
    ai->setOpacity(style->opacity.as_double());
    ai->setIsolation(style->isolation.value);
    ai->setBlendMode(style->mix_blend_mode.value);
    ai->setVisible(!isHidden());
    ai->setSensitive(sensitive);
    views.emplace_back(flags, key, DrawingItemPtr<Inkscape::DrawingItem>(ai));

    if (auto clip = getClipObject()) {
        auto clip_key = SPItem::ensure_key(ai) + ITEM_KEY_CLIP;
        auto ac = clip->show(drawing, clip_key, bbox);
        ai->setClip(ac);
    }
    if (auto mask = getMaskObject()) {
        auto mask_key = SPItem::ensure_key(ai) + ITEM_KEY_MASK;
        auto ac = mask->show(drawing, mask_key, bbox);
        ai->setMask(ac);
    }
    if (auto fill = style->getFillPaintServer()) {
        auto fill_key = SPItem::ensure_key(ai) + ITEM_KEY_FILL;
        auto ap = fill->show(drawing, fill_key, bbox);
        ai->setFillPattern(ap);
    }
    if (auto stroke = style->getStrokePaintServer()) {
        auto stroke_key = SPItem::ensure_key(ai) + ITEM_KEY_STROKE;
        auto ap = stroke->show(drawing, stroke_key, bbox);
        ai->setStrokePattern(ap);
    }
    if (auto filter = style->getFilter()) {
        filter->show(ai);
    }

    return ai;
}

// CPPIFY: make pure virtual
void SPItem::hide(unsigned int /*key*/) {
    //throw;
}

void SPItem::invoke_hide(unsigned key)
{
    hide(key);

    for (auto it = views.begin(); it != views.end(); ) {
        auto &v = *it;
        if (v.key == key) {
            unsigned ai_key = v.drawingitem->key();

            if (auto clip = getClipObject()) {
                clip->hide(ai_key + ITEM_KEY_CLIP);
            }
            if (auto mask = getMaskObject()) {
                mask->hide(ai_key + ITEM_KEY_MASK);
            }
            if (auto fill_ps = style->getFillPaintServer()) {
                fill_ps->hide(ai_key + ITEM_KEY_FILL);
            }
            if (auto stroke_ps = style->getStrokePaintServer()) {
                stroke_ps->hide(ai_key + ITEM_KEY_STROKE);
            }
            if (auto filter = style->getFilter()) {
                filter->hide(v.drawingitem.get());
            }

            v.drawingitem.reset();

            *it = std::move(views.back());
            views.pop_back();
        } else {
            ++it;
        }
    }
}

/**
 * Invoke hide on all non-group items, except for the list of items to keep.
 */
void SPItem::invoke_hide_except(unsigned key, const std::vector<SPItem const *> &to_keep)
{
    // If item is not in the list of items to keep.
    if (to_keep.end() == find(to_keep.begin(), to_keep.end(), this)) {
        // Only hide the item if it's not a group, root or use.
        if (!is<SPRoot>(this) &&
            !is<SPGroup>(this) &&
            !is<SPUse>(this)
            ) {
            this->invoke_hide(key);
        }
        // recurse
        for (auto &obj : this->children) {
            if (auto child = cast<SPItem>(&obj)) {
                child->invoke_hide_except(key, to_keep);
            }
        }
    }
}

// Adjusters

void SPItem::adjust_pattern(Geom::Affine const &postmul, bool set, PaintServerTransform pt)
{
    bool fill = (pt == TRANSFORM_FILL || pt == TRANSFORM_BOTH);
    if (fill && style && (style->fill.isPaintserver())) {
        SPObject *server = style->getFillPaintServer();
        auto serverPatt = cast<SPPattern>(server);
        if ( serverPatt ) {
            SPPattern *pattern = serverPatt->clone_if_necessary(this, "fill");
            pattern->transform_multiply(postmul, set);
        }
    }

    bool stroke = (pt == TRANSFORM_STROKE || pt == TRANSFORM_BOTH);
    if (stroke && style && (style->stroke.isPaintserver())) {
        SPObject *server = style->getStrokePaintServer();
        auto serverPatt = cast<SPPattern>(server);
        if ( serverPatt ) {
            SPPattern *pattern = serverPatt->clone_if_necessary(this, "stroke");
            pattern->transform_multiply(postmul, set);
        }
    }
}

void SPItem::adjust_hatch(Geom::Affine const &postmul, bool set, PaintServerTransform pt)
{
    bool fill = (pt == TRANSFORM_FILL || pt == TRANSFORM_BOTH);
    if (fill && style && (style->fill.isPaintserver())) {
        SPObject *server = style->getFillPaintServer();
        auto serverHatch = cast<SPHatch>(server);
        if (serverHatch) {
            SPHatch *hatch = serverHatch->clone_if_necessary(this, "fill");
            hatch->transform_multiply(postmul, set);
        }
    }

    bool stroke = (pt == TRANSFORM_STROKE || pt == TRANSFORM_BOTH);
    if (stroke && style && (style->stroke.isPaintserver())) {
        SPObject *server = style->getStrokePaintServer();
        auto serverHatch = cast<SPHatch>(server);
        if (serverHatch) {
            SPHatch *hatch = serverHatch->clone_if_necessary(this, "stroke");
            hatch->transform_multiply(postmul, set);
        }
    }
}

void SPItem::adjust_gradient( Geom::Affine const &postmul, bool set )
{
    if ( style && style->fill.isPaintserver() ) {
        SPPaintServer *server = style->getFillPaintServer();
        auto serverGrad = cast<SPGradient>(server);
        if ( serverGrad ) {

            /**
             * \note Bbox units for a gradient are generally a bad idea because
             * with them, you cannot preserve the relative position of the
             * object and its gradient after rotation or skew. So now we
             * convert them to userspace units which are easy to keep in sync
             * just by adding the object's transform to gradientTransform.
             * \todo FIXME: convert back to bbox units after transforming with
             * the item, so as to preserve the original units.
             */
            SPGradient *gradient = sp_gradient_convert_to_userspace( serverGrad, this, "fill" );

            sp_gradient_transform_multiply( gradient, postmul, set );
        }
    }

    if ( style && style->stroke.isPaintserver() ) {
        SPPaintServer *server = style->getStrokePaintServer();
        auto serverGrad = cast<SPGradient>(server);
        if ( serverGrad ) {
            SPGradient *gradient = sp_gradient_convert_to_userspace( serverGrad, this, "stroke");
            sp_gradient_transform_multiply( gradient, postmul, set );
        }
    }
}

void SPItem::adjust_clip(Geom::Affine const &postmul, bool set)
{
    if (auto clip_group = getClipObject()) {
        clip_group->transform_multiply(postmul, set);
    }
}

void SPItem::remove_clip_transforms()
{
    if (auto clip_group = getClipObject()) {
        clip_group->removeTransformsRecursively(clip_group->parent);
    }
}

void SPItem::adjust_stroke( gdouble ex )
{
    if (freeze_stroke_width) {
        return;
    }
    SPStyle *style = this->style;

    if (style && !Geom::are_near(ex, 1.0, Geom::EPSILON) && !style->stroke_extensions.hairline) {

        // If there are any pending attribute updates, SPStyle::read overwrites the new stroke_width when
        // updateRepr applies those other attributes. Write out any pending updates before adjusting stroke.
        updateRepr();

        style->stroke_width.computed *= ex;
        style->stroke_width.set = TRUE;

        if ( !style->stroke_dasharray.values.empty() ) {
            for (auto & value : style->stroke_dasharray.values) {
                value.value    *= ex;
                value.computed *= ex;
            }
            style->stroke_dashoffset.value    *= ex;
            style->stroke_dashoffset.computed *= ex;
        }

        updateRepr();
    }
}

/**
 * Find out the inverse of previous transform of an item (from its repr)
 */
Geom::Affine sp_item_transform_repr (SPItem *item)
{
    Geom::Affine t_old(Geom::identity());
    gchar const *t_attr = item->getRepr()->attribute("transform");
    if (t_attr) {
        Geom::Affine t;
        if (sp_svg_transform_read(t_attr, &t)) {
            t_old = t;
        }
    }

    return t_old;
}


void SPItem::adjust_stroke_width_recursive(double expansion)
{
    adjust_stroke (expansion);

// A clone's child is the ghost of its original - we must not touch it, skip recursion
    if (!is<SPUse>(this)) {
        for (auto& o: children) {
            auto item = cast<SPItem>(&o);
            if (item) {
                item->adjust_stroke_width_recursive(expansion);
            }
        }
    }
}

void SPItem::freeze_stroke_width_recursive(bool freeze)
{
    freeze_stroke_width = freeze;

// A clone's child is the ghost of its original - we must not touch it, skip recursion
    if (!is<SPUse>(this)) {
        for (auto& o: children) {
            auto item = cast<SPItem>(&o);
            if (item) {
                item->freeze_stroke_width_recursive(freeze);
            }
        }
    }
}

/**
 * Recursively adjust rx and ry of rects.
 */
static void
sp_item_adjust_rects_recursive(SPItem *item, Geom::Affine advertized_transform)
{
    auto rect = cast<SPRect>(item);
    if (rect) {
        rect->compensateRxRy(advertized_transform);
    }

    for(auto& o: item->children) {
        auto itm = cast<SPItem>(&o);
        if (itm) {
            sp_item_adjust_rects_recursive(itm, advertized_transform);
        }
    }
}

void SPItem::adjust_paint_recursive(Geom::Affine advertized_transform, Geom::Affine t_ancestors, PaintServerType type)
{
// _Before_ full pattern/gradient transform: t_paint * t_item * t_ancestors
// _After_ full pattern/gradient transform: t_paint_new * t_item * t_ancestors * advertised_transform
// By equating these two expressions we get t_paint_new = t_paint * paint_delta, where:
    Geom::Affine t_item = sp_item_transform_repr (this);
    Geom::Affine paint_delta = t_item * t_ancestors * advertized_transform * t_ancestors.inverse() * t_item.inverse();

// Within text, we do not fork gradients, and so must not recurse to avoid double compensation;
// also we do not recurse into clones, because a clone's child is the ghost of its original -
// we must not touch it
    if (!(cast<SPText>(this) || cast<SPUse>(this))) {
        for (auto& o: children) {
            auto item = cast<SPItem>(&o);
            if (item) {
                // At the level of the transformed item, t_ancestors is identity;
                // below it, it is the accumulated chain of transforms from this level to the top level
                item->adjust_paint_recursive(advertized_transform, t_item * t_ancestors, type);
            }
        }
    }

// We recursed into children first, and are now adjusting this object second;
// this is so that adjustments in a tree are done from leaves up to the root,
// and paintservers on leaves inheriting their values from ancestors could adjust themselves properly
// before ancestors themselves are adjusted, probably differently (bug 1286535)

    switch (type) {
        case PATTERN: {
            adjust_pattern(paint_delta);
            break;
        }
        case HATCH: {
            adjust_hatch(paint_delta);
            break;
        }
        default: {
            adjust_gradient(paint_delta);
        }
    }
}

bool SPItem::collidesWith(Geom::PathVector const &shape) const
{
    auto our_shape = documentExactBounds();
    return our_shape ? pathvs_have_nonempty_overlap(*our_shape, shape) : false;
}

bool SPItem::collidesWith(SPItem const &other) const
{
    auto other_shape = other.documentExactBounds();
    return other_shape ? collidesWith(*other_shape) : false;
}

// CPPIFY:: make pure virtual?
// Not all SPItems must necessarily have a set transform method!
Geom::Affine SPItem::set_transform(Geom::Affine const &transform) {
//	throw;
    return transform;
}

void SPItem::removeTransformsRecursively(SPObject const *root) {
    // The default action is to remove the transform from it's parents
    // but retain it in the given shape. Other item types may remove the
    // transform entirely.
    setAttribute("transform", sp_svg_transform_write(i2i_affine(this, root)));
}

/**
 * Return true if the item is referenced by an LPE.
 */
static bool is_satellite_item(SPItem const &item)
{
    for (SPObject const *ref : item.hrefList) {
        if (is<LivePathEffectObject>(ref)) {
            return true;
        }
    }
    return false;
}

bool SPItem::unoptimized() {
    if (auto path_effect = getAttribute("inkscape:path-effect")) {
        assert(path_effect[0]);
        return true;
    }

    if (is_satellite_item(*this)) {
        return true;
    }

    return false;
}

void SPItem::doWriteTransform(Geom::Affine const &transform, Geom::Affine const *adv, bool compensate)
{
    // Transform delta between the transform currently in the repr and the new transform.
    // This must be used for compensation (stroke, patterns, etc.), otherwise passing an
    // absolute transform via `adv` can cause repeated compensation on pure translations.
    Geom::Affine const relative_transform = sp_item_transform_repr(this).inverse() * transform;

    // calculate the relative transform, if not given by the adv attribute
    auto advertized_transform = adv != nullptr ? *adv : relative_transform;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (compensate) {
        // recursively compensating for stroke scaling will not always work, because it can be scaled to zero or infinite
        // from which we cannot ever recover by applying an inverse scale; therefore we temporarily block any changes
        // to the strokewidth in such a case instead, and unblock these after the transformation
        // (as reported in https://bugs.launchpad.net/inkscape/+bug/825840/comments/4)
        if (!prefs->getBool("/options/transform/stroke", true)) {
            double const expansion = 1. / relative_transform.descrim();
            if (expansion < 1e-9 || expansion > 1e9) {
                freeze_stroke_width_recursive(true);
                // This will only work if the item has a set_transform method (in this method adjust_stroke() will be called)
                // We will still have to apply the inverse scaling to other items, not having a set_transform method
                // such as ellipses and stars
                // PS: We cannot use this freeze_stroke_width_recursive() trick in all circumstances. For example, it will
                // break pasting objects within their group (because in such a case the transformation of the group will affect
                // the strokewidth, and has to be compensated for. See https://bugs.launchpad.net/inkscape/+bug/959223/comments/10)
            } else {
                adjust_stroke_width_recursive(expansion);
            }
        }

        // recursively compensate rx/ry of a rect if requested
        if (!prefs->getBool("/options/transform/rectcorners", true)) {
            sp_item_adjust_rects_recursive(this, relative_transform);
        }

        // recursively compensate pattern fill if it's not to be transformed
        if (!prefs->getBool("/options/transform/pattern", true)) {
            adjust_paint_recursive(relative_transform.inverse(), Geom::identity(), PATTERN);
        }
        if (!prefs->getBool("/options/transform/hatch", true)) {
            adjust_paint_recursive(relative_transform.inverse(), Geom::identity(), HATCH);
        }

        /// \todo FIXME: add the same else branch as for gradients below, to convert patterns to userSpaceOnUse as well
        /// recursively compensate gradient fill if it's not to be transformed
        if (!prefs->getBool("/options/transform/gradient", true)) {
            adjust_paint_recursive(relative_transform.inverse(), Geom::identity(), GRADIENT);
        } else {
            // this converts the gradient/pattern fill/stroke, if any, to userSpaceOnUse; we need to do
            // it here _before_ the new transform is set, so as to use the pre-transform bbox
            adjust_paint_recursive(Geom::identity(), Geom::identity(), GRADIENT);
        }

    } // endif(compensate)

    gint preserve = prefs->getBool("/options/preservetransform/value", false);
    Geom::Affine transform_attr (transform);

    // CPPIFY: check this code.
    // If onSetTransform is not overridden, CItem::onSetTransform will return the transform it was given as a parameter.
    // onSetTransform cannot be pure due to the fact that not all visible Items are transformable.
    auto lpeitem = cast<SPLPEItem>(this);
    if (lpeitem) {
        lpeitem->notifyTransform(transform);
    }
    bool unoptimiced = unoptimized();
    if ( // run the object's set_transform (i.e. embed transform) only if:
        (cast<SPText>(this) && firstChild() && cast<SPTextPath>(firstChild())) ||
             (!preserve && // user did not chose to preserve all transforms
             !getClipObject() && // the object does not have a clippath
             !getMaskObject() && // the object does not have a mask
             !(!transform.isTranslation() && style && style->getFilter()) &&
             !unoptimiced)
                // the object does not have a filter, or the transform is translation (which is supposed to not affect filters)
        )
    {
        transform_attr = this->set_transform(transform);
        // PROOF: the part taken into the object's own coordinates must not turn its box.
        if (!transform_attr.isSingular()) {
            Inkscape::box_angle_embed(*this, transform * transform_attr.inverse());
        }
    }
    if (freeze_stroke_width) {
        freeze_stroke_width_recursive(false);
        if (compensate) {
            if (!prefs->getBool("/options/transform/stroke", true)) {
                // Recursively compensate for stroke scaling, depending on user preference
                // (As to why we need to do this, see the comment a few lines above near the freeze_stroke_width_recursive(true) call)
                double const expansion = 1. / relative_transform.descrim();
                adjust_stroke_width_recursive(expansion);
            }
        }
    }
    // this avoid temporary scaling issues on display when near identity
    // this must be a bit grater than EPSILON * transform.descrim()
    double e = 1e-5 * transform.descrim();
    if (transform_attr.isIdentity(e)) {
        transform_attr = Geom::Affine();
    }
    set_item_transform(transform_attr);

    // Note: updateRepr comes before emitting the transformed signal since
    // it causes clone SPUse's copy of the original object to be brought up to
    // date with the original. Otherwise, sp_use_bbox returns incorrect
    // values if called in code handling the transformed signal.
    updateRepr();

    if (lpeitem) {
        if (!lpeitem->hasPathEffectOfType(Inkscape::LivePathEffect::SLICE)) {
            sp_lpe_item_update_patheffect(lpeitem, false, true);
        }
    }

    // send the relative transform with a _transformed_signal
    _transformed_signal.emit(&advertized_transform, this);
}

void SPItem::set_item_transform(Geom::Affine const &transform_matrix)
{
    if (!Geom::are_near(transform_matrix, transform, 1e-18)) {
        transform = transform_matrix;
        /* The SP_OBJECT_USER_MODIFIED_FLAG_B is used to mark the fact that it's only a
           transformation.  It's apparently not used anywhere else. */
        requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_USER_MODIFIED_FLAG_B);
    }
}

//void SPItem::convert_to_guides() const {
//	// CPPIFY: If not overridden, call SPItem::convert_to_guides() const, see below!
//	this->convert_to_guides();
//}

Geom::Affine i2anc_affine(SPObject const *object, SPObject const *ancestor)
{
    Geom::Affine ret;

    // Stop at first non-renderable ancestor.
    while (object != ancestor && is<SPItem>(object)) {
        if (auto root = cast<SPRoot>(object)) {
            ret *= root->c2p;
        } else {
            auto item = cast_unsafe<SPItem>(object);
            ret *= item->transform;
        }
        object = object->parent;
    }

    return ret;
}

Geom::Affine
i2i_affine(SPObject const *src, SPObject const *dest) {
    g_return_val_if_fail(src != nullptr && dest != nullptr, Geom::identity());
    SPObject const *ancestor = src->nearestCommonAncestor(dest);
    return i2anc_affine(src, ancestor) * i2anc_affine(dest, ancestor).inverse();
}

Geom::Affine SPItem::getRelativeTransform(SPObject const *dest) const {
    return i2i_affine(this, dest);
}

Geom::Affine SPItem::i2doc_affine() const
{
    return i2anc_affine(this, nullptr);
}

Geom::Affine SPItem::i2dt_affine() const
{
    return i2doc_affine() * document->doc2dt();
}

// TODO should be named "set_i2dt_affine"
void SPItem::set_i2d_affine(Geom::Affine const &i2dt)
{
    Geom::Affine dt2p; /* desktop to item parent transform */
    if (parent) {
        dt2p = static_cast<SPItem *>(parent)->i2dt_affine().inverse();
    } else {
        dt2p = document->dt2doc();
    }

    Geom::Affine const i2p( i2dt * dt2p );
    set_item_transform(i2p);
}


Geom::Affine SPItem::dt2i_affine() const
{
    /* fixme: Implement the right way (Lauris) */
    return i2dt_affine().inverse();
}

// Item views
Inkscape::DrawingItem *SPItem::get_arenaitem(unsigned key) const
{
    auto const view_it = std::find_if(views.begin(), views.end(), [key](SPItemView const &v) {
        return v.key == key;
    });
    return view_it == views.end() ? nullptr : view_it->drawingitem.get();
}

int sp_item_repr_compare_position(SPItem const *first, SPItem const *second)
{
    return sp_repr_compare_position(first->getRepr(), second->getRepr());
}

SPItem const *sp_item_first_item_child(SPObject const *obj)
{
    return sp_item_first_item_child( const_cast<SPObject *>(obj) );
}

SPItem *sp_item_first_item_child(SPObject *obj)
{
    SPItem *child = nullptr;
    for (auto& iter: obj->children) {
        auto tmp = cast<SPItem>(&iter);
        if ( tmp ) {
            child = tmp;
            break;
        }
    }
    return child;
}

void SPItem::convert_to_guides() const {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    int prefs_bbox = prefs->getInt("/tools/bounding_box", 0);

    Geom::OptRect bbox = (prefs_bbox == 0) ? desktopVisualBounds() : desktopGeometricBounds();
    if (!bbox) {
        g_warning ("Cannot determine item's bounding box during conversion to guides.\n");
        return;
    }

    std::list<std::pair<Geom::Point, Geom::Point> > pts;

    Geom::Point A((*bbox).min());
    Geom::Point C((*bbox).max());
    Geom::Point B(A[Geom::X], C[Geom::Y]);
    Geom::Point D(C[Geom::X], A[Geom::Y]);

    pts.emplace_back(A, B);
    pts.emplace_back(B, C);
    pts.emplace_back(C, D);
    pts.emplace_back(D, A);

    sp_guide_pt_pairs_to_guides(document, pts);
}

void SPItem::rotate_rel(Geom::Rotate const &rotation)
{
    Geom::Point center = getCenter();
    Geom::Translate const s(getCenter());
    Geom::Affine affine = Geom::Affine(s).inverse() * Geom::Affine(rotation) * Geom::Affine(s);

    // Rotate item.
    set_i2d_affine(i2dt_affine() * (Geom::Affine)affine);
    // Use each item's own transform writer, consistent with sp_selection_apply_affine()
    doWriteTransform(transform);

    // Restore the center position (it's changed because the bbox center changed)
    if (isCenterSet()) {
        setCenter(center * affine);
        updateRepr();
    }
}

void SPItem::scale_rel(Geom::Scale const &scale)
{
    Geom::OptRect bbox = desktopVisualBounds();
    if (bbox) {
        Geom::Translate const s(bbox->midpoint()); // use getCenter?
        set_i2d_affine(i2dt_affine() * s.inverse() * scale * s);
        doWriteTransform(transform);
    }
}

void SPItem::skew_rel(double skewX, double skewY)
{
    Geom::Point center = getCenter();
    Geom::Translate const s(getCenter());

    Geom::Affine const skew(1, skewY, skewX, 1, 0, 0);
    Geom::Affine affine = Geom::Affine(s).inverse() * skew * Geom::Affine(s);

    set_i2d_affine(i2dt_affine() * affine);
    doWriteTransform(transform);

    // Restore the center position (it's changed because the bbox center changed)
    if (isCenterSet()) {
        setCenter(center * affine);
        updateRepr();
    }
}

void SPItem::move_rel( Geom::Translate const &tr)
{
    set_i2d_affine(i2dt_affine() * tr);

    doWriteTransform(transform);
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
