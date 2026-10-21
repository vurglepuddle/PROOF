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

#include "build-page.h"

#include "object/sp-page.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

/**
 * Get the transformation for the given page.
 * 96 to 72 dpi plus flip y axis (for PDF) plus this page's translation in the SVG document.
 */
Geom::Affine PageContext::page_transform(SPPage const *page)
{
    // The position of the page in the svg document
    auto media_box = page->getDocumentBleed();
    return Geom::Translate(-media_box.left(), -media_box.top())
           // Flip the Y-Axis because PDF is bottom-left
           * Geom::Affine(1.0, 0.0, 0.0, -1.0, 0.0, media_box.height())
           // Resize from SVG's 96dpi to PDF's 72dpi
           * px2pt;
}

PageContext::PageContext(Document &doc, SPPage const *page)
    : DrawContext(doc, doc.generator().new_page_context(), false)
    , page_tr(page_transform(page))
{
    set_pagebox(CAPY_BOX_MEDIA, page->getDocumentBleed());
}

void PageContext::set_pagebox(CapyPDF_Page_Box type, Geom::Rect const &size)
{
    // Page boxes are not affected by the cm transformations so must be transformed first
    auto box = size * page_tr;

    if (type == CAPY_BOX_MEDIA && !Geom::are_near(box.corner(0), {0.0, 0.0})) {
        // The specification technically allows non-zero media boxes, but lots of PDF
        // readers get very grumpy if you do this. Including Inkscape's own importer.
        std::cerr << "The media box must start at 0,0, found " << box.left() << ", " << box.top() << "\n";
    }

    page_props.set_pagebox(type, box.left(), box.top(), box.right(), box.bottom());
};

/**
 * PROOF: the colour space the page's transparency is composited in: the page group's /CS,
 * which groups without one of their own inherit. Without it the reader chooses, most often
 * RGB, and opacity and blend modes over CMYK inks come out differently from Illustrator, whose
 * pages of a CMYK document name DeviceCMYK (not isolated, not knockout).
 */
void PageContext::set_blending_space(CapyPDF_Device_Colorspace space)
{
    capypdf::TransparencyGroupProperties group;
    group.set_CS(space);
    group.set_I(false);
    group.set_K(false);
    page_props.set_transparency_group_properties(group);
}

/**
 * Paint the entire canvas as a transparency group.
 */
void PageContext::paint_drawing(CapyPDF_TransparencyGroupId drawing_id, Geom::Affine const &affine)
{
    paint_group(drawing_id, nullptr, affine * page_tr);
}

/**
 * Finalise the page and add any page properties.
 */
void PageContext::finalize()
{
    _ctx.set_custom_page_properties(page_props);
}

/**
 * Add any saved anchors (currently cached in the Document) to this page.
 *
 * @arg page - Limit the anchors to the bounds of just this page.
 */
void PageContext::add_anchors_for_page(SPPage const *page)
{
    for (CapyPDF_AnnotationId aid : _doc.get_anchors_for_page(page)) {
        _ctx.annotate(aid);
    }
}

/**
 * Add the created annotation to the page.
 */
void PageContext::annotate(CapyPDF_AnnotationId aid)
{
    _ctx.annotate(aid);
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
