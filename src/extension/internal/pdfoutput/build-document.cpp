// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Build a capypdf document.
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024-2025 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "build-document.h"

#include <cstdio>
#include <format>
#include <glib/gstdio.h>

#include "build-drawing.h"
#include "build-page.h"
#include "build-text.h"
#include "display/psd-pixels.h"
#include "display/tiff-pixels.h"
#ifdef WITH_POPPLER
#include "extension/internal/pdfinput/placed-page.h"
#endif
#include "object/sp-anchor.h"
#include "object/sp-flowtext.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-marker.h"
#include "object/sp-mask.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "object/sp-symbol.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "style.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

/**
 * Attempt to get an object's id, even if it's a clone.
 */
std::string get_id(SPObject const *obj)
{
    if (auto id = obj->getId()) {
        return id;
    }
    if (auto repr_id = obj->getRepr()->attribute("id")) {
        return repr_id;
    }
    g_error("Object doesn't have any sort of id.");
}

std::string get_document_id(SPDocument const *doc)
{
    if (!doc) {
        return "";
    }
    // Filename based documents
    if (auto fn = doc->getDocumentFilename()) {
        return fn;
    }
    // Memory based documents (a translated string that includes a unique number)
    if (auto name = doc->getDocumentName()) {
        return name;
    }
    g_warning("Couldn't get document_id for PDF output, there may be cache errors.");
    return "unkown"; // Failure, not empty string
}

void Document::set_label(uint32_t page, std::string const &label)
{
    _gen.add_page_labeling(page, {}, label, {});
}

void Document::add_page(PageContext &page)
{
    page.finalize();
    _gen.add_page(page._ctx);
}

std::optional<CapyPDF_TransparencyGroupId> Document::add_group(ItemContext &group_ctx)
{
    if (!group_ctx.is_valid()) {
        return {};
    }
    auto const cache_key = group_ctx.cache_key();

    if (auto const it = _item_cache.find(cache_key); it != _item_cache.end()) {
        return it->second;
    }

    group_ctx.paint();

    auto const item_id = _gen.add_transparency_group(group_ctx._ctx);
    _item_cache[cache_key] = item_id;

    return item_id;
}

/**
 * Render any type of item into a transparency group.
 *
 * @arg item - The item to render into a TransparencyGroup
 * @arg context_style - Used only for rendering markers with context-fill and context-stroke
 * @arg is_soft_mask - Used to indicate that this transparency group is a soft mask use
 */
std::optional<CapyPDF_TransparencyGroupId>
Document::item_to_transparency_group(SPItem const *item, SPStyle const *context_style, bool is_soft_mask)
{
    if (item->isHidden()) {
        return {};
    }

    // Items are cached so they can be reused
    ItemCacheKey cache_key = {get_document_id(item->document), get_id(item), "", ""};
    auto tr = item->transform;

    // Complex caching key modification for when marker styles changes because of context styles
    if (context_style) {
        bool fill_used = false, stroke_used = false;
        get_context_use_recursive(item, fill_used, stroke_used);
        if (fill_used) {
            std::optional<double> opacity;
            if (is_soft_mask) {
                opacity = context_style->fill_opacity.as_double();
            }
            cache_key = {std::get<0>(cache_key), std::get<1>(cache_key), paint_to_cache_key(context_style->fill, opacity), std::get<3>(cache_key)};
        }
        if (stroke_used) {
            std::optional<double> opacity;
            if (is_soft_mask) {
                opacity = context_style->stroke_opacity.as_double();
            }
            cache_key = {std::get<0>(cache_key), std::get<1>(cache_key), std::get<2>(cache_key), paint_to_cache_key(context_style->stroke, opacity)};
        }
    }
    if (auto marker = cast<SPMarker>(item)) {
        tr = marker->c2p * tr;
    }

    if (auto const it = _item_cache.find(cache_key); it != _item_cache.end()) {
        return it->second;
    }

    // Groups require pre-defined clipping regions which must not be transformed
    auto bbox = item->visualBounds(Geom::identity(), true, false, true);
    if (!bbox || bbox->width() == 0 || bbox->height() == 0) {
        return {};
    }

    // Remember all anchors for later post-processing
    if (auto anchor = cast<SPAnchor>(item)) {
        _anchors.insert(anchor);
    }

    // Draw item on a group so a mask, blend-mode, used-by-clone or opacity can be applied to it globally.
    auto group_ctx = PdfBuilder::GroupContext(*this, *bbox, is_soft_mask, is_soft_mask || is_isolated(item));
    group_ctx.set_matrix(tr);
    group_ctx.paint_item(item, Geom::identity(), context_style);

    // We save the group_ctx id so it can be painted in any other contexts (symbols, clones, markers, etc)
    auto const item_id = _gen.add_transparency_group(group_ctx._ctx);
    _item_cache[cache_key] = item_id;
    return item_id;
}

/**
 * Render a mask out to a transparency group context.
 */
std::optional<CapyPDF_TransparencyGroupId> Document::mask_to_transparency_group(SPMask const *mask,
                                                                                Geom::Affine const &transform)
{
    // Note: This would normally run through item_to_transparency_group, but SPMask isn't an SPItem

    // Items are cached so they can be reused
    std::string cache_key = get_id(mask);

    if (auto const it = _mask_cache.find(cache_key); it != _mask_cache.end()) {
        return it->second;
    }

    auto bbox = mask->visualBounds(transform);
    if (!bbox) {
        return {};
    }

    auto group_ctx = PdfBuilder::GroupContext(*this, *bbox);
    // A luminosity mask needs an explicit blending space, including when another
    // mask composites it. Without it, translucent white can become fully opaque.
    capypdf::TransparencyGroupProperties props;
    props.set_CS(CAPY_DEVICE_CS_RGB);
    props.set_I(true);
    props.set_K(false);
    group_ctx._ctx.set_transparency_group_properties(props);
    group_ctx.transform(transform);

    for (auto &obj : mask->children) {
        if (auto child_item = cast<SPItem>(&obj)) {
            std::optional<CapyPDF_TransparencyGroupId> nested_mask;
            if (auto child_mask = child_item->getMaskObject(); child_mask && !group_ctx.should_rasterize(*child_item)) {
                nested_mask = mask_to_transparency_group(child_mask, child_item->transform);
            }
            if (auto item_id = item_to_transparency_group(child_item)) {
                group_ctx.paint_group(*item_id, child_item->style, Geom::identity(), nested_mask);
            }
        }
    }

    auto const item_id = _gen.add_transparency_group(group_ctx._ctx);
    _mask_cache[cache_key] = item_id;
    return item_id;
}

/**
 * Render gradient transparencies into a transparency mask
 */
std::optional<CapyPDF_TransparencyGroupId> Document::style_to_transparency_mask(SPStyle const *style,
                                                                                SPStyle const *context_style)
{
    Geom::OptRect bbox;
    std::vector<SPObject *> objects;
    if (style->fill.set && style->fill.href) {
        if (auto gradient = cast<SPGradient>(style->fill.href->getObject())) {
            if (gradient_has_transparency(gradient)) {
                gradient->getLinkedRecursive(objects, SPObject::LinkedObjectNature::DEPENDENT);
                bbox.unionWith(gradient->getAllItemsBox());
            }
        }
    }
    if (style->stroke.set && style->stroke.href) {
        if (auto gradient = cast<SPGradient>(style->stroke.href->getObject())) {
            if (gradient_has_transparency(gradient)) {
                gradient->getLinkedRecursive(objects, SPObject::LinkedObjectNature::DEPENDENT);
                bbox.unionWith(gradient->getAllItemsBox());
            }
        }
    }

    if (!objects.empty() && bbox) {
        auto gradient_mask = PdfBuilder::GroupContext(*this, *bbox, true);
        bool painted = false;
        for (auto obj : objects) {
            if (auto item = cast<SPItem>(obj)) {
                auto style_map = _paint_memory.get_ifset(item->style);
                gradient_mask.set_paint_style(style_map, item->style, context_style);
                gradient_mask.paint_item(item, Geom::identity(), context_style);
                painted = true;
            }
        }
        if (painted) {
            return _gen.add_transparency_group(gradient_mask._ctx);
        }
    }
    return {};
}

/**
 * Load an anchor link and add it to the page.
 *
 * @arg page - Limit the anchors to just this page.
 * @arg page_tr - The transformation in the PDF used to place the anchor's box.
 */
std::vector<CapyPDF_AnnotationId> Document::get_anchors_for_page(SPPage const *page)
{
    auto page_tr = PageContext::page_transform(page);
    std::vector<CapyPDF_AnnotationId> result;
    for (auto a : _anchors) {
        auto bbox = a->visualBounds(a->i2doc_affine() * page_tr, true, false, true);
        if (!bbox || !a->href || !page->itemOnPage(a)) {
            continue;
        }

        auto annot = capypdf::Annotation();
        annot.set_rectangle(bbox->left(), bbox->bottom(), bbox->right(), bbox->top());
        annot.set_flags(CAPY_ANNOTATION_FLAG_HIDDEN);

        if (a->local_link) {
            auto obj = a->local_link->getObject();
            auto dest = capypdf::Destination();
            if (auto target_page = cast<SPPage>(obj)) {
                dest.set_page_fit(target_page->getPageIndex());
                annot.set_destination(dest);
            } else if (auto item = cast<SPItem>(obj)) {
                auto target_page = item->document->getPageManager().getPageFor(item, false);
                auto target_tr = PageContext::page_transform(target_page);
                auto item_box = item->visualBounds(item->i2doc_affine() * target_tr);
                dest.set_page_xyz(target_page->getPageIndex(), {}, item_box->bottom(), {});
                annot.set_destination(dest);
            } else {
                // This happens because of an Inkscape bug elsewhere in the code.
                annot.set_uri(std::string(a->href));
            }
        } else {
            // This pathway is currently not working because of the above bug
            annot.set_uri(std::string(a->href));
        }
        result.push_back(_gen.add_annotation(annot));
    }
    return result;
}

/**
 * PROOF: a picture from its own pixels (display/raster-pixels.h): a Photoshop file's merged
 * image, which CapyPDF doesn't read, or a TIFF of CMYK or with transparency. Its CMYK goes out
 * as its numbers, DeviceCMYK under the document's output profile (Illustrator's "Preserve
 * Numbers"), not tagged with the picture's own profile, which a RIP would convert from. Its
 * transparency goes into the SMask straight, though Photoshop keeps it premultiplied or matted,
 * and every sample keeps its depth. CapyPDF's own TIFF reader gives every pixel the first
 * pixel's alpha (separate_tif_alpha in imagefileops.cpp; PROOF-cmyk-pictures.md), so a cut-out
 * picture vanished from the PDF.
 */
std::optional<CapyPDF_ImageId> Document::add_own_pixels(std::string_view data, capypdf::ImagePdfProperties &props)
{
    std::optional<RasterPixels> tiff;
    if (looks_like_psd(data)) {
        tiff = read_psd(data);
    } else if (looks_like_tiff(data)) {
        tiff = read_tiff(data);
        // Others keep CapyPDF's own reading, and with it their profile.
        if (tiff && tiff->model != RasterPixels::Model::CMYK && tiff->alpha.empty()) return {};
    }
    if (!tiff) return {};
    // PDF's samples of 16 bits are big-endian.
    auto big_endian = [&](std::vector<std::uint8_t> &samples) {
        if (tiff->bits != 16) return;
        auto *p = reinterpret_cast<std::uint16_t *>(samples.data());
        for (std::size_t i = 0; i < samples.size() / 2; ++i) {
            auto const v = p[i];
            samples[2 * i] = std::uint8_t(v >> 8);
            samples[2 * i + 1] = std::uint8_t(v & 0xff);
        }
    };
    big_endian(tiff->colour);
    big_endian(tiff->alpha);
    auto builder = capypdf::RasterImageBuilder();
    builder.set_size(tiff->width, tiff->height);
    builder.set_colorspace(tiff->model == RasterPixels::Model::CMYK  ? CAPY_IMAGE_CS_CMYK
                           : tiff->model == RasterPixels::Model::RGB ? CAPY_IMAGE_CS_RGB
                                                                   : CAPY_IMAGE_CS_GRAY);
    builder.set_pixel_depth(tiff->bits);
    builder.set_pixel_data(reinterpret_cast<char const *>(tiff->colour.data()), tiff->colour.size());
    if (!tiff->alpha.empty()) {
        builder.set_alpha_depth(tiff->bits);
        builder.set_alpha_data(reinterpret_cast<char const *>(tiff->alpha.data()), tiff->alpha.size());
    }
    auto image = builder.build();
    return _gen.add_image(image, props);
}

std::optional<std::pair<CapyPDF_TransparencyGroupId, Geom::Rect>> Document::placed_page(std::string const &filename)
{
    if (auto const it = _placed_cache.find(filename); it != _placed_cache.end()) {
        return it->second;
    }
    auto &placed = _placed_cache[filename];
#ifdef WITH_POPPLER
    gchar head[8] = {};
    if (FILE *f = g_fopen(filename.c_str(), "rb")) {
        auto const n = std::fread(head, 1, 5, f);
        std::fclose(f);
        if (n != 5) return placed;
    }
    if (!looks_like_pdf(std::string_view(head, 5))) return placed;
    std::string why;
    auto const crop = placed_page_crop(filename, &why);
    if (!crop) {
        g_warning("A placed page can't go into the PDF: %s (%s)", why.c_str(), filename.c_str());
        return placed;
    }
    // A stand-in that names itself, to be found and replaced once the file is written: a path
    // drawn with nothing, where no page draws (CapyPDF's C++ side has no marked content).
    double const name = 7'700'000.0 + double(_placed.size() + 1);
    auto const marker = std::format("{:f} {:f} {:f} {:f} re", name, name, 1.0, 1.0);
    GroupContext stand_in(*this, *crop, false, false);
    stand_in._ctx.cmd_re(name, name, 1.0, 1.0);
    stand_in._ctx.cmd_n();
    placed = std::pair(_gen.add_transparency_group(stand_in._ctx), *crop);
    _placed.push_back({filename, marker});
#endif
    return placed;
}

void Document::write()
{
    _gen.write();
    std::string error;
    if (!splice_placed_pages(_filename, _placed, error)) {
        throw PlacedPagesFailed(error);
    }
}

std::optional<CapyPDF_ImageId> Document::get_image(std::string const &filename, capypdf::ImagePdfProperties &props)
{
    if (auto const it = _raster_cache.find(filename); it != _raster_cache.end()) {
        return it->second;
    }
    {
        gchar *data = nullptr;
        gsize len = 0;
        if (g_file_get_contents(filename.c_str(), &data, &len, nullptr)) {
            auto tiff = add_own_pixels(std::string_view(data, len), props);
            g_free(data);
            if (tiff) {
                _raster_cache[filename] = *tiff;
                return tiff;
            }
        }
    }
    auto image = _gen.load_image(filename.c_str());
    auto raster_id = _gen.add_image(image, props);
    _raster_cache[filename] = raster_id;
    return raster_id;
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
