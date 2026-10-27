// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * TODO: insert short description here
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
/*
 */

#include <glibmm/i18n.h>
#include <cstring>

#include "attributes.h"
#include "desktop-style.h"
#include "desktop.h"
#include "document.h"
#include "layer-manager.h"
#include "snap-candidate.h"
#include "snap-preferences.h"
#include "style.h"
#include "style-text.h"
#include "text-editing.h"
#include "text-tag-attributes.h"

#include "sp-flowdiv.h"
#include "sp-flowregion.h"
#include "sp-flowtext.h"
#include "sp-rect.h"
#include "sp-string.h"
#include "sp-text.h"
#include "sp-use.h"

#include "path/path-curve.h"
#include "display/drawing-group.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "livarot/Shape.h"
#include "svg/svg.h"
#include "xml/repr.h"


SPFlowtext::SPFlowtext() : SPItem(),
    par_indent(0),
    _optimizeScaledText(false)
{
}

SPFlowtext::~SPFlowtext() = default;

void SPFlowtext::release()
{
    view_style_attachments.clear();
    SPItem::release();
}

void SPFlowtext::child_added(Inkscape::XML::Node* child, Inkscape::XML::Node* ref) {
	SPItem::child_added(child, ref);

	this->requestModified(SP_OBJECT_MODIFIED_FLAG);
}


/* fixme: hide (Lauris) */

void SPFlowtext::remove_child(Inkscape::XML::Node* child) {
	SPItem::remove_child(child);

	this->requestModified(SP_OBJECT_MODIFIED_FLAG);
}

void SPFlowtext::update(SPCtx* ctx, unsigned int flags) {
    SPItemCtx *ictx = (SPItemCtx *) ctx;
    SPItemCtx cctx = *ictx;

    unsigned childflags = flags;
    if (flags & SP_OBJECT_MODIFIED_FLAG) {
        childflags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }
    childflags &= SP_OBJECT_MODIFIED_CASCADE;

    std::vector<SPObject *> l;
    for (auto& child: children) {
        sp_object_ref(&child);
        l.push_back(&child);
    }

    for (auto child:l) {
        g_assert(child != nullptr);

        if (childflags || (child->uflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
            auto item = cast<SPItem>(child);
            if (item) {
                SPItem const &chi = *item;
                cctx.i2doc = chi.transform * ictx->i2doc;
                cctx.i2vp = chi.transform * ictx->i2vp;
                child->updateDisplay((SPCtx *)&cctx, childflags);
            } else {
                child->updateDisplay(ctx, childflags);
            }
        }

        sp_object_unref(child);
    }

    SPItem::update(ctx, flags);

    this->rebuildLayout();

    Geom::OptRect pbox = this->geometricBounds();

    for (auto &v : views) {
        auto &sa = view_style_attachments[v.key];
        sa.unattachAll();
        auto g = cast<Inkscape::DrawingGroup>(v.drawingitem.get());
        _clearFlow(g);
        g->setStyle(style);
        // pass the bbox of the flowtext object as paintbox (used for paintserver fills)
        layout.show(g, sa, pbox);
    }
}

void SPFlowtext::modified(unsigned int flags) {
    SPObject *region = nullptr;

    if (flags & SP_OBJECT_MODIFIED_FLAG) {
    	flags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }

    flags &= SP_OBJECT_MODIFIED_CASCADE;

    // FIXME: the below stanza is copied over from sp_text_modified, consider factoring it out
    if (flags & ( SP_OBJECT_STYLE_MODIFIED_FLAG )) {
        Geom::OptRect pbox = geometricBounds();

        for (auto &v : views) {
            auto &sa = view_style_attachments[v.key];
            sa.unattachAll();
            auto g = cast<Inkscape::DrawingGroup>(v.drawingitem.get());
            _clearFlow(g);
            g->setStyle(style);
            layout.show(g, sa, pbox);
        }
    }

    for (auto& o: children) {
        if (is<SPFlowregion>(&o)) {
            region = &o;
            break;
        }
    }

    if (region) {
        if (flags || (region->mflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
            region->emitModified(flags); // pass down to the region only
        }
    }
}

void SPFlowtext::build(SPDocument* doc, Inkscape::XML::Node* repr) {
    this->_requireSVGVersion(Inkscape::Version(1, 2));

    SPItem::build(doc, repr);

    this->readAttr(SPAttr::LAYOUT_OPTIONS);     // must happen after css has been read
}

void SPFlowtext::set(SPAttr key, const gchar* value) {
    switch (key) {
        case SPAttr::LAYOUT_OPTIONS: {
            // deprecated attribute, read for backward compatibility only
            //XML Tree being directly used while it shouldn't be.
            SPCSSAttr *opts = sp_repr_css_attr(this->getRepr(), "inkscape:layoutOptions");
            {
                gchar const *val = sp_repr_css_property(opts, "justification", nullptr);

                if (val != nullptr && !this->style->text_align.set) {
                    if ( strcmp(val, "0") == 0 || strcmp(val, "false") == 0 ) {
                        this->style->text_align.value = SP_CSS_TEXT_ALIGN_LEFT;
                    } else {
                        this->style->text_align.value = SP_CSS_TEXT_ALIGN_JUSTIFY;
                    }

                    this->style->text_align.set = TRUE;
                    this->style->text_align.inherit = FALSE;
                    this->style->text_align.computed = this->style->text_align.value;
                }
            }
            /* no equivalent css attribute for these two (yet)
            {
                gchar const *val = sp_repr_css_property(opts, "layoutAlgo", NULL);
                if ( val == NULL ) {
                    group->algo = 0;
                } else {
                    if ( strcmp(val, "better") == 0 ) {     // knuth-plass, never worked for general cases
                        group->algo = 2;
                    } else if ( strcmp(val, "simple") == 0 ) {   // greedy, but allowed lines to be compressed by up to 20% if it would make them fit
                        group->algo = 1;
                    } else if ( strcmp(val, "default") == 0 ) {    // the same one we use, a standard greedy
                        group->algo = 0;
                    }
                }
            }
            */
            {   // This would probably translate to padding-left, if SPStyle had it.
                gchar const *val = sp_repr_css_property(opts, "par-indent", nullptr);

                if ( val == nullptr ) {
                    this->par_indent = 0.0;
                } else {
                    this->par_indent = g_ascii_strtod(val, nullptr);
                }
            }

            sp_repr_css_attr_unref(opts);
            this->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
        }

        default:
        	SPItem::set(key, value);
            break;
    }
}

Inkscape::XML::Node* SPFlowtext::write(Inkscape::XML::Document* doc, Inkscape::XML::Node* repr, guint flags) {
    if ( flags & SP_OBJECT_WRITE_BUILD ) {
        if ( repr == nullptr ) {
            repr = doc->createElement("svg:flowRoot");
        }

        std::vector<Inkscape::XML::Node *> l;

        for (auto& child: children) {
            Inkscape::XML::Node *c_repr = nullptr;

            if (is<SPFlowdiv>(&child) || is<SPFlowpara>(&child) || is<SPFlowregion>(&child) || is<SPFlowregionExclude>(&child)) {
                c_repr = child.updateRepr(doc, nullptr, flags);
            }

            if ( c_repr ) {
                l.push_back(c_repr);
            }
        }

        for (auto i=l.rbegin();i!=l.rend();++i) {
            repr->addChild(*i, nullptr);
            Inkscape::GC::release(*i);
        }
    } else {
        for (auto& child: children) {
            if (is<SPFlowdiv>(&child) || is<SPFlowpara>(&child) || is<SPFlowregion>(&child) || is<SPFlowregionExclude>(&child)) {
                child.updateRepr(flags);
            }
        }
    }

    this->rebuildLayout();  // copied from update(), see LP Bug 1339305

    SPItem::write(doc, repr, flags);

    return repr;
}

Geom::OptRect SPFlowtext::bbox(Geom::Affine const &transform, SPItem::BBoxType type) const {
    return this->layout.bounds(transform, type == SPItem::VISUAL_BBOX);
}

void SPFlowtext::print(SPPrintContext *ctx) {
    Geom::OptRect pbox, bbox, dbox;
    pbox = this->geometricBounds();
    bbox = this->desktopVisualBounds();
    dbox = Geom::Rect::from_xywh(Geom::Point(0,0), this->document->getDimensions());

    Geom::Affine const ctm (this->i2dt_affine());

    this->layout.print(ctx, pbox, dbox, bbox, ctm);
}

const char* SPFlowtext::typeName() const {
    return "text";
}

const char* SPFlowtext::displayName() const {
    if (has_internal_frame()) {
        return _("Flowed Text");
    } else {
        return _("Linked Flowed Text");
    }
}

gchar* SPFlowtext::description() const {
    int const nChars = layout.iteratorToCharIndex(layout.end());
    char const *trunc = (layout.inputTruncated()) ? _(" [truncated]") : "";

    return g_strdup_printf(ngettext("(%d character%s)", "(%d characters%s)", nChars), nChars, trunc);
}

void SPFlowtext::snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const {
    if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_TEXT_BASELINE)) {
        // Choose a point on the baseline for snapping from or to, with the horizontal position
        // of this point depending on the text alignment (left vs. right)
        Inkscape::Text::Layout const *layout = te_get_layout((SPItem *) this);

        if (layout != nullptr && layout->outputExists()) {
            std::optional<Geom::Point> pt = layout->baselineAnchorPoint();

            if (pt) {
                p.emplace_back((*pt) * this->i2dt_affine(), Inkscape::SNAPSOURCE_TEXT_ANCHOR, Inkscape::SNAPTARGET_TEXT_ANCHOR);
            }
        }
    }
}

Inkscape::DrawingItem* SPFlowtext::show(Inkscape::Drawing &drawing, unsigned int key, unsigned int /*flags*/) {
    Inkscape::DrawingGroup *flowed = new Inkscape::DrawingGroup(drawing);
    flowed->setPickChildren(false);
    flowed->setStyle(this->style);

    // pass the bbox of the flowtext object as paintbox (used for paintserver fills)
    Geom::OptRect bbox = this->geometricBounds();
    layout.show(flowed, view_style_attachments[key], bbox);

    return flowed;
}

void SPFlowtext::hide(unsigned key)
{
    view_style_attachments.erase(key);

    for (auto &v : views) {
        if (v.key == key) {
            auto g = cast<Inkscape::DrawingGroup>(v.drawingitem.get());
            _clearFlow(g);
        }
    }
}

void SPFlowtext::_buildLayoutInput(SPObject *root, std::unique_ptr<Shape> exclusion_shape,
                                   SPObject **pending_line_break_object)
{
    Inkscape::Text::Layout::OptionalTextTagAttrs pi;
    bool with_indent = false;

    if (is<SPFlowpara>(root) || is<SPFlowdiv>(root)) {

        layout.wrap_mode = Inkscape::Text::Layout::WRAP_SHAPE_INSIDE;

        layout.strut.reset();
        if (style) {
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
        }

        // emulate par-indent with the first char's kern
        SPObject *t = root;
        SPFlowtext *ft = nullptr;
        while (t && !ft) {
            ft = cast<SPFlowtext>(t);
            t = t->parent;
        }

        if (ft) {
            double indent = ft->par_indent;
            if (indent != 0) {
                with_indent = true;
                SVGLength sl;
                sl.value = sl.computed = indent;
                sl._set = true;
                pi.dx.push_back(sl);
            }
        }
    }

    if (*pending_line_break_object) {
        if (is<SPFlowregionbreak>(*pending_line_break_object)) {
            layout.appendControlCode(Inkscape::Text::Layout::SHAPE_BREAK, *pending_line_break_object);
        } else {
            layout.appendControlCode(Inkscape::Text::Layout::PARAGRAPH_BREAK, *pending_line_break_object);
        }
        *pending_line_break_object = nullptr;
    }

    for (auto& child: root->children) {
        auto str = cast<SPString>(&child);
        if (str) {
            if (*pending_line_break_object) {
                if (is<SPFlowregionbreak>(*pending_line_break_object))
                    layout.appendControlCode(Inkscape::Text::Layout::SHAPE_BREAK, *pending_line_break_object);
                else {
                    layout.appendControlCode(Inkscape::Text::Layout::PARAGRAPH_BREAK, *pending_line_break_object);
                }
                *pending_line_break_object = nullptr;
            }
            if (with_indent) {
                layout.appendText(str->string, root->style, &child, &pi);
            } else {
                layout.appendText(str->string, root->style, &child);
            }
        } else {
            if (auto region = cast<SPFlowregion>(&child)) {
                for (auto &it : region->computed) {
                    auto shape = std::make_unique<Shape>();
                    if (exclusion_shape->hasEdges()) {
                        shape->Booleen(it.get(), exclusion_shape.get(), bool_op_diff);
                    } else {
                        shape->Copy(it.get());
                    }
                    layout.appendWrapShape(std::move(shape));
                }
            }
            // Xml Tree is being directly used while it shouldn't be.
            else if (!is<SPFlowregionExclude>(&child) && !sp_repr_is_meta_element(child.getRepr())) {
                _buildLayoutInput(&child, std::move(exclusion_shape), pending_line_break_object);
            }
        }
    }
    
    if (is<SPFlowdiv>(root) || is<SPFlowpara>(root) || is<SPFlowregionbreak>(root) || is<SPFlowline>(root)) {
        if (!root->hasChildren()) {
            layout.appendText("", root->style, root);
        }
        *pending_line_break_object = root;
    }
}

std::unique_ptr<Shape> SPFlowtext::_buildExclusionShape() const
{
    auto shape = std::make_unique<Shape>();
    auto shape_temp = std::make_unique<Shape>();

    for (auto &child : children) {
        // RH: is it right that this shouldn't be recursive?
        auto c_child = cast<SPFlowregionExclude>(const_cast<SPObject*>(&child));
        if (!c_child) {
            continue;
        }
        if (auto computed = c_child->getComputed(); computed && computed->hasEdges()) {
            if (shape->hasEdges()) {
                shape_temp->Booleen(shape.get(), computed, bool_op_union);
                std::swap(shape, shape_temp);
            } else {
                shape->Copy(computed);
            }
        }
    }
    return shape;
}

void SPFlowtext::rebuildLayout()
{
    layout.clear();
    SPObject *pending_line_break_object = nullptr;
    _buildLayoutInput(this, _buildExclusionShape(), &pending_line_break_object);
    layout.calculateFlow();
#if DEBUG_TEXTLAYOUT_DUMPASTEXT
    g_print("%s", layout.dumpAsText().c_str());
#endif
}

void SPFlowtext::_clearFlow(Inkscape::DrawingGroup *in_arena)
{
    in_arena->clearChildren();
}

Geom::PathVector SPFlowtext::getNormalizedBpath() const
{
    return layout.convertToCurves();
}

Inkscape::XML::Node *SPFlowtext::getAsText()
{
    if (!this->layout.outputExists()) {
        return nullptr;
    }

    Inkscape::XML::Document *xml_doc = this->document->getReprDoc();
    Inkscape::XML::Node *repr = xml_doc->createElement("svg:text");
    repr->setAttribute("xml:space", "preserve");
    repr->setAttribute("style", this->getRepr()->attribute("style"));
    Geom::Point anchor_point = this->layout.characterAnchorPoint(this->layout.begin());
    repr->setAttributeSvgDouble("x", anchor_point[Geom::X]);
    repr->setAttributeSvgDouble("y", anchor_point[Geom::Y]);

    for (Inkscape::Text::Layout::iterator it = this->layout.begin() ; it != this->layout.end() ; ) {
        Inkscape::XML::Node *line_tspan = xml_doc->createElement("svg:tspan");
        line_tspan->setAttribute("sodipodi:role", "line");

        Inkscape::Text::Layout::iterator it_line_end = it;
        it_line_end.nextStartOfLine();

        while (it != it_line_end) {

            Inkscape::XML::Node *span_tspan = xml_doc->createElement("svg:tspan");
            Geom::Point anchor_point = this->layout.characterAnchorPoint(it);
            // use kerning to simulate justification and whatnot
            Inkscape::Text::Layout::iterator it_span_end = it;
            it_span_end.nextStartOfSpan();
            Inkscape::Text::Layout::OptionalTextTagAttrs attrs;
            this->layout.simulateLayoutUsingKerning(it, it_span_end, &attrs);
            // set x,y attributes only when we need to
            bool set_x = false;
            bool set_y = false;
            if (!this->transform.isIdentity()) {
                set_x = set_y = true;
            } else {
                Inkscape::Text::Layout::iterator it_chunk_start = it;
                it_chunk_start.thisStartOfChunk();
                if (it == it_chunk_start) {
                    set_x = true;
                    // don't set y so linespacing adjustments and things will still work
                }
                Inkscape::Text::Layout::iterator it_shape_start = it;
                it_shape_start.thisStartOfShape();
                if (it == it_shape_start)
                    set_y = true;
            }
            if (set_x && !attrs.dx.empty())
                attrs.dx[0] = 0.0;
            TextTagAttributes(attrs).writeTo(span_tspan);
            if (set_x)
                span_tspan->setAttributeSvgDouble("x", anchor_point[Geom::X]);  // FIXME: this will pick up the wrong end of counter-directional runs
            if (set_y)
                span_tspan->setAttributeSvgDouble("y", anchor_point[Geom::Y]);
            if (line_tspan->childCount() == 0) {
                line_tspan->setAttributeSvgDouble("x", anchor_point[Geom::X]);  // FIXME: this will pick up the wrong end of counter-directional runs
                line_tspan->setAttributeSvgDouble("y", anchor_point[Geom::Y]);
            }

            SPObject *source_obj = nullptr;
            Glib::ustring::iterator span_text_start_iter;
            this->layout.getSourceOfCharacter(it, &source_obj, &span_text_start_iter);

            Glib::ustring style_text = (cast<SPString>(source_obj) ? source_obj->parent : source_obj)
                                           ->style->writeIfDiff(this->style);
            span_tspan->setAttributeOrRemoveIfEmpty("style", style_text);

            auto str = cast<SPString>(source_obj);
            if (str) {
                Glib::ustring *string = &(str->string); // TODO fixme: dangerous, unsafe premature-optimization
                SPObject *span_end_obj = nullptr;
                Glib::ustring::iterator span_text_end_iter;
                this->layout.getSourceOfCharacter(it_span_end, &span_end_obj, &span_text_end_iter);
                if (span_end_obj != source_obj) {
                    if (it_span_end == this->layout.end()) {
                        span_text_end_iter = span_text_start_iter;
                        for (int i = this->layout.iteratorToCharIndex(it_span_end) - this->layout.iteratorToCharIndex(it) ; i ; --i)
                            ++span_text_end_iter;
                    } else
                        span_text_end_iter = string->end();    // spans will never straddle a source boundary
                }

                if (span_text_start_iter != span_text_end_iter) {
                    Glib::ustring new_string;
                    while (span_text_start_iter != span_text_end_iter)
                        new_string += *span_text_start_iter++;    // grr. no substr() with iterators
                    Inkscape::XML::Node *new_text = xml_doc->createTextNode(new_string.c_str());
                    span_tspan->appendChild(new_text);
                    Inkscape::GC::release(new_text);
                }
            }
            it = it_span_end;

            line_tspan->appendChild(span_tspan);
            Inkscape::GC::release(span_tspan);
        }
        repr->appendChild(line_tspan);
        Inkscape::GC::release(line_tspan);
    }

    return repr;
}

SPItem const *SPFlowtext::get_frame(SPItem const *after) const
{
    SPItem *item = const_cast<SPFlowtext *>(this)->get_frame(after);
    return item;
}

SPItem *SPFlowtext::get_frame(SPItem const *after)
{
    SPItem *frame = nullptr;

    SPObject *region = nullptr;
    for (auto& o: children) {
        if (is<SPFlowregion>(&o)) {
            region = &o;
            break;
        }
    }

    if (region) {
        bool past = false;

        for (auto& o: region->children) {
            auto item = cast<SPItem>(&o);
            if (item) {
                if ( (after == nullptr) || past ) {
                    frame = item;
                } else {
                    if (item == after) {
                        past = true;
                    }
                }
            }
        }

        auto use = cast<SPUse>(frame);
        if ( use ) {
            frame = use->get_original();
        }
    }
    return frame;
}

bool SPFlowtext::has_internal_frame() const
{
    SPItem const *frame = get_frame(nullptr);

    return (frame && isAncestorOf(frame) && cast<SPRect>(frame));
}

SPItem *create_flowtext_with_internal_frame (SPGroup *parent, SPCSSAttr *css, Geom::Rect rect_doc)
{
    SPDocument *doc = parent->document;

    Inkscape::XML::Document *xml_doc = doc->getReprDoc();
    Inkscape::XML::Node *root_repr = xml_doc->createElement("svg:flowRoot");
    root_repr->setAttribute("xml:space", "preserve"); // we preserve spaces in the text objects we create
    root_repr->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(parent->i2doc_affine().inverse()));

    auto ft_item = cast<SPItem>(parent->appendChildRepr(root_repr));
    g_assert(ft_item != nullptr);
    SPObject *root_object = doc->getObjectByRepr(root_repr);
    g_assert(cast<SPFlowtext>(root_object) != nullptr);

    Inkscape::XML::Node *region_repr = xml_doc->createElement("svg:flowRegion");
    root_repr->appendChild(region_repr);
    SPObject *region_object = doc->getObjectByRepr(region_repr);
    g_assert(cast<SPFlowregion>(region_object) != nullptr);

    Inkscape::XML::Node *rect_repr = xml_doc->createElement("svg:rect"); // FIXME: use path!!! after rects are converted to use path
    region_repr->appendChild(rect_repr);

    auto rect = cast<SPRect>(doc->getObjectByRepr(rect_repr));
    g_assert(rect != nullptr);

    rect->setPosition(rect_doc.min()[Geom::X], rect_doc.min()[Geom::Y], rect_doc.width(), rect_doc.height());
    rect->updateRepr();

    Inkscape::XML::Node *para_repr = xml_doc->createElement("svg:flowPara");
    root_repr->appendChild(para_repr);
    SPObject *para_object = doc->getObjectByRepr(para_repr);
    g_assert(cast<SPFlowpara>(para_object) != nullptr);

    Inkscape::XML::Node *text = xml_doc->createTextNode("");
    para_repr->appendChild(text);

    Inkscape::GC::release(root_repr);
    Inkscape::GC::release(region_repr);
    Inkscape::GC::release(para_repr);
    Inkscape::GC::release(rect_repr);

    return ft_item;
}

void SPFlowtext::fix_overflow_flowregion(bool inverse)
{
    SPObject *object = this;
    for (auto child : object->childList(false)) {
        auto flowregion = cast<SPFlowregion>(child);
        if (flowregion) {
            object = flowregion;
            for (auto childshapes : object->childList(false)) {
                Geom::Scale scale = Geom::Scale(1000); //200? maybe find better way to fix overglow issue removing new lines...
                if (inverse) {
                    scale = scale.inverse();
                }
                cast<SPItem>(childshapes)->doWriteTransform(scale, nullptr, true);
            }
            break;
        }
    }
}

Geom::Affine SPFlowtext::set_transform (Geom::Affine const &xform, Inkscape::StrokeTransform *preview)
{
    if ((this->_optimizeScaledText && !xform.withoutTranslation().isNonzeroUniformScale())
        || (!this->_optimizeScaledText && !xform.isNonzeroUniformScale())) {
        if (!preview) this->_optimizeScaledText = false;
        return xform;
    }
    if (!preview) this->_optimizeScaledText = false;
    
    SPText *text = reinterpret_cast<SPText *>(this);
    
    double const ex = xform.descrim();
    if (ex == 0) {
        return xform;
    }

    if (preview) {
        preview->expansion = ex;
        return Geom::identity();
    }

    SPObject *region = nullptr;
    for (auto& o: children) {
        if (is<SPFlowregion>(&o)) {
            region = &o;
            break;
        }
    }
    if (region) {
        auto rect = cast<SPRect>(region->firstChild());
        if (rect) {
            rect->set_i2d_affine(xform * rect->i2dt_affine());
            rect->doWriteTransform(rect->transform, nullptr, true);
        }
    }

    Geom::Affine ret(xform);
    ret[0] /= ex;
    ret[1] /= ex;
    ret[2] /= ex;
    ret[3] /= ex;

    // Adjust font size
    text->_adjustFontsizeRecursive (this, ex);

    // Adjust stroke width
    this->adjust_stroke_width_recursive (ex);

    // Adjust pattern fill
    this->adjust_pattern(xform * ret.inverse());

    // Adjust gradient fill
    this->adjust_gradient(xform * ret.inverse());

    return Geom::Affine();
}

/**
 * Get the position of the baseline point for this text object.
 */
std::optional<Geom::Point> SPFlowtext::getBaselinePoint() const
{
    if (layout.outputExists()) {
        return layout.baselineAnchorPoint();
    }
    return std::optional<Geom::Point>();
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
