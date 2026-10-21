// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * SVG <feImage> implementation.
 */
/*
 * Authors:
 *   Felipe Corrêa da Silva Sanches <juca@members.fsf.org>
 *   hugo Rodrigues <haa.rodrigues@gmail.com>
 *   Abhishek Sharma
 *
 * Copyright (C) 2007 Felipe Sanches
 * Copyright (C) 2006 Hugo Rodrigues
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "image.h"

#include <algorithm>                             // for find_if
#include <cassert>                               // for assert
#include <cstring>                               // for strcmp, memcpy

#include <sigc++/signal.h>                       // for signal

#include "attributes.h"                          // for SPAttr
#include "bad-uri-exception.h"                   // for BadURIException
#include "document.h"                            // for SPDocument

#include "display/cairo-utils.h"                 // for Pixbuf
#include "display/drawing-image.h"               // for DrawingImage
#include "display/drawing-item.h"                // for DrawingItem
#include "display/nr-filter-image.h"             // for FilterImage
#include "display/nr-filter-primitive.h"         // for FilterPrimitive
#include "display/nr-filter.h"                   // for Filter
#include "io/link-policy.h"                      // for may_follow_link

#include "object/filters/sp-filter-primitive.h"  // for SPFilterPrimitive
#include "object/sp-item.h"                      // for SPItem, SP_ITEM_SHOW...
#include "object/sp-object.h"                    // for SPObject, SP_OBJECT_...
#include "object/uri-references.h"               // for URIReference
#include "object/uri.h"                          // for URI

namespace Inkscape::XML {
class Node;
} // namespace Inkscape::XML


SPFeImage::SPFeImage()
    : elemref(std::make_unique<Inkscape::URIReference>(this)) {}

SPFeImage::~SPFeImage() = default;

void SPFeImage::build(SPDocument *document, Inkscape::XML::Node *repr)
{
    SPFilterPrimitive::build(document, repr);

    readAttr(SPAttr::XLINK_HREF);
    readAttr(SPAttr::PRESERVEASPECTRATIO);
}

void SPFeImage::set(SPAttr key, char const *value)
{
    switch (key) {
        case SPAttr::XLINK_HREF:
            href = value ? value : "";
            reread_href();
            requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;

        case SPAttr::PRESERVEASPECTRATIO:
            /* Copied from sp-image.cpp */
            /* Do setup before, so we can use break to escape */
            aspect_align = SP_ASPECT_XMID_YMID; // Default
            aspect_clip = SP_ASPECT_MEET; // Default
            requestModified(SP_OBJECT_MODIFIED_FLAG);
            if (value) {
                int len;
                char c[256];
                char const *p, *e;
                unsigned int align, clip;
                p = value;
                while (*p && *p == 32) p += 1;
                if (!*p) break;
                e = p;
                while (*e && *e != 32) e += 1;
                len = e - p;
                if (len > 8) break;
                std::memcpy(c, value, len);
                c[len] = 0;
                /* Now the actual part */
                if (!std::strcmp(c, "none")) {
                    align = SP_ASPECT_NONE;
                } else if (!std::strcmp(c, "xMinYMin")) {
                    align = SP_ASPECT_XMIN_YMIN;
                } else if (!std::strcmp(c, "xMidYMin")) {
                    align = SP_ASPECT_XMID_YMIN;
                } else if (!std::strcmp(c, "xMaxYMin")) {
                    align = SP_ASPECT_XMAX_YMIN;
                } else if (!std::strcmp(c, "xMinYMid")) {
                    align = SP_ASPECT_XMIN_YMID;
                } else if (!std::strcmp(c, "xMidYMid")) {
                    align = SP_ASPECT_XMID_YMID;
                } else if (!std::strcmp(c, "xMaxYMid")) {
                    align = SP_ASPECT_XMAX_YMID;
                } else if (!std::strcmp(c, "xMinYMax")) {
                    align = SP_ASPECT_XMIN_YMAX;
                } else if (!std::strcmp(c, "xMidYMax")) {
                    align = SP_ASPECT_XMID_YMAX;
                } else if (!std::strcmp(c, "xMaxYMax")) {
                    align = SP_ASPECT_XMAX_YMAX;
                } else {
                    g_warning("Illegal preserveAspectRatio: %s", c);
                    break;
                }
                clip = SP_ASPECT_MEET;
                while (*e && *e == 32) e += 1;
                if (*e) {
                    if (!std::strcmp(e, "meet")) {
                        clip = SP_ASPECT_MEET;
                    } else if (!std::strcmp(e, "slice")) {
                        clip = SP_ASPECT_SLICE;
                    } else {
                        break;
                    }
                }
                aspect_align = align;
                aspect_clip = clip;
            } else {
                aspect_align = SP_ASPECT_XMID_YMID; // Default
                aspect_clip = SP_ASPECT_MEET; // Default
            }
            break;

        default:
            SPFilterPrimitive::set(key, value);
            break;
    }
}

void SPFeImage::try_load_image()
{
    /* TODO: If feImageHref is absolute, then use that (preferably handling the
     * case that it's not a file URI). Otherwise, go up the tree looking
     * for an xml:base attribute, and use that as the base URI for resolving
     * the relative feImageHref URI. Otherwise, if document->base is valid,
     * then use that as the base URI. Otherwise, use feImageHref directly
     * (i.e. interpreting it as relative to our current working directory).
     * (See http://www.w3.org/TR/xmlbase/#resolution .) */

    auto try_assign = [this] (char const *name) {
        // PROOF: a picture on another computer isn't looked up (io/link-policy.h).
        if (!Inkscape::IO::may_follow_link(name, document->getDocumentBase())) {
            return false;
        }
        if (!g_file_test(name, G_FILE_TEST_IS_REGULAR)) {
            return false;
        }

        auto img = Inkscape::Pixbuf::create_from_file(name);
        if (!img) {
            return false;
        }

        // Rendering code expects cairo format, so ensure this before making pixbuf immutable.
        img->ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO);

        pixbuf.reset(img);
        return true;
    };

    if (try_assign(href.data())) {
        // pass
    } else {
        auto fullname = g_build_filename(document->getDocumentBase(), href.data(), nullptr);
        if (try_assign(fullname)) {
            // pass
        } else {
            pixbuf.reset();
        }
        g_free(fullname);
    }
}

void SPFeImage::reread_href()
{
    // Disconnect from modification signals.
    _href_changed_connection.disconnect();
    if (type == ELEM) {
        _href_modified_connection.disconnect();
    }

    for (auto &v : views) {
        destroy_view(v);
    }

    // Set type, elemref, elem and pixbuf.
    try {
        elemref->attach(Inkscape::URI(href.data()));
    } catch (Inkscape::BadURIException const &) {
        elemref->detach();
    }
    pixbuf.reset();
    if (auto obj = elemref->getObject()) {
        elem = cast<SPItem>(obj);
        if (elem) {
            type = ELEM;
        } else {
            type = NONE;
            g_warning("SPFeImage::reread_href: %s points to non-item element", href.data());
        }
    } else {
        try_load_image();
        if (pixbuf) {
            type = IMAGE;
        } else {
            type = NONE;
            g_warning("SPFeImage::reread_href: failed to load image: %s", href.data());
        }
    }

    for (auto &v : views) {
        create_view(v);
    }

    // Connect to modification signals.
    _href_changed_connection = elemref->changedSignal().connect([this] (SPObject*, SPObject *to) { on_href_changed(to); });
    if (type == ELEM) {
        _href_modified_connection = elemref->getObject()->connectModified([this] (SPObject*, unsigned) { on_href_modified(); });
    }
}

void SPFeImage::on_href_changed(SPObject *new_obj)
{
    if (type == ELEM) {
        _href_modified_connection.disconnect();
    }

    for (auto &v : views) {
        destroy_view(v);
    }

    // Set type and image.
    pixbuf.reset();
    if (new_obj) {
        elem = cast<SPItem>(new_obj);
        if (elem) {
            type = ELEM;
        } else {
            type = NONE;
            g_warning("SPFeImage::on_href_changed: %s points to non-item element", href.data());
        }
    } else {
        try_load_image();
        if (pixbuf) {
            type = IMAGE;
        } else {
            type = NONE;
            g_warning("SPFeImage::on_href_changed: failed to load image: %s", href.data());
        }
    }

    for (auto &v : views) {
        create_view(v);
    }

    if (type == ELEM) {
        _href_modified_connection = elem->connectModified([this] (SPObject*, unsigned) { on_href_modified(); });
    }

    requestModified(SP_OBJECT_MODIFIED_FLAG);
}

void SPFeImage::on_href_modified()
{
    requestModified(SP_OBJECT_MODIFIED_FLAG);
}

void SPFeImage::release()
{
    _href_changed_connection.disconnect();
    _href_modified_connection.disconnect();
    elemref.reset();
    pixbuf.reset();

    // All views on this element should have been closed prior to release.
    assert(views.empty());

    SPFilterPrimitive::release();
}

void SPFeImage::destroy_view(View &v)
{
    if (type == ELEM) {
        elem->invoke_hide(v.inner_key);
    } else if (type == IMAGE) {
        v.child->unlink();
    }

    // Defensive-coding measure: clear filter renderer immediately.
    v.parent->setFilterRenderer(nullptr);
}

void SPFeImage::create_view(View &v)
{
    if (type == ELEM) {
        auto ai = elem->invoke_show(v.parent->drawing(), v.inner_key, SP_ITEM_SHOW_DISPLAY);
        v.child = ai;
        if (!v.child) {
            g_warning("SPFeImage::show: error creating DrawingItem for SVG Element");
        }
    } else if (type == IMAGE) {
        auto ai = new Inkscape::DrawingImage(v.parent->drawing());
        ai->setStyle(style);
        ai->setPixbuf(pixbuf);
        ai->setOrigin(Geom::Point(0, 0));
        ai->setScale(1.0, 1.0);
        ai->setClipbox(Geom::Rect(0, 0, pixbuf->width(), pixbuf->height()));
        v.child = ai;
    }
}

void SPFeImage::show(Inkscape::DrawingItem *parent)
{
    views.emplace_back();
    auto &v = views.back();

    v.parent = parent;
    v.inner_key = SPItem::display_key_new(1);

    create_view(v);
}

void SPFeImage::hide(Inkscape::DrawingItem *parent)
{
    auto it = std::find_if(views.begin(), views.end(), [parent] (auto &v) {
        return v.parent == parent;
    });
    assert(it != views.end());
    auto &v = *it;

    destroy_view(v);

    views.erase(it);
}

/*
 * Check if the object is being used in the filter's definition
 * and returns true if it is being used (to avoid infinite loops)
 */
bool SPFeImage::valid_for(SPObject const *obj) const
{
    // elem could be nullptr, but this should still work.
    return obj && cast<SPItem>(obj) != elem;
}

std::unique_ptr<Inkscape::Filters::FilterPrimitive> SPFeImage::build_renderer(Inkscape::DrawingItem *parent) const
{
    Inkscape::DrawingItem *child = nullptr;

    if (type != NONE) {
        auto it = std::find_if(views.begin(), views.end(), [parent] (auto &v) {
            return v.parent == parent;
        });
        assert(it != views.end());
        child = it->child;
    }

    auto image = std::make_unique<Inkscape::Filters::FilterImage>();
    build_renderer_common(image.get());

    image->item = child;
    image->from_element = type == ELEM;
    image->set_align(aspect_align);
    image->set_clip(aspect_clip);

    return image;
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
