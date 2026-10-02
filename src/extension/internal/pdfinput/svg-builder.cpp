// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Native PDF import using libpoppler.
 *
 * Authors:
 *   miklos erdelyi
 *   Jon A. Cruz <jon@joncruz.org>
 *   Tavmjong Bah
 *
 * Copyright (C) 2007 Authors
 *               2024 Tavmjong Bah
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 */

#include "svg-builder.h"

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include <Function.h>
#include <GfxFont.h>
#include <GfxState.h>
#include <Page.h>
#include <Stream.h>
#include <goo/gmem.h>

#ifdef _WIN32
#undef near
#undef DOUBLE_CLICK
#endif

#include "document.h"
#include "pdf-parser.h"
#include "pdf-utils.h"
#include <png.h>
#include "poppler-cairo-font-engine.h"
#include "rdf.h"

#include "colors/cms/profile.h"
#include "colors/document-cms.h"
#include "colors/manager.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/lab.h"
#include "spot-ink.h"
#include "display/cairo-utils.h"
#include "display/nr-filter-utils.h"
#include "object/color-profile.h"
#include "object/sp-defs.h"
#include "object/sp-namedview.h"
#include "object/sp-text.h"
#include "svg/css-ostringstream.h"
#include "svg/path-string.h"
#include "svg/svg.h"
#include "util/units.h"
#include "util/uri.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/repr.h"
#include "xml/sp-css-attr.h"
#include "helper/geom.h"
#include "util-string/string-convert.h"

namespace Inkscape {
namespace Extension {
namespace Internal {

//#define IFTRACE(_code)  _code
#define IFTRACE(_code)

#define TRACE(_args) IFTRACE(g_print _args)


static Colors::RenderingIntent _getIntent(GfxState *state)
{
    if (auto c = state->getRenderingIntent()) {
        auto intent = std::string(c);
        if (intent == "AbsoluteColorimetric") {
            return Colors::RenderingIntent::ABSOLUTE_COLORIMETRIC;
        } else if(intent == "RelativeColorimetric") {
            return Colors::RenderingIntent::RELATIVE_COLORIMETRIC;
        } else if(intent == "Saturation") {
            return Colors::RenderingIntent::SATURATION;
        } else if(intent == "Perceptual") {
            return Colors::RenderingIntent::PERCEPTUAL;
        }
    }
    return Colors::RenderingIntent::RELATIVE_COLORIMETRIC;
}

/**
 * \class SvgBuilder
 *
 */

SvgBuilder::SvgBuilder(SPDocument *document, gchar *docname, XRef *xref)
{
    _is_top_level = true;
    _doc = document;
    _docname = docname;
    _xref = xref;
    _xml_doc = _doc->getReprDoc();
    _container = _root = _doc->getReprRoot();
    _init();
}

SvgBuilder::SvgBuilder(SvgBuilder *parent, Inkscape::XML::Node *root) {
    _is_top_level = false;
    _doc = parent->_doc;
    _docname = parent->_docname;
    _xref = parent->_xref;
    _xml_doc = parent->_xml_doc;
    _container = this->_root = root;
    _init();
}

SvgBuilder::~SvgBuilder()
{
    if (_clip_history) {
        delete _clip_history;
        _clip_history = nullptr;
    }
}

void SvgBuilder::_init() {
    _clip_history = new ClipHistoryEntry();
    _css_font = nullptr;
    _in_text_object = false;
    _invalidated_style = true;
    _width = 0;
    _height = 0;

    _node_stack.push_back(_container);
}

/**
 * We're creating a multi-page document, push page number.
 */
void SvgBuilder::pushPage(const std::string &label, GfxState *state)
{
    // Move page over by the last page width
    if (_page_offset && this->_width) {
        int gap = 20;
        _page_left += this->_width + gap;
        // TODO: A more interesting page layout could be implemented here.
    }
    _page_num += 1;
    _page_offset = true;

    if (_page) {
        Inkscape::GC::release(_page);
    }

    if (_as_pages) {
        _page = _xml_doc->createElement("svg:view");

        if (!label.empty()) {
            _page->setAttribute("inkscape:label", validateString(label));
        }
        _doc->getDefs()->getRepr()->appendChild(_page);
    }

    // Page translation is somehow lost in the way we're using poppler and the state management
    // Applying the state directly doesn't work as many of the flips/rotates are baked in already.
    // The translation alone must be added back to the page position so items end up in the
    // right places. If a better method is found, please replace this code.
    auto st = stateToAffine(state);
    auto tr = st.translation();
    if (st[0] < 0 || st[2] < 0) { // Flip or rotate in X
        tr[Geom::X] = -tr[Geom::X] + state->getPageWidth();
    }
    if (st[1] < 0 || st[3] < 0) { // Flip or rotate in Y
        tr[Geom::Y] = -tr[Geom::Y] + state->getPageHeight();
    }
    // Note: This translation is very rare in pdf files, most of the time their initial state doesn't contain
    // any real translations, just a flip and the because of our GfxState constructor, the pt/px scale.
    // Please use an example pdf which produces a non-zero translation in order to change this code!
    _page_affine = Geom::Translate(tr).inverse() * Geom::Translate(_page_left, _page_top);

    // No OptionalContentGroups means no layers, so make a default layer for this page.
    if (_ocgs.empty()) {
        // Reset to root
        while (_container != _root) {
            _popGroup();
        }
        _pushGroup();
        setAsLayer(label.c_str(), true);
    }
}

void SvgBuilder::setDocumentSize(double width, double height) {
    this->_width = width;
    this->_height = height;

    // Build the document size to include all page widths together.
    if (!_as_pages) {
        width += _page_left;
    }
    if (_page_num < 2 || !_as_pages) {
        _root->setAttributeSvgDouble("width", width);
        _root->setAttributeSvgDouble("height", height);
    }
    if (_page) {
        auto rect = Geom::Rect::from_xywh(_page_left, _page_top, _width, _height);
        _page->setAttributeRect("viewBox", rect);
    }
}

/**
 * Crop to this bounding box, do this before setMargins() but after setDocumentSize
 */
void SvgBuilder::cropPage(const Geom::Rect &bbox)
{   
    if (_container == _root) {
        // We're not going to crop when there's PDF Layers
        return;
    }
    // Wrap in a Path object for convenience
    auto box = Geom::Path(bbox * _page_affine);

    // add to the clip history
    _clip_history->setClip(Geom::PathVector(box), FillRule::fill_nonZero);
    auto clip_path = _createClip(sp_svg_write_path(box), false);
    gchar *urltext = g_strdup_printf("url(#%s)", clip_path->attribute("id"));
    _container->setAttribute("clip-path", urltext);
    g_free(urltext);
}

/**
 * Calculate the page margin size based on the pdf settings.
 */
void SvgBuilder::setMargins(const Geom::Rect &page, const Geom::Rect &margins, const Geom::Rect &bleed)
{
    if (page.width() != _width || page.height() != _height) {
        // We need to re-set the page size and change the page_affine.
        _page_affine *= Geom::Translate(-page.left(), -page.top());
        setDocumentSize(page.width(), page.height());
    }
    if (_as_pages && page != margins) {
        if (!_page) {
            g_warning("Can not store PDF margins in bare document.");
            return;
        }
        // Calculate the margins from the pdf art box.
        Inkscape::CSSOStringStream val;
        val << margins.top() - page.top() << " "
            << page.right() - margins.right() << " "
            << page.bottom() - margins.bottom() << " "
            << margins.left() - page.left();
        _page->setAttribute("inkscape:margin", val.str());
    }
    if (_as_pages && page != bleed) {
        if (!_page) {
            g_warning("Can not store PDF bleed in bare document.");
            return;
        }
        Inkscape::CSSOStringStream val;
        val << page.top() - bleed.top() << " "
            << bleed.right() - page.right() << " "
            << bleed.bottom() - page.bottom() << " "
            << page.left() - bleed.left();
        _page->setAttribute("inkscape:bleed", val.str());
    }
}

void SvgBuilder::setMetadata(char const *name, const std::string &content)
{
    if (name && !content.empty()) {
        rdf_set_work_entity(_doc, rdf_find_entity(name), validateString(content).c_str());
    }
}

/**
 * \brief Sets groupmode of the current container to 'layer' and sets its label if given
 */
void SvgBuilder::setAsLayer(const char *layer_name, bool visible)
{
    _container->setAttribute("inkscape:groupmode", "layer");
    if (layer_name) {
        _container->setAttribute("inkscape:label", validateString(layer_name));
    }
    if (!visible) {
        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "display", "none");
        sp_repr_css_change(_container, css, "style");
    }
}

/**
 * \brief Sets the current container's opacity
 */
void SvgBuilder::setGroupOpacity(double opacity) {
    if (_group_by == GroupBy::BY_XOBJECT) {
        _container->setAttributeSvgDouble("opacity", CLAMP(opacity, 0.0, 1.0));
    } else {
        _group_alpha = CLAMP(opacity, 0.0, 1.0);
    }
}

void SvgBuilder::saveState(GfxState *state)
{
    _clip_history = _clip_history->save();
}

void SvgBuilder::restoreState(GfxState *state) {
    _clip_history = _clip_history->restore();

    if (!_mask_groups.empty()) {
        GfxState *mask_state = _mask_groups.back();
        if (state == mask_state) {
            popGroup(state);
            _mask_groups.pop_back();
        }
    }
    while (_clip_groups > 0) {
        if (_container != _root) {
            _clip_history = _clip_history->restore();
        }
        popGroup(nullptr);
        _clip_groups--;
    }
}

Inkscape::XML::Node *SvgBuilder::_pushContainer(const char *name)
{
    return _pushContainer(_xml_doc->createElement(name));
}

Inkscape::XML::Node *SvgBuilder::_pushContainer(Inkscape::XML::Node *node)
{
    _node_stack.push_back(node);
    _container = node;
    return node;
}

Inkscape::XML::Node *SvgBuilder::_popContainer()
{
    Inkscape::XML::Node *node = nullptr;
    if ( _node_stack.size() > 1 ) {
        node = _node_stack.back();
        _node_stack.pop_back();
        _container = _node_stack.back();    // Re-set container
    } else {
        TRACE(("_popContainer() called when stack is empty\n"));
        node = _root;
    }
    return node;
}

/**
 * Create an svg element and append it to the current container object.
 */
Inkscape::XML::Node *SvgBuilder::_addToContainer(const char *name)
{
    Inkscape::XML::Node *node = _xml_doc->createElement(name);
    _addToContainer(node);
    return node;
}

/**
 * Append the given xml element to the current container object, clipping and masking as needed.
 *
 * if release is true (default), the XML node will be GC released too.
 */
void SvgBuilder::_addToContainer(Inkscape::XML::Node *node, bool release)
{
    if (!node->parent()) {
        _container->appendChild(node);
    }
    if (release) {
        Inkscape::GC::release(node);
    }
    if (_group_alpha < 1) {
        _alpha_objs.push_back(node);
    }
}

void SvgBuilder::_setClipPath(Inkscape::XML::Node *node)
{
    if (_clip_history->hasClipPath() || _clip_text) {
        if (auto clip_path = _getClip(node)) {
            gchar *urltext = g_strdup_printf("url(#%s)", clip_path->attribute("id"));
            node->setAttribute("clip-path", urltext);
            g_free(urltext);
        }
    }
}

Inkscape::XML::Node *SvgBuilder::_pushGroup()
{
    Inkscape::XML::Node *saved_container = _container;
    Inkscape::XML::Node *node = _pushContainer("svg:g");
    saved_container->appendChild(node);
    Inkscape::GC::release(node);
    return _container;
}

Inkscape::XML::Node *SvgBuilder::_popGroup()
{
    if (_container != _root) { // Pop if the current container isn't root
        _popContainer();
    }
    return _container;
}

void SvgBuilder::setGroupBy(const std::string &group_by) {
    if (group_by == "by-xobject") {
        _group_by = GroupBy::BY_XOBJECT;
    } else if (group_by == "by-layer") {
        _group_by = GroupBy::BY_OCGS;
    } else {
        g_warning("Unknown group mode %s selected, falling back to XObjects\n", group_by.c_str());
        _group_by = GroupBy::BY_XOBJECT;
    }
}

// for poppler < 26.06.0
std::string SvgBuilder::convertGfxColor(const GfxColor *color, GfxColorSpace *space, Colors::RenderingIntent intent,
                                        bool as_paint) {
    if (!color) {
        return "";
    }
    return convertGfxColor(*color, space, intent, as_paint);
}

std::string SvgBuilder::convertGfxColor(const GfxColor &color, GfxColorSpace *space, Colors::RenderingIntent intent,
                                        bool as_paint)
{
    using Colors::Space::Type;
    auto icc_space = _icc_profile ? _icc_profile->getColorSpace() : cmsSigXYZData;
    auto &cm = Colors::Manager::get();

    // Each space can either be an icc profile applies to the whole PDF or
    // if there's no icc profile, we use the inkscape CSS color spaces.
    // Which might not always be correct for the PDFs color models.
    auto get_space = [this, intent, icc_space, &cm](int cmsSig, Type type) {
        return icc_space == cmsSig ? _getColorSpace(_icc_profile, intent) : cm.find(type);
    };

    if (!_convert_colors) {
        switch (space->getMode()) {
            case csDeviceGray:
            case csCalGray:
                GfxGray gray;
                space->_POPPLER_GET_GRAY(color, &gray);
                return Colors::Color(get_space(cmsSigGrayData, Type::Gray), {colToDbl(gray)}).toString();
            case csDeviceRGB:
            case csCalRGB:
                GfxRGB rgb;
                space->_POPPLER_GET_RGB(color, &rgb);
                return Colors::Color(get_space(cmsSigRgbData, Type::RGB), 
                    {colToDbl(rgb.r), colToDbl(rgb.g), colToDbl(rgb.b)}).toString();
            case csDeviceN:
                g_warning("DeviceN color unsupported, falling back to CMYK");
            case csDeviceCMYK:
                GfxCMYK cmyk;
                space->_POPPLER_GET_CMYK(color, &cmyk);
                return Colors::Color(get_space(cmsSigCmykData, Type::CMYK),
                    {colToDbl(cmyk.c), colToDbl(cmyk.m), colToDbl(cmyk.y), colToDbl(cmyk.k)}).toString();
            case csLab:
                g_warning("Lab color unsupported, falling back to sRGB");
                break;
            case csSeparation:
                // PROOF: keep the named ink instead of flattening it to sRGB.
                return _convertSeparation(color, static_cast<GfxSeparationColorSpace *>(space), as_paint);
            case csPattern:
                g_warning("Pattern color unsupported, falling back to sRGB");
                break;
            case csIndexed:
                g_warning("Indexed color unsupported, falling back to sRGB");
                break;
            case csICCBased:
#if POPPLER_CHECK_VERSION(0, 90, 0)
                if (auto gfx_space = dynamic_cast<GfxICCBasedColorSpace *>(space)) {
                    if (auto profile = Colors::CMS::Profile::create_from_copy(gfx_space->getProfile().get())) {
                        if (auto space = _getColorSpace(profile, intent)) {
                            // Then the rest of the components after sRGB backup (see above)
                            std::vector<double> comps;
                            for (int i = 0; i < gfx_space->getNComps(); ++i) {
                                comps.emplace_back(colToDbl((color).c[i]));
                            }
                            return Colors::Color(std::move(space), std::move(comps)).toString();
                        }
                    }
                }
#else
                g_warning("ICC profile ignored; libpoppler >= 0.90.0 required.");
#endif
                break;
        }
    }
    // sRGB is the default and poppler will generate one for us
    GfxRGB rgb;
    space->_POPPLER_GET_RGB(color, &rgb);
    return Colors::Color(cm.find(Colors::Space::Type::RGB),
         {colToDbl(rgb.r), colToDbl(rgb.g), colToDbl(rgb.b)}).toString();
}

/**
 * PROOF: convert a Separation (spot ink) colour.
 *
 * The ink keeps its name, its full-strength alternate in the space the PDF used
 * (Lab for Illustrator's Pantone inks, CMYK for most others) and its tint. Fill and
 * stroke values become a link to a spot swatch; other uses get the tinted alternate.
 * Unusual alternate spaces fall back to CMYK rather than sRGB.
 */
std::string SvgBuilder::_convertSeparation(const GfxColor &color, GfxSeparationColorSpace *space, bool as_paint)
{
    using Colors::Space::Type;
    if (space->isNonMarking()) {
        return as_paint ? "none" : "#ffffff"; // the /None ink paints nothing
    }

    // GfxColor is 16-bit fixed point: 30% arrives as 0.29998779. Keep four decimals.
    auto const tint = std::round(std::clamp(colToDbl(color.c[0]), 0.0, 1.0) * 1e4) / 1e4;
    auto *alt = space->getAlt();
    double full[gfxColorMaxComps] = {};
    double const one = 1.0;
    space->getFunc()->transform(&one, full);

    auto mode = alt->getMode();
#if POPPLER_CHECK_VERSION(0, 90, 0)
    // Illustrator wraps every alternate in an ICCBased space: Lab profiles for Pantone books,
    // the document RGB profile for inks defined in RGB (e.g. RAL libraries). The tint function
    // still yields the library's own values, so keep them in the profile's data space rather
    // than converting to CMYK. RGB values are kept as sRGB numbers.
    if (mode == csICCBased) {
        if (auto icc = dynamic_cast<GfxICCBasedColorSpace *>(alt); icc && icc->getProfile()) {
            switch (cmsGetColorSpace(icc->getProfile().get())) {
                case cmsSigLabData:
                    mode = csLab;
                    break;
                case cmsSigCmykData:
                    mode = csDeviceCMYK;
                    break;
                case cmsSigRgbData:
                    mode = csDeviceRGB;
                    break;
                case cmsSigGrayData:
                    mode = csDeviceGray;
                    break;
                default:
                    break;
            }
        }
    }
#endif

    std::optional<Colors::Color> alternate;
    switch (mode) {
        case csDeviceCMYK:
            alternate = Colors::Color(Type::CMYK, {full[0], full[1], full[2], full[3]});
            break;
        case csLab: {
            // PDF gives actual L*a*b* values; Inkscape stores L/100 and a, b scaled from [-128, 127].
            using Lab = Colors::Space::Lab;
            auto const span = Lab::MAX_SCALE - Lab::MIN_SCALE;
            alternate = Colors::Color(Type::LAB, {full[0] / Lab::LUMA_SCALE, (full[1] - Lab::MIN_SCALE) / span,
                                                  (full[2] - Lab::MIN_SCALE) / span});
            break;
        }
        case csDeviceRGB:
        case csCalRGB:
            alternate = Colors::Color(Type::RGB, {full[0], full[1], full[2]});
            break;
        case csDeviceGray:
        case csCalGray:
            alternate = Colors::Color(Type::Gray, {full[0]});
            break;
        default:
            break;
    }
    if (!alternate) {
        // CMYK floor for ICC-based and other alternates.
        GfxColor full_color;
        full_color.c[0] = dblToCol(1.0);
        GfxCMYK cmyk;
        space->_POPPLER_GET_CMYK(full_color, &cmyk);
        alternate = Colors::Color(Type::CMYK, {colToDbl(cmyk.c), colToDbl(cmyk.m), colToDbl(cmyk.y), colToDbl(cmyk.k)});
    }

    auto const name = space->getName() ? space->getName()->toStr() : std::string("Unnamed ink");
    SpotInk::Ink ink{name, *alternate, tint};
    if (!as_paint) {
        return SpotInk::display_color(ink.alternate, tint).toString();
    }
    return "url(#" + SpotInk::ensure(_xml_doc, _doc->getDefs()->getRepr(), _doc, ink) + ")";
}

static void svgSetTransform(Inkscape::XML::Node *node, Geom::Affine matrix) {
    if (node->attribute("clip-path")) {
        g_error("Adding transform AFTER clipping path.");
    }
    node->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(matrix));
}

/**
 * \brief Generates a SVG path string from poppler's data structure
 */
static gchar *svgInterpretPath(_POPPLER_CONST_83 GfxPath *path) {
    Inkscape::SVG::PathString pathString;
    for (int i = 0 ; i < path->getNumSubpaths() ; ++i ) {
        _POPPLER_CONST_83 GfxSubpath *subpath = path->getSubpath(i);
        if (subpath->getNumPoints() > 0) {
            pathString.moveTo(subpath->getX(0), subpath->getY(0));
            int j = 1;
            while (j < subpath->getNumPoints()) {
                if (subpath->getCurve(j)) {
                    pathString.curveTo(subpath->getX(j), subpath->getY(j),
                                       subpath->getX(j+1), subpath->getY(j+1),
                                       subpath->getX(j+2), subpath->getY(j+2));

                    j += 3;
                } else {
                    pathString.lineTo(subpath->getX(j), subpath->getY(j));
                    ++j;
                }
            }
            if (subpath->isClosed()) {
                pathString.closePath();
            }
        }
    }

    return g_strdup(pathString.c_str());
}

/**
 * \brief Sets stroke style from poppler's GfxState data structure
 * Uses the given SPCSSAttr for storing the style properties
 */
void SvgBuilder::_setStrokeStyle(SPCSSAttr *css, GfxState *state) {
    // Stroke color/pattern
    auto space = state->getStrokeColorSpace();
    if (space->getMode() == csPattern) {
        gchar *urltext = _createPattern(state->getStrokePattern(), state, true);
        sp_repr_css_set_property(css, "stroke", urltext);
        if (urltext) {
            g_free(urltext);
        }
    } else {
        sp_repr_css_set_property(css, "stroke", convertGfxColor(state->getStrokeColor(), space,
                                                                _getIntent(state), true).c_str());
    }

    // Opacity
    Inkscape::CSSOStringStream os_opacity;
    os_opacity << state->getStrokeOpacity();
    sp_repr_css_set_property(css, "stroke-opacity", os_opacity.str().c_str());

    // Line width
    Inkscape::CSSOStringStream os_width;
    double lw = state->getLineWidth();
    // emit a stroke which is 1px in toplevel user units
    os_width << (lw > 0.0 ? lw : 1.0);
    sp_repr_css_set_property(css, "stroke-width", os_width.str().c_str());

    // Line cap
    switch (state->getLineCap()) {
        case 0:
            sp_repr_css_set_property(css, "stroke-linecap", "butt");
            break;
        case 1:
            sp_repr_css_set_property(css, "stroke-linecap", "round");
            break;
        case 2:
            sp_repr_css_set_property(css, "stroke-linecap", "square");
            break;
    }

    // Line join
    switch (state->getLineJoin()) {
        case 0:
            sp_repr_css_set_property(css, "stroke-linejoin", "miter");
            break;
        case 1:
            sp_repr_css_set_property(css, "stroke-linejoin", "round");
            break;
        case 2:
            sp_repr_css_set_property(css, "stroke-linejoin", "bevel");
            break;
    }

    // Miterlimit
    Inkscape::CSSOStringStream os_ml;
    os_ml << state->getMiterLimit();
    sp_repr_css_set_property(css, "stroke-miterlimit", os_ml.str().c_str());

    // Line dash
    int dash_length;
    double dash_start;
#if POPPLER_CHECK_VERSION(22, 9, 0)
    const double *dash_pattern;
    const std::vector<double> &dash = state->getLineDash(&dash_start);
    dash_pattern = dash.data();
    dash_length = dash.size();
#else
    double *dash_pattern;
    state->getLineDash(&dash_pattern, &dash_length, &dash_start);
#endif
    if ( dash_length > 0 ) {
        Inkscape::CSSOStringStream os_array;
        for ( int i = 0 ; i < dash_length ; i++ ) {
            os_array << dash_pattern[i];
            if (i < (dash_length - 1)) {
                os_array << ",";
            }
        }
        sp_repr_css_set_property(css, "stroke-dasharray", os_array.str().c_str());

        Inkscape::CSSOStringStream os_offset;
        os_offset << dash_start;
        sp_repr_css_set_property(css, "stroke-dashoffset", os_offset.str().c_str());
    } else {
        sp_repr_css_set_property(css, "stroke-dasharray", "none");
        sp_repr_css_set_property(css, "stroke-dashoffset", nullptr);
    }
}

/**
 * \brief Sets fill style from poppler's GfxState data structure
 * Uses the given SPCSSAttr for storing the style properties.
 */
void SvgBuilder::_setFillStyle(SPCSSAttr *css, GfxState *state, bool even_odd) {

    // Fill color/pattern
    auto space = state->getFillColorSpace();
    if (space->getMode() == csPattern) {
        gchar *urltext = _createPattern(state->getFillPattern(), state);
        sp_repr_css_set_property(css, "fill", urltext);
        if (urltext) {
            g_free(urltext);
        }
    } else {
        sp_repr_css_set_property(css, "fill", convertGfxColor(state->getFillColor(), space, _getIntent(state), true).c_str());
    }

    // Opacity
    Inkscape::CSSOStringStream os_opacity;
    os_opacity << state->getFillOpacity();
    sp_repr_css_set_property(css, "fill-opacity", os_opacity.str().c_str());

    // Fill rule
    sp_repr_css_set_property(css, "fill-rule", even_odd ? "evenodd" : "nonzero");
}

/**
 * \brief Sets blend style properties from poppler's GfxState data structure
 * \update a SPCSSAttr with all mix-blend-mode set
 */
void SvgBuilder::_setBlendMode(Inkscape::XML::Node *node, GfxState *state)
{
    SPCSSAttr *css = sp_repr_css_attr(node, "style");
    GfxBlendMode blendmode = state->getBlendMode();
    if (blendmode) {
        sp_repr_css_set_property(css, "mix-blend-mode", enum_blend_mode[blendmode].key);
    }
    Glib::ustring value;
    sp_repr_css_write_string(css, value);
    node->setAttributeOrRemoveIfEmpty("style", value);
    sp_repr_css_attr_unref(css);
}

void SvgBuilder::_setTransform(Inkscape::XML::Node *node, GfxState *state, Geom::Affine extra)
{
    svgSetTransform(node, extra * stateToAffine(state) * _page_affine);
}

/**
 * \brief Sets style properties from poppler's GfxState data structure
 * \return SPCSSAttr with all the relevant properties set
 */
SPCSSAttr *SvgBuilder::_setStyle(GfxState *state, bool fill, bool stroke, bool even_odd) {
    SPCSSAttr *css = sp_repr_css_attr_new();
    if (fill) {
        _setFillStyle(css, state, even_odd);
    } else {
        sp_repr_css_set_property(css, "fill", "none");
    }

    if (stroke) {
        _setStrokeStyle(css, state);
    } else {
        sp_repr_css_set_property(css, "stroke", "none");
    }

    return css;
}

/**
 * Returns the CSSAttr of the previously added path if it's exactly
 * the same path AND is missing the fill or stroke that is now being painted.
 */
bool SvgBuilder::shouldMergePath(bool is_fill, const std::string &path)
{
    auto prev = _container->lastChild();
    if (!prev || prev->attribute("mask"))
        return false;

    auto prev_d = prev->attribute("d");
    if (!prev_d)
        return false;

    if (path != prev_d && path != std::string(prev_d) + " Z")
        return false;

    auto prev_css = sp_repr_css_attr(prev, "style");
    std::string prev_val = sp_repr_css_property(prev_css, is_fill ? "fill" : "stroke", "");
    // Very specific check excludes paths created elsewhere who's fill/stroke was unset.
    return prev_val == "none";
}

/**
 * Set the fill XOR stroke of the previously added path, if that path
 * is missing the given attribute AND the path is exactly the same.
 *
 * This effectively merges the two objects and is an 'interpretation' step.
 */
bool SvgBuilder::mergePath(GfxState *state, bool is_fill, const std::string &path, bool even_odd)
{
    if (shouldMergePath(is_fill, path)) {
        auto prev = _container->lastChild();
        SPCSSAttr *css = sp_repr_css_attr_new();
        if (is_fill) {
            _setFillStyle(css, state, even_odd);
            // Fill after stroke indicates a different paint order.
            sp_repr_css_set_property(css, "paint-order", "stroke fill markers");
        } else {
            _setStrokeStyle(css, state);
        }
        sp_repr_css_change(prev, css, "style");
        sp_repr_css_attr_unref(css);
        return true;
    }
    return false;
}

/**
 * \brief Emits the current path in poppler's GfxState data structure
 * Can be used to do filling and stroking at once.
 *
 * \param fill whether the path should be filled
 * \param stroke whether the path should be stroked
 * \param even_odd whether the even-odd rule should be used when filling the path
 */
void SvgBuilder::addPath(GfxState *state, bool fill, bool stroke, bool even_odd) {
    gchar *pathtext = svgInterpretPath(state->getPath());

    if (!pathtext)
        return;

    if (!strlen(pathtext) || (fill != stroke && mergePath(state, fill, pathtext, even_odd))) {
        g_free(pathtext);
        return;
    }

    Inkscape::XML::Node *path = _addToContainer("svg:path");
    path->setAttribute("d", pathtext);
    g_free(pathtext);

    // Set style
    SPCSSAttr *css = _setStyle(state, fill, stroke, even_odd);
    sp_repr_css_change(path, css, "style");
    sp_repr_css_attr_unref(css);
    _setBlendMode(path, state);
    _setTransform(path, state);
    _setClipPath(path);
}

/**
 * \brief Emits the current path in poppler's GfxState data structure
 * The path is set to be filled with the given shading.
 */
void SvgBuilder::addShadedFill(GfxState *state, GfxShading *shading, const Geom::Affine shading_tr)
{
    auto path = _clip_history->getClipPath();
    if (_group_by == GroupBy::BY_OCGS) {
        path = _clip_history->getFlattenedClipPath();
    }

    if (path.empty()) {
        // For consistent behaviour with previous addClippedFill wrapper, but add a warning
        g_warning("No clipping path found, skipping shaded fill");
        return;
    }

    auto prev = _container->lastChild();   
    auto pathtext = sp_svg_write_path(path);
    // Create a new gradient object before comitting to creating a path for it
    // And package it into a css bundle which can be applied
    SPCSSAttr *css = sp_repr_css_attr_new();
    gchar *id = _createGradient(state, shading, shading_tr);
    if (id) {
        gchar *urltext = g_strdup_printf ("url(#%s)", id);
        sp_repr_css_set_property(css, "fill", urltext);
        g_free(urltext);
        g_free(id);
    } else {
        sp_repr_css_attr_unref(css);
        return;
    }
    if (_clip_history->getFillRule() == FillRule::fill_oddEven) {
        sp_repr_css_set_property(css, "fill-rule", "evenodd");
    }
    // Merge the style with the previous shape
    if (shouldMergePath(true, pathtext)) {
        // POSSIBLE: The gradientTransform might now incorrect if the
        // state of the transformation was different between the two paths.
        sp_repr_css_change(prev, css, "style");
        return;
    }

    Inkscape::XML::Node *path_node = _addToContainer("svg:path");
    path_node->setAttribute("d", pathtext);

    // Don't add transforms to mask children.
    if (std::string("svg:mask") != _container->name()) {
        svgSetTransform(path_node, _page_affine);
    }

    // Set the gradient into this new path.
    sp_repr_css_set_property(css, "stroke", "none");
    sp_repr_css_change(path_node, css, "style");
    sp_repr_css_attr_unref(css);
}

/**
 * \brief Clips to the current path set in GfxState
 * \param state poppler's data structure
 * \param even_odd whether the even-odd rule should be applied
 */
void SvgBuilder::setClip(GfxState *state, GfxClipType clip, bool is_bbox)
{
    // When there's already a clip path, we add clipping groups to handle them.
    if (!is_bbox && _clip_history->hasClipPath() && _group_by == GroupBy::BY_XOBJECT) {
        _pushContainer("svg:g");
        _clip_groups++;
        _clip_history = _clip_history->save();
    }
    
    _clip_history->setClip(state, clip);
}

/**
 * Return the active clip as a new xml node.
 */
Inkscape::XML::Node *SvgBuilder::_getClip(const Inkscape::XML::Node *node)
{
    // In SVG the path-clip transforms are compounded, so we have to do extra work to
    // pull transforms back out of the clipping object and set them. Otherwise this
    // would all be a lot simpler.

    // node_tr includes _page_affine
    Geom::Affine node_tr = Geom::identity();
    if (auto attr = node->attribute("transform")) {
        sp_svg_transform_read(attr, &node_tr);
    }

    if (_clip_text) {
        auto clip_node = _clip_text;

        // clip transform should now be baked into the path
        // auto text_tr = Geom::identity();
        // if (auto attr = clip_node->attribute("transform")) {
        //     sp_svg_transform_read(attr, &text_tr);
        //     clip_node->removeAttribute("transform");
        // }

        for (auto child = clip_node->firstChild(); child; child = child->next()) {
            Geom::Affine child_tr = _page_affine * node_tr.inverse();
            svgSetTransform(child, child_tr);
        }

        _clip_text = nullptr;
        return clip_node;
    }
    auto clip_pathv = _checkClip(node, node_tr);
    if (!clip_pathv.empty()) {
        // page and clip transforms are applied in _createClip, but we need to apply the
        // node inverse so that it compounds properly when clipping in SVG
        auto clip_d = sp_svg_write_path(clip_pathv * node_tr.inverse());
        return _createClip(clip_d, _clip_history->getFillRule() == FillRule::fill_oddEven);
    }
    return nullptr;
}

Geom::PathVector SvgBuilder::_checkClip(const Inkscape::XML::Node *node, const Geom::Affine &node_tr) const
{
    Geom::PathVector current_clip;
    if (node->attribute("clip-path")) {
        // If the node already has a clip path, compound it
        if (auto clip_txt = try_extract_uri(node->attribute("clip_path"))) {
            current_clip = sp_svg_read_pathv(clip_txt.value().c_str());
        }
    }

    // node_tr includes _page_affine, so we need to apply it to the clipping path
    auto clip_pathv = maybeIntersect(_clip_history->getClipPath() * _page_affine, current_clip);

    // if this is a clipping or masking group situation, just return the clip
    if (strcmp(node->name(), "svg:g") == 0 || _clip_groups > 0 || _mask_groups.size() > 0) {
        return clip_pathv;
    }

    if (_group_by == GroupBy::BY_OCGS) {
        // if we're not using clipping groups, flatten the clip path.
        clip_pathv = maybeIntersect(_clip_history->getFlattenedClipPath() * _page_affine, current_clip);
    }

    if (clip_pathv.empty()) {
        // possible to have an empty clipping path at this point
        return clip_pathv;
    }

    // Calculate bounding boxes for both the node and the clip path
    Geom::PathVector node_vec = sp_svg_read_pathv(node->attribute("d"));

    if (node_vec.empty()) {
        // Non-path node (text, image, etc)
        // Create a PathVector of the bounding box instead
        _doc->ensureUpToDate();
        auto item = cast<SPItem>(_doc->getObjectByRepr(const_cast<Inkscape::XML::Node *>(node)));
        // transform will be applied later, so default identity is good
        Geom::OptRect bounds;
        bounds = item ? item->visualBounds() : bounds;

        if (!bounds.empty()) {
            node_vec.push_back(Geom::Path(*bounds));
        } else {
            // Text nodes in forms in XObject mode haven't been added to the doc yet,
            // so bounds can't be computed in this way. Not an issue in OCG grouping mode.
            // Default to clipping.
            return clip_pathv;
        }
    }

    node_vec *= node_tr;

    if (pathv_fully_contains(clip_pathv, node_vec, _clip_history->getFillRule())) {
        return Geom::PathVector();
    } else {
        return clip_pathv;
    }
}

Inkscape::XML::Node *SvgBuilder::_createClip(const std::string &d, bool even_odd)
{
    if (_prev_clip) {
        // Check if the previous clipping path would be identical to the new one.
        auto prev_path = _prev_clip->firstChild();
        auto prev_d = prev_path->attribute("d");
        auto prev_even_odd =
            prev_path->attribute("clip-rule") ? std::string(prev_path->attribute("clip-rule")) == "evenodd" : false;

        // Don't create an identical new clipping path
        if (prev_d == d && prev_even_odd == even_odd) {
            return _prev_clip;
        }
    }

    Inkscape::XML::Node *clip_path = _xml_doc->createElement("svg:clipPath");
    clip_path->setAttribute("clipPathUnits", "userSpaceOnUse");

    // Create the path
    Inkscape::XML::Node *path = _xml_doc->createElement("svg:path");
    path->setAttribute("d", d);

    if (even_odd) {
        path->setAttribute("clip-rule", "evenodd");
    }
    clip_path->appendChild(path);
    Inkscape::GC::release(path);

    // Append clipPath to defs and get id
    _doc->getDefs()->getRepr()->appendChild(clip_path);
    Inkscape::GC::release(clip_path);

    // update the previous clip path
    _prev_clip = clip_path;

    return clip_path;
}

void SvgBuilder::beginMarkedContent(const char *name, const char *group)
{
    if (name && group && std::string(name) == "OC") {
        auto layer_id = std::string("layer-") + sanitizeId(group);
        if (auto existing = _doc->getObjectById(layer_id)) {
            if (existing->getRepr()->parent() == _container) {
                _container = existing->getRepr();
                _node_stack.push_back(_container);
            } else {
                g_warning("Unexpected marked content group in PDF!");
                _pushGroup();
            }
        } else {
            auto node = _pushGroup();
            node->setAttribute("id", layer_id);
            if (_ocgs.find(group) != _ocgs.end()) {
                auto pair = _ocgs[group];
                setAsLayer(pair.first.c_str(), pair.second);
            } else {
                // assume visible
                g_warning("Found undefined marked content group in PDF!");
                setAsLayer(group, true);
            }
        }
    } else {
        auto node = _pushGroup();
        if (group) {
            node->setAttribute("id", std::string("group-") + sanitizeId(group));
        }
    }
}

void SvgBuilder::addOptionalGroup(const std::string &oc, const std::string &label, bool visible)
{
    _ocgs[oc] = {label, visible};
}

void SvgBuilder::beginXObjectLayer(const std::string &label)
{
    // find the group key for the label (reverse map search)
    auto group = label;
    for (const auto& [key, value] : _ocgs) {
        if (value.first == label) {
            group = key;
            break;
        }
    }

    // Reset to root
    while (_container != _root) {
        _popGroup();
    }
    
    beginMarkedContent("OC", group.c_str());
}

void SvgBuilder::endMarkedContent()
{
    _popGroup();
}

void SvgBuilder::addColorProfile(unsigned char *profBuf, int length)
{
    cmsHPROFILE hp = cmsOpenProfileFromMem(profBuf, length);
    if (!hp) {
        g_warning("Failed to read ICCBased color space profile from PDF file.");
        return;
    }
    _icc_profile = Colors::CMS::Profile::create(hp);
}

/**
 * Return the color profile as an Inkscape color space or none if it can't be constructed.
 */
std::shared_ptr<Colors::Space::AnySpace> SvgBuilder::_getColorSpace(std::shared_ptr<Colors::CMS::Profile> const &profile, Colors::RenderingIntent intent)
{
    if (!profile)
        return {};

    auto &cms = _doc->getDocumentCMS();
    // Attempts to attach the profile to the document, if it already exists returns the name
    auto name = cms.attachProfileToDoc(*profile, ColorProfileStorage::HREF_DATA, intent);
    return cms.getSpace(name);
}

/**
 * \brief Checks whether the given pattern type can be represented in SVG
 * Used by PdfParser to decide when to do fallback operations.
 */
bool SvgBuilder::isPatternTypeSupported(GfxPattern *pattern) {
    if ( pattern != nullptr ) {
        if ( pattern->getType() == 2 ) {    // shading pattern
            GfxShading *shading = (static_cast<GfxShadingPattern *>(pattern))->getShading();
            int shadingType = shading->getType();
            if ( shadingType == 2 || // axial shading
                 shadingType == 3 ) {   // radial shading
                return true;
            }
            return false;
        } else if ( pattern->getType() == 1 ) {   // tiling pattern
            return true;
        }
    }

    return false;
}

/**
 * \brief Creates a pattern from poppler's data structure
 * Handles linear and radial gradients. Creates a new PdfParser and uses it to
 * build a tiling pattern.
 * \return a url pointing to the created pattern
 */
gchar *SvgBuilder::_createPattern(GfxPattern *pattern, GfxState *state, bool is_stroke) {
    gchar *id = nullptr;
    if ( pattern != nullptr ) {
        if ( pattern->getType() == 2 ) {  // Shading pattern
            GfxShadingPattern *shading_pattern = static_cast<GfxShadingPattern *>(pattern);
            // construct a (pattern space) -> (current space) transform matrix
            auto flip = Geom::Affine(1.0, 0.0, 0.0, -1.0, 0.0, _height);
            auto pt = Geom::Scale(Inkscape::Util::Quantity::convert(1.0, "pt", "px"));
            auto grad_affine = ctmToAffine(shading_pattern->getMatrix());
            auto obj_affine = stateToAffine(state);
            // SVG applies the object's affine on top of the gradient's affine,
            // So we must remove the object affine to move it back into place.
            auto affine = (grad_affine * pt * flip) * obj_affine.inverse();
            id = _createGradient(state, shading_pattern->getShading(), affine);
        } else if ( pattern->getType() == 1 ) {   // Tiling pattern
            id = _createTilingPattern(static_cast<GfxTilingPattern*>(pattern), state, is_stroke);
        }
    } else {
        return nullptr;
    }
    gchar *urltext = g_strdup_printf ("url(#%s)", id);
    g_free(id);
    return urltext;
}

/**
 * \brief Creates a tiling pattern from poppler's data structure
 * Creates a sub-page PdfParser and uses it to parse the pattern's content stream.
 * \return id of the created pattern
 */
gchar *SvgBuilder::_createTilingPattern(GfxTilingPattern *tiling_pattern,
                                        GfxState *state, bool is_stroke) {

    Inkscape::XML::Node *pattern_node = _xml_doc->createElement("svg:pattern");
    // Set pattern transform matrix
    auto pat_matrix = ctmToAffine(tiling_pattern->getMatrix());
    pattern_node->setAttributeOrRemoveIfEmpty("patternTransform", sp_svg_transform_write(pat_matrix));
    pattern_node->setAttribute("patternUnits", "userSpaceOnUse");
    // Set pattern tiling
    // FIXME: don't ignore XStep and YStep
    const auto& bbox = tiling_pattern->getBBox();
    pattern_node->setAttributeSvgDouble("x", 0.0);
    pattern_node->setAttributeSvgDouble("y", 0.0);
    pattern_node->setAttributeSvgDouble("width", bbox[2] - bbox[0]);
    pattern_node->setAttributeSvgDouble("height", bbox[3] - bbox[1]);

    // Convert BBox for PdfParser
    PDFRectangle box;
    box.x1 = bbox[0];
    box.y1 = bbox[1];
    box.x2 = bbox[2];
    box.y2 = bbox[3];
    // Create new SvgBuilder and sub-page PdfParser
    SvgBuilder *pattern_builder = new SvgBuilder(this, pattern_node);
    PdfParser *pdf_parser = new PdfParser(_xref, pattern_builder, tiling_pattern->getResDict(),
                                          &box);
    // Get pattern color space
    GfxPatternColorSpace *pat_cs = (GfxPatternColorSpace *)( is_stroke ? state->getStrokeColorSpace()
                                                            : state->getFillColorSpace() );
    // Set fill/stroke colors if this is an uncolored tiling pattern
    GfxColorSpace *cs = nullptr;
    if ( tiling_pattern->getPaintType() == 2 && ( cs = pat_cs->getUnder() ) ) {
        GfxState *pattern_state = pdf_parser->getState();
        pattern_state->setFillColorSpace(cs->copy());
        pattern_state->setFillColor(state->getFillColor());
        pattern_state->setStrokeColorSpace(cs->copy());
        pattern_state->setStrokeColor(state->getFillColor());
    }

    // Generate the SVG pattern
    pdf_parser->parse(tiling_pattern->getContentStream());

    // Cleanup
    delete pdf_parser;
    delete pattern_builder;

    // Append the pattern to defs
    _doc->getDefs()->getRepr()->appendChild(pattern_node);
    gchar *id = g_strdup(pattern_node->attribute("id"));
    Inkscape::GC::release(pattern_node);

    return id;
}

/**
 * \brief Creates a linear or radial gradient from poppler's data structure
 * \param shading poppler's data structure for the shading
 * \param matrix gradient transformation, can be null
 * \return id of the created object
 */
gchar *SvgBuilder::_createGradient(GfxState *state, GfxShading *shading, const Geom::Affine pat_matrix)
{
    Inkscape::XML::Node *gradient;
    _POPPLER_CONST Function *func;
    int num_funcs;
    bool extend0, extend1;

    if ( shading->getType() == 2 ) {  // Axial shading
        gradient = _xml_doc->createElement("svg:linearGradient");
        GfxAxialShading *axial_shading = static_cast<GfxAxialShading*>(shading);
        double x1, y1, x2, y2;
        axial_shading->getCoords(&x1, &y1, &x2, &y2);
        gradient->setAttributeSvgDouble("x1", x1);
        gradient->setAttributeSvgDouble("y1", y1);
        gradient->setAttributeSvgDouble("x2", x2);
        gradient->setAttributeSvgDouble("y2", y2);
        extend0 = axial_shading->getExtend0();
        extend1 = axial_shading->getExtend1();
        num_funcs = axial_shading->getNFuncs();
        func = axial_shading->getFunc(0);
    } else if (shading->getType() == 3) {   // Radial shading
        gradient = _xml_doc->createElement("svg:radialGradient");
        GfxRadialShading *radial_shading = static_cast<GfxRadialShading*>(shading);
        double x1, y1, r1, x2, y2, r2;
        radial_shading->getCoords(&x1, &y1, &r1, &x2, &y2, &r2);
        // FIXME: the inner circle's radius is ignored here
        gradient->setAttributeSvgDouble("fx", x1);
        gradient->setAttributeSvgDouble("fy", y1);
        gradient->setAttributeSvgDouble("cx", x2);
        gradient->setAttributeSvgDouble("cy", y2);
        gradient->setAttributeSvgDouble("r", r2);
        extend0 = radial_shading->getExtend0();
        extend1 = radial_shading->getExtend1();
        num_funcs = radial_shading->getNFuncs();
        func = radial_shading->getFunc(0);
    } else {    // Unsupported shading type
        return nullptr;
    }
    gradient->setAttribute("gradientUnits", "userSpaceOnUse");
    // If needed, flip the gradient transform around the y axis
    if (pat_matrix != Geom::identity()) {
        gradient->setAttributeOrRemoveIfEmpty("gradientTransform", sp_svg_transform_write(pat_matrix));
    }

    if ( extend0 && extend1 ) {
        gradient->setAttribute("spreadMethod", "pad");
    }

    if ( num_funcs > 1 || !_addGradientStops(gradient, state, shading, func) ) {
        Inkscape::GC::release(gradient);
        return nullptr;
    }

    _doc->getDefs()->getRepr()->appendChild(gradient);
    gchar *id = g_strdup(gradient->attribute("id"));
    Inkscape::GC::release(gradient);

    return id;
}

#define EPSILON 0.0001
/**
 * \brief Adds a stop with the given properties to the gradient's representation
 */
void SvgBuilder::_addStopToGradient(Inkscape::XML::Node *gradient, double offset, GfxColor &color, GfxColorSpace *space,
                                    Colors::RenderingIntent intent, double opacity)
{
    Inkscape::XML::Node *stop = _xml_doc->createElement("svg:stop");
    SPCSSAttr *css = sp_repr_css_attr_new();
    Inkscape::CSSOStringStream os_opacity;
    std::string color_text = "#ffffff";
    if (space->getMode() == csDeviceGray) {
        // This is a transparency mask.
        GfxRGB rgb;
        space->_POPPLER_GET_RGB(color, &rgb);
        double gray = (double)rgb.r / 65535.0;
        gray = CLAMP(gray, 0.0, 1.0);
        os_opacity << gray;
    } else {
        os_opacity << opacity;
        color_text = convertGfxColor(color, space, intent);
    }
    sp_repr_css_set_property(css, "stop-opacity", os_opacity.str().c_str());
    sp_repr_css_set_property(css, "stop-color", color_text.c_str());

    sp_repr_css_change(stop, css, "style");
    sp_repr_css_attr_unref(css);
    stop->setAttributeCssDouble("offset", offset);

    gradient->appendChild(stop);
    Inkscape::GC::release(stop);
}

static bool svgGetShadingColor(GfxShading *shading, double offset, GfxColor *result)
{
    if ( shading->getType() == 2 ) {  // Axial shading
        (static_cast<GfxAxialShading *>(shading))->getColor(offset, result);
    } else if ( shading->getType() == 3 ) { // Radial shading
        (static_cast<GfxRadialShading *>(shading))->getColor(offset, result);
    } else {
        return false;
    }
    return true;
}


#define INT_EPSILON 8
bool SvgBuilder::_addGradientStops(Inkscape::XML::Node *gradient, GfxState *state, GfxShading *shading,
                                   _POPPLER_CONST Function *func) {
    auto type = func->getType();
    auto space = shading->getColorSpace();
    auto intent = _getIntent(state);
    if (type == _POPPLER_FUNCTION_TYPE_SAMPLED || type == _POPPLER_FUNCTION_TYPE_EXPONENTIAL) {
        GfxColor stop1, stop2;
        if (!svgGetShadingColor(shading, 0.0, &stop1) || !svgGetShadingColor(shading, 1.0, &stop2)) {
            return false;
        } else {
            _addStopToGradient(gradient, 0.0, stop1, space, intent, 1.0);
            _addStopToGradient(gradient, 1.0, stop2, space, intent, 1.0);
        }
    } else if (type == _POPPLER_FUNCTION_TYPE_STITCHING) {
        auto stitchingFunc = static_cast<_POPPLER_CONST StitchingFunction*>(func);
        const double *bounds = stitchingFunc->getBounds();
        const double *encode = stitchingFunc->getEncode();
        int num_funcs = stitchingFunc->getNumFuncs();
        // Adjust gradient so it's always between 0.0 - 1.0
        double max_bound = std::max({1.0, bounds[num_funcs]});

        // Add stops from all the stitched functions
        GfxColor prev_color, color;
        svgGetShadingColor(shading, bounds[0], &prev_color);
        _addStopToGradient(gradient, bounds[0], prev_color, space, intent, 1.0);
        for ( int i = 0 ; i < num_funcs ; i++ ) {
            svgGetShadingColor(shading, bounds[i + 1], &color);
            // Add stops
            if (stitchingFunc->getFunc(i)->getType() == _POPPLER_FUNCTION_TYPE_EXPONENTIAL) {
                double expE = (static_cast<_POPPLER_CONST ExponentialFunction*>(stitchingFunc->getFunc(i)))->getE();
                if (expE > 1.0) {
                    expE = (bounds[i + 1] - bounds[i])/expE;    // approximate exponential as a single straight line at x=1
                    if (encode[2*i] == 0) {    // normal sequence
                        auto offset = (bounds[i + 1] - expE) / max_bound;
                        _addStopToGradient(gradient, offset, prev_color, space, intent, 1.0);
                    } else {                   // reflected sequence
                        auto offset = (bounds[i] + expE) / max_bound;
                        _addStopToGradient(gradient, offset, color, space, intent, 1.0);
                    }
                }
            }
            _addStopToGradient(gradient, bounds[i + 1] / max_bound, color, space, intent, 1.0);
            prev_color = color;
        }
    } else { // Unsupported function type
        return false;
    }

    return true;
}

/**
 * \brief Sets _invalidated_style to true to indicate that styles have to be updated
 * Used for text output when glyphs are buffered till a font change
 */
void SvgBuilder::updateStyle(GfxState *state) {
    if (_in_text_object) {
        _invalidated_style = true;
    }
}

/**
 * \brief Updates _css_font according to the font set in parameter state
 */
void SvgBuilder::updateFont(GfxState *state, std::shared_ptr<CairoFont> cairo_font, bool flip)
{
    TRACE(("updateFont()\n"));
    // If the font size is negative, don't flip
    auto new_font_size = state->getFontSize();
    updateTextMatrix(state, flip && new_font_size > 0);    // Ensure that we have a text matrix built

    auto font = state->getFont();  // GfxFont
    auto font_id = font->getID()->num;

    if (font->getType() == fontType3) {
        const auto& font_matrix = font->getFontMatrix();
        if (font_matrix[0] != 0.0) {
            new_font_size *= font_matrix[3] / font_matrix[0];
        }
    }
    if (new_font_size != _css_font_size) {
        _css_font_size = fabs(new_font_size);
        _invalidated_style = true;
    }

    bool was_css_font = (bool)_css_font;
    // Clean up any previous css font
    if (_css_font) {
        sp_repr_css_attr_unref(_css_font);
        _css_font = nullptr;
    }

    auto font_strategy = FontFallback::AS_TEXT;
    if (_font_strategies.find(font_id) != _font_strategies.end()) {
        font_strategy = _font_strategies[font_id];
    }

    if (font_strategy == FontFallback::DELETE_TEXT) {
        // Delete all text when font is missing.
        _invalidated_strategy = true; // Flush any text in buffer.
        _cairo_font = nullptr;
        return;
    }

    if (font_strategy == FontFallback::AS_SHAPES) {
        // Render text as paths when font is missing.
        _invalidated_strategy = _invalidated_strategy || was_css_font;
        _invalidated_style = (_cairo_font != cairo_font);
        _cairo_font = cairo_font;
        return;
    }

    auto font_data = FontData(font);
    auto new_font_specification = font_data.getSpecification();
    TRACE(("FontSpecification: %s\n", new_font_specification.c_str()));
    if (_font_specification != new_font_specification) {
        // If any font property changes, we need a new <tspan> or <path>.
        _font_specification = new_font_specification;
        _invalidated_strategy = false; // We don't need to flush text which creates a <text> element,
                                       // we will just create new <tspan>.
        _invalidated_style = true; // Changed style
    }

    // Font family
    _cairo_font = nullptr;
    _css_font = sp_repr_css_attr_new();
    if (font_data.found) {
        sp_repr_css_set_property(_css_font, "font-family", font_data.family.c_str());
    } else if (font_strategy == FontFallback::AS_SUB) {
        sp_repr_css_set_property(_css_font, "font-family", font_data.getSubstitute().c_str());
    } else {
        auto keep_name = font_data.family.size() ? font_data.family : font_data.name;
        sp_repr_css_set_property(_css_font, "font-family", keep_name.c_str());
    }

    // Set the font data (are these really necessary if they have default values?).
    sp_repr_css_set_property(_css_font, "font-style", font_data.style.c_str());
    sp_repr_css_set_property(_css_font, "font-weight", font_data.weight.c_str());
    sp_repr_css_set_property(_css_font, "font-stretch", font_data.stretch.c_str());
    sp_repr_css_set_property(_css_font, "font-variant", "normal");

    // Writing mode
    if ( font->getWMode() == _POPPLER_WMODE_HORIZONTAL ) {
        sp_repr_css_set_property(_css_font, "writing-mode", "lr");
    } else {
        sp_repr_css_set_property(_css_font, "writing-mode", "tb");
    }
}

/**
 * \brief Shifts the current text position by the given amount (specified in text space)
 */
void SvgBuilder::updateTextShift(GfxState *state, double shift) {
    double shift_value = -shift * 0.001 * fabs(state->getFontSize());
    if (state->getFont()->getWMode() != _POPPLER_WMODE_HORIZONTAL) {
        _text_position[1] += shift_value;
    } else {
        _text_position[0] += shift_value;
    }
}

/**
 * \brief Updates current text position
 */
void SvgBuilder::updateTextPosition(double tx, double ty) {
    _text_position = Geom::Point(tx, ty);
}

/**
 * \brief Flushes the buffered characters
 */
void SvgBuilder::updateTextMatrix(GfxState *state, bool flip) {
    // Update text matrix, it contains an extra flip which we must undo.
    auto new_matrix = Geom::Scale(1, flip ? -1 : 1) * ctmToAffine(state->getTextMat());
    // TODO: Detect if the text matrix is actually just a rotational kern
    // this can help stich back together texts where letters are rotated
    if (new_matrix != _text_matrix) {
        _flushText(state);
        _text_matrix = new_matrix;
    }
}

/**
 * \brief Notifies the svg builder the state will change
 *
 * Used to flushText if we are in text object.
 * This is necessary as the state stored in glyphs is only a pointer to the current state,
 * thus changing the state changes a glyphs every glyph style. This needs fixing!
*/
void SvgBuilder::beforeStateChange(GfxState *old_state) {
    if (_in_text_object) {
        _flushText(old_state);
    }
}

/**
 * Create text node for text.
 */
Inkscape::XML::Node* SvgBuilder::_flushTextText(GfxState *state, double text_scale, const Geom::Affine& text_transform)
{
    auto text_node = _addToContainer("svg:text");
    assert (text_node);

    // We preserve spaces in the text objects we create, this applies to any descendant.
    text_node->setAttribute("xml:space", "preserve");

    // Text direction is a property of the <text> element.
    auto font = state->getFont();
    if (font->getWMode() == _POPPLER_WMODE_VERTICAL) {
        // Only set if vertical.
        auto css_text = sp_repr_css_attr_new();
        sp_repr_css_set_property(css_text, "writing-mode", "tb");
        sp_repr_css_change(text_node, css_text, "style");
        sp_repr_css_attr_unref(css_text);
    }

    // Prepare to loop over all glyphs in buffer.
    unsigned int glyphs_in_tspan = 0;
    Glib::ustring text_buffer;

    // SVG attributes, only spaces and digits.
    std::string x_coords;
    std::string y_coords;
    std::string dx_coords;
    std::string dy_coords;

    auto first_glyph = _glyphs.front();
    auto prev_glyph  = _glyphs.front();
    for (auto it = _glyphs.begin(); it != _glyphs.end();  ++it ) {

        // Add glyph
        auto glyph = *it;

        // Absolute position (used to position tspan, only on first character).
        if (glyphs_in_tspan == 0) {
            //first_glyph = glyph;
            prev_glyph = glyph; // So dx and dy for first glyph in tspan are both zero.
            Geom::Point delta_pos(glyph.text_position - first_glyph.text_position);
            delta_pos[1] += glyph.rise;
            delta_pos[1] *= -1.0;   // flip it
            delta_pos *= Geom::Scale(text_scale);
            delta_pos += glyph.origin; // Corrects vertical text position.

            Inkscape::CSSOStringStream os_x;
            os_x << delta_pos[0];
            x_coords.append(os_x.str());

            Inkscape::CSSOStringStream os_y;
            os_y << delta_pos[1];
            y_coords.append(os_y.str());
        }

        // Relative position (used to position characters within tspan).
        Geom::Point delta_dpos;
        if (glyphs_in_tspan != 0) {
            // Subtract off previous glyph position and advance.
            delta_dpos = glyph.text_position - prev_glyph.text_position - prev_glyph.advance;
        }

        // Eliminate small rounding errors.
        if (std::abs(delta_dpos[0]) < 0.005) {
            delta_dpos[0] = 0.0;
        }
        if (std::abs(delta_dpos[1]) < 0.005) {
            delta_dpos[1] = 0.0;
        }

        delta_dpos[1] += glyph.rise;
        delta_dpos[1] *= -1.0;   // flip it

        delta_dpos *= Geom::Scale(text_scale);


        Inkscape::CSSOStringStream os_dx;
        os_dx << delta_dpos[0] << " ";
        dx_coords.append(os_dx.str());

        Inkscape::CSSOStringStream os_dy;
        os_dy << delta_dpos[1] << " ";
        dy_coords.append(os_dy.str());

        // Add Unicode points to buffer.
        // There may be a glyph to many Unicode point mapping (e.g. a ligature).
        for (int i = 0; i < glyph.code.size(); i++) {
            text_buffer.append(1, glyph.code[i]);
            if (i != 0) {
                dx_coords.append("0 ");
                dy_coords.append("0 ");
            }
        }

        // Check to see if we need to output <tspan>.
        // We output if:
        //  1. Last glyph.
        //  2. Next glyph has different style.
        //  3. Next glyph on new line. TODO: remove this as we can have multiline text now without <tspan>s.

        auto writing_mode = state->getFont()->getWMode(); // Horizontal or vertical text.
        auto next_it = it + 1;
        bool output_tspan =
            next_it == _glyphs.end() ||
            next_it->style_changed   ||
            (writing_mode == _POPPLER_WMODE_HORIZONTAL && std::abs(glyph.text_position[1] - next_it->text_position[1]) > 0.1) ||
            (writing_mode == _POPPLER_WMODE_VERTICAL   && std::abs(glyph.text_position[0] - next_it->text_position[0]) > 0.1);

        if (output_tspan) {

            // Create and add new <tspan> to <text>.
            auto tspan_node = _xml_doc->createElement("svg:tspan");
            text_node->appendChild(tspan_node);
            Inkscape::GC::release(tspan_node);

            // Create and add text content node to <tspan>.
            Inkscape::XML::Node *text_content = _xml_doc->createTextNode(text_buffer.c_str());
            tspan_node->appendChild(text_content);
            Inkscape::GC::release(text_content);

            // Set style.
            double text_size = text_scale * glyph.text_size;
            sp_repr_css_set_property_double(glyph.css_font, "font-size", text_size);
            _setTextStyle(tspan_node, glyph.state, glyph.css_font, text_transform);

            // Unref SPCSSAttr if it won't be needed.
            // TODO: Remove 'if' wraper once we don't use <tspans> for new lines.
            // (Style is the same for all glyphs in a tspan.)
            if (next_it == _glyphs.end() ||
                next_it->style_changed  ) {
                sp_repr_css_attr_unref(glyph.css_font);
            }

            // Remove ' 0's at end.
            while (dx_coords.ends_with(" 0 ")) {
                dx_coords.erase(dx_coords.length() - 2);
            }

            while (dy_coords.ends_with(" 0 ")) {
                dy_coords.erase(dy_coords.length() - 2);
            }

            // Remove last entry if 0.
            if (dx_coords == "0 ") {
                dx_coords.clear();
            }

            if (dy_coords == "0 ") {
                dy_coords.clear();
            }

            // Remove space at end.
            if (dx_coords.length() > 0) {
                dx_coords.pop_back();
            }

            if (dy_coords.length() > 0) {
                dy_coords.pop_back();
            }

            tspan_node->setAttributeOrRemoveIfEmpty("x", x_coords);
            tspan_node->setAttributeOrRemoveIfEmpty("dx", dx_coords);

            tspan_node->setAttributeOrRemoveIfEmpty("y", y_coords);
            tspan_node->setAttributeOrRemoveIfEmpty("dy", dy_coords);

            // Reset.
            x_coords.clear();
            y_coords.clear();
            dx_coords.clear();
            dy_coords.clear();
            text_buffer.clear();
            glyphs_in_tspan = 0;

            TRACE(("tspan content: %s\n", text_buffer.c_str()));
        } else {
            glyphs_in_tspan++;
        }
        prev_glyph = glyph;
    }

    return text_node;
}

/**
 * Create path node(s) for text.
 */
Inkscape::XML::Node* SvgBuilder::_flushTextPath(GfxState *state, double text_scale, const Geom::Affine& text_transform)
{
    auto cairo_glyphs = (cairo_glyph_t *)gmallocn(_glyphs.size(), sizeof(cairo_glyph_t));
    unsigned int cairo_glyph_count = 0;

    Inkscape::XML::Node *node = nullptr;
    Inkscape::XML::Node *text_group = nullptr;  // Used to wrap paths if more that one path needed due
                                                // to style changes.

    auto first_glyph = _glyphs.front();
    for (auto it = _glyphs.begin(); it != _glyphs.end();  ++it ) {

        auto glyph = *it;

        // Append the coordinates to their respective strings
        Geom::Point delta_pos(glyph.text_position - first_glyph.text_position);
        delta_pos[1] += glyph.rise;
        delta_pos[1] *= -1.0;   // flip it
        delta_pos *= Geom::Scale(text_scale);

        // Push the data into the cairo glyph list for later rendering.
        cairo_glyphs[cairo_glyph_count].index = glyph.cairo_index;
        cairo_glyphs[cairo_glyph_count].x = delta_pos[Geom::X];
        cairo_glyphs[cairo_glyph_count].y = delta_pos[Geom::Y];
        cairo_glyph_count++;

        bool is_last_glyph = (it + 1) == _glyphs.end();
        bool flush_text = is_last_glyph ? true : (it+1)->style_changed;

        if (flush_text) {
            if (!is_last_glyph && !text_group) {
                text_group = _pushGroup(); // Create <g> wrapper if we have a style change mid-stream.
            }

            double text_size = text_scale * glyph.text_size;

            // Set to 'node' because if the style does NOT change, we won't have a group
            // but still need to set this text's position and blend modes.
            node = _renderText(glyph.cairo_font, text_size, text_transform, cairo_glyphs, cairo_glyph_count);
            if (!node) {
                g_warning("Empty or broken text in PDF file.");
                return nullptr;
            }
            _setTextStyle(node, glyph.state, nullptr, text_transform);

            if (text_group) {
                // Handled by _renderText
                // text_group->appendChild(node);
                // Inkscape::GC::release(node);
            }

            cairo_glyph_count = 0;

            if (is_last_glyph) {
                break;
            }
        }
    }

    // Clean up
    gfree(cairo_glyphs);
    cairo_glyphs = nullptr;

    if (text_group) {
        node = text_group;
        _popGroup();
    }

    node->setAttribute("aria-label", _aria_label);
    _aria_label = "";

    return node;
}

/**
 * \brief Writes the buffered characters to the SVG document.
 *
 * This is a dual path function that can produce either a text element
 * or a group of path elements depending on the font handling mode.
 */
void SvgBuilder::_flushText(GfxState *state)
{
    // Ignore empty strings
    if (_glyphs.empty()) {
        return;
    }

    // Set up a clipPath group (if required).
    if (state->getRender() & 4 && !_clip_text_group) {
        auto defs = _doc->getDefs()->getRepr();
        _clip_text_group = _pushContainer("svg:clipPath");
        _clip_text_group->setAttribute("clipPathUnits", "userSpaceOnUse");
        defs->appendChild(_clip_text_group);
        Inkscape::GC::release(_clip_text_group);
    }

    // Ignore invisible characters.
    if (state->getRender() == 3) {
        std::cerr << "SVGBuilder::_flushText: Invisible pdf glyphs removed!" << std::endl;
        _glyphs.clear();
        return;
    }

    // Strip out text size from text_matrix and remove from text_transform
    double text_scale = _text_matrix.expansionX();
    Geom::Affine tr = stateToAffine(state);
    Geom::Affine text_transform = _text_matrix * tr * Geom::Scale(text_scale).inverse();
    std::vector<SvgGlyph>::iterator i = _glyphs.begin();
    const SvgGlyph& first_glyph = (*i);

    // The glyph position must be moved by the document scale without flipping
    // the text object itself. This is why the text affine is applied to the
    // translation point and not simply used in the text element directly.
    auto pos = first_glyph.position * tr;
    text_transform.setTranslation(pos);

    // Cache the text transform when clipping
    if (_clip_text_group) {
        svgSetTransform(_clip_text_group, text_transform);
    }

    Inkscape::XML::Node *text_node = nullptr; // The text node or the path node.
    if (first_glyph.cairo_font) {
        text_node = _flushTextPath(state, text_scale, text_transform);
    } else {
        text_node = _flushTextText(state, text_scale, text_transform);
    }

    if (text_node) {
        _setBlendMode(text_node, state);
        svgSetTransform(text_node, text_transform * _page_affine);
        _setClipPath(text_node);
    }

    _aria_label = "";
    _glyphs.clear();
}

/**
 * Sets the style for the text, rendered or un-rendered, preserving the text_transform for any
 * gradients or other patterns. These values were promised to us when the font was updated.
 */
void SvgBuilder::_setTextStyle(Inkscape::XML::Node *node, GfxState *state, SPCSSAttr *font_style, Geom::Affine ta)
{
    int render_mode = state->getRender();
    bool has_fill = !(render_mode & 1);
    bool has_stroke = ( render_mode & 3 ) == 1 || ( render_mode & 3 ) == 2;

    state = state->save();
    state->setCTM(ta[0], ta[1], ta[2], ta[3], ta[4], ta[5]);
    auto style = _setStyle(state, has_fill, has_stroke);
    sp_repr_css_change(node, style, "style");
    state = state->restore();
    if (font_style) {
        sp_repr_css_merge(style, font_style);
    }
    sp_repr_css_change(node, style, "style");
    sp_repr_css_attr_unref(style);
}

/**
 * Renders the text as a path object using cairo and returns the node object.
 *
 * If the path is empty (e.g. due to trying to render a color bitmap font),
 * return path node with empty "d" attribute. The aria attribute will still
 * contain the original text.
 *
 * cairo_font   - The font that cairo can use to convert text to path.
 * font_size    - The size of the text when drawing the path.
 * transform    - The matrix which will place the text on the page, this is critical
 *                to allow cairo to render all the required parts of the text.
 * cairo_glyphs - A pointer to a list of glyphs to render.
 * count        - A count of the number of glyphs to render.
 */
Inkscape::XML::Node *SvgBuilder::_renderText(std::shared_ptr<CairoFont> cairo_font, double font_size,
                                             const Geom::Affine &transform,
                                             cairo_glyph_t *cairo_glyphs, unsigned int count)
{
    Inkscape::XML::Node *path = _addToContainer("svg:path");
    path->setAttribute("d", "");

    if (!cairo_glyphs || !cairo_font || _aria_label.empty()) {
        std::cerr << "SvgBuilder::_renderText: Invalid argument!" << std::endl;
        return path;
    }

    // The surface isn't actually used, no rendering in cairo takes place.
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, _width, _height);
    cairo_t *cairo = cairo_create(surface);
    cairo_set_font_face(cairo, cairo_font->getFontFace());
    cairo_set_font_size(cairo, font_size);
    ink_cairo_transform(cairo, transform);
    cairo_glyph_path(cairo, cairo_glyphs, count);
    auto pathv = extract_pathvector_from_cairo(cairo);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);

    // Failing to render text.
    if (!pathv) {
        std::cerr << "SvgBuilder::_renderText: Failed to render PDF text! " << _aria_label << std::endl;
        return path;
    }

    auto textpath = sp_svg_write_path(*pathv);
    path->setAttribute("d", textpath);

    if (textpath.empty()) {
        std::cerr << "SvgBuilder::_renderText: Empty path! " << _aria_label << std::endl;
    }

    return path;
}

/**
 * Begin and end string is the inner most text processing step
 * which tells us we're about to have a certain number of chars.
 */
void SvgBuilder::beginString(GfxState *state, int len)
{
    if (!_glyphs.empty()) {
        // What to do about unflushed text in the buffer.
        if (_invalidated_strategy) {
            _flushText(state);
            _invalidated_strategy = false;
        } else {
            // Add seperator for aria text.
            _aria_space = true;
        }
    }
    IFTRACE(double *m = state->getTextMat());
    TRACE(("tm: %f %f %f %f %f %f\n",m[0], m[1],m[2], m[3], m[4], m[5]));
    IFTRACE(m = state->getCTM());
    TRACE(("ctm: %f %f %f %f %f %f\n",m[0], m[1],m[2], m[3], m[4], m[5]));
}
void SvgBuilder::endString(GfxState *state)
{
}

/**
 * \brief Adds the specified character to the text buffer
 * Takes care of converting it to UTF-8 and generates a new style repr if style
 * has changed since the last call.
 *
 * x, y:    Position of glyph.
 * dx, dy:  Advance of glyph.
 * originX, originY
 * code: 8-bit char code, 16 bit CID, or Unicode of glyph.
 * u: Unicode mapping of character. "Unicode" is an unsigned int.
 */
void SvgBuilder::addChar(GfxState *state,
                         double x, double y,
                         double dx, double dy,
                         double ax, double ay,
                         double originX, double originY,
                         CharCode code, int nBytes, Unicode const *u, int uLen)
{
    assert (state);

    if (_aria_space && !_glyphs.empty()) {
        const SvgGlyph& prev_glyph = _glyphs.back();
        // This helps reconstruct the aria text, though it could be made better.
        if (prev_glyph.position[Geom::Y] != (y - originY)) {
            _aria_label += "\n";
        }
    }
    _aria_space = false;

    std::string utf8_code;
    if (u) {
        // 'u' maybe null if there is not a "ToUnicode" table in the PDF!
        utf8_code = unicode_char_to_utf8(*u);
        _aria_label += utf8_code;
    }

    // Skip control characters, found in LaTeX generated PDFs
    // https://gitlab.com/inkscape/inkscape/-/issues/1369
    if (uLen > 0 && u[0] < 0x80 && g_ascii_iscntrl(u[0]) && !g_ascii_isspace(u[0])) {
        g_warning("Skipping ASCII control character %u", u[0]);
        _text_position += Geom::Point(dx, dy);
        return;
    }

    if (!_css_font && !_cairo_font) {
        // Deleted text.
        return;
    }

    Geom::Point delta(dx, dy);
    Geom::Point advance(ax, ay);

    bool is_space = ( uLen == 1 && u[0] == 32 );

    SvgGlyph new_glyph;
    new_glyph.code = utf8_code;
    new_glyph.is_space = is_space;
    new_glyph.delta = delta;
    new_glyph.advance = advance;
    new_glyph.position = Geom::Point( x - originX, y - originY );
    new_glyph.origin = Geom::Point(originX, -originY);
    new_glyph.text_position = _text_position;
    new_glyph.text_size = _css_font_size;
    new_glyph.state = state;
    if (_cairo_font) {
        // We are rendering text as a path.
        new_glyph.cairo_font = _cairo_font;
        new_glyph.cairo_index = _cairo_font->getGlyph(code, u, uLen);
    }
    _text_position += delta;

    // Copy current style if it has changed since the previous glyph
    if (_invalidated_style || _glyphs.empty()) {
        _invalidated_style = false;
        new_glyph.style_changed = true;
        if (_css_font) {
            new_glyph.css_font = sp_repr_css_attr_new();
            sp_repr_css_merge(new_glyph.css_font, _css_font);
        }
    } else {
        new_glyph.style_changed = false;
        // Point to previous glyph's style information
        const SvgGlyph& prev_glyph = _glyphs.back();
        new_glyph.css_font = prev_glyph.css_font;
    }
    new_glyph.font_specification = _font_specification;
    new_glyph.rise = state->getRise();
    new_glyph.char_space = state->getCharSpace();
    new_glyph.word_space = state->getWordSpace();
    new_glyph.horiz_scaling = state->getHorizScaling() / 100.0;
    _glyphs.push_back(new_glyph);

    IFTRACE(
    std::cout << "SVGBuilder::addChar:  " << new_glyph.code
              << "  style changed: " << std::boolalpha << new_glyph.style_changed
              << std::setprecision(4)
              << "  position: " << new_glyph.position
              << "  delta: "    << new_glyph.delta
              << "  x,y: (" << x << ", " << y << ") "
              << "  origin: ("   << originX << ", " << originY << ") "
              << "  rise: "          << new_glyph.rise
              << "  text_position: " << new_glyph.text_position
              << "  state: " << (void*)new_glyph.state
              << std::endl;
        );

    // ligatures without unicode mapping (e.g. "ti" in Calibri) may be represented in PDF as multiple characters.
    // Cairo handles this when drawing glyphs, but we need to append the characters individually otherwise.
    if (uLen > 1 && !_cairo_font) {
        addChar(state, x, y, dx, dy, ax, ay, originX, originY, code, nBytes, u + 1, uLen - 1);
    }
}

/**
 * These text object functions are the outer most calls for begining and
 * ending text. No text functions should be called outside of these two calls
 */
void SvgBuilder::beginTextObject(GfxState *state) {
    _in_text_object = true;
    _invalidated_style = true;  // Force copying of current state
}

void SvgBuilder::endTextObject(GfxState *state)
{
    _in_text_object = false;
    _flushText(state);

    if (_clip_text_group) {
        // Use the clip as a real clip path
        _clip_text = _popContainer();
        _clip_text_group = nullptr;
    }
}

/**
 * Helper functions for supporting direct PNG output into a base64 encoded stream
 */
void png_write_vector(png_structp png_ptr, png_bytep data, png_size_t length)
{
    auto *v_ptr = reinterpret_cast<std::vector<guchar> *>(png_get_io_ptr(png_ptr)); // Get pointer to stream
    for ( unsigned i = 0 ; i < length ; i++ ) {
        v_ptr->push_back(data[i]);
    }
}

/**
 * \brief Creates an <image> element containing the given ImageStream as a PNG
 *
 */
Inkscape::XML::Node *SvgBuilder::_createImage(Stream *str, int width, int height,
                                              GfxImageColorMap *color_map, bool interpolate,
                                              int *mask_colors, bool alpha_only,
                                              bool invert_alpha) {

    // Create PNG write struct
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if ( png_ptr == nullptr ) {
        return nullptr;
    }
    // Create PNG info struct
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if ( info_ptr == nullptr ) {
        png_destroy_write_struct(&png_ptr, nullptr);
        return nullptr;
    }
    // Set error handler
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return nullptr;
    }

    // Set read/write functions
    std::vector<guchar> png_buffer;
    FILE *fp = nullptr;
    gchar *file_name = nullptr;
    if (_embed_images) {
        png_set_write_fn(png_ptr, &png_buffer, png_write_vector, nullptr);
    } else {
        static int counter = 0;
        file_name = g_strdup_printf("%s_img%d.png", _docname, counter++);
        fp = fopen(file_name, "wb");
        if ( fp == nullptr ) {
            png_destroy_write_struct(&png_ptr, &info_ptr);
            g_free(file_name);
            return nullptr;
        }
        png_init_io(png_ptr, fp);
    }

    // Set header data
    if ( !invert_alpha && !alpha_only ) {
        png_set_invert_alpha(png_ptr);
    }
    png_color_8 sig_bit;
    if (alpha_only) {
        png_set_IHDR(png_ptr, info_ptr,
                     width,
                     height,
                     8, /* bit_depth */
                     PNG_COLOR_TYPE_GRAY,
                     PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_BASE,
                     PNG_FILTER_TYPE_BASE);
        sig_bit.red = 0;
        sig_bit.green = 0;
        sig_bit.blue = 0;
        sig_bit.gray = 8;
        sig_bit.alpha = 0;
    } else {
        png_set_IHDR(png_ptr, info_ptr,
                     width,
                     height,
                     8, /* bit_depth */
                     PNG_COLOR_TYPE_RGB_ALPHA,
                     PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_BASE,
                     PNG_FILTER_TYPE_BASE);
        sig_bit.red = 8;
        sig_bit.green = 8;
        sig_bit.blue = 8;
        sig_bit.alpha = 8;
    }
    png_set_sBIT(png_ptr, info_ptr, &sig_bit);
    png_set_bgr(png_ptr);
    // Write the file header
    png_write_info(png_ptr, info_ptr);

    // Convert pixels
    ImageStream *image_stream;
    if (alpha_only) {
        if (color_map) {
            image_stream = new ImageStream(str, width, color_map->getNumPixelComps(),
                                           color_map->getBits());
        } else {
            image_stream = new ImageStream(str, width, 1, 1);
        }
#if POPPLER_CHECK_VERSION(26, 0, 0)
        if(!image_stream->rewind())
        {
            g_warning("ImageStream: Failed to rewind image stream");
            png_destroy_write_struct(&png_ptr, &info_ptr);
            if (!_embed_images) {
                fclose(fp);
                g_free(file_name);
            }
            delete image_stream;
            return nullptr;
        }
#else
        image_stream->reset();
#endif

        // Convert grayscale values
        unsigned char *buffer = new unsigned char[width];
        int invert_bit = invert_alpha ? 1 : 0;
        for ( int y = 0 ; y < height ; y++ ) {
            unsigned char *row = image_stream->getLine();
            if (color_map) {
                color_map->getGrayLine(row, buffer, width);
            } else {
                unsigned char *buf_ptr = buffer;
                for ( int x = 0 ; x < width ; x++ ) {
                    if ( row[x] ^ invert_bit ) {
                        *buf_ptr++ = 0;
                    } else {
                        *buf_ptr++ = 255;
                    }
                }
            }
            png_write_row(png_ptr, (png_bytep)buffer);
        }
        delete [] buffer;
    } else if (color_map) {
        image_stream = new ImageStream(str, width,
                                       color_map->getNumPixelComps(),
                                       color_map->getBits());
#if POPPLER_CHECK_VERSION(26, 0, 0)
        if(!image_stream->rewind())
        {
            g_warning("ImageStream: Failed to rewind image stream");
            png_destroy_write_struct(&png_ptr, &info_ptr);
            if (!_embed_images) {
                fclose(fp);
                g_free(file_name);
            }
            delete image_stream;
            return nullptr;
        }
#else
        image_stream->reset();
#endif

        // Convert RGB values
        unsigned int *buffer = new unsigned int[width];
        if (mask_colors) {
            for ( int y = 0 ; y < height ; y++ ) {
                unsigned char *row = image_stream->getLine();
                color_map->getRGBLine(row, buffer, width);

                unsigned int *dest = buffer;
                for ( int x = 0 ; x < width ; x++ ) {
                    // Check each color component against the mask
                    for ( int i = 0; i < color_map->getNumPixelComps() ; i++) {
                        if ( row[i] < mask_colors[2*i] * 255 ||
                             row[i] > mask_colors[2*i + 1] * 255 ) {
                            *dest = *dest | 0xff000000;
                            break;
                        }
                    }
                    // Advance to the next pixel
                    row += color_map->getNumPixelComps();
                    dest++;
                }
                // Write it to the PNG
                png_write_row(png_ptr, (png_bytep)buffer);
            }
        } else {
            for ( int i = 0 ; i < height ; i++ ) {
                unsigned char *row = image_stream->getLine();
                memset((void*)buffer, 0xff, sizeof(int) * width);
                color_map->getRGBLine(row, buffer, width);
                png_write_row(png_ptr, (png_bytep)buffer);
            }
        }
        delete [] buffer;

    } else {    // A colormap must be provided, so quit
        png_destroy_write_struct(&png_ptr, &info_ptr);
        if (!_embed_images) {
            fclose(fp);
            g_free(file_name);
        }
        return nullptr;
    }
    delete image_stream;
    str->close();
    // Close PNG
    png_write_end(png_ptr, info_ptr);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    // Create repr
    Inkscape::XML::Node *image_node = _xml_doc->createElement("svg:image");
    image_node->setAttributeSvgDouble("width", 1);
    image_node->setAttributeSvgDouble("height", 1);
    if( !interpolate ) {
        SPCSSAttr *css = sp_repr_css_attr_new();
        // This should be changed after CSS4 Images widely supported.
        sp_repr_css_set_property(css, "image-rendering", "optimizeSpeed");
        sp_repr_css_change(image_node, css, "style");
        sp_repr_css_attr_unref(css);
    }

    // PS/PDF images are placed via a transformation matrix, no preserveAspectRatio used
    image_node->setAttribute("preserveAspectRatio", "none");

    // Create href
    if (_embed_images) {
        // Append format specification to the URI
        auto *base64String = g_base64_encode(png_buffer.data(), png_buffer.size());
        auto png_data = std::string("data:image/png;base64,") + base64String;
        g_free(base64String);
        image_node->setAttributeOrRemoveIfEmpty("xlink:href", png_data);
    } else {
        fclose(fp);
        image_node->setAttribute("xlink:href", file_name);
        g_free(file_name);
    }

    return image_node;
}

/**
 * \brief Creates a <mask> with the specified width and height and adds to <defs>
 *  If we're not the top-level SvgBuilder, creates a <defs> too and adds the mask to it.
 * \return the created XML node
 */
Inkscape::XML::Node *SvgBuilder::_createMask(double width, double height) {
    Inkscape::XML::Node *mask_node = _xml_doc->createElement("svg:mask");
    mask_node->setAttribute("maskUnits", "userSpaceOnUse");
    mask_node->setAttributeSvgDouble("x", 0.0);
    mask_node->setAttributeSvgDouble("y", 0.0);
    mask_node->setAttributeSvgDouble("width", width);
    mask_node->setAttributeSvgDouble("height", height);
    // Append mask to defs
    if (_is_top_level) {
        _doc->getDefs()->getRepr()->appendChild(mask_node);
        Inkscape::GC::release(mask_node);
        return _doc->getDefs()->getRepr()->lastChild();
    } else {    // Work around for renderer bug when mask isn't defined in pattern
        static int mask_count = 0;
        gchar *mask_id = g_strdup_printf("_mask%d", mask_count++);
        mask_node->setAttribute("id", mask_id);
        g_free(mask_id);
        _doc->getDefs()->getRepr()->appendChild(mask_node);
        Inkscape::GC::release(mask_node);
        return mask_node;
    }
}

void SvgBuilder::addImage(GfxState *state, Stream *str, int width, int height, GfxImageColorMap *color_map,
                          bool interpolate, int *mask_colors)
{
    Inkscape::XML::Node *image_node = _createImage(str, width, height, color_map, interpolate, mask_colors);
    if (image_node) {
        _setBlendMode(image_node, state);
        _setTransform(image_node, state, Geom::Affine(1.0, 0.0, 0.0, -1.0, 0.0, 1.0));
        _addToContainer(image_node);
        _setClipPath(image_node);
    }
}

void SvgBuilder::addImageMask(GfxState *state, Stream *str, int width, int height,
                              bool invert, bool interpolate) {

    // Create a rectangle
    Inkscape::XML::Node *rect = _addToContainer("svg:rect");
    rect->setAttributeSvgDouble("x", 0.0);
    rect->setAttributeSvgDouble("y", 0.0);
    rect->setAttributeSvgDouble("width", 1.0);
    rect->setAttributeSvgDouble("height", 1.0);

    // Get current fill style and set it on the rectangle
    SPCSSAttr *css = sp_repr_css_attr_new();
    _setFillStyle(css, state, false);
    sp_repr_css_change(rect, css, "style");
    sp_repr_css_attr_unref(css);
    _setBlendMode(rect, state);
    _setTransform(rect, state, Geom::Affine(1.0, 0.0, 0.0, -1.0, 0.0, 1.0));
    _setClipPath(rect);

    // Scaling 1x1 surfaces might not work so skip setting a mask with this size
    if ( width > 1 || height > 1 ) {
        Inkscape::XML::Node *mask_image_node =
            _createImage(str, width, height, nullptr, interpolate, nullptr, true, invert);
        if (mask_image_node) {
            // Create the mask
            Inkscape::XML::Node *mask_node = _createMask(1.0, 1.0);
            // Remove unnecessary transformation from the mask image
            mask_image_node->removeAttribute("transform");
            mask_node->appendChild(mask_image_node);
            Inkscape::GC::release(mask_image_node);
            gchar *mask_url = g_strdup_printf("url(#%s)", mask_node->attribute("id"));
            rect->setAttribute("mask", mask_url);
            g_free(mask_url);
        }
    }
}

void SvgBuilder::addMaskedImage(GfxState *state, Stream *str, int width, int height, GfxImageColorMap *color_map,
                                bool interpolate, Stream *mask_str, int mask_width, int mask_height, bool invert_mask,
                                bool mask_interpolate)
{
    Inkscape::XML::Node *mask_image_node = _createImage(mask_str, mask_width, mask_height,
                                          nullptr, mask_interpolate, nullptr, true, invert_mask);
    Inkscape::XML::Node *image_node = _createImage(str, width, height, color_map, interpolate, nullptr);
    if ( mask_image_node && image_node ) {
        // Create mask for the image
        Inkscape::XML::Node *mask_node = _createMask(1.0, 1.0);
        // Remove unnecessary transformation from the mask image
        mask_image_node->removeAttribute("transform");
        mask_node->appendChild(mask_image_node);
        // Scale the mask to the size of the image
        Geom::Affine mask_transform((double)width, 0.0, 0.0, (double)height, 0.0, 0.0);
        mask_node->setAttributeOrRemoveIfEmpty("maskTransform", sp_svg_transform_write(mask_transform));
        // Set mask and add image
        gchar *mask_url = g_strdup_printf("url(#%s)", mask_node->attribute("id"));
        image_node->setAttribute("mask", mask_url);
        g_free(mask_url);
        _setBlendMode(image_node, state);
        _setTransform(image_node, state, Geom::Affine(1.0, 0.0, 0.0, -1.0, 0.0, 1.0));
        _addToContainer(image_node);
        _setClipPath(image_node);
    } else if (image_node) {
        Inkscape::GC::release(image_node);
    }
    if (mask_image_node) {
        Inkscape::GC::release(mask_image_node);
    }
}

void SvgBuilder::addSoftMaskedImage(GfxState *state, Stream *str, int width, int height, GfxImageColorMap *color_map,
                                    bool interpolate, Stream *mask_str, int mask_width, int mask_height,
                                    GfxImageColorMap *mask_color_map, bool mask_interpolate)
{
    Inkscape::XML::Node *mask_image_node = _createImage(mask_str, mask_width, mask_height,
                                                        mask_color_map, mask_interpolate, nullptr, true);
    Inkscape::XML::Node *image_node = _createImage(str, width, height, color_map, interpolate, nullptr);
    if ( mask_image_node && image_node ) {
        // Create mask for the image
        Inkscape::XML::Node *mask_node = _createMask(1.0, 1.0);
        // Remove unnecessary transformation from the mask image
        mask_image_node->removeAttribute("transform");
        mask_node->appendChild(mask_image_node);
        // Set mask and add image
        gchar *mask_url = g_strdup_printf("url(#%s)", mask_node->attribute("id"));
        image_node->setAttribute("mask", mask_url);
        g_free(mask_url);
        _addToContainer(image_node);
        _setBlendMode(image_node, state);
        _setTransform(image_node, state, Geom::Affine(1.0, 0.0, 0.0, -1.0, 0.0, 1.0));
        _setClipPath(image_node);
    } else if (image_node) {
        Inkscape::GC::release(image_node);
    }
    if (mask_image_node) {
        Inkscape::GC::release(mask_image_node);
    }
}

/**
 * Find the fill or stroke gradient we previously set on this node.
 */
Inkscape::XML::Node *SvgBuilder::_getGradientNode(Inkscape::XML::Node *node, bool is_fill)
{
    auto css = sp_repr_css_attr(node, "style");
    if (auto id = try_extract_uri_id(css->attribute(is_fill ? "fill" : "stroke"))) {
        if (auto obj = _doc->getObjectById(*id)) {
            return obj->getRepr();
        }
    }
    return nullptr;
}

bool SvgBuilder::_attrEqual(Inkscape::XML::Node *a, Inkscape::XML::Node *b, char const *attr)
{
    return (!a->attribute(attr) && !b->attribute(attr)) || std::string(a->attribute(attr)) == b->attribute(attr);
}

/**
 * Take a constructed mask and decide how to apply it to the target.
 */
void SvgBuilder::applyOptionalMask(Inkscape::XML::Node *mask, Inkscape::XML::Node *target)
{
    // Merge transparency gradient back into real gradient if possible
    if (mask->childCount() == 1) {
        auto source = mask->firstChild();
        auto source_gr = _getGradientNode(source, true);
        auto target_gr = _getGradientNode(target, true);
        // Both objects have a gradient, try and merge them
        if (source_gr && target_gr && source_gr->childCount() == target_gr->childCount()) {
            bool same_pos = _attrEqual(source_gr, target_gr, "x1") && _attrEqual(source_gr, target_gr, "x2")
                         && _attrEqual(source_gr, target_gr, "y1") && _attrEqual(source_gr, target_gr, "y2");

            bool white_mask = false;
            for (auto source_st = source_gr->firstChild(); source_st != nullptr; source_st = source_st->next()) {
                auto source_css = sp_repr_css_attr(source_st, "style");
                white_mask = white_mask or source_css->getAttributeDouble("stop-opacity") != 1.0;
                if (std::string(source_css->attribute("stop-color")) != "#ffffff") {
                    white_mask = false;
                    break;
                }
            }

            if (same_pos && white_mask) {
                // We move the stop-opacity from the source to the target
                auto target_st = target_gr->firstChild();
                for (auto source_st = source_gr->firstChild(); source_st != nullptr; source_st = source_st->next()) {
                    auto target_css = sp_repr_css_attr(target_st, "style");
                    auto source_css = sp_repr_css_attr(source_st, "style");
                    sp_repr_css_set_property(target_css, "stop-opacity", source_css->attribute("stop-opacity"));
                    sp_repr_css_change(target_st, target_css, "style");
                    target_st = target_st->next();
                }
                // Remove mask and gradient xml objects
                mask->parent()->removeChild(mask);
                source_gr->parent()->removeChild(source_gr);
                return;
            }
        }
    }
    gchar *mask_url = g_strdup_printf("url(#%s)", mask->attribute("id"));
    target->setAttribute("mask", mask_url);
    g_free(mask_url);
}


/**
 * \brief Starts building a new transparency group
 */
void SvgBuilder::startGroup(GfxState *state, double *bbox, GfxColorSpace * /*blending_color_space*/, bool isolated,
                           bool knockout, bool for_softmask)
{
    if (_group_by == GroupBy::BY_XOBJECT || for_softmask) {
        // Push group node, but don't attach to previous container yet
        _pushContainer("svg:g");
    }

    if (for_softmask) {
        _mask_groups.push_back(state);
        // Create a container for the mask
        _pushContainer(_createMask(1.0, 1.0));
    }

    // TODO: In the future we could use state to insert transforms
    // and then remove the inverse from the items added into the children
    // to reduce the transformational duplication.
}

void SvgBuilder::finishGroup(GfxState *state, bool for_softmask)
{

    if (for_softmask) {
        // Create mask
        auto mask_node = _popContainer();
        applyOptionalMask(mask_node, _container);
    } else if (_group_by == GroupBy::BY_XOBJECT) {
        popGroup(state);
    } else {
        while (!_alpha_objs.empty()) {
            auto node = _alpha_objs.back();
            auto orig = node->getAttributeDouble("opacity", 1.0);
            node->setAttributeSvgDouble("opacity", orig * _group_alpha);
            _alpha_objs.pop_back();
        }
        _group_alpha = 1.0;
    }
}

void SvgBuilder::popGroup(GfxState *state)
{
    // Restore node stack
    auto parent = _popContainer();

    if (parent->childCount() == 1) {
        // Merge this opacity and remove unnecessary group
        auto child = parent->firstChild();

        // Do not merge masked children with masked parents
        // Clipping paths will be compounded in _checkClip
        if (!(child->attribute("mask") && parent->attribute("mask"))) {
            auto orig = child->getAttributeDouble("opacity", 1.0);
            auto grp = parent->getAttributeDouble("opacity", 1.0);
            child->setAttributeSvgDouble("opacity", orig * grp);

            // compound the transforms
            Geom::Affine grp_tr;
            Geom::Affine child_tr;
            sp_svg_transform_read(child->attribute("transform"), &child_tr);
            if (sp_svg_transform_read(parent->attribute("transform"), &grp_tr)) {
                child_tr *= grp_tr;
                child->setAttribute("transform", sp_svg_transform_write(child_tr));
            }

            // if the parent has a mask, apply it to the child
            if (auto mask_id = try_extract_uri_id(parent->attribute("mask"))) {
                if (auto obj = _doc->getObjectById(*mask_id)) {
                    auto mask_node = obj->getRepr();
                    applyOptionalMask(obj->getRepr(), child);
                    // if the child has a transform, undo it on the mask children
                    if (child_tr != Geom::identity()) {
                        for (auto m_child = mask_node->firstChild(); m_child != nullptr; m_child = m_child->next()) {
                            Geom::Affine mask_tr;
                            sp_svg_transform_read(m_child->attribute("transform"), &mask_tr);
                            mask_tr *= child_tr.inverse();
                            m_child->setAttribute("transform", sp_svg_transform_write(mask_tr));
                        }
                    }
                }
            }
            // this really shouldn't happen, as we haven't set the clip path on the parent yet
            if (auto clip = parent->attribute("clip-path")) {
                if (child->attribute("clip-path")) {
                    g_warning("Discarding group clipping path");
                } else {
                    child->setAttribute("clip-path", clip);
                }
            }

            // This duplicate child will get applied in the place of the group
            parent->removeChild(child);
            Inkscape::GC::anchor(child);
            parent = child;
        }
    }

    // Add the parent to the last container
    _addToContainer(parent);
    _setClipPath(parent);
}

/**
 * Decide what to do for each font in the font list, with the given strategy.
 */
FontStrategies SvgBuilder::autoFontStrategies(FontStrategy s, FontList fonts)
{
    FontStrategies ret;
    for (auto font : *fonts.get()) {
        int id = font.first->getID()->num;
        bool found = font.second.found;
        switch (s) {
            case FontStrategy::RENDER_ALL:
                ret[id] = FontFallback::AS_SHAPES;
                break;
            case FontStrategy::DELETE_ALL:
                ret[id] = FontFallback::DELETE_TEXT;
                break;
            case FontStrategy::RENDER_MISSING:
                ret[id] = found ? FontFallback::AS_TEXT : FontFallback::AS_SHAPES;
                break;
            case FontStrategy::SUBSTITUTE_MISSING:
                ret[id] = found ? FontFallback::AS_TEXT : FontFallback::AS_SUB;
                break;
            case FontStrategy::KEEP_MISSING:
                ret[id] = FontFallback::AS_TEXT;
                break;
            case FontStrategy::DELETE_MISSING:
                ret[id] = found ? FontFallback::AS_TEXT : FontFallback::DELETE_TEXT;
                break;
        }
    }
    return ret;
}
} } } /* namespace Inkscape, Extension, Internal */

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
