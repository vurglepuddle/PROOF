// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Build PDF drawing elements
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com?
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef EXTENSION_INTERNAL_PDFOUTPUT_BUILD_DRAWING_H
#define EXTENSION_INTERNAL_PDFOUTPUT_BUILD_DRAWING_H

#include "build-document.h"

class SPGroup;
class SPImage;
class SPShape;
class SPText;
class SPUse;

namespace Inkscape::Text {
class Layout;
}

namespace Inkscape::Extension::Internal::PdfBuilder {

class DrawContext
{
public:
    DrawContext(Document &doc, capypdf::DrawContext ctx, bool soft_mask)
        : _ctx{std::move(ctx)}
        , _doc{doc}
        , _soft_mask{soft_mask}
    {}
    ~DrawContext() = default;

    void paint_item(SPItem const *item, Geom::Affine const &tr = Geom::identity(),
                    SPStyle const *context_style = nullptr);
    void paint_item_group(SPGroup const *group, SPStyle const *context_style);
    void paint_item_clone(SPUse const *use, SPStyle const *context_style);

    void paint_group(CapyPDF_TransparencyGroupId child_id, SPStyle const *style = nullptr,
                     Geom::Affine const &tr = Geom::identity(),
                     std::optional<CapyPDF_TransparencyGroupId> soft_mask = {});
    void paint_shape(SPShape const *shape, SPStyle const *context_style);
    void paint_raster(SPImage const *image);
    void paint_item_to_raster(SPItem const *item, Geom::Affine const &tr, double resolution, bool antialias);
    void paint_text_layout(Text::Layout const &layout, SPStyle const *context_style);
    void clip_text_layout(Text::Layout const &layout);

    void start_ocg(CapyPDF_OptionalContentGroupId ocgid);
    void end_ocg();

    bool set_shape(SPShape const *shape);
    bool set_shape_pathvector(Geom::PathVector const &pathv);
    void set_clip_path(std::optional<Geom::PathVector> clip, SPStyle *style = nullptr);
    void set_clip_rectangle(Geom::OptRect const &rect);

    void set_paint_style(StyleMap const &map, SPStyle const *style, SPStyle const *context_style,
                         Geom::OptRect const &bbox = {});

    Document &get_document() { return _doc; }
    std::optional<double> get_softmask(double opacity) const;

protected:
    friend class Document;

    void set_matrix(Geom::Affine const &affine);
    void transform(Geom::Affine const &affine);
    bool should_rasterize(SPObject const &object, bool recursive = false);

    capypdf::DrawContext _ctx;
    Document &_doc;

private:
    void set_shape_path(Geom::Path const &path);
    bool set_shape_rectangle(Geom::Rect const &rect);

    bool _soft_mask = false;
};

bool is_isolated(SPItem const *item);

class GroupContext : public DrawContext
{
public:
    /// @param isolate Whether what is in the group blends only inside it (see is_isolated()).
    GroupContext(Document &doc, Geom::OptRect const &clip, bool soft_mask = false, bool isolate = true);

private:
};

class ItemContext : public GroupContext
{
public:
    ItemContext(Document &doc, SPItem const *item);

    bool is_valid() const;
    ItemCacheKey cache_key() const;
    void paint();

private:
    SPItem const *_item;
};

} // namespace Inkscape::Extension::Internal::PdfBuilder

#endif /* !EXTENSION_INTERNAL_PDF_CAPYPDF_H */

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
