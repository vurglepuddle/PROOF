// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * The data describing a single loaded font.
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2022 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef LIBNRTYPE_FONT_INSTANCE_H
#define LIBNRTYPE_FONT_INSTANCE_H

#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <2geom/forward.h>
#include <pango/pango-font.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "font-glyph.h"
#include "OpenTypeUtil.h"
#include "style-enums.h"

/**
 * FontInstance provides metrics, OpenType data, and glyph curves/pixbufs for a font.
 *
 * Most data is loaded upon construction. Some rarely-used OpenType tables are lazy-loaded,
 * as are the curves/pixbufs for each glyph.
 *
 * Although FontInstance can be used on its own, in practice it is always obtained through
 * a FontFactory.
 *
 * Note: The font size is a scale factor in the transform matrix of the style.
 */
class FontInstance
{
public:
    /// Constructor; takes ownership of both arguments, which must be non-null. Throws CtorException on failure.
    FontInstance(PangoFont *p_font, PangoFontDescription *descr);

    /// Exception thrown if construction fails.
    struct CtorException : std::runtime_error
    {
        template <typename... Args>
        CtorException(Args&&... args) : std::runtime_error(std::forward<Args>(args)...) {}
    };

    ~FontInstance();

    unsigned int MapUnicodeChar(gunichar c) const; // calls the relevant unicode->glyph index function

    // Loads the given glyph's info. Glyphs are lazy-loaded, but never unloaded or modified
    // as long as the FontInstance still exists. Pointers to FontGlyphs also remain valid.
    FontGlyph const *LoadGlyph(unsigned int glyph_id);

    // nota: all coordinates returned by these functions are on a [0..1] scale; you need to multiply
    // by the fontsize to get the real sizes

    // Return 2geom pathvector for glyph. Deallocated when font instance dies.
    Geom::PathVector const *PathVector(unsigned int glyph_id);

    // Various bounding boxes. Color fonts complicate this as some types don't have paths.
    Geom::Rect BBoxExact(unsigned int glyph_id); // Bounding box of glyph. From Harfbuzz.
    Geom::Rect BBoxPick( unsigned int glyph_id); // For picking. (Height: embox, Width: glyph advance.)
    Geom::Rect BBoxDraw( unsigned int glyph_id); // Contains all inked areas including possible text decorations.

    // Return if font has various tables.
    bool                  FontHasSVG() const { return has_svg; };

    auto const &get_opentype_varaxes() const { return data->openTypeVarAxes; }

    // Return the font's OpenType tables, possibly loading them on-demand.
    std::map<Glib::ustring, OTSubstitution> const &get_opentype_tables();

    // Return svg document as a string of a SVG glyph or empty string if no SVG glyph exists.
    Glib::ustring SvgDocument(unsigned int glyph_id);

    std::string GlyphSvg(unsigned int glyph_id);

    // Horizontal advance if 'vertical' is false, vertical advance if true.
    double Advance(unsigned int glyph_id, bool vertical);

    // Return a shared pointer that will keep alive the pathvector and pixbuf data, but nothing else.
    std::shared_ptr<void const> share_data() const { return data; }

    double        GetTypoAscent()  const { return _ascent; }
    double        GetTypoDescent() const { return _descent; }
    double        GetXHeight()     const { return _xheight; }
    double        GetCapHeight();
    double        GetMaxAscent()   const { return _ascent_max; }
    double        GetMaxDescent()  const { return _descent_max; }
    const double* GetBaselines()   const { return _baselines; }
    int           GetDesignUnits() const { return _design_units; }

    Glib::ustring GetFilename() const;

    bool FontMetrics(double &ascent, double &descent, double &leading) const;

    bool FontDecoration(double &underline_position, double &underline_thickness,
                        double &linethrough_position, double &linethrough_thickness) const;

    bool FontSlope(double &run, double &rise) const; // for generating slanted cursors for oblique fonts

    bool has_vertical() const { return _has_vertical; }

    auto get_descr() const { return descr; }
    auto get_hash() const { return descr_hash; }
    auto get_font() const { return p_font; }

    bool is_fixed_width() const { return _fixed_width; }
    bool is_oblique() const { return _oblique; }
    unsigned short family_class() const { return _family_class; }

    struct CharInfo {
        uint32_t unicode;
        uint32_t glyph_index;
    };
    // traverse the font to find all defined characters returning (unicode, glyph index) pairs;
    // return only characters in (from, to) range
    std::vector<CharInfo> find_all_characters(uint32_t from = 0, uint32_t to = 0xffff'ffff) const;

private:
    void acquire(PangoFont *p_font, PangoFontDescription *descr);
    void release();
    void init_face();
    void find_font_metrics(); // Find ascent, descent, x-height, and baselines.

    /*
     * Resources
     */

    // The font's fingerprint; this particular PangoFontDescription gives the key at which this font instance
    // resides in the font cache. It may differ from the PangoFontDescription belonging to p_font.
    PangoFontDescription *descr;
    unsigned int descr_hash;

    // The real source of the font
    PangoFont *p_font;

    // We need to keep around an rw copy of the (read-only) hb font to extract the freetype face.
    hb_font_t *hb_font;
    hb_font_t *hb_font_copy;

    // Useful for getting metrics without dealing with FreeType archaics.
    hb_face_t *hb_face;

    // It's a pointer in fact; no worries to ref/unref it, pango does its magic
    // as long as p_font is valid, face is too.
    FT_Face face;

    bool has_svg = false;   // SVG glyphs

    /*
     * Metrics
     */

    // Font metrics in em-box units
    double  _ascent;       // Typographic ascent.
    double  _descent;      // Typographic descent.
    double  _xheight;      // x-height of font.
    double  _ascent_max;   // Maximum ascent of all glyphs in font.
    double  _descent_max;  // Maximum descent of all glyphs in font.
    int     _design_units; // Design units, (units per em, typically 1000 or 2048).
    double  _italic_angle = 0.0; // angle for oblique fonts, if specified in a font
    bool _fixed_width = false; // monospaced font (if advertised as such)
    bool _oblique = false;  // oblique or italic font
    bool _has_vertical = false; // Has vertical metrics
    unsigned short _family_class = 0; // OS/2 sFamily class field

    // Baselines
    double _baselines[SP_CSS_BASELINE_SIZE];

    // Set of OpenType tables in font.
    std::unordered_set<std::string> openTypeTableList;

    struct Data
    {
        /*
         * Tables
         */

        // Map of SVG in OpenType entries
        std::map<int, std::string> openTypeSVGData;

        // Map of SVG in OpenType glyphs
        std::map<unsigned int, SVGGlyphEntry> openTypeSVGGlyphs;

        // Vector for font variations.
        std::vector<OTVarAxis> openTypeVarAxes; // Axes with ranges

        // Map of named instances. Key is name, data is corresponding Pango string.
        std::map<Glib::ustring, Glib::ustring> openTypeVarNamedInstances;

        // Map of GSUB OpenType tables found in font. Transparently lazy-loaded.
        std::optional<std::map<Glib::ustring, OTSubstitution>> openTypeTables;

        /*
         * Glyphs
         */

        // Lookup table mapping pango glyph ids to glyphs.
        std::unordered_map<unsigned int, std::unique_ptr<FontGlyph const>> glyphs;
    };

    std::shared_ptr<Data> data;
};

#endif // LIBNRTYPE_FONT_INSTANCE_H

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
