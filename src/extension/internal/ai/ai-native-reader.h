// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: read Illustrator's native records into a document structure.
 *
 * The records (see ai-native-lexer.h and doc/PROOF-ai-native-records.md) hold
 * Illustrator's own copy of the art: layers and their options, groups, compound
 * paths, clipping groups, object names, hidden and locked objects, paths with
 * their fills and strokes, process colours, spot inks, global colours and
 * Registration, gradients, transparency, overprint, embedded images and every
 * artboard. This reads them into plain structures, in art space (y up), for the
 * importer and for checking the native writer.
 *
 * Reading fails closed: anything a layer that shows holds and this doesn't read
 * (an unknown operator, a mesh, a pattern fill, a placed file, a symbol, an
 * opacity mask, legacy type) makes read() return nothing with the reason, and the
 * caller opens the PDF page instead. What a hidden layer holds and this doesn't
 * read is left out of it, and listed in Document::left_out.
 *
 * Nothing is executed and reading is bounded (Limits).
 *
 * The operator meanings are ported from VectorCraft's reader
 * (crates/eps/src/import/ai/{read,paint,obj}.rs at 4cf912f), Copyright (c) 2026
 * ArtCraft Team and the VectorCraft contributors, MIT licence; see
 * LICENSES/MIT-VectorCraft.txt. PROOF adds global process colours (Xk), Lab spot
 * definitions, Registration (Xs, Xz) and fails closed on opacity masks, from a
 * census of real files.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_READER_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_READER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <2geom/affine.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/rect.h>

namespace Inkscape::Extension::Internal::AiNative {

/// A colour in the model the file defines it in.
struct Color
{
    enum class Model
    {
        Gray, ///< v[0]: grey level, 1 = white (as Illustrator's g and PDF's DeviceGray).
        Cmyk, ///< v[0..3]: C, M, Y, K, 0-1.
        Rgb,  ///< v[0..2]: R, G, B, 0-1.
        Lab,  ///< v[0..2]: L 0-100, a, b.
    };
    Model model = Model::Gray;
    std::array<double, 4> v{};

    static Color gray(double level) { return {Model::Gray, {level, 0, 0, 0}}; }
    static Color cmyk(double c, double m, double y, double k) { return {Model::Cmyk, {c, m, y, k}}; }
    static Color rgb(double r, double g, double b) { return {Model::Rgb, {r, g, b, 0}}; }
    static Color lab(double l, double a, double b) { return {Model::Lab, {l, a, b, 0}}; }

    bool operator==(Color const &o) const { return model == o.model && v == o.v; }
};

/// A named colour of the document.
struct Ink
{
    enum class Kind
    {
        Spot,         ///< A spot ink (x/X, Xx/XX): prints on its own plate.
        Process,      ///< A global process colour (Xk/XK): a named process mix.
        Registration, ///< [Registration] (Xs/XS, Xz/XZ): every plate.
    };
    std::string name;
    Kind kind = Kind::Spot;
    Color color;                     ///< At full strength, in its defining model.
    std::array<double, 4> cmyk{};    ///< Illustrator's CMYK equivalent.
};

/// Where a gradient sits on an object, in art space (Bg, Bm, Bh).
struct GradientPlacement
{
    int gradient = -1; ///< Index into Document::gradients.
    Geom::Point origin;
    double angle = 0.0; ///< Degrees.
    double length = 1.0;
    Geom::Affine bg;    ///< Bg's own matrix.
    Geom::Affine bm;    ///< The gradient matrix (Bm), identity without one.
    Geom::Point hilight; ///< The focal point's offset from the origin.
};

struct Paint
{
    enum class Kind
    {
        Solid,
        Gradient,
    };
    Kind kind = Kind::Solid;
    Color color;        ///< Solid: the colour; with an ink, the ink's full colour.
    int ink = -1;       ///< Solid: index into Document::inks.
    double tint = 1.0;  ///< Solid with an ink: 0-1, 1 = full strength.
    GradientPlacement gradient;
};

struct GradientStop
{
    double offset = 0.0;   ///< 0-1.
    double midpoint = 0.5; ///< 0.13-0.87, from the previous stop.
    double opacity = 1.0;
    Color color;
    int ink = -1;
    double tint = 1.0;
};

struct Gradient
{
    std::string name;
    bool radial = false;
    std::vector<GradientStop> stops; ///< In ramp order.
};

/// An embedded image's samples, as the file has them.
struct Image
{
    Geom::Affine matrix; ///< Pixels (y down) to art space, as the file writes it (y flipped).
    int width = 0;
    int height = 0;
    int bits = 8;     ///< 1 or 8.
    int channels = 3; ///< 1 grey, 3 RGB, 4 CMYK.
    std::string space; ///< The XN colour space name ("DeviceCMYK"...), if given.
    std::string samples; ///< Rows of width * channels samples (bits per sample), top row first.
    std::string alpha;   ///< One byte a pixel, or empty.
};

struct StrokeStyle
{
    double width = 1.0;
    int cap = 0;  ///< 0 butt, 1 round, 2 square.
    int join = 0; ///< 0 miter, 1 round, 2 bevel.
    double miter = 10.0;
    std::vector<double> dash;
    double dash_offset = 0.0;
};

struct Transparency
{
    int blend = 0; ///< 0 Normal ... 15 Luminosity, Xy's order.
    double opacity = 1.0;
    bool isolate = false;
    int knockout = 0; ///< 0 off, 1 on, 2 neutral.
    bool knockout_shape = false;

    bool is_default() const { return blend == 0 && opacity == 1.0 && !isolate && knockout == 0 && !knockout_shape; }
};

struct LayerOptions
{
    bool preview = true;
    bool printable = true;
    std::optional<int> dim;      ///< Dim images to this %, when on.
    int color_index = 0;         ///< Illustrator's layer colour preset, or -1 for a custom colour.
    std::array<int, 3> color_rgb{};
};

struct Node
{
    enum class Kind
    {
        Layer,
        Group,
        Compound,
        Path,
        Image,
        Text, ///< A text object; its characters are in the text document (story).
    };
    Kind kind = Kind::Group;
    std::string name;
    bool visible = true;
    bool locked = false;
    std::vector<Node> children; ///< Layer, Group, Compound.
    /// Layer, Group: the first child clips the rest (several clipping paths are
    /// joined into one compound path first).
    bool clipped = false;
    LayerOptions layer;
    Transparency transparency;

    // Path (and Compound: the first child's paint is the compound's).
    Geom::PathVector path;
    bool evenodd = false;
    std::optional<Paint> fill;
    std::optional<Paint> stroke;
    StrokeStyle stroke_style;
    bool overprint_fill = false;
    bool overprint_stroke = false;
    bool clipping = false; ///< A clipping path (W).
    bool guide = false;

    int image = -1; ///< Image: index into Document::images.
    std::optional<unsigned> story; ///< Text: its story in the text document.
    std::optional<double> box_rotation; ///< BBAccumRotation (radians), when the box is turned.
    /// Group: an object with several fills or strokes, effects or a brush, read as
    /// its drawn look.
    bool drawn_look = false;
    /// Read from %_ lines (Illustrator's own copy of an object).
    bool commented = false;
};

struct Artboard
{
    std::string name;
    Geom::Rect rect; ///< Art space.
};

struct Document
{
    std::vector<Node> layers; ///< Bottom first, as the file writes them.
    std::vector<Artboard> artboards;
    /// Art space (y up) to the document (y down, the first artboard's top-left at 0,0).
    Geom::Affine to_doc;
    bool cmyk = false; ///< The document colour mode.
    std::vector<Ink> inks;
    std::vector<Gradient> gradients;
    std::vector<Image> images;
    std::optional<Geom::Rect> bbox; ///< %%HiResBoundingBox, else %%BoundingBox (art space).
    std::string creator_version;    ///< %%AI8_CreatorVersion.
    std::size_t drawn_looks = 0;
    std::size_t commented_kept = 0; ///< Objects from %_ lines kept (not replaced by a drawn look).
    std::vector<std::string> left_out; ///< What hidden layers had that wasn't read.
    std::vector<std::string> warnings;
};

struct Limits
{
    std::size_t max_depth = 256;
    std::size_t max_stack = 1u << 20;
    std::size_t max_nodes = 4'000'000;
    std::size_t max_points = 4'000'000; ///< In one path.
    std::uint64_t max_pixels = 1u << 25; ///< In one image.
    std::size_t max_inks = 65536;
    std::size_t max_gradient_stops = 256;
};

/**
 * Read native records (decoded; see read_ai_native_records()).
 *
 * @return the document, or nothing with the reason in `error` when the records
 *         hold something on a layer that shows that this doesn't read.
 */
std::optional<Document> read(std::string_view records, std::string &error, Limits const &limits = {});

/// An object name from its XML id (AI10_ArtUID): '_' is a space and "_xHH_" a
/// character; the "_000..._" suffix that makes ids unique goes.
std::string xml_name(std::string_view id);

/// Text from the records' bytes: UTF-8, else Windows-1252; UTF-16BE with a byte order mark.
std::string text_of(std::string_view bytes);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_EXTENSION_INTERNAL_AI_NATIVE_READER_H
