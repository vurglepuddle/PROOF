// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * SVG <image> implementation
 *//*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Edward Flick (EAF)
 *
 * Copyright (C) 1999-2005 Authors
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_INKSCAPE_SP_IMAGE_H
#define SEEN_INKSCAPE_SP_IMAGE_H

#include "sp-item.h"

#include <memory>
#include <string>
#include <vector>

#include <glibmm/ustring.h>
#include <2geom/pathvector.h>

#include "sp-dimensions.h"
#include "viewbox.h"

#define SP_IMAGE_HREF_MODIFIED_FLAG SP_OBJECT_USER_MODIFIED_FLAG_A

namespace Inkscape { class Pixbuf; class URI; namespace Colors::CMS { class Profile; } }
class SPImage final : public SPItem, public SPViewBox, public SPDimensions {
public:
    SPImage();
    ~SPImage() override;
    int tag() const override { return tag_of<decltype(*this)>; }

    Geom::Rect clipbox;
    double sx, sy;
    double ox, oy;
    double dpi;
    double prev_width, prev_height;

    std::optional<Geom::PathVector> curve; // This curve is at the image's boundary for snapping

    char *href;
    char *color_profile;

    std::shared_ptr<Inkscape::Pixbuf const> pixbuf;
    bool missing = true;

    /// PROOF: on a linked picture, the small picture its document keeps of it, as a data:
    /// address. It shows while the file can't be found, as it does in Illustrator.
    static constexpr char const *LINK_PREVIEW_ATTRIBUTE = "proof:link-preview";

    /// PROOF: whether this is a link to a file that can't be found or read.
    bool linkMissing() const;
    /// PROOF: the file a link names, as a file name where it is one; empty for a picture the
    /// document holds itself.
    std::string linkedFile() const;
    /// PROOF: link to `file` (a file name in UTF-8) instead, in the box the picture lies in.
    /// A picture of another shape keeps its own shape inside that box.
    void relink(std::string const &file);

    void build(SPDocument *document, Inkscape::XML::Node *repr) override;
    void release() override;
    void set(SPAttr key, char const* value) override;
    void update(SPCtx *ctx, unsigned int flags) override;
    Inkscape::XML::Node* write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, unsigned int flags) override;
    void modified(unsigned int flags) override;

    Geom::OptRect bbox(Geom::Affine const &transform, SPItem::BBoxType type) const override;
    void print(SPPrintContext *ctx) override;
    const char* typeName() const override;
    const char* displayName() const override;
    char* description() const override;
    Inkscape::DrawingItem* show(Inkscape::Drawing &drawing, unsigned int key, unsigned int flags) override;
    void snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const override;
    Geom::Affine set_transform(Geom::Affine const &transform) override;

    void apply_profile(Inkscape::Pixbuf *pixbuf);

    Geom::PathVector const *get_curve() const;
    void refresh_if_outdated();
    bool cropToArea(Geom::Rect area);
    bool cropToArea(const Geom::IntRect &area);

    Inkscape::URI getURI() const;
private:
    /// PROOF: `cmyk` is the document's CMYK profile, for a CMYK TIFF (cairo-utils.h).
    static Inkscape::Pixbuf *readImage(gchar const *href, gchar const *absref, gchar const *base, double svgdpi = 0,
                                       std::shared_ptr<Inkscape::Colors::CMS::Profile> const &cmyk = {});
    static Inkscape::Pixbuf *getBrokenImage(double width, double height);
};

/* Return duplicate of curve or NULL */
void sp_embed_image(Inkscape::XML::Node *imgnode, Inkscape::Pixbuf *pb);
void sp_embed_svg(Inkscape::XML::Node *image_node, std::string const &fn);

/// PROOF: the linked pictures of `document` whose files can't be found or read, in its order.
std::vector<SPImage *> sp_images_missing(SPDocument *document);
/**
 * PROOF: link `image` to `file`, and with it every other picture of its document that is
 * missing and has a file of its own name in the same folder, as Illustrator looks for them.
 *
 * @return how many pictures were linked; 0 when `file` isn't a picture that can be read.
 */
unsigned sp_images_relink(SPImage *image, std::string const &file);

#endif
