// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Output an SVG to a PDF using capypdf
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef EXTENSION_INTERNAL_PDFOUTPUT_BUILDER_H
#define EXTENSION_INTERNAL_PDFOUTPUT_BUILDER_H

#include <2geom/2geom.h>
#include <memory>
#include <optional>

#include "attributes.h"
#include "capypdf.hpp"
#include "remember-styles.h"
#include "style-enums.h"

class FontInstance;
class SPIPaint;
class SPAnchor;
class SPItem;
class SPGradientVector;
class SPLinearGradient;
class SPMeshGradient;
class SPRadialGradient;
class SPMask;
class SPPattern;
class SPPaintServer;
class SPStyle;

namespace Inkscape {
class URI;
namespace Colors {
class Color;
namespace Space {
class AnySpace;
class CMS;
} // namespace Space
} // namespace Colors
} // namespace Inkscape

inline auto const px2pt = Geom::Scale{72.0 / 96.0};

namespace Inkscape::Extension::Internal::PdfBuilder {

class DrawContext;
class PageContext;
class GroupContext;
class ShapeContext;
class ItemContext;

// ItemCacheKey{doc_id, item_id, context_fill, context_stroke}
typedef std::tuple<std::string, std::string, std::string, std::string> ItemCacheKey;

enum PaintLayer : std::uint_least8_t
{
    PAINT_FILLSTROKE,
    PAINT_FILL,
    PAINT_STROKE,
    PAINT_MARKERS
};

CapyPDF_Blend_Mode get_blendmode(SPBlendMode mode);
CapyPDF_Line_Cap get_linecap(SPStrokeCapType mode);
CapyPDF_Line_Join get_linejoin(SPStrokeJoinType mode);

std::string get_id(SPObject const *obj);
std::string get_document_id(SPDocument const *doc);
bool style_has_gradient_transparency(SPStyle const *style);
bool style_needs_group(SPStyle const *style);
bool gradient_has_transparency(SPPaintServer const *paint);
std::string paint_to_cache_key(SPIPaint const &paint, std::optional<double> opacity);
void get_context_use_recursive(SPItem const *item, bool &fill, bool &stroke);
std::vector<PaintLayer> get_paint_layers(SPStyle const *style, SPStyle const *context_style);

class Document
{
public:
    Document(char const *filename, capypdf::DocumentProperties &opt)
        : _gen(filename, opt)
        , _paint_memory({
              SPAttr::FILL,
              SPAttr::FILL_OPACITY,
              SPAttr::STROKE,
              SPAttr::STROKE_OPACITY,
              SPAttr::STROKE_WIDTH,
              SPAttr::STROKE_LINECAP,
              SPAttr::STROKE_LINEJOIN,
              SPAttr::STROKE_MITERLIMIT,
              SPAttr::STROKE_DASHARRAY,
              SPAttr::STROKE_DASHOFFSET,
          })
    {}

    void add_page(PageContext &page);
    void set_label(uint32_t page, std::string const &label);
    void write() { _gen.write(); }

    void set_filter_resolution(unsigned res = 0) { _filter_resolution = res; }
    unsigned get_filter_resolution() const { return _filter_resolution; }

    void set_text_enabled(bool enabled) { _text_enabled = enabled; }
    bool get_text_enabled() const { return _text_enabled; }

    std::optional<CapyPDF_TransparencyGroupId> add_group(ItemContext &context);
    std::optional<CapyPDF_TransparencyGroupId>
    item_to_transparency_group(SPItem const *item, SPStyle const *context_style = nullptr, bool is_soft_mask = false);
    std::optional<CapyPDF_TransparencyGroupId> mask_to_transparency_group(SPMask const *mask,
                                                                          Geom::Affine const &transform);
    std::optional<CapyPDF_TransparencyGroupId> style_to_transparency_mask(SPStyle const *style,
                                                                          SPStyle const *context_style);

    std::optional<CapyPDF_GraphicsStateId> get_group_graphics_state(SPStyle const *style,
                                                                    std::optional<CapyPDF_TransparencyGroupId> sm);
    std::optional<CapyPDF_GraphicsStateId> get_shape_graphics_state(SPStyle const *style);
    std::optional<std::pair<std::string, CapyPDF_FontId>> get_font(std::shared_ptr<FontInstance> font, SPIFontVariationSettings &var);

    std::optional<capypdf::Color> get_paint(SPIPaint const &paint, SPStyle const *context_style,
                                            std::optional<double> opacity);
    capypdf::Color get_color(Colors::Color const &color, std::optional<double> opacity);
    capypdf::Color get_shading_color(Colors::Color const &color, std::optional<double> opacity);

    std::optional<CapyPDF_PatternId> get_pattern(SPPaintServer const *paint, std::optional<double> opacity);

    std::optional<CapyPDF_ImageId> get_image(std::string const &filename, capypdf::ImagePdfProperties &props);
private:
    std::optional<CapyPDF_PatternId> get_linear_pattern(SPLinearGradient const *linear, std::optional<double> opacity);
    std::optional<CapyPDF_PatternId> get_radial_pattern(SPRadialGradient const *radial, std::optional<double> opacity);
    std::optional<CapyPDF_PatternId> get_mesh_pattern(SPMeshGradient const *mesh, std::optional<double> opacity);
    std::optional<CapyPDF_PatternId> get_tiling_pattern(SPPattern const *pat, Geom::Affine const &transform);
    std::optional<CapyPDF_FunctionId> get_gradient_function(SPGradientVector const &vector,
                                                            std::optional<double> opacity,
                                                            CapyPDF_Device_Colorspace *space);
    std::optional<CapyPDF_FunctionId> get_repeat_function(CapyPDF_FunctionId gradient, bool reflected, int from,
                                                          int to);

public:
    CapyPDF_Device_Colorspace get_default_colorspace() const;
    CapyPDF_Device_Colorspace get_colorspace(std::shared_ptr<Colors::Space::AnySpace> const &space) const;
    std::optional<CapyPDF_IccColorSpaceId> get_icc_profile(std::shared_ptr<Colors::Space::CMS> const &profile);

protected:
    friend class DrawContext;
    friend class PageContext;
    friend class GroupContext;
    friend class PatternContext;
    friend class TextContext;

    capypdf::Generator &generator() { return _gen; }

    // Used by set_paint_style processes and should include all the SPAttrs used there.
    StyleMemory &paint_memory() { return _paint_memory; }

    std::vector<CapyPDF_AnnotationId> get_anchors_for_page(SPPage const *page);

private:
    capypdf::Generator _gen;

    StyleMemory _paint_memory;

    std::map<std::string, CapyPDF_IccColorSpaceId> _icc_cache;
    std::map<ItemCacheKey, CapyPDF_TransparencyGroupId> _item_cache;
    std::map<std::string, CapyPDF_TransparencyGroupId> _mask_cache;
    std::map<std::string, CapyPDF_PatternId> _pattern_cache;
    std::map<std::string, CapyPDF_FontId> _font_cache;
    std::map<std::string, CapyPDF_ImageId> _raster_cache;

    // Anchors are post-processed into pages
    std::set<SPAnchor const *> _anchors;

    unsigned _filter_resolution = 0; // Do not rasterise
    bool _text_enabled = true;
};

} // namespace Inkscape::Extension::Internal::PdfBuilder

#endif /* !EXTENSION_INTERNAL_PDFOUTPUT_BUILDER_H */

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
