// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_EXTENSION_INTERNAL_PDFINPUT_SVGBUILDER_H
#define SEEN_EXTENSION_INTERNAL_PDFINPUT_SVGBUILDER_H

/*
 * Authors:
 *   miklos erdelyi
 *
 * Copyright (C) 2007 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include "poppler-transition-api.h"

class SPDocument;
namespace Inkscape {
    namespace XML {
        struct Document;
        class Node;
    }
namespace Colors {
enum class RenderingIntent;
namespace CMS {
class Profile;
}
namespace Space {
class AnySpace;
}
}
}

#define Operator Operator_Gfx
#include <gdkmm/enums.h>
#include <Gfx.h>
#undef Operator
#undef DOUBLE_CLICK

#include <2geom/affine.h>
#include <2geom/point.h>
#include <cairo-ft.h>
#include <glibmm/ustring.h>
#include <lcms2.h>

#include <CharTypes.h>
#include "enums.h"
#include "poppler-utils.h"
class Function;
class GfxState;
struct GfxColor;
class GfxColorSpace;
class GfxSeparationColorSpace;
enum GfxClipType;
struct GfxRGB;
class GfxPath;
class GfxPattern;
class GfxTilingPattern;
class GfxShading;
class GfxFont;
class GfxImageColorMap;
class Stream;
class XRef;

class CairoFont;
class SPCSSAttr;
class ClipHistoryEntry;

#include <glib.h>
#include <map>
#include <memory>
#include <vector>

namespace Inkscape {
namespace Extension {
namespace Internal {

/**
 * Holds information about glyphs added by PdfParser which haven't been added
 * to the document yet.
 * TODO Turn this into a class and store the actual GfxState.
 */
struct SvgGlyph {
    Geom::Point position;      // Absolute glyph coords
    Geom::Point text_position; // Absolute glyph coords in text space
    Geom::Point origin;        // Origin of glyph (used for fixing vertical text position)
    Geom::Point delta;         // X, Y displacement values (distance between glyph origins)
    Geom::Point advance;       // X, Y advance values
    double rise;               // Text rise parameter (Ts operator)
    double char_space;         // Character spacing   (Tc operator)
    double word_space;         // Word spacing        (Tw operator)
    double horiz_scaling;      // Horizontal scaling  (Tz operator)
    Glib::ustring code;        // UTF-8 coded character
    bool is_space;

    bool style_changed;        // Set to true if style has to be reset
    GfxState *state;           // A promise of the future text style
    double text_size;          // Text size

    std::string font_specification;        // Current font specification
    SPCSSAttr *css_font;                   // The font style as a css style
    unsigned int cairo_index;              // The index into the selected cairo font
    std::shared_ptr<CairoFont> cairo_font; // A pointer to the selected cairo font
};

/**
 * Builds the inner SVG representation using libpoppler from the calls of PdfParser.
 */
class SvgBuilder {
public:
    SvgBuilder(SPDocument *document, gchar *docname, XRef *xref);
    SvgBuilder(SvgBuilder *parent, Inkscape::XML::Node *root);
    virtual ~SvgBuilder();

    // Property setting
    void setDocumentSize(double width, double height);  // Document size in px
    void setMargins(const Geom::Rect &page, const Geom::Rect &margins, const Geom::Rect &bleed);
    void cropPage(const Geom::Rect &bbox);
    void setMetadata(char const * name, const std::string &content);
    void setAsLayer(const char *layer_name = nullptr, bool visible = true);
    void setGroupOpacity(double opacity);
    void pushPage(const std::string &label, GfxState *state);
    void setPageMode(bool as_pages) { _as_pages = as_pages; }

    // Path adding
    bool shouldMergePath(bool is_fill, const std::string &path);
    bool mergePath(GfxState *state, bool is_fill, const std::string &path, bool even_odd = false);
    void addPath(GfxState *state, bool fill, bool stroke, bool even_odd=false);
    void addShadedFill(GfxState *state, GfxShading *shading, const Geom::Affine shading_tr);

    // Image handling
    void addImage(GfxState *state, Stream *str, int width, int height,
                  GfxImageColorMap *color_map, bool interpolate, int *mask_colors);
    void addImageMask(GfxState *state, Stream *str, int width, int height,
                      bool invert, bool interpolate);
    void addMaskedImage(GfxState *state, Stream *str, int width, int height,
                        GfxImageColorMap *color_map, bool interpolate,
                        Stream *mask_str, int mask_width, int mask_height,
                        bool invert_mask, bool mask_interpolate);
    void addSoftMaskedImage(GfxState *state, Stream *str, int width, int height,
                            GfxImageColorMap *color_map, bool interpolate,
                            Stream *mask_str, int mask_width, int mask_height,
                            GfxImageColorMap *mask_color_map, bool mask_interpolate);
    void applyOptionalMask(Inkscape::XML::Node *mask, Inkscape::XML::Node *target);

    // Groups, Transparency group and soft mask handling
    void startGroup(GfxState *state, double *bbox, GfxColorSpace *blending_color_space, bool isolated, bool knockout,
                   bool for_softmask);
    void finishGroup(GfxState *state, bool for_softmask);
    void popGroup(GfxState *state);
    void pruneGroups();
    void setGroupBy(const std::string &group_by);
    GroupBy getGroupBy() { return _group_by; }

    // Text handling
    void beginString(GfxState *state, int len);
    void endString(GfxState *state);
    void addChar(GfxState *state, double x, double y,
                 double dx, double dy,
                 double ax, double ay,
                 double originX, double originY,
                 CharCode code, int nBytes, Unicode const *u, int uLen);
    void beginTextObject(GfxState *state);
    void endTextObject(GfxState *state);

    bool isPatternTypeSupported(GfxPattern *pattern);
    void setFontStrategies(FontStrategies fs) { _font_strategies = fs; }
    static FontStrategies autoFontStrategies(FontStrategy s, FontList fonts);

    // State manipulation
    void saveState(GfxState *state);
    void restoreState(GfxState *state);
    void updateStyle(GfxState *state);
    void updateFont(GfxState *state, std::shared_ptr<CairoFont> cairo_font, bool flip);
    void updateTextPosition(double tx, double ty);
    void updateTextShift(GfxState *state, double shift);
    void updateTextMatrix(GfxState *state, bool flip);
    void beforeStateChange(GfxState *old_state);

    // Clipping
    void setClip(GfxState *state, GfxClipType clip, bool is_bbox = false);

    // Layers i.e Optional Groups
    void addOptionalGroup(const std::string &oc, const std::string &label, bool visible = true);
    void beginXObjectLayer(const std::string &label);
    void beginMarkedContent(const char *name = nullptr, const char *group = nullptr);
    void endMarkedContent();

    void addColorProfile(unsigned char *profBuf, int length);

    void setEmbedImages(bool embed_images) { _embed_images = embed_images; }
    void setConvertColors(bool convert_colors) { _convert_colors = convert_colors; }
private:
    void _init();

    // Pattern creation
    gchar *_createPattern(GfxPattern *pattern, GfxState *state, bool is_stroke=false);
    gchar *_createGradient(GfxState *state, GfxShading *shading, const Geom::Affine pat_matrix);
    void _addStopToGradient(Inkscape::XML::Node *gradient, double offset, GfxColor &color, GfxColorSpace *space,
                            Colors::RenderingIntent intent, double opacity);
    bool _addGradientStops(Inkscape::XML::Node *gradient, GfxState *state, GfxShading *shading,
                           _POPPLER_CONST Function *func);
    gchar *_createTilingPattern(GfxTilingPattern *tiling_pattern, GfxState *state,
                                bool is_stroke=false);
    // Image/mask creation
    Inkscape::XML::Node *_createImage(Stream *str, int width, int height,
                                      GfxImageColorMap *color_map, bool interpolate,
                                      int *mask_colors, bool alpha_only=false,
                                      bool invert_alpha=false);
    Inkscape::XML::Node *_createMask(double width, double height);
    Inkscape::XML::Node *_createClip(const std::string &d, bool even_odd);

    // Style setting
    SPCSSAttr *_setStyle(GfxState *state, bool fill, bool stroke, bool even_odd=false);
    void _setStrokeStyle(SPCSSAttr *css, GfxState *state);
    void _setFillStyle(SPCSSAttr *css, GfxState *state, bool even_odd);
    void _setTextStyle(Inkscape::XML::Node *node, GfxState *state, SPCSSAttr *font_style, Geom::Affine text_affine);
    void _setBlendMode(Inkscape::XML::Node *node, GfxState *state);
    void _setTransform(Inkscape::XML::Node *node, GfxState *state, Geom::Affine extra = Geom::identity());

    // Write buffered text into doc
    Inkscape::XML::Node* _flushTextText(GfxState *state, double text_scale, const Geom::Affine& text_transform);
    Inkscape::XML::Node* _flushTextPath(GfxState *state, double text_scale, const Geom::Affine& text_transform);
    void _flushText(GfxState *state);
    std::string _aria_label;
    bool _aria_space = false;

    // Handling of node stack
    Inkscape::XML::Node *_pushGroup();
    Inkscape::XML::Node *_popGroup();
    Inkscape::XML::Node *_pushContainer(const char *name);
    Inkscape::XML::Node *_pushContainer(Inkscape::XML::Node *node);
    Inkscape::XML::Node *_popContainer();
    std::vector<Inkscape::XML::Node *> _node_stack;
    std::vector<GfxState *> _mask_groups;
    int _clip_groups = 0;

    Inkscape::XML::Node *_getClip(const Inkscape::XML::Node *node);
    Geom::PathVector _checkClip(const Inkscape::XML::Node *node, const Geom::Affine &node_tr) const;
    Inkscape::XML::Node *_addToContainer(const char *name);
    Inkscape::XML::Node *_renderText(std::shared_ptr<CairoFont> cairo_font, double font_size,
                                     const Geom::Affine &transform,
                                     cairo_glyph_t *cairo_glyphs, unsigned int count);

    void _setClipPath(Inkscape::XML::Node *node);
    void _addToContainer(Inkscape::XML::Node *node, bool release = true);

    Inkscape::XML::Node *_getGradientNode(Inkscape::XML::Node *node, bool is_fill);
    static bool _attrEqual(Inkscape::XML::Node *a, Inkscape::XML::Node *b, char const *attr);

    // Colors
    // as_paint: the result is a fill/stroke value, so a spot ink may be returned as url(#swatch).
    // Otherwise (gradient stops, masks) a spot ink becomes its tinted alternate colour.
    std::string convertGfxColor(const GfxColor &color, GfxColorSpace *space, Colors::RenderingIntent intent,
                                bool as_paint = false);
    std::string convertGfxColor(const GfxColor *color, GfxColorSpace *space, Colors::RenderingIntent intent,
                                bool as_paint = false);
    std::string _convertSeparation(const GfxColor &color, GfxSeparationColorSpace *space, bool as_paint);
    std::string _getColorSpace(cmsHPROFILE hp, Colors::RenderingIntent intent);
    std::shared_ptr<Colors::Space::AnySpace> _getColorSpace(std::shared_ptr<Colors::CMS::Profile> const &profile, Colors::RenderingIntent intent);

    // The calculated font style, if not set, the text must be rendered with cairo instead.
    FontStrategies _font_strategies;
    double _css_font_size = 1.0;
    SPCSSAttr *_css_font;
    std::string _font_specification;
    double _text_size;
    Geom::Affine _text_matrix;
    Geom::Point _text_position;
    std::vector<SvgGlyph> _glyphs;   // Added characters

    bool _embed_images = true;
    bool _convert_colors = true;

    // The font when drawing the text into vector glyphs instead of text elements.
    std::shared_ptr<CairoFont> _cairo_font;

    bool _in_text_object;   // Whether we are inside a text object
    bool _invalidated_style;
    bool _invalidated_strategy = false;
    bool _for_softmask = false;

    bool _is_top_level;  // Whether this SvgBuilder is the top-level one
    SPDocument *_doc;
    gchar *_docname;    // Basename of the URI from which this document is created
    XRef *_xref;    // Cross-reference table from the PDF doc we're converting from
    Inkscape::XML::Document *_xml_doc;
    Inkscape::XML::Node *_root;  // Root node from the point of view of this SvgBuilder
    Inkscape::XML::Node *_container; // Current container (group/pattern/mask)
    double _width;       // Document size in px
    double _height;       // Document size in px

    bool _as_pages = true; // If set to false, page objects are not created
    
    // Group handling to allow OCGs as top-level layer, with transparency
    GroupBy _group_by = GroupBy::BY_XOBJECT;
    double _group_alpha = 1.0;
    std::vector<Inkscape::XML::Node *> _alpha_objs;

    Inkscape::XML::Node *_page = nullptr; // XML Page definition
    int _page_num = 0; // Are we on a page
    double _page_left = 0 ; // Move to the left for more pages
    double _page_top = 0 ; // Move to the top (maybe)
    bool _page_offset = false;
    Geom::Affine _page_affine = Geom::identity();

    std::map<std::string, std::pair<std::string, bool>> _ocgs;

    std::shared_ptr<Colors::CMS::Profile> _icc_profile;

    ClipHistoryEntry *_clip_history; // clip path stack
    Inkscape::XML::Node *_clip_text = nullptr;
    Inkscape::XML::Node *_clip_text_group = nullptr;
    
    // Keep track of the previously created clip path
    Inkscape::XML::Node *_prev_clip = nullptr;
};


} // namespace Internal
} // namespace Extension
} // namespace Inkscape

#endif // SEEN_EXTENSION_INTERNAL_PDFINPUT_SVGBUILDER_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
