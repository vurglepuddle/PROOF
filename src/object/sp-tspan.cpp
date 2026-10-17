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

#include "sp-tspan.h"
#include "libnrtype/tab-stops.h"

#include <glibmm/i18n.h>
#include <glibmm/regex.h>

#include "document.h"
#include "livarot/Path.h"
#include "sp-item-group.h"
#include "sp-textpath.h"
#include "sp-tref.h"
#include "sp-use-reference.h"
#include "style.h"
#include "text-editing.h"
#include "xml/href-attribute-helper.h"

/*#####################################################
#  SPTSPAN
#####################################################*/
SPTSpan::SPTSpan() : SPItem() {
    this->role = SP_TSPAN_ROLE_UNSPECIFIED;
}

SPTSpan::~SPTSpan() = default;

void SPTSpan::build(SPDocument *doc, Inkscape::XML::Node *repr) {
    this->readAttr(SPAttr::X);
    this->readAttr(SPAttr::Y);
    this->readAttr(SPAttr::DX);
    this->readAttr(SPAttr::DY);
    this->readAttr(SPAttr::ROTATE);

    // Strip sodipodi:role from SVG 2 flowed text.
    // this->role = SP_TSPAN_ROLE_UNSPECIFIED;
    auto text = cast<SPText>(parent);
    if (text && !(text->has_shape_inside()|| text->has_inline_size())) {
        this->readAttr(SPAttr::SODIPODI_ROLE);
    }

    // We'll intercept "style" to strip "visibility" property (SVG 1.1 fallback for SVG 2 text) then pass it on.
    this->readAttr(SPAttr::STYLE);

    SPItem::build(doc, repr);
}

void SPTSpan::release() {
    SPItem::release();
}

void SPTSpan::set(SPAttr key, const gchar* value) {
    if (this->attributes.readSingleAttribute(key, value, style, &viewport)) {
        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    } else {
        switch (key) {
            case SPAttr::SODIPODI_ROLE:
                if (value && (!strcmp(value, "line") || !strcmp(value, "paragraph"))) {
                    this->role = SP_TSPAN_ROLE_LINE;
                } else {
                    this->role = SP_TSPAN_ROLE_UNSPECIFIED;
                }
                break;
                
            case SPAttr::STYLE:
                if (value) {
                    Glib::ustring style(value);
                    Glib::RefPtr<Glib::Regex> regex = Glib::Regex::create("visibility\\s*:\\s*hidden;*");
                    Glib::ustring stripped = regex->replace_literal(style, 0, "", static_cast<Glib::Regex::MatchFlags >(0));
                    Inkscape::XML::Node *repr = getRepr();
                    repr->setAttributeOrRemoveIfEmpty("style", stripped);
                }
                // Fall through
            default:
                SPItem::set(key, value);
                break;
        }
    }
}

void SPTSpan::update(SPCtx *ctx, guint flags) {
    unsigned childflags = flags;
    if (flags & SP_OBJECT_MODIFIED_FLAG) {
        childflags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }
    childflags &= SP_OBJECT_MODIFIED_CASCADE;

    for (auto& ochild: children) {
        if ( flags || ( ochild.uflags & SP_OBJECT_MODIFIED_FLAG )) {
        	ochild.updateDisplay(ctx, childflags);
        }
    }

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
    }
}

void SPTSpan::modified(unsigned int flags) {
//    SPItem::onModified(flags);

    if (flags & SP_OBJECT_MODIFIED_FLAG) {
        flags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }
    
    flags &= SP_OBJECT_MODIFIED_CASCADE;

    for (auto& ochild: children) {
        if ((flags & SP_OBJECT_FLAGS_ALL) || (ochild.mflags & SP_OBJECT_MODIFIED_FLAG)) {
            ochild.emitModified(flags);
        }
    }
}

Geom::OptRect SPTSpan::bbox(Geom::Affine const &transform, SPItem::BBoxType type) const {
    Geom::OptRect bbox;
    // find out the ancestor text which holds our layout
    SPObject const *parent_text = this;
    
    while (parent_text && !is<SPText>(parent_text)) {
        parent_text = parent_text->parent;
    }
    
    if (parent_text == nullptr) {
        return bbox;
    }

    // get the bbox of our portion of the layout
    return cast<SPText>(parent_text)->layout.bounds(transform,
            type == SPItem::VISUAL_BBOX,
            sp_text_get_length_upto(parent_text, this),
            sp_text_get_length_upto(this, nullptr) - 1);
}

Inkscape::XML::Node* SPTSpan::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = xml_doc->createElement("svg:tspan");
    }

    Inkscape::Text::LeftTabStops::copy_attributes(repr, getRepr());
    this->attributes.writeTo(repr);

    if ( flags&SP_OBJECT_WRITE_BUILD ) {
        std::vector<Inkscape::XML::Node *> l;

        for (auto& child: children) {
            Inkscape::XML::Node* c_repr=nullptr;

            if ( is<SPTSpan>(&child) || is<SPTRef>(&child) ) {
                c_repr = child.updateRepr(xml_doc, nullptr, flags);
            } else if ( is<SPTextPath>(&child) ) {
                //c_repr = child.updateRepr(xml_doc, NULL, flags); // shouldn't happen
            } else if ( is<SPString>(&child) ) {
                c_repr = xml_doc->createTextNode(cast<SPString>(&child)->string.c_str());
            }

            if ( c_repr ) {
                l.push_back(c_repr);
            }
        }

        for (auto i = l.rbegin(); i!= l.rend(); ++i) {
            repr->addChild((*i), nullptr);
            Inkscape::GC::release(*i);
        }
    } else {
        for (auto& child: children) {
            if ( is<SPTSpan>(&child) || is<SPTRef>(&child) ) {
                child.updateRepr(flags);
            } else if ( is<SPTextPath>(&child) ) {
                //c_repr = child->updateRepr(xml_doc, NULL, flags); // shouldn't happen
            } else if ( is<SPString>(&child) ) {
                child.getRepr()->setContent(cast<SPString>(&child)->string.c_str());
            }
        }
    }

    SPItem::write(xml_doc, repr, flags);

    return repr;
}

const char* SPTSpan::typeName() const {
    return "text-data";
}

const char* SPTSpan::displayName() const {
    return _("Text Span");
}


/*#####################################################
#  SPTEXTPATH
#####################################################*/
void   refresh_textpath_source(SPTextPath* offset);

SPTextPath::SPTextPath() : SPItem() {
    this->startOffset._set = false;
    this->side = SP_TEXT_PATH_SIDE_LEFT;
    this->originalPath = nullptr;
    this->isUpdating=false;

    // set up the uri reference
    this->sourcePath = new SPUsePath(this);
    this->sourcePath->user_unlink = sp_textpath_to_text;
}

SPTextPath::~SPTextPath() {
	delete this->sourcePath;
}

void SPTextPath::build(SPDocument *doc, Inkscape::XML::Node *repr) {
    this->readAttr(SPAttr::X);
    this->readAttr(SPAttr::Y);
    this->readAttr(SPAttr::DX);
    this->readAttr(SPAttr::DY);
    this->readAttr(SPAttr::ROTATE);
    this->readAttr(SPAttr::STARTOFFSET);
    this->readAttr(SPAttr::SIDE);
    this->readAttr(SPAttr::XLINK_HREF);

    this->readAttr(SPAttr::STYLE);

    SPItem::build(doc, repr);
}

void SPTextPath::release() {
    //this->attributes.~TextTagAttributes();

    if (this->originalPath) {
    	delete this->originalPath;
    }

    this->originalPath = nullptr;

    SPItem::release();
}

void SPTextPath::set(SPAttr key, const gchar* value) {

    if (this->attributes.readSingleAttribute(key, value, style, &viewport)) {
        this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    } else {
        switch (key) {
            case SPAttr::XLINK_HREF:
                this->sourcePath->link((char*)value);
                break;
            case SPAttr::SIDE:
                if (!value) {
                    return;
                }

                if      (strncmp(value, "left",  4) == 0)
                    side = SP_TEXT_PATH_SIDE_LEFT;
                else if (strncmp(value, "right", 5) == 0)
                    side = SP_TEXT_PATH_SIDE_RIGHT;
                else {
                    std::cerr << "SPTextPath: Bad side value: " << (value?value:"null") << std::endl;
                    side = SP_TEXT_PATH_SIDE_LEFT;
                }
                sourcePath->sourceDirty = true;
                this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
                break;
            case SPAttr::STARTOFFSET:
                this->startOffset.readOrUnset(value);
                this->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
                break;
            default:
                SPItem::set(key, value);
                break;
        }
    }
}

void SPTextPath::setStartOffset(const char *offset) {
    setAttribute("startOffset", offset);
}

void SPTextPath::setSide(TextPathSide new_side) {
    setAttribute("side", new_side ? "right" : "left");
}

void SPTextPath::update(SPCtx *ctx, guint flags) {
    this->isUpdating = true;

    if ( this->sourcePath->sourceDirty ) {
        refresh_textpath_source(this);
    }

    this->isUpdating = false;

    unsigned childflags = (flags & SP_OBJECT_MODIFIED_CASCADE);
    if (flags & SP_OBJECT_MODIFIED_FLAG) {
        childflags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }

    for (auto& ochild: children) {
        if (childflags || (ochild.uflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG))) {
            ochild.updateDisplay(ctx, childflags);
        }
    }

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
    }
}


void refresh_textpath_source(SPTextPath* tp)
{
    if ( tp == nullptr ) {
    	return;
    }

    tp->sourcePath->refresh_source();
    tp->sourcePath->sourceDirty=false;

    if ( tp->sourcePath->originalPath ) {
        if (tp->originalPath) {
            delete tp->originalPath;
        }

        auto curve_copy = *tp->sourcePath->originalPath;
        if (tp->side == SP_TEXT_PATH_SIDE_RIGHT) {
            curve_copy.reverse();
        }

        auto item = cast<SPItem>(tp->sourcePath->sourceObject);
        tp->originalPath = new Path;
        tp->originalPath->LoadPathVector(curve_copy, item->transform, true);
        tp->originalPath->ConvertWithBackData(0.01);
    }
}

void SPTextPath::modified(unsigned int flags) {
//    SPItem::onModified(flags);

    if (flags & SP_OBJECT_MODIFIED_FLAG) {
        flags |= SP_OBJECT_PARENT_MODIFIED_FLAG;
    }

    flags &= SP_OBJECT_MODIFIED_CASCADE;

    for (auto& ochild: children) {
        if ((flags & SP_OBJECT_FLAGS_ALL) || (ochild.mflags & SP_OBJECT_MODIFIED_FLAG)) {
            ochild.emitModified(flags);
        }
    }
}

Inkscape::XML::Node* SPTextPath::write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags) {
    if ((flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = xml_doc->createElement("svg:textPath");
    }

    this->attributes.writeTo(repr);
    if (repr != getRepr()) {
        for (auto const name : {"proof:path-start", "proof:path-end"}) {
            repr->setAttribute(name, getRepr()->attribute(name));
        }
    }

    if (this->side == SP_TEXT_PATH_SIDE_RIGHT) {
        this->setAttribute("side", "right");
    }

    if (this->startOffset._set) {
        if (this->startOffset.unit == SVGLength::PERCENT) {
	        Inkscape::SVGOStringStream os;
            os << (this->startOffset.computed * 100.0) << "%";
            this->setAttribute("startOffset", os.str());
        } else {
            /* FIXME: This logic looks rather undesirable if e.g. startOffset is to be
               in ems. */
            repr->setAttributeSvgDouble("startOffset", this->startOffset.computed);
        }
    }

    if ( this->sourcePath->sourceHref ) {
        Inkscape::setHrefAttribute(*repr, this->sourcePath->sourceHref);
    }

    if ( flags & SP_OBJECT_WRITE_BUILD ) {
        std::vector<Inkscape::XML::Node *> l;

        for (auto& child: children) {
            Inkscape::XML::Node* c_repr=nullptr;

            if ( is<SPTSpan>(&child) || is<SPTRef>(&child) ) {
                c_repr = child.updateRepr(xml_doc, nullptr, flags);
            } else if ( is<SPTextPath>(&child) ) {
                //c_repr = child->updateRepr(xml_doc, NULL, flags); // shouldn't happen
            } else if ( is<SPString>(&child) ) {
                c_repr = xml_doc->createTextNode(cast<SPString>(&child)->string.c_str());
            }

            if ( c_repr ) {
                l.push_back(c_repr);
            }
        }

        for( auto i = l.rbegin(); i != l.rend(); ++i ) {
            repr->addChild(*i, nullptr);
            Inkscape::GC::release(*i);
        }
    } else {
        for (auto& child: children) {
            if ( is<SPTSpan>(&child) || is<SPTRef>(&child) ) {
                child.updateRepr(flags);
            } else if ( is<SPTextPath>(&child) ) {
                //c_repr = child.updateRepr(xml_doc, NULL, flags); // shouldn't happen
            } else if ( is<SPString>(&child) ) {
                child.getRepr()->setContent(cast<SPString>(&child)->string.c_str());
            }
        }
    }

    SPItem::write(xml_doc, repr, flags);

    return repr;
}


SPItem *sp_textpath_get_path_item(SPTextPath const *tp)
{
    if (tp && tp->sourcePath) {
        return tp->sourcePath->getObject();
    }
    return nullptr;
}

void sp_textpath_to_text(SPObject *tp)
{
    SPObject *text = tp->parent;
    
    // make a list of textpath children
    std::vector<Inkscape::XML::Node *> tp_reprs;

    for (auto& o: tp->children) {
        tp_reprs.push_back(o.getRepr());
    }

    for (auto i = tp_reprs.rbegin(); i != tp_reprs.rend(); ++i) {
        // make a copy of each textpath child
        Inkscape::XML::Node *copy = (*i)->duplicate(text->getRepr()->document());
        // remove the old repr from under textpath
        tp->getRepr()->removeChild(*i);
        // put its copy under text
        text->getRepr()->addChild(copy, nullptr); // fixme: copy id
    }

    // set x/y on text (to be near where it was when on path)
    // Copied from Layout::fitToPathAlign
    // SVG 2 allows the path to be set by the "path" attribute within <textPath>, so
    // path may not exist (we don't support this yet).
    Geom::Point midpoint;
    if (auto path = cast<SPTextPath>(tp)->originalPath) {
        SVGLength const startOffset = cast<SPTextPath>(tp)->startOffset;
        double offset = 0.0;
        if (startOffset._set) {
            if (startOffset.unit == SVGLength::PERCENT)
                offset = startOffset.computed * path->Length();
            else
                offset = startOffset.computed;
        }
        int unused = 0;
        Path::cut_position *cut_pos = path->CurvilignToPosition(1, &offset, unused);
        if (cut_pos) {
            Geom::Point tangent;
            path->PointAndTangentAt(cut_pos[0].piece, cut_pos[0].t, midpoint, tangent);
        }
    } else {
        std::cerr << "sp_textpath_to_text: no path" << std::endl;
    }
    text->getRepr()->setAttributeSvgDouble("x", midpoint[Geom::X]);
    text->getRepr()->setAttributeSvgDouble("y", midpoint[Geom::Y]);

    //remove textpath
    tp->deleteObject();
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
