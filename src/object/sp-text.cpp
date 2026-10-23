// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SVG <text> and <tspan> implementation
 *
 * Author:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

/*
 * fixme:
 *
 * These subcomponents should not be items, or alternately
 * we have to invent set of flags to mark, whether standard
 * attributes are applicable to given item (I even like this
 * idea somewhat - Lauris)
 *
 */

#include "sp-text.h"
#include "libnrtype/tab-stops.h"

#include <glibmm/i18n.h>

#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "libnrtype/Layout-TNG-Scanline-Maker.h"

#include "desktop-style.h"
#include "document.h"
#include "layer-manager.h"
#include "mod360.h"
#include "preferences.h"                             // for Preferences
#include "snap-candidate.h"                          // for SnapCandidatePoint
#include "snap-preferences.h"                        // for SnapPreferences
#include "text-editing.h"
#include "style-text.h"

#include "sp-desc.h"
#include "sp-rect.h"
#include "sp-textpath.h"
#include "sp-title.h"
#include "sp-tref.h"
#include "sp-tspan.h"
#include "sp-defs.h"

#include "display/drawing-text.h"
#include "path/path-boolop.h"
#include "svg/svg.h"
#include "util/units.h"
#include "util/numeric/converters.h"
#include "xml/href-attribute-helper.h"
#include "xml/quote.h"

// For SVG 2 text flow
#include <glibmm/regex.h>

#include "livarot/Path.h"

/*#####################################################
#  SPTEXT
#####################################################*/
SPText::SPText() : SPItem() {
}

SPText::~SPText()
{
    if (css) {
        sp_repr_css_attr_unref(css);
    }
};

void SPText::build(SPDocument *doc, Inkscape::XML::Node *repr) {
    this->readAttr(SPAttr::X);
    this->readAttr(SPAttr::Y);
    this->readAttr(SPAttr::DX);
    this->readAttr(SPAttr::DY);
    this->readAttr(SPAttr::ROTATE);

    // textLength and friends
    this->readAttr(SPAttr::TEXTLENGTH);
    this->readAttr(SPAttr::LENGTHADJUST);
    SPItem::build(doc, repr);
    css = nullptr;
    this->readAttr(SPAttr::SODIPODI_LINESPACING);    // has to happen after the styles are read
}

void SPText::release()
{
    view_style_attachments.clear();
    SPItem::release();
}

void SPText::set(SPAttr key, const gchar* value) {
    //std::cout << "SPText::set: " << sp_attribute_name( key ) << ": " << (value?value:"Null") << std::endl;

    if (this->attributes.readSingleAttribute(key, value, style, &viewport)) {
        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    } else {
        switch (key) {
            case SPAttr::SODIPODI_LINESPACING:
                // convert deprecated tag to css... but only if 'line-height' missing.
                if (value && !this->style->line_height.set) {
                    this->style->line_height.set = TRUE;
                    this->style->line_height.inherit = FALSE;
                    this->style->line_height.normal = FALSE;
                    this->style->line_height.unit = SP_CSS_UNIT_PERCENT;
                    this->style->line_height.value = this->style->line_height.computed = sp_svg_read_percentage (value, 1.0);
                }
                // Remove deprecated attribute
                this->removeAttribute("sodipodi:linespacing");

                this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_TEXT_LAYOUT_MODIFIED_FLAG);
                break;

            default:
                SPItem::set(key, value);
                break;
        }
    }
}

void SPText::child_added(Inkscape::XML::Node *rch, Inkscape::XML::Node *ref) {
    SPItem::child_added(rch, ref);

    this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_TEXT_CONTENT_MODIFIED_FLAG | SP_TEXT_LAYOUT_MODIFIED_FLAG);
}

void SPText::remove_child(Inkscape::XML::Node *rch) {
    SPItem::remove_child(rch);

    this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_TEXT_CONTENT_MODIFIED_FLAG | SP_TEXT_LAYOUT_MODIFIED_FLAG);
}


void SPText::update(SPCtx *ctx, guint flags) {

    unsigned childflags = (flags & SP_OBJECT_MODIFIED_CASCADE);
    if (flags & SP_OBJECT_MODIFIED_FLAG) {
        childflags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }

    // Create temporary list of children
    std::vector<SPObject *> l;
    for (auto& child: children) {
        sp_object_ref(&child, this);
        l.push_back(&child);
    }

    for (auto child:l) {
        if (childflags || (child->uflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
            /* fixme: Do we need transform? */
            child->updateDisplay(ctx, childflags);
        }
        sp_object_unref(child, this);
    }

    // update ourselves after updating children
    SPItem::update(ctx, flags);

    if (flags & ( SP_OBJECT_STYLE_MODIFIED_FLAG |
                  SP_OBJECT_CHILD_MODIFIED_FLAG |
                  SP_TEXT_LAYOUT_MODIFIED_FLAG   ) )
    {

        SPItemCtx const *ictx = reinterpret_cast<SPItemCtx const *>(ctx);

        double const w = ictx->viewport.width();
        double const h = ictx->viewport.height();
        double const em = style->font_size.computed;
        double const ex = 0.5 * em;  // fixme: get x height from pango or libnrtype.

        attributes.update( em, ex, w, h );

        // Set inline_size computed value if necessary (i.e. if unit is %).
        if (has_inline_size()) {
            if (style->inline_size.unit == SP_CSS_UNIT_PERCENT) {
                if (is_horizontal()) {
                    style->inline_size.computed = style->inline_size.value * ictx->viewport.width();
                } else {
                    style->inline_size.computed = style->inline_size.value * ictx->viewport.height();
                }
            }
        }

        /* fixme: It is not nice to have it here, but otherwise children content changes does not work */
        /* fixme: Even now it may not work, as we are delayed */
        /* fixme: So check modification flag everywhere immediate state is used */
        this->rebuildLayout();

        Geom::OptRect paintbox = this->geometricBounds();

        for (auto &v : views) {
            auto &sa = view_style_attachments[v.key];
            sa.unattachAll();
            auto g = cast<Inkscape::DrawingGroup>(v.drawingitem.get());
            _clearFlow(g);
            g->setStyle(style, parent->style);
            // pass the bbox of this as paintbox (used for paintserver fills)
            layout.show(g, sa, paintbox);
        }
    }
}

void SPText::modified(guint flags) {
//	SPItem::onModified(flags);

    guint cflags = (flags & SP_OBJECT_MODIFIED_CASCADE);

    if (flags & SP_OBJECT_MODIFIED_FLAG) {
        cflags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }

    // FIXME: all that we need to do here is to call setStyle, to set the changed
    // style, but there's no easy way to access the drawing glyphs or texts corresponding to a
    // text this. Therefore we do here the same as in _update, that is, destroy all items
    // and create new ones. This is probably quite wasteful.
    if (flags & ( SP_OBJECT_STYLE_MODIFIED_FLAG )) {
        Geom::OptRect paintbox = geometricBounds();

        for (auto &v : views) {
            auto &sa = view_style_attachments[v.key];
            sa.unattachAll();
            auto g = cast<Inkscape::DrawingGroup>(v.drawingitem.get());
            _clearFlow(g);
            g->setStyle(style, parent->style);
            layout.show(g, sa, paintbox);
        }
    }

    // Create temporary list of children
    std::vector<SPObject *> l;
    for (auto& child: children) {
        sp_object_ref(&child, this);
        l.push_back(&child);
    }

    for (auto child:l) {
        if (cflags || (child->mflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
            child->emitModified(cflags);
        }
        sp_object_unref(child, this);
    }
}

Inkscape::XML::Node *SPText::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    if (flags & SP_OBJECT_WRITE_BUILD) {
        if (!repr) {
            repr = xml_doc->createElement("svg:text");
	    // we preserve spaces in the text objects we create
            repr->setAttribute("xml:space", "preserve");
        }

        Inkscape::Text::LeftTabStops::copy_attributes(repr, getRepr());
        if (repr != getRepr()) {
            repr->setAttribute(Inkscape::Text::LINE_ALIGNMENT_ATTRIBUTE, getAttribute(Inkscape::Text::LINE_ALIGNMENT_ATTRIBUTE));
        }
        std::vector<Inkscape::XML::Node *> l;

        for (auto& child: children) {
            if (is<SPTitle>(&child) || is<SPDesc>(&child)) {
                continue;
            }

            Inkscape::XML::Node *crepr = nullptr;

            if (is<SPString>(&child)) {
                crepr = xml_doc->createTextNode(cast<SPString>(&child)->string.c_str());
            } else {
                crepr = child.updateRepr(xml_doc, nullptr, flags);
            }

            if (crepr) {
                l.push_back(crepr);
            }
        }

        for (auto i=l.rbegin();i!=l.rend();++i) {
            repr->addChild(*i, nullptr);
            Inkscape::GC::release(*i);
        }
    } else {
        for (auto& child: children) {
            if (is<SPTitle>(&child) || is<SPDesc>(&child)) {
                continue;
            }

            if (is<SPString>(&child)) {
                child.getRepr()->setContent(cast<SPString>(&child)->string.c_str());
            } else {
                child.updateRepr(flags);
            }
        }
    }

    this->attributes.writeTo(repr);

    SPItem::write(xml_doc, repr, flags);

    return repr;
}


Geom::OptRect SPText::bbox(Geom::Affine const &transform, SPItem::BBoxType type) const {
    return this->layout.bounds(transform, type == SPItem::VISUAL_BBOX);
}

Inkscape::DrawingItem* SPText::show(Inkscape::Drawing &drawing, unsigned key, unsigned /*flags*/) {
    Inkscape::DrawingGroup *flowed = new Inkscape::DrawingGroup(drawing);
    flowed->setPickChildren(false);
    flowed->setStyle(this->style, this->parent->style);

    // pass the bbox of the text object as paintbox (used for paintserver fills)
    layout.show(flowed, view_style_attachments[key], geometricBounds());

    return flowed;
}


void SPText::hide(unsigned key)
{
    view_style_attachments.erase(key);
    for (auto &v : views) {
        if (v.key == key) {
            auto g = cast<Inkscape::DrawingGroup>(v.drawingitem.get());
            _clearFlow(g);
        }
    }
}

const char* SPText::typeName() const {
    if (has_inline_size() || has_shape_inside())
        return "text-flow";
    return "text";
}

const char* SPText::displayName() const {
    if (has_inline_size()) {
        return _("Auto-wrapped text");
    } else if (has_shape_inside()) {
        return _("Text in-a-shape");
    } else {
        return _("Text");
    }
}

gchar* SPText::description() const {
    int const nChars = layout.iteratorToCharIndex(layout.end());
    char const *trunc = (layout.inputTruncated()) ? _(" [truncated]") : "";

    return g_strdup_printf(ngettext("(%d character%s)", "(%d characters%s)", nChars), nChars, trunc);
}

void SPText::snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const {
    if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_TEXT_BASELINE)) {
        // Choose a point on the baseline for snapping from or to, with the horizontal position
        // of this point depending on the text alignment (left vs. right)
        Inkscape::Text::Layout const *layout = te_get_layout(this);

        if (layout != nullptr && layout->outputExists()) {
            std::optional<Geom::Point> pt = layout->baselineAnchorPoint();

            if (pt) {
                p.emplace_back((*pt) * this->i2dt_affine(), Inkscape::SNAPSOURCE_TEXT_ANCHOR, Inkscape::SNAPTARGET_TEXT_ANCHOR);
            }
        }
    }
}

void SPText::hide_shape_inside()
{
    auto text = this;
    SPStyle *item_style = this->style;
    if (item_style && text && item_style->shape_inside.set) {
        SPCSSAttr *css_unset = sp_css_attr_from_style(item_style, SP_STYLE_FLAG_IFSET);
        css = sp_css_attr_from_style(item_style, SP_STYLE_FLAG_IFSET);
        sp_repr_css_unset_property(css_unset, "shape-inside");
        sp_repr_css_attr_unref(css_unset);
        this->changeCSS(css_unset, "style");
    } else {
        css = nullptr;
    }
}

void SPText::show_shape_inside()
{
    if (css) {
        this->changeCSS(css, "style");
    }
}

Geom::Affine SPText::set_transform(Geom::Affine const &xform) {
    // See if 'shape-inside' has rectangle
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/tools/text/use_svg2", true)) {
        if (style->shape_inside.set) {
            return xform;
        }
    }
    // we cannot optimize textpath because changing its fontsize will break its match to the path

    if (SP_IS_TEXT_TEXTPATH (this)) {
        if (!this->_optimizeTextpathText) {
            return xform;
        } else {
            this->_optimizeTextpathText = false;
        }
    }

    // we cannot optimize text with textLength because it may show different size than specified
    if (this->attributes.getTextLength()->_set)
        return xform;

    if (this->style && this->style->inline_size.set)
        return xform;

    /* This function takes care of scaling & translation only, we return whatever parts we can't
       handle. */

// TODO: pjrm tried to use fontsize_expansion(xform) here and it works for text in that font size
// is scaled more intuitively when scaling non-uniformly; however this necessitated using
// fontsize_expansion instead of expansion in other places too, where it was not appropriate
// (e.g. it broke stroke width on copy/pasting of style from horizontally stretched to vertically
// stretched shape). Using fontsize_expansion only here broke setting the style via font
// dialog. This needs to be investigated further.
    double const ex = xform.descrim();
    if (ex == 0) {
        return xform;
    }

    Geom::Affine ret(Geom::Affine(xform).withoutTranslation());
    ret[0] /= ex;
    ret[1] /= ex;
    ret[2] /= ex;
    ret[3] /= ex;

    // Adjust x/y, dx/dy
    this->_adjustCoordsRecursive (this, xform * ret.inverse(), ex);

    // Adjust font size
    this->_adjustFontsizeRecursive (this, ex);

    // Adjust stroke width
    this->adjust_stroke_width_recursive (ex);

    // Adjust pattern fill
    this->adjust_pattern(xform * ret.inverse());

    // Adjust gradient fill
    this->adjust_gradient(xform * ret.inverse());

    return ret;
}

void SPText::print(SPPrintContext *ctx) {
    Geom::OptRect pbox, bbox, dbox;
    pbox = this->geometricBounds();
    bbox = this->desktopVisualBounds();
    dbox = Geom::Rect::from_xywh(Geom::Point(0,0), this->document->getDimensions());

    Geom::Affine const ctm (this->i2dt_affine());

    this->layout.print(ctx,pbox,dbox,bbox,ctm);
}

/*
 * Member functions
 */

void SPText::_buildLayoutInit()
{

    layout.strut.reset();
    layout.wrap_mode = Inkscape::Text::Layout::WRAP_NONE; // Default to SVG 1.1

    if (style) {

        // Strut
        auto font = ink_font_from_style(style);
        if (font) {
            font->FontMetrics(layout.strut.ascent, layout.strut.descent, layout.strut.xheight);
        }
        layout.strut *= style->font_size.computed;
        if (style->line_height.normal ) {
            layout.strut.computeEffective( Inkscape::Text::Layout::LINE_HEIGHT_NORMAL );
        } else if (style->line_height.unit == SP_CSS_UNIT_NONE) {
            layout.strut.computeEffective( style->line_height.computed );
        } else {
            if( style->font_size.computed > 0.0 ) {
                layout.strut.computeEffective( style->line_height.computed/style->font_size.computed );
            }
        }


        // To do: follow SPItem clip_ref/mask_ref code
        if (style->shape_inside.set ) {
            layout.wrap_mode = Inkscape::Text::Layout::WRAP_SHAPE_INSIDE;
            for (auto &&wrap_shape : makeEffectiveShapes()) {
                layout.appendWrapShape(std::move(wrap_shape));
            }
        } else if (has_inline_size()) {

            layout.wrap_mode = Inkscape::Text::Layout::WRAP_INLINE_SIZE;

            // If both shape_inside and inline_size are set, shape_inside wins out.

            // We construct a rectangle with one dimension set by the computed value of 'inline-size'
            // and the other dimension set to infinity. Text is laid out starting at the 'x' and 'y'
            // attribute values. This is handled elsewhere.

            Geom::OptRect opt_frame = get_frame();
            Geom::Rect frame = *opt_frame;

            Shape shape;
            int v0 = shape.AddPoint(frame.corner(0));
            int v1 = shape.AddPoint(frame.corner(1));
            int v2 = shape.AddPoint(frame.corner(2));
            int v3 = shape.AddPoint(frame.corner(3));
            shape.AddEdge(v0, v1);
            shape.AddEdge(v1, v2);
            shape.AddEdge(v2, v3);
            shape.AddEdge(v3, v0);

            auto uncross = std::make_unique<Shape>();
            uncross->ConvertToShape(&shape);
            layout.appendWrapShape(std::move(uncross));
        } else if (style->white_space.value == SP_CSS_WHITE_SPACE_PRE     ||
                   style->white_space.value == SP_CSS_WHITE_SPACE_PREWRAP ||
                   style->white_space.value == SP_CSS_WHITE_SPACE_PRELINE ) {
            layout.wrap_mode = Inkscape::Text::Layout::WRAP_WHITE_SPACE;
        }

    } // if (style)
}

unsigned SPText::_buildLayoutInput(SPObject *object, Inkscape::Text::Layout::OptionalTextTagAttrs const &parent_optional_attrs, unsigned parent_attrs_offset, bool in_textpath)
{
    unsigned length = 0;
    unsigned child_attrs_offset = 0;
    Inkscape::Text::Layout::OptionalTextTagAttrs optional_attrs;

    // Per SVG spec, an object with 'display:none' doesn't contribute to text layout.
    if (object->style->display.computed == SP_CSS_DISPLAY_NONE) {
        return 0;
    }

    auto text_object = cast<SPText>(object);
    auto tspan_object = cast<SPTSpan>(object);
    auto tref_object = cast<SPTRef>(object);
    auto textpath_object = cast<SPTextPath>(object);

    if (text_object) {

        bool use_xy = true;
        bool use_dxdyrotate = true;

        // SVG 2 Text wrapping.
        if (layout.wrap_mode == Inkscape::Text::Layout::WRAP_SHAPE_INSIDE ||
            layout.wrap_mode == Inkscape::Text::Layout::WRAP_INLINE_SIZE) {
            use_xy = false;
            use_dxdyrotate = false;
        }

        text_object->attributes.mergeInto(&optional_attrs, parent_optional_attrs, parent_attrs_offset, use_xy, use_dxdyrotate);

        // SVG 2 Text wrapping
        if (layout.wrap_mode == Inkscape::Text::Layout::WRAP_INLINE_SIZE) {

            // For horizontal text:
            //   'x' is used to calculate the left/right edges of the rectangle but is not
            //   needed later. If not deleted here, it will cause an incorrect positioning
            //   of the first line.
            //   'y' is used to determine where the first line box is located and is needed
            //   during the output stage.
            // For vertical text:
            //   Follow above but exchange 'x' and 'y'.
            // The SVG 2 spec currently says use the 'x' and 'y' from the <text> element,
            // if not defined in the <text> element, use the 'x' and 'y' from the first child.
            // We only look at the <text> element. (Doing otherwise means tracking if
            // we've found 'x' and 'y' and then creating the Shape at the end.)
            if (is_horizontal()) {
                // Horizontal text
                SVGLength* y = _getFirstYLength();
                if (y) {
                    optional_attrs.y.push_back(*y);
                } else {
                    std::cerr << "SPText::_buildLayoutInput: No 'y' attribute value with horizontal 'inline-size'!" << std::endl;
                }
            } else {
                // Vertical text
                SVGLength* x = _getFirstXLength();
                if (x) {
                    optional_attrs.x.push_back(*x);
                } else {
                    std::cerr << "SPText::_buildLayoutInput: No 'x' attribute value with vertical 'inline-size'!" << std::endl;
                }
            }
        }

        // set textLength on the entire layout, see note in TNG-Layout.h
        if (text_object->attributes.getTextLength()->_set) {
            layout.textLength._set = true;
            layout.textLength.value    = text_object->attributes.getTextLength()->value;
            layout.textLength.computed = text_object->attributes.getTextLength()->computed;
            layout.textLength.unit     = text_object->attributes.getTextLength()->unit;
            layout.lengthAdjust = (Inkscape::Text::Layout::LengthAdjust) text_object->attributes.getLengthAdjust();
        }
    }

    else if (tspan_object) {

        // x, y attributes are stripped from some tspans marked with role="line" as we do our own line layout.
        // This should be checked carefully, as it can undo line layout in imported SVG files.
        bool use_xy = !in_textpath &&
            (tspan_object->role == SP_TSPAN_ROLE_UNSPECIFIED || !tspan_object->attributes.singleXYCoordinates());
        bool use_dxdyrotate = true;

        // SVG 2 Text wrapping: see comment above.
        if (layout.wrap_mode == Inkscape::Text::Layout::WRAP_SHAPE_INSIDE ||
            layout.wrap_mode == Inkscape::Text::Layout::WRAP_INLINE_SIZE) {
            use_xy = false;
            use_dxdyrotate = false;
        }

        tspan_object->attributes.mergeInto(&optional_attrs, parent_optional_attrs, parent_attrs_offset, use_xy, use_dxdyrotate);

        if (tspan_object->role != SP_TSPAN_ROLE_UNSPECIFIED) {
            // We are doing line wrapping using sodipodi:role="line". New lines have been stripped.

            // Insert paragraph break before text if not first tspan.
            SPObject *prev_object = object->getPrev();
            if (prev_object && cast<SPTSpan>(prev_object)) {
                if (!layout.inputExists()) {
                    // Add an object to store style, needed even if there is no text. When does this happen?
                    layout.appendText("", prev_object->style, prev_object, &optional_attrs);
                }
                layout.appendControlCode(Inkscape::Text::Layout::PARAGRAPH_BREAK, prev_object);
            }

            // Create empty span to store info (any non-empty tspan with sodipodi:role="line" has a child).
            if (!object->hasChildren()) {
                layout.appendText("", object->style, object, &optional_attrs);
            }

            length++;     // interpreting line breaks as a character for the purposes of x/y/etc attributes
                          // is a liberal interpretation of the svg spec, but a strict reading would mean
                          // that if the first line is empty the second line would take its place at the
                          // start position. Very confusing.
                          // SVG 2 clarifies, attributes are matched to unicode input characters so line
                          // breaks do match to an x/y/etc attribute.
            child_attrs_offset--;
        }
    }

    else if (tref_object) {
        tref_object->attributes.mergeInto(&optional_attrs, parent_optional_attrs, parent_attrs_offset, true, true);
    }

    else if (textpath_object) {
        in_textpath = true; // This should be made local so we can mix normal text with textpath per SVG 2.
        textpath_object->attributes.mergeInto(&optional_attrs, parent_optional_attrs, parent_attrs_offset, false, true);
        optional_attrs.x.clear(); // Hmm, you can use x with horizontal text. So this is probably wrong.
        optional_attrs.y.clear();
    }

    else {
        optional_attrs = parent_optional_attrs;
        child_attrs_offset = parent_attrs_offset;
    }

    // Recurse
    for (auto& child: object->children) {
        auto str = cast<SPString>(&child);
        if (str) {
            Glib::ustring const &string = str->string;
            // std::cout << "  Appending: >" << string << "<" << std::endl;
            layout.appendText(string, object->style, &child, &optional_attrs, child_attrs_offset + length);
            length += string.length();
        } else if (!sp_repr_is_meta_element(child.getRepr())) {
            /*      ^^^^ XML Tree being directly used here while it shouldn't be.*/
            length += _buildLayoutInput(&child, optional_attrs, child_attrs_offset + length, in_textpath);
        }
    }

    return length;
}

std::unique_ptr<Shape> SPText::getExclusionShape() const
{
    auto result = std::make_unique<Shape>(); // Union of all exclusion shapes

    for (auto *href : style->shape_subtract.hrefs) {
        auto shape = href->getObject();
        if (!shape) {
            continue;
        }
        if (!shape->curve()) {
            shape->set_shape();
        }
        auto const *curve = shape->curve();
        if (!curve) {
            continue;
        }

        auto temp = std::make_unique<Path>();
        temp->LoadPathVector(*curve, shape->getRelativeTransform(this), true);

        auto margin = std::make_unique<Path>();
        if (shape->style->shape_margin.set) {
            temp->OutsideOutline(margin.get(), -shape->style->shape_margin.computed, join_round, butt_straight, 20.0);
        } else {
            margin = std::move(temp);
        }

        margin->Convert(0.25);  // Convert to polyline
        auto livarot_shape = std::make_unique<Shape>();
        margin->Fill(livarot_shape.get(), 0);

        auto uncrossed = std::make_unique<Shape>();
        uncrossed->ConvertToShape(livarot_shape.get());

        if (result->hasEdges()) {
            auto shape_temp = std::make_unique<Shape>();
            shape_temp->Booleen(result.get(), uncrossed.get(), bool_op_union);
            std::swap(result, shape_temp);
        } else {
            result->Copy(uncrossed.get());
        }

    }
    return result;
}

std::unique_ptr<Shape> SPText::getInclusionShape(SPShape *shape) const
{
    std::optional<double> padding = std::abs(style->shape_padding.computed);
    if (!style->shape_padding.set || *padding < 1e-12) {
        padding.reset();
    }
    return makeInclusionShape(shape, padding);
}

std::unique_ptr<Shape> SPText::makeInclusionShape(SPShape *shape, std::optional<double> padding)
{
    if (!shape) {
        return {};
    }
    if (!shape->curve()) {
        shape->set_shape();
    }
    auto curve = shape->curve();
    if (!curve) {
        return {};
    }

    auto pathvector = *curve;
    flatten(pathvector, fill_nonZero);

    auto temp_path = std::make_unique<Path>();
    temp_path->LoadPathVector(pathvector, shape->transform, true);

    auto const make_nice_shape = [](std::unique_ptr<Path> const &contour) -> std::unique_ptr<Shape> {
        auto temp = std::make_unique<Shape>();
        contour->ConvertWithBackData(1.0);
        contour->Fill(temp.get(), 0);
        auto result = std::make_unique<Shape>();
        result->ConvertToShape(temp.get());
        return result;
    };

    if (padding) {
        auto outline = std::make_unique<Path>();
        temp_path->Outline(outline.get(), *padding, join_round, butt_straight, 20.0);

        auto inclusion_shape = make_nice_shape(temp_path);
        auto thickened_border = make_nice_shape(outline);

        auto result = std::make_unique<Shape>();
        result->Booleen(inclusion_shape.get(), thickened_border.get(), bool_op_diff);
        return result;
    }
    return make_nice_shape(temp_path);
}

Geom::Point SPText::getFirstInsertionPosition(SPShape *shape, bool ltr)
{
    auto layout_shape = SPText::makeInclusionShape(shape);
    // TODO: Layout direction would need to return the value unrotated, since
    // the scanline maker can return interesting results when not in TTB.
    auto scan = Inkscape::Text::Layout::ShapeScanlineMaker(layout_shape.get(), Inkscape::Text::Layout::TOP_TO_BOTTOM);
    auto line = scan.makeScanline({});
    return Geom::Point(ltr ? line[0].x_start : line[0].x_end, line[0].y);
}

std::vector<std::unique_ptr<Shape>> SPText::makeEffectiveShapes() const
{
    // Find union of all exclusion shapes
    std::unique_ptr<Shape> exclusion_shape;
    if (style->shape_subtract.set) {
        exclusion_shape = getExclusionShape();
    }
    bool const has_exclusion = exclusion_shape && exclusion_shape->hasEdges();

    std::vector<std::unique_ptr<Shape>> result;
    // Find inside shape curves
    for (auto *href : style->shape_inside.hrefs) {
        auto obj = href->getObject();
        if (auto textarea_shape = getInclusionShape(obj)) {
            if (has_exclusion) {
                // Subtract exclusion shape
                auto copy = std::make_unique<Shape>();
                copy->Booleen(textarea_shape.get(), exclusion_shape.get(), bool_op_diff);
                textarea_shape = std::move(copy);
            }
            result.push_back(std::move(textarea_shape));
        } else {
            std::cerr << __FUNCTION__ <<  ": Failed to get curve." << std::endl;
        }
    }
    return result;
}


// SVG requires one to use the first x/y value found on a child element if x/y not given on text
// element. TODO: Recurse.
SVGLength*
SPText::_getFirstXLength()
{
    SVGLength* x = attributes.getFirstXLength();

    if (!x) {
        for (auto& child: children) {
            if (is<SPTSpan>(&child)) {
                auto tspan = cast<SPTSpan>(&child);
                x = tspan->attributes.getFirstXLength();
                break;
            }
        }
    }

    return x;
}


SVGLength*
SPText::_getFirstYLength()
{
    SVGLength* y = attributes.getFirstYLength();

    if (!y) {
        for (auto& child: children) {
            if (is<SPTSpan>(&child)) {
                auto tspan = cast<SPTSpan>(&child);
                y = tspan->attributes.getFirstYLength();
                break;
            }
        }
    }

    return y;
}

Geom::PathVector SPText::getNormalizedBpath() const
{
    return layout.convertToCurves();
}

void SPText::rebuildLayout()
{
    layout.clear();
    _buildLayoutInit();

    Inkscape::Text::Layout::OptionalTextTagAttrs optional_attrs;
    _buildLayoutInput(this, optional_attrs, 0, false);

    layout.calculateFlow();

    for (auto& child: children) {
        if (is<SPTextPath>(&child)) {
            SPTextPath const *textpath = cast<SPTextPath>(&child);
            if (textpath->originalPath != nullptr) {
#if DEBUG_TEXTLAYOUT_DUMPASTEXT
                g_print("%s", layout.dumpAsText().c_str());
#endif
                std::optional<std::pair<double, double>> active;
                auto const start = textpath->getRepr()->attribute("proof:path-start");
                auto const end = textpath->getRepr()->attribute("proof:path-end");
                if (start || end) {
                    // Malformed metadata hides the active portion rather than revealing
                    // text the original document did not draw.
                    active = std::pair{0.0, -1.0};
                    char *after_start = nullptr, *after_end = nullptr;
                    double const a = start ? g_ascii_strtod(start, &after_start) : -1;
                    double const b = end ? g_ascii_strtod(end, &after_end) : -1;
                    if (start && end && after_start != start && after_end != end && !*after_start && !*after_end &&
                        std::isfinite(a) && std::isfinite(b) && a >= 0 && b >= a) active = std::pair{a, b};
                }
                bool const automatic_spacing = g_strcmp0(textpath->getAttribute("proof:path-spacing"), "auto") == 0;
                double const points_per_unit = std::sqrt(std::abs(i2doc_affine().det())) * 0.75;
                layout.fitToPathAlign(textpath->startOffset, *textpath->originalPath, active,
                                      automatic_spacing, points_per_unit);
            }
        }
    }
#if DEBUG_TEXTLAYOUT_DUMPASTEXT
    g_print("%s", layout.dumpAsText().c_str());
#endif

    // set the x,y attributes on role:line spans
    for (auto& child: children) {
        if (is<SPTSpan>(&child)) {
            auto tspan = cast<SPTSpan>(&child);
            if ((tspan->role != SP_TSPAN_ROLE_UNSPECIFIED)
                 && tspan->attributes.singleXYCoordinates() ) {
                Inkscape::Text::Layout::iterator iter = layout.sourceToIterator(tspan);
                Geom::Point anchor_point = layout.chunkAnchorPoint(iter);
                tspan->attributes.setFirstXY(anchor_point);
                // repr needs to be updated but if we do it here we get a loop.
            }
        }
    }
}


void SPText::_adjustFontsizeRecursive(SPItem *item, double ex, bool is_root)
{
    SPStyle *style = item->style;

    if (style && !Geom::are_near(ex, 1.0)) {
        if (!style->font_size.set && is_root) {
            style->font_size.set = true;
        }
        style->font_size.type = SP_FONT_SIZE_LENGTH;
        style->font_size.computed *= ex;
        style->letter_spacing.computed *= ex;
        style->word_spacing.computed *= ex;
        if (style->line_height.unit != SP_CSS_UNIT_NONE &&
            style->line_height.unit != SP_CSS_UNIT_PERCENT &&
            style->line_height.unit != SP_CSS_UNIT_EM &&
            style->line_height.unit != SP_CSS_UNIT_EX) {
            // No unit on 'line-height' property has special behavior.
            style->line_height.computed *= ex;
        }
        item->updateRepr();
    }

    for(auto& o: item->children) {
        if (is<SPItem>(&o))
            _adjustFontsizeRecursive(cast<SPItem>(&o), ex, false);
    }
}

/**
 * Get the position of the baseline point for this text object.
 */
std::optional<Geom::Point> SPText::getBaselinePoint() const
{
    if (layout.outputExists()) {
        return layout.baselineAnchorPoint();
    }
    return std::optional<Geom::Point>();
}

void
remove_newlines_recursive(SPObject* object, bool is_svg2)
{
    // Replace '\n' by space.
    auto string = cast<SPString>(object);
    if (string) {
        static Glib::RefPtr<Glib::Regex> r = Glib::Regex::create("\n+");
        string->string = r->replace(string->string, 0, " ", (Glib::Regex::MatchFlags)0);
        string->getRepr()->setContent(string->string.c_str());
    }

    for (auto child : object->childList(false)) {
        remove_newlines_recursive(child, is_svg2);
    }

    // Add space at end of a line if line is created by sodipodi:role="line".
    auto tspan = cast<SPTSpan>(object);
    if (tspan                             &&
        tspan->role == SP_TSPAN_ROLE_LINE &&
        tspan->getNext() != nullptr       &&  // Don't add space at end of last line.
        !is_svg2) {                           // SVG2 uses newlines, should not have sodipodi:role.

        std::vector<SPObject *> children = tspan->childList(false);

        // Find last string (could be more than one if there is tspan in the middle of a tspan).
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            auto string = cast<SPString>(*it);
            if (string) {
                string->string += ' ';
                string->getRepr()->setContent(string->string.c_str());
                break;
            }
        }
    }
}

// Prepare multi-line text for putting on path.
void
SPText::remove_newlines()
{
    remove_newlines_recursive(this, has_shape_inside() || has_inline_size());
    style->inline_size.clear();
    style->shape_inside.clear();
    updateRepr();
}

void SPText::_adjustCoordsRecursive(SPItem *item, Geom::Affine const &m, double ex, bool is_root)
{
    if (is<SPTSpan>(item))
        cast<SPTSpan>(item)->attributes.transform(m, ex, ex, is_root);
              // it doesn't matter if we change the x,y for role=line spans because we'll just overwrite them anyway
    else if (is<SPText>(item))
        cast<SPText>(item)->attributes.transform(m, ex, ex, is_root);
    else if (is<SPTextPath>(item))
        cast<SPTextPath>(item)->attributes.transform(m, ex, ex, is_root);
    else if (is<SPTRef>(item)) {
        cast<SPTRef>(item)->attributes.transform(m, ex, ex, is_root);
    } else {
        g_warning("element is not text");
	return;
    }

    for(auto& o: item->children) {
        if (is<SPItem>(&o))
            _adjustCoordsRecursive(cast<SPItem>(&o), m, ex, false);
    }
}


void SPText::_clearFlow(Inkscape::DrawingGroup *in_arena)
{
    in_arena->clearChildren();
}


/** Remove 'x' and 'y' values on children (lines) or they will be interpreted as absolute positions
 * when 'inline-size' is removed.
 */
void SPText::remove_svg11_fallback() {
    for (auto& child:  children) {
        child.removeAttribute("x");
        child.removeAttribute("y");
    }
}

/** Convert new lines in 'inline-size' text to tspans with sodipodi:role="tspan".
 *  Note sodipodi:role="tspan" will be removed in the future!
 */
void SPText::newline_to_sodipodi() {

    // New lines can come anywhere, we must search character-by-character.
    auto it = layout.begin();
    while (it != layout.end()) {
        if (layout.characterAt(it) == '\n') {

            // Delete newline ('\n').
            iterator_pair pair;
            auto it_end = it;
            it_end.nextCharacter();
            sp_te_delete (this, it, it_end, pair);
            it = pair.first;

            // Insert newline (sodipodi:role="line").
            it = sp_te_insert_line(this, it);
        }

        it.nextCharacter();
        layout.validateIterator(&it);
    }
}

/** Convert tspans with sodipodi:role="tspans" to '\n'.
 *  Note sodipodi:role="tspan" will be removed in the future!
 */
void SPText::sodipodi_to_newline() {

    // tspans with sodipodi:role="line" are only direct children of a <text> element.
    for (auto child : childList(false)) {
        auto tspan = cast<SPTSpan>(child);  // Could have <desc> or <title>.
        if (tspan && tspan->role == SP_TSPAN_ROLE_LINE) {

            // Remove sodipodi:role attribute.
            tspan->removeAttribute("sodipodi:role");
            tspan->updateRepr();

            // Insert '/n' if not last line.
            // This may screw up dx, dy, rotate attribute counting but... SVG 2 text cannot have these values.
            if (tspan != lastChild()) {
                tspan->style->white_space.computed = SP_CSS_WHITE_SPACE_PRE; // Set so '\n' is not immediately stripped out before CSS recascaded!
                auto last_child = tspan->lastChild();
                auto last_string = cast<SPString>(last_child);
                if (last_string) {
                    // Add '\n' to string.
                    last_string->string += "\n";
                    last_string->updateRepr();
                } else {
                    // Insert new string with '\n'.
                    auto tspan_node = tspan->getRepr();
                    auto xml_doc = tspan_node->document();
                    tspan_node->appendChild(xml_doc->createTextNode("\n"));
                }
            }
        }
    }
}

bool SPText::is_horizontal() const
{
    unsigned mode = style->writing_mode.computed;
    return (mode == SP_CSS_WRITING_MODE_LR_TB || mode == SP_CSS_WRITING_MODE_RL_TB);
}

bool SPText::has_inline_size() const
{
    // If inline size is '0' it is as if it is not set.
    return (style->inline_size.set && style->inline_size.value != 0);
}

bool SPText::has_shape_inside() const
{
    return (style->shape_inside.set);
}

// Gets rectangle defined by <text> x, y and inline-size ("infinite" in one direction).
Geom::OptRect SPText::get_frame()
{
    Geom::OptRect opt_frame;
    Geom::Rect frame;

    if (has_inline_size()) {
        double inline_size = style->inline_size.computed;
        //unsigned mode      = style->writing_mode.computed;
        unsigned anchor    = style->text_anchor.computed;
        unsigned direction = style->direction.computed;

        if (is_horizontal()) {
            // horizontal
            frame = Geom::Rect::from_xywh(attributes.firstXY()[Geom::X], -100000, inline_size, 200000);
            if (anchor == SP_CSS_TEXT_ANCHOR_MIDDLE) {
                frame *= Geom::Translate (-inline_size/2.0, 0 );
            } else if ( (direction == SP_CSS_DIRECTION_LTR && anchor == SP_CSS_TEXT_ANCHOR_END  ) ||
                        (direction == SP_CSS_DIRECTION_RTL && anchor == SP_CSS_TEXT_ANCHOR_START) ) {
                frame *= Geom::Translate (-inline_size, 0);
            }
        } else {
            // vertical
            frame = Geom::Rect::from_xywh(-100000, attributes.firstXY()[Geom::Y], 200000, inline_size);
            if (anchor == SP_CSS_TEXT_ANCHOR_MIDDLE) {
                frame *= Geom::Translate (0, -inline_size/2.0);
            } else if (anchor == SP_CSS_TEXT_ANCHOR_END) {
                frame *= Geom::Translate (0, -inline_size);
            }
        }

        opt_frame = frame;

    } else {
        // See if 'shape-inside' has rectangle
        Inkscape::XML::Node* rectangle = get_first_rectangle();

        if (rectangle) {
            double x = rectangle->getAttributeDouble("x", 0.0);
            double y = rectangle->getAttributeDouble("y", 0.0);
            double width = rectangle->getAttributeDouble("width", 0.0);
            double height = rectangle->getAttributeDouble("height", 0.0);
            frame = Geom::Rect::from_xywh(x, y, width, height);
            opt_frame = frame;
        }
    }

    return opt_frame;
}

// Find the node of the first rectangle (if it exists) in 'shape-inside'.
Inkscape::XML::Node* SPText::get_first_rectangle()
{
    if (style->shape_inside.set) {

        for (auto *href : style->shape_inside.hrefs) {
            auto *shape = href->getObject();
            if (is<SPRect>(shape)) {
                auto *item = shape->getRepr();
                g_return_val_if_fail(item, nullptr);
                assert(strncmp("svg:rect", item->name(), 8) == 0);
                return item;
            }
        }
    }

    return nullptr;
}

void SPText::getLinked(std::vector<SPObject *> &objects, LinkedObjectNature direction) const
{
    if (direction == LinkedObjectNature::ANY || direction == LinkedObjectNature::DEPENDENCY) {
        for (auto item : get_all_shape_dependencies()) {
            objects.push_back(item);
        }
    }
    SPObject::getLinked(objects, direction);
}

/**
 * Get the first shape reference which affects the position and layout of
 * this text item. This can be either a shape-inside or a textPath referenced
 * shape. If this text does not depend on any other shape, then return NULL.
 */
SPItem *SPText::get_first_shape_dependency()
{
    for (auto item : get_all_shape_dependencies()) {
        return item;
    }
    return nullptr;
}

const std::vector<SPItem *> SPText::get_all_shape_dependencies() const
{
    std::vector<SPItem *> ret;
    if (style->shape_inside.set) {
        for (auto *href : style->shape_inside.hrefs) {
            // If the link is set but the object doesn't exist, skip it
            if (auto obj = href->getObject()) {
                ret.push_back(obj);
            }
        }
    } else if (auto textpath = cast<SPTextPath>(firstChild())) {
        ret.push_back(sp_textpath_get_path_item(textpath));
    }
    return ret;
}

/**
 * Create a free standing piece of text at the given position, within the given parent.
 * no style or transform is applied.
 *
 * @arg parent      - The parent element to append to
 * @arg pos         - The position of the new free standing text element in document units
 * @arg inline_size - Optional inline size for the text.
 *
 * @returns the new text object.
 */
SPText *create_text_at_position(SPGroup *parent, SPCSSAttr *css, Geom::Point doc_pos, std::optional<double> inline_size)
{
    Inkscape::XML::Document *xml_doc = parent->document->getReprDoc();
    Inkscape::XML::Node *text_repr = xml_doc->createElement("svg:text");
    text_repr->setAttribute("xml:space", "preserve"); // we preserve spaces in the text objects we create

    auto pos = doc_pos * parent->i2doc_affine().inverse();
    text_repr->setAttributeSvgDouble("x", pos.x());
    text_repr->setAttributeSvgDouble("y", pos.y());

    /* Create <tspan> */
    Inkscape::XML::Node *span_repr = xml_doc->createElement("svg:tspan");
    span_repr->setAttribute("sodipodi:role", "line"); // otherwise, why bother creating the tspan?
    // CSS property "vector-effect" doesn't inherit, so add it to <tspan> if part of default style.
    if (sp_repr_css_property(css, Glib::ustring("-inkscape-stroke"), Glib::ustring()) == "hairline" &&
        sp_repr_css_property(css, Glib::ustring("vector-effect"), Glib::ustring()) == "non-scaling-stroke") {
        span_repr->setAttribute("style", "vector-effect:non-scaling-stroke;-inkscape-stroke:hairline");
    }
    text_repr->addChild(span_repr, nullptr);
    Inkscape::GC::release(span_repr);

    /* Create TEXT */
    Inkscape::XML::Node *rstring = xml_doc->createTextNode("");
    span_repr->addChild(rstring, nullptr);
    Inkscape::GC::release(rstring);
    auto text_item = cast<SPText>(parent->appendChildRepr(text_repr));
    Inkscape::GC::release(text_repr);

    if (!css) {
        css = sp_repr_css_attr(text_repr, "style");
    } else {
        intrusive_ptr_add_ref(css);
    }
    if (inline_size) {
        sp_repr_css_set_property_double(css, "inline-size", *inline_size);
    }
    sp_repr_css_set(text_repr, css, "style");
    sp_repr_css_attr_unref(css);

    return text_item;
}

SPText *create_text_in_rectangle(SPGroup *parent, SPCSSAttr *css, Geom::Rect doc_rect)
{
    Inkscape::XML::Document *xml_doc = parent->document->getReprDoc();

    // Create rectangle
    auto rect = doc_rect * parent->i2doc_affine().inverse();
    Inkscape::XML::Node *rect_repr = xml_doc->createElement("svg:rect");
    rect_repr->setAttributeSvgDouble("x", rect.left());
    rect_repr->setAttributeSvgDouble("y", rect.top());
    rect_repr->setAttributeSvgDouble("width", rect.width());
    rect_repr->setAttributeSvgDouble("height", rect.height());

    // Add rectangle to defs.
    auto defs = parent->document->getDefs();
    auto box = cast<SPRect>(defs->appendChildRepr(rect_repr));
    Inkscape::GC::release(rect_repr);

    return create_text_in_shape(parent, css, box);
}

SPText *create_text_in_shape (SPGroup *parent, SPCSSAttr *css, SPShape *shape)
{
    Inkscape::XML::Document *xml_doc = parent->document->getReprDoc();

    Inkscape::XML::Node *text_repr = xml_doc->createElement("svg:text");
    text_repr->setAttribute("xml:space", "preserve"); // we preserve spaces in the text objects we create

    // Text transform is uneffected by the shape's parent transforms (groups etc) but does already
    // have the transformation of the shape itself, if any.
    auto tr = is<SPItem>(shape->parent) ? parent->getRelativeTransform(shape->parent)
                                        : Geom::identity();
    text_repr->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(tr.inverse()));

    auto text_item = cast<SPText>(parent->appendChildRepr(text_repr));
    g_assert(text_item != nullptr);

    if (!css) {
        css = sp_repr_css_attr(text_repr, "style");
    } else {
        intrusive_ptr_add_ref(css);
    }

    sp_repr_css_set_property (css, "white-space", "pre");
    sp_repr_css_set_property (css, "shape-inside", shape->getUrl().c_str());
    sp_repr_css_set(text_repr, css, "style");
    sp_repr_css_attr_unref(css);

    /* Create <tspan> */
    Inkscape::XML::Node *rtspan = xml_doc->createElement("svg:tspan");
    rtspan->setAttribute("sodipodi:role", "line"); // otherwise, why bother creating the tspan?
    Inkscape::XML::Node *text_node = xml_doc->createTextNode("");
    rtspan->appendChild(text_node);
    text_repr->appendChild(rtspan);

    Inkscape::GC::release(rtspan);
    Inkscape::GC::release(text_repr);
    Inkscape::GC::release(text_node);

    return text_item;
}

SPText *create_text_on_path (SPGroup *parent, SPCSSAttr *css, SPShape *path, double offset, int align, bool right_side)
{
    Inkscape::XML::Document *xml_doc = parent->document->getReprDoc();

    Inkscape::XML::Node *text_repr = xml_doc->createElement("svg:text");
    text_repr->setAttribute("xml:space", "preserve"); // we preserve spaces in the text objects we create

    auto text_item = cast<SPText>(parent->appendChildRepr(text_repr));
    g_assert(text_item != nullptr);

    // Create textPath object and link to path
    auto textpath_repr = xml_doc->createElement("svg:textPath");
    Inkscape::setHrefAttribute(*textpath_repr, std::string("#") + path->getId());
    Inkscape::XML::Node *text_node = xml_doc->createTextNode("");
    textpath_repr->appendChild(text_node);
    text_repr->appendChild(textpath_repr);

    if (!css) {
        css = sp_repr_css_attr(text_repr, "style");
    } else {
        intrusive_ptr_add_ref(css);
    }
    if (align < 0) {
        sp_repr_css_set_property (css, "text-anchor", "start");
        sp_repr_css_set_property (css, "text-align",  "start");
    } else if (align > 0) {
        sp_repr_css_set_property (css, "text-anchor", "end");
        sp_repr_css_set_property (css, "text-align",  "end");
    } else {
        sp_repr_css_set_property (css, "text-anchor", "middle");
        sp_repr_css_set_property (css, "text-align",  "center");
    }
    textpath_repr->setAttribute("startOffset", Inkscape::Util::format_number(offset * 100.0, 3) + "%");
    textpath_repr->setAttribute("side", right_side ? "right" : "left");

    sp_repr_css_set(text_repr, css, "style");
    sp_repr_css_attr_unref(css);

    Inkscape::GC::release(textpath_repr);
    Inkscape::GC::release(text_repr);
    Inkscape::GC::release(text_node);
    return text_item;
}

/*
 * TextTagAttributes implementation
 */

// Not used.
// void TextTagAttributes::readFrom(Inkscape::XML::Node const *node)
// {
//     readSingleAttribute(SPAttr::X, node->attribute("x"));
//     readSingleAttribute(SPAttr::Y, node->attribute("y"));
//     readSingleAttribute(SPAttr::DX, node->attribute("dx"));
//     readSingleAttribute(SPAttr::DY, node->attribute("dy"));
//     readSingleAttribute(SPAttr::ROTATE, node->attribute("rotate"));
//     readSingleAttribute(SPAttr::TEXTLENGTH, node->attribute("textLength"));
//     readSingleAttribute(SPAttr::LENGTHADJUST, node->attribute("lengthAdjust"));
// }

bool TextTagAttributes::readSingleAttribute(SPAttr key, gchar const *value, SPStyle const *style, Geom::Rect const *viewport)
{
    // std::cout << "TextTagAttributes::readSingleAttribute: key: " << key
    //           << "  value: " << (value?value:"Null") << std::endl;
    std::vector<SVGLength> *attr_vector;
    bool update_x = false;
    bool update_y = false;
    switch (key) {
        case SPAttr::X:      attr_vector = &attributes.x;  update_x = true; break;
        case SPAttr::Y:      attr_vector = &attributes.y;  update_y = true; break;
        case SPAttr::DX:     attr_vector = &attributes.dx; update_x = true; break;
        case SPAttr::DY:     attr_vector = &attributes.dy; update_y = true; break;
        case SPAttr::ROTATE: attr_vector = &attributes.rotate; break;
        case SPAttr::TEXTLENGTH:
            attributes.textLength.readOrUnset(value);
            return true;
            break;
        case SPAttr::LENGTHADJUST:
            attributes.lengthAdjust = (value && !strcmp(value, "spacingAndGlyphs")?
                                        Inkscape::Text::Layout::LENGTHADJUST_SPACINGANDGLYPHS :
                                        Inkscape::Text::Layout::LENGTHADJUST_SPACING); // default is "spacing"
            return true;
            break;
        default: return false;
    }

    // FIXME: sp_svg_length_list_read() amalgamates repeated separators. This prevents unset values.
    *attr_vector = sp_svg_length_list_read(value);

    if( (update_x || update_y) && style != nullptr && viewport != nullptr ) {
        double const w = viewport->width();
        double const h = viewport->height();
        double const em = style->font_size.computed;
        double const ex = em * 0.5;
        for(auto & it : *attr_vector) {
            if( update_x )
                it.update( em, ex, w );
            if( update_y )
                it.update( em, ex, h );
        }
    }
    return true;
}

void TextTagAttributes::writeTo(Inkscape::XML::Node *node) const
{
    writeSingleAttributeVector(node, "x", attributes.x);
    writeSingleAttributeVector(node, "y", attributes.y);
    writeSingleAttributeVector(node, "dx", attributes.dx);
    writeSingleAttributeVector(node, "dy", attributes.dy);
    writeSingleAttributeVector(node, "rotate", attributes.rotate);

    writeSingleAttributeLength(node, "textLength", attributes.textLength);

    if (attributes.textLength._set) {
        if (attributes.lengthAdjust == Inkscape::Text::Layout::LENGTHADJUST_SPACING) {
            node->setAttribute("lengthAdjust", "spacing");
        } else if (attributes.lengthAdjust == Inkscape::Text::Layout::LENGTHADJUST_SPACINGANDGLYPHS) {
            node->setAttribute("lengthAdjust", "spacingAndGlyphs");
        }
    }
}

void TextTagAttributes::update( double em, double ex, double w, double h )
{
    for(auto & it : attributes.x) {
        it.update( em, ex, w );
    }
    for(auto & it : attributes.y) {
        it.update( em, ex, h );
    }
    for(auto & it : attributes.dx) {
        it.update( em, ex, w );
    }
    for(auto & it : attributes.dy) {
        it.update( em, ex, h );
    }
}

void TextTagAttributes::writeSingleAttributeLength(Inkscape::XML::Node *node, gchar const *key, const SVGLength &length)
{
    if (length._set) {
        node->setAttribute(key, length.write());
    } else
        node->removeAttribute(key);
}

void TextTagAttributes::writeSingleAttributeVector(Inkscape::XML::Node *node, gchar const *key, std::vector<SVGLength> const &attr_vector)
{
    if (attr_vector.empty())
        node->removeAttribute(key);
    else {
        Glib::ustring string;

        // FIXME: this has no concept of unset values because sp_svg_length_list_read() can't read them back in
        for (auto it : attr_vector) {
            if (!string.empty()) string += ' ';
            string += it.write();
        }
        node->setAttributeOrRemoveIfEmpty(key, string);
    }
}

bool TextTagAttributes::singleXYCoordinates() const
{
    return attributes.x.size() <= 1 && attributes.y.size() <= 1;
}

bool TextTagAttributes::anyAttributesSet() const
{
    return !attributes.x.empty() || !attributes.y.empty() || !attributes.dx.empty() || !attributes.dy.empty() || !attributes.rotate.empty();
}

Geom::Point TextTagAttributes::firstXY() const
{
    Geom::Point point;
    if (attributes.x.empty()) point[Geom::X] = 0.0;
    else point[Geom::X] = attributes.x[0].computed;
    if (attributes.y.empty()) point[Geom::Y] = 0.0;
    else point[Geom::Y] = attributes.y[0].computed;
    return point;
}

void TextTagAttributes::setFirstXY(Geom::Point &point)
{
    SVGLength zero_length;
    zero_length = 0.0;

    if (attributes.x.empty())
        attributes.x.resize(1, zero_length);
    if (attributes.y.empty())
        attributes.y.resize(1, zero_length);
    attributes.x[0] = point[Geom::X];
    attributes.y[0] = point[Geom::Y];
}

SVGLength* TextTagAttributes::getFirstXLength()
{
    if (!attributes.x.empty()) {
        return &attributes.x[0];
    } else {
        return nullptr;
    }
}

SVGLength* TextTagAttributes::getFirstYLength()
{
    if (!attributes.y.empty()) {
        return &attributes.y[0];
    } else {
        return nullptr;
    }
}

// Instance of TextTagAttributes contains attributes as defined by text/tspan element.
// output: What will be sent to the rendering engine.
// parent_attrs: Attributes collected from all ancestors.
// parent_attrs_offset: Where this element fits into the parent_attrs.
// copy_xy: Should this elements x, y attributes contribute to output (can preserve set values but not use them... kind of strange).
// copy_dxdxrotate: Should this elements dx, dy, rotate attributes contribute to output.
void TextTagAttributes::mergeInto(Inkscape::Text::Layout::OptionalTextTagAttrs *output, Inkscape::Text::Layout::OptionalTextTagAttrs const &parent_attrs, unsigned parent_attrs_offset, bool copy_xy, bool copy_dxdyrotate) const
{
    mergeSingleAttribute(&output->x,      parent_attrs.x,      parent_attrs_offset, copy_xy ? &attributes.x : nullptr);
    mergeSingleAttribute(&output->y,      parent_attrs.y,      parent_attrs_offset, copy_xy ? &attributes.y : nullptr);
    mergeSingleAttribute(&output->dx,     parent_attrs.dx,     parent_attrs_offset, copy_dxdyrotate ? &attributes.dx : nullptr);
    mergeSingleAttribute(&output->dy,     parent_attrs.dy,     parent_attrs_offset, copy_dxdyrotate ? &attributes.dy : nullptr);
    mergeSingleAttribute(&output->rotate, parent_attrs.rotate, parent_attrs_offset, copy_dxdyrotate ? &attributes.rotate : nullptr);
    if (attributes.textLength._set) { // only from current node, this is not inherited from parent
        output->textLength.value = attributes.textLength.value;
        output->textLength.computed = attributes.textLength.computed;
        output->textLength.unit = attributes.textLength.unit;
        output->textLength._set = attributes.textLength._set;
        output->lengthAdjust = attributes.lengthAdjust;
    }
}

void TextTagAttributes::mergeSingleAttribute(std::vector<SVGLength> *output_list, std::vector<SVGLength> const &parent_list, unsigned parent_offset, std::vector<SVGLength> const *overlay_list)
{
    output_list->clear();
    if (overlay_list == nullptr) {
        if (parent_list.size() > parent_offset)
        {
            output_list->reserve(parent_list.size() - parent_offset);
            std::copy(parent_list.begin() + parent_offset, parent_list.end(), std::back_inserter(*output_list));
        }
    } else {
        output_list->reserve(std::max((int)parent_list.size() - (int)parent_offset, (int)overlay_list->size()));
        unsigned overlay_offset = 0;
        while (parent_offset < parent_list.size() || overlay_offset < overlay_list->size()) {
            SVGLength const *this_item;
            if (overlay_offset < overlay_list->size()) {
                this_item = &(*overlay_list)[overlay_offset];
                overlay_offset++;
                parent_offset++;
            } else {
                this_item = &parent_list[parent_offset];
                parent_offset++;
            }
            output_list->push_back(*this_item);
        }
    }
}

void TextTagAttributes::erase(unsigned start_index, unsigned n)
{
    if (n == 0) return;
    if (!singleXYCoordinates()) {
        eraseSingleAttribute(&attributes.x, start_index, n);
        eraseSingleAttribute(&attributes.y, start_index, n);
    }
    eraseSingleAttribute(&attributes.dx, start_index, n);
    eraseSingleAttribute(&attributes.dy, start_index, n);
    eraseSingleAttribute(&attributes.rotate, start_index, n);
}

void TextTagAttributes::eraseSingleAttribute(std::vector<SVGLength> *attr_vector, unsigned start_index, unsigned n)
{
    if (attr_vector->size() <= start_index) return;
    if (attr_vector->size() <= start_index + n)
        attr_vector->erase(attr_vector->begin() + start_index, attr_vector->end());
    else
        attr_vector->erase(attr_vector->begin() + start_index, attr_vector->begin() + start_index + n);
}

void TextTagAttributes::insert(unsigned start_index, unsigned n)
{
    if (n == 0) return;
    if (!singleXYCoordinates()) {
        insertSingleAttribute(&attributes.x, start_index, n, true);
        insertSingleAttribute(&attributes.y, start_index, n, true);
    }
    insertSingleAttribute(&attributes.dx, start_index, n, false);
    insertSingleAttribute(&attributes.dy, start_index, n, false);
    insertSingleAttribute(&attributes.rotate, start_index, n, false);
}

void TextTagAttributes::insertSingleAttribute(std::vector<SVGLength> *attr_vector, unsigned start_index, unsigned n, bool is_xy)
{
    if (attr_vector->size() <= start_index) return;
    SVGLength zero_length;
    zero_length = 0.0;
    attr_vector->insert(attr_vector->begin() + start_index, n, zero_length);
    if (is_xy) {
        double begin = start_index == 0 ? (*attr_vector)[start_index + n].computed : (*attr_vector)[start_index - 1].computed;
        double diff = ((*attr_vector)[start_index + n].computed - begin) / n;   // n tested for nonzero in insert()
        for (unsigned i = 0 ; i < n ; i++)
            (*attr_vector)[start_index + i] = begin + diff * i;
    }
}

void TextTagAttributes::split(unsigned index, TextTagAttributes *second)
{
    if (!singleXYCoordinates()) {
        splitSingleAttribute(&attributes.x, index, &second->attributes.x, false);
        splitSingleAttribute(&attributes.y, index, &second->attributes.y, false);
    }
    splitSingleAttribute(&attributes.dx, index, &second->attributes.dx, true);
    splitSingleAttribute(&attributes.dy, index, &second->attributes.dy, true);
    splitSingleAttribute(&attributes.rotate, index, &second->attributes.rotate, true);
}

void TextTagAttributes::splitSingleAttribute(std::vector<SVGLength> *first_vector, unsigned index, std::vector<SVGLength> *second_vector, bool trimZeros)
{
    second_vector->clear();
    if (first_vector->size() <= index) return;
    second_vector->resize(first_vector->size() - index);
    std::copy(first_vector->begin() + index, first_vector->end(), second_vector->begin());
    first_vector->resize(index);
    if (trimZeros)
        while (!first_vector->empty() && (!first_vector->back()._set || first_vector->back().value == 0.0))
            first_vector->resize(first_vector->size() - 1);
}

void TextTagAttributes::join(TextTagAttributes const &first, TextTagAttributes const &second, unsigned second_index)
{
    if (second.singleXYCoordinates()) {
        attributes.x = first.attributes.x;
        attributes.y = first.attributes.y;
    } else {
        joinSingleAttribute(&attributes.x, first.attributes.x, second.attributes.x, second_index);
        joinSingleAttribute(&attributes.y, first.attributes.y, second.attributes.y, second_index);
    }
    joinSingleAttribute(&attributes.dx, first.attributes.dx, second.attributes.dx, second_index);
    joinSingleAttribute(&attributes.dy, first.attributes.dy, second.attributes.dy, second_index);
    joinSingleAttribute(&attributes.rotate, first.attributes.rotate, second.attributes.rotate, second_index);
}

void TextTagAttributes::joinSingleAttribute(std::vector<SVGLength> *dest_vector, std::vector<SVGLength> const &first_vector, std::vector<SVGLength> const &second_vector, unsigned second_index)
{
    if (second_vector.empty())
        *dest_vector = first_vector;
    else {
        dest_vector->resize(second_index + second_vector.size());
        if (first_vector.size() < second_index) {
            std::copy(first_vector.begin(), first_vector.end(), dest_vector->begin());
            SVGLength zero_length;
            zero_length = 0.0;
            std::fill(dest_vector->begin() + first_vector.size(), dest_vector->begin() + second_index, zero_length);
        } else
            std::copy(first_vector.begin(), first_vector.begin() + second_index, dest_vector->begin());
        std::copy(second_vector.begin(), second_vector.end(), dest_vector->begin() + second_index);
    }
}

void TextTagAttributes::transform(Geom::Affine const &matrix, double scale_x, double scale_y, bool extend_zero_length)
{
    SVGLength zero_length;
    zero_length = 0.0;

    /* edge testcases for this code:
       1) moving text elements whose position is done entirely with transform="...", no x,y attributes
       2) unflowing multi-line flowtext then moving it (it has x but not y)
    */
    unsigned points_count = std::max(attributes.x.size(), attributes.y.size());
    if (extend_zero_length && points_count < 1)
        points_count = 1;
    for (unsigned i = 0 ; i < points_count ; i++) {
        Geom::Point point;
        if (i < attributes.x.size()) point[Geom::X] = attributes.x[i].computed;
        else point[Geom::X] = 0.0;
        if (i < attributes.y.size()) point[Geom::Y] = attributes.y[i].computed;
        else point[Geom::Y] = 0.0;
        point *= matrix;
        if (i < attributes.x.size())
            attributes.x[i] = point[Geom::X];
        else if (point[Geom::X] != 0.0 && extend_zero_length) {
            attributes.x.resize(i + 1, zero_length);
            attributes.x[i] = point[Geom::X];
        }
        if (i < attributes.y.size())
            attributes.y[i] = point[Geom::Y];
        else if (point[Geom::Y] != 0.0 && extend_zero_length) {
            attributes.y.resize(i + 1, zero_length);
            attributes.y[i] = point[Geom::Y];
        }
    }
    for (auto & it : attributes.dx)
        it = it.computed * scale_x;
    for (auto & it : attributes.dy)
        it = it.computed * scale_y;
}

double TextTagAttributes::getDx(unsigned index)
{
    if( attributes.dx.empty()) {
        return 0.0;
    }
    if( index < attributes.dx.size() ) {
        return attributes.dx[index].computed;
    } else {
        return 0.0; // attributes.dx.back().computed;
    }
}


double TextTagAttributes::getDy(unsigned index)
{
    if( attributes.dy.empty() ) {
        return 0.0;
    }
    if( index < attributes.dy.size() ) {
        return attributes.dy[index].computed;
    } else {
        return 0.0; // attributes.dy.back().computed;
    }
}


void TextTagAttributes::addToDx(unsigned index, double delta)
{
    SVGLength zero_length;
    zero_length = 0.0;

    if (attributes.dx.size() < index + 1) attributes.dx.resize(index + 1, zero_length);
    attributes.dx[index] = attributes.dx[index].computed + delta;
}

void TextTagAttributes::addToDy(unsigned index, double delta)
{
    SVGLength zero_length;
    zero_length = 0.0;

    if (attributes.dy.size() < index + 1) attributes.dy.resize(index + 1, zero_length);
    attributes.dy[index] = attributes.dy[index].computed + delta;
}

void TextTagAttributes::addToDxDy(unsigned index, Geom::Point const &adjust)
{
    SVGLength zero_length;
    zero_length = 0.0;

    if (adjust[Geom::X] != 0.0) {
        if (attributes.dx.size() < index + 1) attributes.dx.resize(index + 1, zero_length);
        attributes.dx[index] = attributes.dx[index].computed + adjust[Geom::X];
    }
    if (adjust[Geom::Y] != 0.0) {
        if (attributes.dy.size() < index + 1) attributes.dy.resize(index + 1, zero_length);
        attributes.dy[index] = attributes.dy[index].computed + adjust[Geom::Y];
    }
}

double TextTagAttributes::getRotate(unsigned index)
{
    if( attributes.rotate.empty() ) {
        return 0.0;
    }
    if( index < attributes.rotate.size() ) {
        return attributes.rotate[index].computed;
    } else {
        return attributes.rotate.back().computed;
    }
}


void TextTagAttributes::addToRotate(unsigned index, double delta)
{
    SVGLength zero_length;
    zero_length = 0.0;

    if (attributes.rotate.size() < index + 2) {
        if (attributes.rotate.empty())
            attributes.rotate.resize(index + 2, zero_length);
        else
            attributes.rotate.resize(index + 2, attributes.rotate.back());
    }
    attributes.rotate[index] = mod360(attributes.rotate[index].computed + delta);
}


void TextTagAttributes::setRotate(unsigned index, double angle)
{
    SVGLength zero_length;
    zero_length = 0.0;

    if (attributes.rotate.size() < index + 2) {
        if (attributes.rotate.empty())
            attributes.rotate.resize(index + 2, zero_length);
        else
            attributes.rotate.resize(index + 2, attributes.rotate.back());
    }
    attributes.rotate[index] = mod360(angle);
}

/*
 * Returns the effective physical length of the text along
 * the baseline.
 */
double SPText::length() const
{
    auto iter = layout.begin();
    auto const start_pt = layout.characterAnchorPoint(iter);
    iter = layout.end();
    auto const end_pt = layout.characterAnchorPoint(iter);
    return Geom::distance(start_pt, end_pt);
}

/*
 * This method determines a multiplier to adjust the startOffset when the
 * side of text on a path is changed. When the text's side is flipped, the
 * effective direction of the path relative to the text also flips. This
 * can cause an unexpected shift in text position.
 *
 * The `(100 - oldOffset)` adjustment (for percentage-based offsets) is
 * often insufficient because it doesn't account for the intrinsic offset
 * produced by the text itself, which depends on its `text-anchor` and
 * `direction` properties.
 *
 * This function resolves an additional multiplier (either -1, 1, or 0)
 * that, when applied to the text's "length-based" offset (i.e., the space
 * occupied by the text based on its content, direction, and alignment),
 * helps correctly reposition the text-path knots. This ensures that the
 * text appears in the expected location after the flip.
 */
int SPText::resolve_flip_offset_multiplier() const
{
    auto const anchor = style->text_anchor.computed;
    auto const direction = style->direction.computed;

    if ((anchor == SP_CSS_TEXT_ANCHOR_START && direction == SP_CSS_DIRECTION_RTL) ||
        (anchor == SP_CSS_TEXT_ANCHOR_END && direction == SP_CSS_DIRECTION_LTR)) {
        return -1;
    }

    if ((anchor == SP_CSS_TEXT_ANCHOR_END && direction == SP_CSS_DIRECTION_RTL) ||
        (anchor == SP_CSS_TEXT_ANCHOR_START && direction == SP_CSS_DIRECTION_LTR)) {
        return 1;
    }

    return 0;
}

/*
 * This method determines a multiplier used to calculate an offset for positioning
 * text path knots. This offset helps align the text correctly along the path
 * based on its 'text-anchor' (start, middle, end) and 'direction' (left-to-right,
 * right-to-left) properties.
 */
double SPText::resolve_alignment_offset_multiplier() const
{
    auto const anchor = style->text_anchor.computed;
    auto const direction = style->direction.computed;

    if (anchor == SP_CSS_TEXT_ANCHOR_MIDDLE) {
        return -0.5;
    }

    if ((anchor == SP_CSS_TEXT_ANCHOR_START && direction == SP_CSS_DIRECTION_RTL) ||
        (anchor == SP_CSS_TEXT_ANCHOR_END && direction == SP_CSS_DIRECTION_LTR)) {
        return -1.0;
    }

    return 0;
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
