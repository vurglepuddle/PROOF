// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Build PDF pages for output.
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef EXTENSION_INTERNAL_PDFOUTPUT_BUILD_PAGE_H
#define EXTENSION_INTERNAL_PDFOUTPUT_BUILD_PAGE_H

#include "build-drawing.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

class PageContext : public DrawContext
{
public:
    static Geom::Affine page_transform(SPPage const *page);

    PageContext(Document &doc, SPPage const *page);

    void set_pagebox(CapyPDF_Page_Box type, Geom::Rect const &size);
    void set_blending_space(CapyPDF_Device_Colorspace space);
    void paint_drawing(CapyPDF_TransparencyGroupId drawing_id, Geom::Affine const &affine);

    void add_anchors_for_page(SPPage const *page);
    void annotate(CapyPDF_AnnotationId aid);

protected:
    friend class Document;

    void finalize();

private:
    Geom::Affine page_tr;
    capypdf::PageProperties page_props;
};

} // namespace Inkscape::Extension::Internal::PdfBuilder

#endif /* !EXTENSION_INTERNAL_PDFOUTPUT_BUILD_PAGE_H */

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
