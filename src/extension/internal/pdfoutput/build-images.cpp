// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Specific functionality for image handling
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "bad-uri-exception.h"
#include "build-drawing.h"
#include "build-page.h"
#include "display/cairo-utils.h"
#include "display/drawing-item.h"
#include "display/tiff-pixels.h"
#include "document.h"
#include "helper/pixbuf-ops.h"
#include "io/link-policy.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "object/uri.h"
#include "page-manager.h"
#include "style.h"
#include "svg/svg.h"
#include "util/units.h"
#include "util/uri.h"
#include "xml/href-attribute-helper.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

static CapyPDF_Image_Interpolation get_interpolation(SPImageRendering rendering)
{
    switch (rendering) {
        case SP_CSS_IMAGE_RENDERING_OPTIMIZEQUALITY:
            return CAPY_INTERPOLATION_SMOOTH;
            break;
        case SP_CSS_IMAGE_RENDERING_OPTIMIZESPEED:
        case SP_CSS_IMAGE_RENDERING_PIXELATED:
        case SP_CSS_IMAGE_RENDERING_CRISPEDGES:
            return CAPY_INTERPOLATION_PIXELATED;
            break;
        case SP_CSS_IMAGE_RENDERING_AUTO:
        default:
            break;
    }
    return CAPY_INTERPOLATION_AUTO;
}

/**
 * Draw the raster data stored in URI into the PDF context.
 */
void DrawContext::paint_raster(SPImage const *image)
{
    auto image_box = image->bbox(Geom::identity(), SPItem::GEOMETRIC_BBOX);
    if (!image_box) {
        g_warning("Couldn't get the size of image, ignoring in PDF output.");
        return;
    }

    auto props = capypdf::ImagePdfProperties();
    props.set_interpolate(get_interpolation(image->style->image_rendering.computed));
    // TODO: props.set_conversion_intent(...)

    // If pixbuf is requested AFTER getURI it will sometimes return zero. This is a bug.
    auto img_width = image->pixbuf->width();
    auto img_height = image->pixbuf->height();

    auto href = Inkscape::getHrefAttribute(*image->getRepr()).second;
    auto [base64, base64_type] = extract_uri_data(href);
    std::optional<CapyPDF_ImageId> raster_id;
    std::unique_ptr<SPDocument> svg;
    std::optional<std::pair<CapyPDF_TransparencyGroupId, Geom::Rect>> placed;

    gsize decoded_len = 0;
    const char *decoded = nullptr;

    if (base64_type == Base64Data::NONE) {
        // We don't use Inkscape::URI for base64 because if it's memory limits
        try {
            auto uri = image->getURI();
            if (!Inkscape::IO::may_follow_link(uri.toNativeFilename(), image->document->getDocumentBase())) {
                // PROOF: a file on another computer isn't looked up (io/link-policy.h).
            } else if (uri.getMimeType() == "image/svg+xml") {
                svg = SPDocument::createNewDoc(uri.toNativeFilename().c_str());
            } else if ((placed = _doc.placed_page(uri.toNativeFilename(), image->pdfPage()))) {
                // PROOF: a placed PDF page (a linked .pdf or .ai), the page itself.
            } else {
                // This is cached against the doc cache as the same filename might
                // be loaded multiple times in the same document.
                raster_id = _doc.get_image(uri.toNativeFilename(), props);
            }
        } catch (Inkscape::BadURIException const &e) {
            g_warning("Couldn't read image: %s", e.what());
        } catch (std::exception const &e) {
            g_warning("Could not add image file to PDF: %s", e.what());
        }
    } else {
        decoded = (char *)g_base64_decode(base64, &decoded_len);
    }

    // PROOF: an Illustrator picture of CMYK or grey keeps its samples apart from the RGB it
    // shows with (proof:samples, a TIFF); they go to print, while the picture is still theirs.
    if (auto const *samples = image->getRepr()->attribute("proof:samples")) {
        auto [samples_base64, samples_type] = extract_uri_data(samples);
        if (samples_type == Base64Data::RASTER) {
            gsize len = 0;
            auto *bytes = reinterpret_cast<char *>(g_base64_decode(samples_base64, &len));
            std::string_view const tiff(bytes, len);
            if (auto pixels = read_tiff(tiff); pixels && int(pixels->width) == img_width &&
                                               int(pixels->height) == img_height) {
                try {
                    raster_id = _doc.add_own_pixels(tiff, props, true);
                } catch (std::exception const &e) {
                    g_warning("Could not add image samples to PDF: %s", e.what());
                }
            }
            g_free(bytes);
        }
    }

    if (decoded && base64_type == Base64Data::RASTER && !raster_id) {
        try {
            raster_id = _doc.add_own_pixels(std::string_view(decoded, decoded_len), props);
            if (!raster_id) {
                // Note this memory image is not cached.
                auto image = _doc._gen.load_image_from_memory(decoded, decoded_len);
                raster_id = _doc._gen.add_image(image, props);
            }
        } catch (std::exception &e) {
            g_warning("Could not add image file to PDF: %s", e.what());
        }
    }

    if (decoded && base64_type == Base64Data::SVG) {
        // Sizing and painting the loaded document depends on the SVG being
        // up to date and having a viewBox and width/height that makes sense.
        svg = SPDocument::createNewDocFromMem({decoded, decoded_len});
    }

    if (svg) {
        svg->ensureUpToDate();
        auto doc_width = svg->getWidth().value("px");
        auto doc_height = svg->getHeight().value("px");

        // The inside is how the SVG is painted wrt its own viewbox in the svg tag.
        auto inside_box = svg->getRoot()->get_paintbox(doc_width, doc_height, svg->getViewBox());
        auto inside =
            Geom::Affine(inside_box->width(), 0, 0, inside_box->height(), inside_box->left(), inside_box->bottom());

        // The outside is where on the document this svg will be placed.
        auto outside_box = image->get_paintbox(doc_width, doc_height, image_box);
        auto outside =
            Geom::Affine(outside_box->width(), 0, 0, outside_box->height(), outside_box->left(), outside_box->bottom());

        if (auto drawing_id = _doc.item_to_transparency_group(svg->getRoot())) {
            _ctx.cmd_q();
            // Clip to the outside box, because svg's can run over their defined edges.
            set_clip_rectangle(outside_box);

            // The inverse of Inside reduces the svg rendering to a unit square.
            transform(inside.inverse() * outside);

            _ctx.cmd_Do(*drawing_id);
            _ctx.cmd_Q();
        } else {
            g_warning("Unable to paint embedded SVG image into PDF.");
        }
    } else if (placed) {
        // The page's crop box (PDF units, y up) onto where the picture goes, as a picture's
        // pixels are: its x across, its y turned down.
        auto const &[group, crop] = *placed;
        auto const paint_box = image->get_paintbox(crop.width(), crop.height(), image_box);
        double const sx = paint_box->width() / crop.width(), sy = paint_box->height() / crop.height();
        _ctx.cmd_q();
        set_clip_rectangle(image_box);
        transform(Geom::Affine(sx, 0, 0, -sy, paint_box->left() - crop.left() * sx, paint_box->bottom() + crop.top() * sy));
        _ctx.cmd_Do(group);
        _ctx.cmd_Q();
    } else if (raster_id) {
        // Format the width and height into a transformation matrix, the image is a unit square painted
        // from the bottom upwards so must be scaled out and flipped. A slice's paint box
        // can be larger than its viewport: clip before applying the pixel transform, as
        // the canvas does, keeping any existing explicit clip as well.
        auto paint_box = image->get_paintbox(img_width, img_height, image_box);
        auto outside =
            Geom::Affine(paint_box->width(), 0, 0, -paint_box->height(), paint_box->left(), paint_box->bottom());

        _ctx.cmd_q();
        set_clip_rectangle(image_box);
        transform(outside);
        _ctx.cmd_Do(*raster_id);
        _ctx.cmd_Q();
    } else {
        g_warning("No image loaded for image tag.");
    }

    g_free(const_cast<char *>(decoded));
}

/**
 * Draw an item as a bitmap and return.
 *
 * @arg item - The SPItem to convert to a bitmap
 * @arg tr - Item transformation to apply
 * @arg resolution - The DPI resolution to use.
 * @arg antialias - Optionally turn off antialiasing.
 */
void DrawContext::paint_item_to_raster(SPItem const *item, Geom::Affine const &tr, double resolution, bool antialias)
{
    auto doc = item->document;

    std::vector<SPItem const *> items = {item};
    auto const bbox = item->visualBounds(item->i2doc_affine(), true, false, true);
    auto const gbox = item->visualBounds(Geom::identity(), true, false, true);
    auto aa = antialias ? Antialiasing::Good : Antialiasing::None;

    auto pb = std::unique_ptr<Pixbuf>{sp_generate_internal_bitmap(doc, *bbox, resolution, items, false, nullptr, 1, aa)};
    if (!pb || pb->height() <= 0) {
        return;
    }

    auto surface = pb->getSurfaceRaw();

    cairo_surface_flush(surface);
    // cairo_surface_write_to_png(surface, "/tmp/out.png");
    auto data = cairo_image_surface_get_data(surface);
    auto width = cairo_image_surface_get_width(surface);
    auto height = cairo_image_surface_get_height(surface);
    auto stride = cairo_image_surface_get_stride(surface);

    auto builder = capypdf::RasterImageBuilder();
    builder.set_size(width, pb->height());
    builder.set_colorspace(CAPY_IMAGE_CS_RGB);
    builder.set_pixel_depth(8);
    builder.set_alpha_depth(8);

    std::vector<char> pixels;
    std::vector<char> alpha;
    pixels.reserve(width * height * 3);
    alpha.reserve(width * height * 1);

    // Split the color and alpha from each other
    for (int y = 0; y < height; y++) {
        int p = y * stride;
        for (int x = 0; x < width; x++) {
            // Cairo surfaces are alpha pre-multiplied, PDF is not.
            pixels.push_back(unpremul_alpha(data[p + 2], data[p + 3]));
            pixels.push_back(unpremul_alpha(data[p + 1], data[p + 3]));
            pixels.push_back(unpremul_alpha(data[p],     data[p + 3]));
            alpha.push_back(data[p + 3]);
            p += 4;
        }
    }

    builder.set_pixel_data(pixels.data(), pixels.size());
    builder.set_alpha_data(alpha.data(), alpha.size());

    auto image = builder.build();

    auto boxtr = Geom::Affine(gbox->width(), 0, 0, -gbox->height(), gbox->left(), gbox->bottom());
    auto props = capypdf::ImagePdfProperties();

    auto image_id = _doc.generator().add_image(image, props);
    _ctx.cmd_q();
    transform(boxtr * tr);
    _ctx.cmd_Do(image_id);
    _ctx.cmd_Q();
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
