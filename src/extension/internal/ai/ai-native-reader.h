// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: read Illustrator's native records into a document structure.
 *
 * The records (see ai-native-lexer.h and doc/PROOF-ai-native-records.md) hold
 * Illustrator's own copy of the art: layers and their options, groups, compound
 * paths, clipping groups, object names, hidden and locked objects, paths with
 * their fills and strokes, process colours, spot inks, global colours and
 * Registration, gradients, patterns, transparency, overprint, embedded images,
 * placed (linked) pictures and every artboard. This reads them into plain
 * structures, in art space (y up), for the importer and for checking the native
 * writer.
 *
 * Reading fails closed: anything a layer that shows holds and this doesn't read
 * (an unknown operator, an unsupported mesh or opacity mask, a pattern of the old
 * format, a symbol, legacy type) makes read() return nothing with the reason, and the
 * caller opens the PDF page instead. What a hidden layer holds and this doesn't
 * read is left out of it, and listed in Document::left_out.
 *
 * Nothing is executed and reading is bounded (Limits).
 *
 * The operator meanings are ported from VectorCraft's reader
 * (crates/eps/src/import/ai/{read,paint,obj}.rs at 4cf912f), Copyright (c) 2026
 * ArtCraft Team and the VectorCraft contributors, MIT licence; see
 * LICENSES/MIT-VectorCraft.txt. Three later findings of theirs (at 0d0ea352) are
 * followed too, each checked against real files first: a compound path is painted as
 * its last path, a group has the transparency that holds at its Xd, and a gradient
 * instance with no path in it paints nothing. PROOF adds global process colours (Xk),
 * Lab spot definitions, Registration (Xs, Xz), objects that plug-ins keep live (XP),
 * clipping paths in groups of their own, and bounded mesh reading (mesh.rs at 0d0ea352),
 * checked against the corpus. Extra mesh boundary nodes and the omitted Gray colour
 * space come from that census; unsupported meshes and opacity masks fail closed.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_READER_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_READER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <2geom/affine.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/rect.h>

namespace Inkscape::Extension::Internal::AiNative {

struct TextDocument;

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

/**
 * Where a gradient sits on an object (Bg, Xm, Bm, Bh).
 *
 * Old files give Bg's origin, angle and length in art space, with Bg's own matrix for the
 * transformations since. Later ones also write a matrix that carries the gradient's own
 * unit space onto the art, and then Bg's values only describe the same placement
 * relative to the object's bounds:
 *  - Xm (linear): the whole ramp runs from 0 to 1 along x.
 *  - Bm, radial: the whole ramp is the unit circle about the origin.
 *  - Bm, linear: the matrix for PostScript, which draws a gradient ramp by ramp. 0 to 1
 *    is the span between the first two stops only; Xm is the same matrix for all of it.
 */
struct GradientPlacement
{
    int gradient = -1; ///< Index into Document::gradients.
    Geom::Point origin;
    double angle = 0.0; ///< Degrees.
    double length = 1.0;
    Geom::Affine bg;    ///< Bg's own matrix.
    Geom::Affine bm;    ///< Bm, identity without one.
    bool bm_given = false;
    std::optional<Geom::Affine> xm; ///< Xm.
    Geom::Point hilight; ///< The focal point's offset from the origin, in art space.
    /// The same offset in the gradient's unit space, as Bh also gives it: the angle in
    /// degrees (clockwise in that space) and the length as a share of the radius.
    double hilight_angle = 0.0;
    double hilight_length = 0.0;
};

struct Paint
{
    enum class Kind
    {
        Solid,
        Gradient,
        Pattern,
    };
    Kind kind = Kind::Solid;
    Color color;        ///< Solid: the colour; with an ink, the ink's full colour.
    int ink = -1;       ///< Solid: index into Document::inks.
    double tint = 1.0;  ///< Solid with an ink: 0-1, 1 = full strength.
    GradientPlacement gradient;
    int pattern = -1;            ///< Pattern: index into Document::patterns.
    Geom::Affine pattern_matrix; ///< Pattern: the pattern's own space to art space (p's matrix).
};

struct GradientStop
{
    double offset = 0.0; ///< 0-1.
    /// 0.13-0.87: where, between this stop and the next, the blend is half-way. Illustrator
    /// blends along u^N with N = ln 0.5 / ln midpoint (u running 0-1 between the stops), as
    /// its PDF pages write it (exponential functions).
    double midpoint = 0.5;
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
    /// The picture's space to art space, as the file writes it: for rows counted upwards,
    /// so row y of the picture (counted down) is the matrix's -y.
    Geom::Affine matrix;
    int width = 0;
    int height = 0;
    int bits = 8;     ///< 1 or 8.
    int channels = 3; ///< 1 grey, 3 RGB, 4 CMYK.
    std::string space; ///< The XN colour space name ("DeviceCMYK"...), if given.
    std::string samples; ///< Rows of width * channels samples (bits per sample), top row first.
    std::string alpha;   ///< One byte a pixel, or empty.
};

/**
 * The picture of a placed file that the document keeps beside its link
 * (%AI26_BeginPlacedObjectPreview): what Illustrator shows while the file can't be found.
 * It is small, 512 pixels at most on its longer side in the files seen.
 */
struct Preview
{
    int width = 0, height = 0; ///< Both 0 when the document keeps none.
    /// Rows of width * 4 bytes, top row first: red, green, blue, and an alpha that the
    /// colours aren't multiplied by.
    std::string pixels;
};

/**
 * A placed file: a picture the document links to instead of holding (%AI5_BeginPlace).
 *
 * The file is named as the machine that saved the document knew it. Newer files also give
 * the folder it was found in from the document's own, empty when that is the same folder.
 */
struct Placed
{
    Geom::Affine matrix; ///< The picture's own space to art space.
    /// Where the picture lies in its own space, whose y points down: its top edge is the
    /// box's least y.
    Geom::Rect box;
    std::string path;    ///< The file's bytes for the name, undecoded.
    std::string folder;  ///< %AI28_BeginPlacedRelativePath, undecoded.
    int file_type = -1;  ///< %%FileType.
    Preview preview;
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

/// One corner of a gradient-mesh patch, in art space.
struct MeshCorner
{
    Color color;
    double opacity = 1.0;
    Geom::Point point, next, previous;
};

struct MeshPatch
{
    std::array<MeshCorner, 4> corners;
    Geom::PathVector boundary; ///< Includes every /S and /A boundary node, not only /N corners.
    bool outlined = false; ///< Uniform colour, extra boundary nodes: imported as its exact outline.
};

struct Mesh
{
    std::vector<MeshPatch> patches;
    /// Checked only after envelopes have discarded their non-art mesh.
    bool unread = false;
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
        Text,   ///< A text object; its characters are in the text document (story).
        Placed, ///< A placed (linked) file.
        /// A gradient mesh, or (while reading) the non-art mesh of an envelope.
        Mesh,
        Foreign, ///< A bounded embedded PDF, placed by its own native record.
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

    /// A non-inverted opacity mask made of greyscale vector artwork (including gradients).
    /// Empty clipped artwork hides the object; no value means no active mask. Other forms
    /// remain explicit native-import refusals rather than losing their mask.
    std::optional<std::vector<Node>> opacity_mask;
    bool opacity_mask_clipped = true; ///< Unclipped masks have an opaque white backdrop.

    // Path (and Compound: the first child's paint is the compound's).
    Geom::PathVector path;
    bool evenodd = false;
    std::optional<Paint> fill;
    std::optional<Paint> stroke;
    StrokeStyle stroke_style;
    bool overprint_fill = false;
    bool overprint_stroke = false;
    bool clipping = false; ///< A clipping path (W).
    /// Group: made by the reader in a group that has more than one clipping path, to hold
    /// what the next of them clips. The paths clip together, one group in the other.
    bool clip_chain = false;
    bool guide = false;

    int image = -1; ///< Image: index into Document::images.
    int placed = -1; ///< Placed: index into Document::placed.
    int foreign = -1; ///< Foreign: index into Document::foreign.
    Mesh mesh; ///< Mesh: bounded, independently drawn patches.
    std::optional<unsigned> story; ///< Text: its story in the text document.
    /// Text: drawn by an appearance (FreeUndo), not typed. Its story is one of the text
    /// document's drawn stories (TextDocument::drawn), not of those that were typed.
    bool text_proxy = false;
    /// Only while the records are read: the object that a drawn look holding type stands
    /// for, kept until the stories of both can be compared. Empty in what read() returns.
    std::vector<Node> typed;
    std::optional<double> box_rotation; ///< BBAccumRotation (radians), when the box is turned.
    /// Group: an object with several fills or strokes, effects or a brush, read as
    /// its drawn look.
    bool drawn_look = false;
    /// Group: what an envelope (Object > Envelope Distort) drew of the object it distorts.
    /// It is a drawn look too. The object itself, type or art, isn't kept.
    bool envelope = false;
    /// Group: what a plug-in drew of an object it keeps live (a compound shape, a blend, a
    /// brush stroke), under the plug-in's name as the records give it. What the object was
    /// made from, written beside it on %_ lines, is no art and isn't kept.
    std::string plugin;
    /// Read from %_ lines (Illustrator's own copy of an object).
    bool commented = false;
};

/**
 * A pattern (%AI3_BeginPattern): a tile of art that repeats, in a space of its own.
 *
 * The tile is the box of the definition; art that reaches past it is cut off there (the
 * pattern editor adds the neighbouring tiles' copies so that the cut edges join up).
 */
struct Pattern
{
    std::string name;
    Geom::Rect tile;       ///< The pattern's own space (y up).
    std::vector<Node> art; ///< In that space, bottom first.
    /// Why its art can't be used, when it holds something that isn't read. Only an object
    /// painted with it is refused for that.
    std::string unread;
};

struct Artboard
{
    std::string name;
    Geom::Rect rect; ///< Art space.
};

struct ForeignObject
{
    std::string pdf;
    Geom::Rect bounds; ///< Native object space (y down).
    Geom::Affine to_art; ///< Normalized PDF space (y up) to native art space.
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
    std::vector<Pattern> patterns;
    std::vector<Image> images;
    std::vector<Placed> placed;
    std::vector<ForeignObject> foreign;
    std::shared_ptr<TextDocument const> texts; ///< Bounded point-type stories, when text slots occur.
    std::optional<Geom::Point> template_center; ///< Centre used by the text document's canvas.
    std::optional<Geom::Rect> bbox; ///< %%HiResBoundingBox, else %%BoundingBox (art space).
    /// The records list no artboard and have no crop marks: the one artboard has the
    /// document's size, and its place is a guess from the document's centre.
    bool board_guessed = false;
    std::string creator_version;    ///< %%AI8_CreatorVersion.
    /// %AI5_RulerUnits: 0 inches, 1 millimetres, 2 points, 3 picas, 4 centimetres, 6 pixels.
    std::optional<int> ruler_units;
    std::size_t drawn_looks = 0;
    std::size_t envelopes = 0; ///< Objects an envelope distorts, read as what it drew.
    /// Objects a plug-in keeps live, read as what it drew, by the plug-in's name. Those in a
    /// drawn look, or that are one, count there.
    std::map<std::string, std::size_t> plugin_objects;
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
    std::size_t max_mesh_patches = 4096; ///< In one mesh; dimensions are checked before allocating.
    std::size_t max_foreign_bytes = 64u << 20; ///< Total decoded foreign PDFs in one document.
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

/**
 * Put the one artboard of a document whose records list none where its page lies.
 *
 * Such a document (Illustrator CS3 and older, and files kept in that form) says how large
 * it is and where its centre is, and the artboard guessed from those is often a fraction of
 * a point from where the art was drawn for, and now and then far from it. The file's PDF
 * part knows better: `page` is its page and `art` the bounds of the art on that page, both
 * in the page's coordinates, and the records give the same bounds in art space
 * (Document::bbox). Along each axis on which the two bounds are as large as each other,
 * they are the same art, uncut by the page, and say where the page lies.
 *
 * Nothing changes for a document with listed artboards, one whose page isn't the size of
 * its artboard, or along an axis on which the bounds differ.
 *
 * @return whether the artboard moved.
 */
bool place_board_by_page(Document &doc, Geom::Rect const &page, Geom::Rect const &art);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_EXTENSION_INTERNAL_AI_NATIVE_READER_H
