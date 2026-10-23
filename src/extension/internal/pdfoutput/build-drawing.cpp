// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Provide a capypdf interface that understands 2geom, styles, etc.
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "build-drawing.h"

#include "build-document.h"
#include "build-page.h"
#include "build-patterns.h"
#include "build-text.h"
#include "object/sp-anchor.h"
#include "object/sp-clippath.h"
#include "object/sp-flowtext.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-marker.h"
#include "object/sp-mask.h"
#include "object/sp-page.h"
#include "object/sp-pattern.h"
#include "object/sp-root.h"
#include "object/sp-symbol.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "object/uri.h"
#include "style.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

/**
 * Set the transformation matrix for this context Group.
 */
void DrawContext::set_matrix(Geom::Affine const &tr)
{
    _ctx.set_group_matrix(tr[0], tr[1], tr[2], tr[3], tr[4], tr[5]);
}

/**
 * Add a transform to the current context stream.
 */
void DrawContext::transform(Geom::Affine const &tr)
{
    if (tr != Geom::identity()) {
        _ctx.cmd_cm(tr[0], tr[1], tr[2], tr[3], tr[4], tr[5]);
    }
}

// Some items cannot be drawn using pdf primitives alone.
// If this returns true, we should render the item to an image and insert that into the pdf instead.
bool DrawContext::should_rasterize(SPObject const &object, bool recursive)
{
    if (auto item = cast<SPItem>(&object)) {
        if (_doc.get_filter_resolution() && item->isFiltered()) {
            return true;
        }

        // Our rendering pipeline isn't set up to render something directly from defs.
        // So if the mask would need rendering, we flag the base object itself for rendering (which includes its mask).
        // Note that we don't also check the clip object, because clips don't support filters.
        if (auto mask = item->getMaskObject()) {
            if (should_rasterize(*mask, true)) {
                return true;
            }
        }
    }

    if (recursive) {
        for (auto &child : object.children) {
            if (should_rasterize(child, true)) {
                return true;
            }
        }
    }

    return false;
}

/**
 * PROOF: whether what an item holds blends only inside the item.
 *
 * A group with anything of its own to apply is written as a transparency group of its own
 * (one without is drawn in place: paint_item_group). Isolated, such a group starts
 * from nothing, and a shape in it that is set to Multiply darkens only what else is in the
 * group: in a layer or a plain group it comes out as its own flat colour, and prints so.
 * A group is isolated on the canvas only when it has opacity, a blend mode, a mask or a
 * filter of its own, or is told to be (`isolation`). A plain group, a layer and a group
 * with only a clipping path aren't, and neither are they here: what is in them blends with
 * what lies under them, as it shows, and as in Illustrator.
 */
bool is_isolated(SPItem const *item)
{
    if (!is<SPGroup>(item)) {
        // A shape, a clone, text or a picture has nothing in it that looks under itself.
        return true;
    }
    auto const *style = item->style;
    if (!style) {
        return true;
    }
    return style->opacity.as_double() < 1.0
        || (style->mix_blend_mode.set && style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL)
        || style->isolation.value == SP_CSS_ISOLATION_ISOLATE
        || item->getMaskObject()
        || item->isFiltered();
}

GroupContext::GroupContext(Document &doc, Geom::OptRect const &clip, bool soft_mask, bool isolate)
    : DrawContext(doc,
                  doc.generator().new_transparency_group_context(
                      // CapyPDF is very strict about clipping regions being valid. We want to
                      // be more flexible to allow our painting flow to be less repetative
                      // So we check the clip before constructing the new capypdf context.
                      clip ? clip->left() : 0, clip ? clip->top() : 0,
                      clip ? std::max(clip->right(), clip->left() + 0.0001) : 1,
                      clip ? std::max(clip->bottom(), clip->top() + 0.0001) : 1),
                  soft_mask)
{
    capypdf::TransparencyGroupProperties props;
    if (soft_mask) {
        props.set_CS(CAPY_DEVICE_CS_GRAY);
    } else {
        // Do groups have a color space?
    }
    props.set_I(isolate);
    props.set_K(false); // Do not knock out
    _ctx.set_transparency_group_properties(props);
}

/**
 * Paint the given object into the given context, making groups if needed.
 */
void DrawContext::paint_item(SPItem const *item, Geom::Affine const &tr, SPStyle const *context_style)
{
    // Directly painted shapes, images and text must honour visibility just as
    // item_to_transparency_group does. Hidden native objects remain editable.
    if (item->isHidden()) return;
    // Special exception, return without drawing anything when using LaTeX
    if (!_doc.get_text_enabled() && (is<SPText>(item) || is<SPFlowtext>(item))) {
        return;
    }

    // Normally groups inherit from their parent, PDF and SVG are the same. But markers don't exist
    // in PDF land, so their special style handling which does NOT inherit must be a special case.
    auto style_map = is<SPMarker>(item)
        ? _doc.paint_memory().get_changes(item->style)
        : _doc.paint_memory().get_ifset(item->style);

    auto style_scope = _doc.paint_memory().remember(style_map);
    auto rasterize = should_rasterize(*item);

    bool isolate = tr != Geom::identity() || !style_map.empty() || true; // has_pattern || has_opacity etc etc
    if (isolate) {
        // Isolate everything in the item
        _ctx.cmd_q();

        if (!rasterize) {
            transform(tr);
            // Set styles for cascading
            // Gradient units refer to this shape's local geometric bounds, not
            // the union of all objects which happen to use the same paint server.
            set_paint_style(style_map, item->style, context_style,
                            is<SPShape>(item) ? item->geometricBounds() : Geom::OptRect{});
        }

        // This text is not affected by the get_text_enabled option.
        if (auto text_clip = item->getClipTextObject()) {
            clip_text_layout(text_clip->layout);
        } else {
            // The clip geometry owns clip-rule, independently of the clipped artwork.
            // A single path (including a compound path) retains its winding rule.
            // Multiple clip objects still use getClipPathVector's merged geometry.
            SPItem const *geometry = nullptr;
            if (auto clip = item->getClipObject()) {
                for (auto const &child : clip->children) {
                    if (auto shape = cast<SPItem>(&child)) {
                        if (geometry) {
                            geometry = nullptr;
                            break;
                        }
                        geometry = shape;
                    }
                }
            }
            set_clip_path(item->getClipPathVector(), geometry ? geometry->style : nullptr);
        }
    }

    // These styles are never cascaded because of the complexity in PDF transparency groups.
    if (!rasterize && !is<SPGroup>(item) && !_soft_mask) {
        if (auto gsid = _doc.get_shape_graphics_state(item->style)) {
            _ctx.cmd_gs(*gsid);
        }
    }

    if (rasterize) {
        // Turn the item into a raster for the PDF
        paint_item_to_raster(item, tr, _doc.get_filter_resolution(), true);
    } else if (auto shape = cast<SPShape>(item)) {
        if (shape->curve() && !shape->curve()->empty()) {
            paint_shape(shape, context_style);
        }
    } else if (auto use = cast<SPUse>(item)) {
        paint_item_clone(use, context_style);
    } else if (auto text = cast<SPText>(item)) {
        paint_text_layout(text->layout, context_style);
    } else if (auto flowtext = cast<SPFlowtext>(item)) {
        paint_text_layout(flowtext->layout, context_style);
    } else if (auto image = cast<SPImage>(item)) {
        paint_raster(image);
    } else if (auto group = cast<SPGroup>(item)) { // SPSymbol, SPRoot, SPMarker

        // Optional Content Group tracks layers
        bool has_ocg = false;
        if (group->isLayer()) {
            if (auto label = group->label()) {
                auto ocg = capypdf::OptionalContentGroup(label);
                start_ocg(_doc._gen.add_optional_content_group(ocg));
                has_ocg = true;
            }
        }

        paint_item_group(group, context_style);

        if (has_ocg) {
            end_ocg();
        }
    } else {
        g_warning("Unknown object: %s", get_id(item).c_str());
    }
    if (isolate) {
        _ctx.cmd_Q();
    }
}

void DrawContext::paint_item_group(SPGroup const *group, SPStyle const *context_style)
{
    // Render children in the group
    for (auto &obj : group->children) {
        if (auto child_item = cast<SPItem>(&obj)) {
            // Calculate a soft mask
            // const cast because mask references are not created and tracked properly.
            std::optional<CapyPDF_TransparencyGroupId> mask_id;
            auto mask = child_item->getMaskObject();
            // If the item needs to be rasterized, that process will handle masking for us, no need to do it ourselves
            if (mask && !should_rasterize(*child_item)) {
                mask_id = _doc.mask_to_transparency_group(mask, child_item->transform);
            }

            // Find out if this object is a source for a clone
            std::vector<SPObject *> links;
            child_item->getLinkedRecursive(links, SPObject::LinkedObjectNature::DEPENDENT);

            // Try not creating groups for *every* shape if they don't need them.
            if (!is<SPGroup>(child_item) && !mask_id && links.empty() && !style_needs_group(child_item->style)) {
                // Paint the child-shape directly
                paint_item(child_item, child_item->transform, context_style);

            } else if (is<SPGroup>(child_item) && !is<SPAnchor>(child_item) && !is<SPSymbol>(child_item) &&
                       !is<SPMarker>(child_item) && !mask_id && links.empty() && !is_isolated(child_item) &&
                       !style_needs_group(child_item->style) &&
                       !(cast<SPGroup>(child_item)->isLayer() && _open_ocgs > 0)) {
                // PROOF: a group with nothing of its own to apply (a layer, a plain or clipped group)
                // is drawn where it is, as Illustrator's pages draw one. A non-isolated transparency
                // group of its own is the same in PDF, but Poppler's Cairo backend, which PROOF's
                // check of an .ai against its page and its PDF import draw with, starts every group
                // from nothing: a Multiply in it came out flat over the colours under it. A layer in
                // a layer keeps a group of its own: its marked content may not nest in the other's.
                if (!child_item->isHidden()) {
                    paint_item(child_item, child_item->transform, context_style);
                }
            } else if (auto item_id = _doc.item_to_transparency_group(child_item, context_style)) {
                // Each reused transparency group has to re-specify it's transform and opacity settings
                // since PDF applies properties from the outside of the group being drawn.
                paint_group(*item_id, child_item->style, Geom::identity(), mask_id);
            }
        }
    }
}

/**
 * Paint the given clone object, finding or generating a transparency group from it.
 */
void DrawContext::paint_item_clone(SPUse const *use, SPStyle const *context_style)
{
    // Children contains a copy of the clone with the right context style
    // if (auto child_item = use->get_original()) {
    for (auto &child_obj : use->children) {
        if (auto child_item = cast<SPItem>(&child_obj)) {
            // Consume the SPUse object as the context style
            if (auto item_id = _doc.item_to_transparency_group(child_item, use->style)) {
                paint_group(*item_id, child_item->style, Geom::Translate(use->x.computed, use->y.computed));
            } else {
                g_warning("Couldn't paint clone: '%s'", get_id(use).c_str());
            }
        }
    }
}

/**
 * Paint a child group at the requested location
 */
void DrawContext::paint_group(CapyPDF_TransparencyGroupId child_id, SPStyle const *style, Geom::Affine const &tr,
                              std::optional<CapyPDF_TransparencyGroupId> soft_mask)
{
    auto gsid = _doc.get_group_graphics_state(style, soft_mask);

    if (gsid || tr != Geom::identity()) {
        _ctx.cmd_q();
    }

    transform(tr);
    if (gsid) {
        _ctx.cmd_gs(*gsid);
    }

    _ctx.cmd_Do(child_id);

    if (gsid || tr != Geom::identity()) {
        _ctx.cmd_Q();
    }
}

/**
 * Paint a single shape path
 *
 * @param shape - The shape we want to paint
 */
void DrawContext::paint_shape(SPShape const *shape, SPStyle const *context_style)
{
    auto const style = shape->style;

    bool evenodd = style->fill_rule.computed == SP_WIND_RULE_EVENODD;
    for (auto layer : get_paint_layers(style, context_style)) {
        switch (layer) {
            case PAINT_FILLSTROKE:
                if (set_shape(shape)) {
                    if (evenodd) {
                        _ctx.cmd_bstar();
                    } else {
                        _ctx.cmd_b();
                    }
                } else { // Not closed path
                    if (evenodd) {
                        _ctx.cmd_Bstar();
                    } else {
                        _ctx.cmd_B();
                    }
                }
                break;
            case PAINT_FILL:
                // Fill only without stroke, either because it's only fill, or not in order
                set_shape(shape);

                if (evenodd) {
                    _ctx.cmd_fstar();
                } else {
                    _ctx.cmd_f();
                }
                break;
            case PAINT_STROKE:
                // Stroke only without fill, either because it's only stroke, or not in order
                if (set_shape(shape)) {
                    _ctx.cmd_s();
                } else { // Not closed path
                    _ctx.cmd_S();
                }
                break;
            case PAINT_MARKERS:
                // Markers can still be visible if no_stroke is true
                for (auto [loc, marker, tr] : shape->get_markers()) {
                    // Isolate each marker render
                    if (auto item_id = _doc.item_to_transparency_group(marker, style, _soft_mask)) {
                        // We don't pass on the style at this stage
                        paint_group(*item_id, nullptr, tr);
                    }
                }
                break;
        }
    }
}

/**
 * Apply the clip path to the existing context.
 */
void DrawContext::set_clip_path(std::optional<Geom::PathVector> clip, SPStyle *style)
{
    if (clip) {
        set_shape_pathvector(*clip);
        // Default to NONZERO when style is nullptr.
        if (style && style->clip_rule.computed == SP_WIND_RULE_EVENODD) {
            _ctx.cmd_Wstar();
        } else {
            _ctx.cmd_W();
        }
        _ctx.cmd_n();
    }
}

/**
 * Apply the clipping rectangle with a NONZERO fill rule.
 */
void DrawContext::set_clip_rectangle(Geom::OptRect const &rect)
{
    if (rect) {
        set_clip_path(Geom::PathVector(Geom::Path(*rect)));
    }
}

void DrawContext::start_ocg(CapyPDF_OptionalContentGroupId ocgid)
{
    _ctx.cmd_BDC(ocgid);
    ++_open_ocgs;
}

void DrawContext::end_ocg()
{
    _ctx.cmd_EMC();
    --_open_ocgs;
}

ItemContext::ItemContext(Document &doc, SPItem const *item)
    : GroupContext(doc, item ? item->visualBounds(Geom::identity(), true, false, true) : Geom::OptRect(), false)
    , _item{item}
{}

bool ItemContext::is_valid() const
{
    return !_item->isHidden();
}

ItemCacheKey ItemContext::cache_key() const
{
    return {get_document_id(_item->document), get_id(_item), "", ""};
}

void ItemContext::paint()
{
    paint_item(_item);
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
