// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: make a document from Illustrator's native records. See ai-native-import.h.
 */
#include "ai-native-import.h"
#include "ai-native-fonts.h"
#include "ai-native-text.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef WITH_POPPLER
#include "extension/internal/pdfinput/ai-native-foreign.h"
#ifdef WITH_POPPLER
#include "extension/internal/pdfinput/placed-page.h"
#endif
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <functional>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <lcms2.h>
#include <zlib.h>

#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/transforms.h>

#include "box-frame.h"
#include "colors/cms/profile.h"
#include "colors/color.h"
#include "colors/document-cms.h"
#include "colors/document-colors.h"
#include "colors/manager.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/enum.h"
#include "colors/spaces/lab.h"
#include "display/psd-pixels.h"
#include "display/tiff-pixels.h"
#include "document-undo.h"
#include "document.h"
#include "io/link-policy.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "object/color-profile.h"
#include "object/sp-defs.h"
#include "object/sp-guide.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "spot-ink.h"
#include "style.h"
#include "svg/css-ostringstream.h"
#include "svg/svg.h"
#include "util/units.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;

constexpr char const *BLEND_MODES[] = {"normal",     "multiply",   "screen",      "overlay",
                                       "soft-light", "hard-light", "color-dodge", "color-burn",
                                       "darken",     "lighten",    "difference",  "exclusion",
                                       "hue",        "saturation", "color",       "luminosity"};
constexpr char const *CAPS[] = {"butt", "round", "square"};
constexpr char const *JOINS[] = {"miter", "round", "bevel"};

/// The ink PDF calls registration: it prints on every plate.
constexpr char const *REGISTRATION_INK = "All";

bool finite(Geom::Affine const &m)
{
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

std::string num(double v)
{
    CSSOStringStream os;
    os << v;
    return os.str();
}

/// The unit of %AI5_RulerUnits.
char const *unit_of(std::optional<int> ruler)
{
    switch (ruler.value_or(2)) {
        case 0:
            return "in";
        case 1:
            return "mm";
        case 3:
            return "pc";
        case 4:
            return "cm";
        case 6:
            return "px";
        default:
            return "pt";
    }
}

/// An ink's definition in the model it was defined in, unmanaged, as spot-ink.h keeps it.
Colors::Color definition(Color const &c)
{
    switch (c.model) {
        case Color::Model::Cmyk:
            return Colors::Color(Type::CMYK, {c.v[0], c.v[1], c.v[2], c.v[3]});
        case Color::Model::Rgb:
            return Colors::Color(Type::RGB, {c.v[0], c.v[1], c.v[2]});
        case Color::Model::Gray:
            return Colors::Color(Type::Gray, {c.v[0]});
        case Color::Model::Lab: {
            // Inkscape keeps L/100 and a, b scaled from [-128, 127].
            using Lab = Colors::Space::Lab;
            auto const span = Lab::MAX_SCALE - Lab::MIN_SCALE;
            return Colors::Color(Type::LAB, {std::clamp(c.v[0] / Lab::LUMA_SCALE, 0.0, 1.0),
                                             std::clamp((c.v[1] - Lab::MIN_SCALE) / span, 0.0, 1.0),
                                             std::clamp((c.v[2] - Lab::MIN_SCALE) / span, 0.0, 1.0)});
        }
    }
    return Colors::Color(Type::RGB, {0.0, 0.0, 0.0});
}

/// A text object's story: of those typed, or of those appearances drew.
using StoryKey = std::pair<bool, std::size_t>;

/// The font that sets what a text document names.
struct TextFont
{
    FontMatch font;
    bool missing = false; ///< It isn't installed: `font` stands in for it.
};

/// The font for each font name the text uses; nothing when none can set it.
using Fonts = std::map<std::string, std::optional<TextFont>>;

/// What looking through the text finds, for building it.
struct TextCheck
{
    Fonts fonts;
    std::set<StoryKey> unsupported_hidden; ///< Stories that don't show and can't be set.
    std::vector<std::string> missing;      ///< The fonts that aren't installed, in the order met.
    /// Stories with characters their font lacks, which another font draws.
    std::set<StoryKey> glyphs_missing;
    /// Stories that show and are set by a font other than their own.
    std::set<StoryKey> substituted_shown;
    /// Where Illustrator set those, in the document's user units.
    std::vector<std::array<Geom::Point, 4>> substituted_type;
};

std::string check_text(Document const &ai, std::vector<Node> const &nodes, bool shown, TextCheck &check)
{
    auto &fonts = check.fonts;
    for (auto const &n : nodes) {
        bool const visible = shown && n.visible;
        if (n.kind == Node::Kind::Text) {
            std::string error;
            auto const *stories = ai.texts ? (n.text_proxy ? &ai.texts->drawn : &ai.texts->stories) : nullptr;
            auto const *errors = ai.texts ? (n.text_proxy ? &ai.texts->drawn_errors : &ai.texts->errors) : nullptr;
            if (!ai.texts) error = "the text document is missing";
            else if (!ai.texts->error.empty()) error = ai.texts->error;
            else if (!n.story || *n.story >= stories->size()) error = "a text object has no matching story";
            else if (!(*stories)[*n.story]) error = (*errors)[*n.story];
            else {
                // A font that isn't installed doesn't refuse the text, nor does a character its
                // font lacks: another font sets it, as in Illustrator, and the text says so.
                auto const &story = *(*stories)[*n.story];
                bool substituted = false;
                for (auto const &line : story.lines) for (auto const &run : line.runs) {
                    auto const &name = run.style.font;
                    if (!fonts.count(name)) {
                        if (auto found = find_font(name, run.style.design)) {
                            fonts[name] = TextFont{std::move(*found), false};
                        } else if (auto stand_in = substitute_font()) {
                            fonts[name] = TextFont{std::move(*stand_in), true};
                            check.missing.push_back(name);
                        } else {
                            fonts[name] = std::nullopt;
                        }
                    }
                    for (auto const *used : {&run.style.fill_pattern, &run.style.stroke_pattern}) {
                        if (!*used) continue;
                        auto const pattern = std::find_if(ai.patterns.begin(), ai.patterns.end(),
                                                          [&](Pattern const &p) { return p.name == (*used)->name; });
                        if (pattern == ai.patterns.end()) {
                            error = "type is painted with a pattern that isn't in the file";
                        } else if (!pattern->unread.empty()) {
                            error = "type is painted with a pattern that has " + pattern->unread;
                        }
                    }
                    auto const &font = fonts[name];
                    if (!font) {
                        error = "the font '" + name + "' isn't installed, and no font could be loaded in its place";
                    } else if (font->missing) {
                        substituted = true;
                    } else for (auto const *at = run.text.data(); at < run.text.data() + run.text.size(); at = g_utf8_next_char(at)) {
                        // The character that is set: its capital under All Caps, and for small
                        // caps made of capitals.
                        auto const cp = run.style.all_caps || run.style.small_caps ? g_unichar_toupper(g_utf8_get_char(at))
                                                                                  : g_utf8_get_char(at);
                        // A paragraph tab has an advance, supplied by layout, and no glyph.
                        // It must not exclude the story from the page comparison as missing type.
                        if (cp != '\t' && g_unichar_type(cp) != G_UNICODE_FORMAT && !font->font.face->MapUnicodeChar(cp)) {
                            check.glyphs_missing.insert({n.text_proxy, *n.story});
                            substituted = true;
                            break;
                        }
                    }
                }
                if (error.empty() && substituted && visible && !story.empty) {
                    check.substituted_shown.insert({n.text_proxy, *n.story});
                    for (auto const &box : story.set) {
                        std::array<Geom::Point, 4> corners;
                        for (unsigned i = 0; i < 4; ++i) corners[i] = box.corner(i) * story.to_art * ai.to_doc;
                        check.substituted_type.push_back(corners);
                    }
                }
            }
            if (visible && !error.empty()) return "its visible text can't be reconstructed: " + error;
            if (!visible && !error.empty() && n.story) check.unsupported_hidden.insert({n.text_proxy, *n.story});
        } else if (auto error = check_text(ai, n.children, visible, check); !error.empty()) return error;
    }
    return {};
}

/// Every path of an object and its children, in art space.
void outline(Node const &n, Geom::PathVector &out, std::string *nodetypes = nullptr)
{
    for (auto const &p : n.path) out.push_back(p);
    if (nodetypes) {
        std::size_t count = 0;
        for (auto const &p : n.path) if (!p.empty()) count += p.size() + 1;
        // Generated outlines have no authored types: let the node tool choose its
        // best fit for them, without shifting a later child's explicit types.
        *nodetypes += n.nodetypes.size() == count ? n.nodetypes : std::string(count, 'b');
    }
    for (auto const &k : n.children) outline(k, out, nodetypes);
}

/**
 * The kids that tile: flat-coloured, unstroked shapes of straight edges that meet a
 * neighbour along a whole edge. Illustrator leaves them when a gradient is expanded
 * into strips; mosaics are made of them.
 *
 * Each shape is antialiased on its own, so a pixel on a shared edge is covered in part
 * by each tile and the background shows through: a hairline along every joint. Drawn
 * without antialiasing, tiles share their pixels out exactly. That is only right where
 * a tile's other edges can't turn ragged for it: they must run along the pixel rows and
 * columns, or lie where the group's clipping path cuts them away. The clipping path
 * itself is still drawn smooth.
 */
std::set<std::size_t> tiles(std::vector<Node> const &kids, std::size_t start, Node const *clip, Geom::Affine const &to_doc)
{
    using Edge = std::array<std::int64_t, 4>; // Both ends in thousandths of a point, the lesser first.
    struct Side
    {
        Edge edge;
        Geom::Point a, b;
    };
    constexpr std::size_t MOST_SIDES = 64;
    auto thousandths = [](double v) { return static_cast<std::int64_t>(std::llround(v * 1000.0)); };

    std::map<std::size_t, std::vector<Side>> shapes;
    std::map<Edge, std::vector<std::size_t>> users;
    for (std::size_t i = start; i < kids.size(); ++i) {
        auto const &n = kids[i];
        if (n.kind != Node::Kind::Path || n.guide || !n.visible || n.stroke || !n.fill || n.opacity_mask ||
            n.fill->kind != Paint::Kind::Solid || n.transparency.opacity < 1.0 || n.transparency.blend != 0) {
            continue;
        }
        auto const pv = n.path * to_doc;
        auto const box = pv.boundsFast();
        if (!box || box->width() < 1e-3 || box->height() < 1e-3) continue;
        std::vector<Side> sides;
        bool straight = true;
        auto add = [&](Geom::Point const &a, Geom::Point const &b) {
            Edge edge{thousandths(a.x()), thousandths(a.y()), thousandths(b.x()), thousandths(b.y())};
            if (edge[0] == edge[2] && edge[1] == edge[3]) return;
            if (std::pair(edge[0], edge[1]) > std::pair(edge[2], edge[3])) {
                std::swap(edge[0], edge[2]);
                std::swap(edge[1], edge[3]);
            }
            sides.push_back({edge, a, b});
        };
        for (auto const &path : pv) {
            for (std::size_t c = 0; c < path.size_default() && straight; ++c) {
                straight = path[c].isLineSegment() && sides.size() < MOST_SIDES;
                if (straight) add(path[c].initialPoint(), path[c].finalPoint());
            }
            // A fill closes an open path.
            if (straight && !path.closed() && !path.empty()) add(path.finalPoint(), path.initialPoint());
        }
        if (!straight || sides.empty()) continue;
        for (auto const &s : sides) {
            auto &list = users[s.edge];
            if (list.empty() || list.back() != i) list.push_back(i);
        }
        shapes.emplace(i, std::move(sides));
    }
    if (shapes.size() < 2) return {};

    std::optional<Geom::PathVector> mask;
    if (clip) {
        Geom::PathVector pv;
        outline(*clip, pv);
        mask = pv * to_doc;
    }
    // Whether the clipping path leaves `p` showing.
    auto shows = [&](Geom::Point const &p) {
        try {
            int const turns = mask->winding(p);
            return clip->evenodd ? turns % 2 != 0 : turns != 0;
        } catch (...) {
            return true;
        }
    };
    // A shape whose every unshared edge is safe to draw without antialiasing.
    std::set<std::size_t> safe;
    for (auto const &[i, sides] : shapes) {
        Geom::Point centre;
        for (auto const &s : sides) centre += s.a;
        centre /= double(sides.size());
        bool ok = true;
        for (auto const &s : sides) {
            if (users[s.edge].size() > 1) continue;
            auto const d = s.b - s.a;
            if (std::abs(d.x()) < 1e-3 || std::abs(d.y()) < 1e-3) continue;
            ok = false;
            if (!mask) break;
            // Slanted: fine only where the clipping path hides it, just inside the shape too.
            ok = true;
            for (double t : {0.1, 0.3, 0.5, 0.7, 0.9}) {
                auto p = Geom::lerp(t, s.a, s.b);
                auto const inward = centre - p;
                if (inward.length() > 1e-6) p += inward * (0.05 / inward.length());
                ok = ok && !shows(p);
            }
            if (!ok) break;
        }
        if (ok) safe.insert(i);
    }
    std::set<std::size_t> found;
    for (auto const i : safe) {
        bool joined = false;
        for (auto const &s : shapes[i]) {
            for (auto const other : users[s.edge]) joined = joined || (other != i && safe.count(other));
        }
        if (joined) found.insert(i);
    }
    return found;
}

std::string base64(std::string_view bytes)
{
    gchar *b = g_base64_encode(reinterpret_cast<guchar const *>(bytes.data()), bytes.size());
    std::string out(b);
    g_free(b);
    return out;
}

/// PNG of 8-bit RGB (3 channels) or unassociated RGBA (4) pixels.
std::optional<std::string> png(std::vector<std::uint8_t> const &pixels, int w, int h, int channels)
{
    auto *pb = gdk_pixbuf_new_from_data(pixels.data(), GDK_COLORSPACE_RGB, channels == 4, 8, w, h, w * channels,
                                        nullptr, nullptr);
    if (!pb) return {};
    gchar *buf = nullptr;
    gsize len = 0;
    GError *err = nullptr;
    // A document for print holds hundreds of megabytes of samples, and opening it waits for
    // this: a light setting packs them several times faster for a tenth more bytes.
    bool const ok = gdk_pixbuf_save_to_buffer(pb, &buf, &len, "png", &err, "compression", "3", nullptr);
    g_object_unref(pb);
    if (!ok) {
        if (err) g_error_free(err);
        return {};
    }
    std::string out(buf, len);
    g_free(buf);
    return out;
}

/// A grey (`channels` 1, black at 0) or CMYK (4) TIFF (Deflate) of 8-bit samples, with
/// unassociated alpha when given.
std::optional<std::string> samples_tiff(std::uint32_t w, std::uint32_t h, std::string_view colour, int channels,
                                        std::string_view alpha)
{
    std::uint64_t const pixels = std::uint64_t(w) * h;
    if ((channels != 1 && channels != 4) || colour.size() < pixels * channels) return {};
    bool const with_alpha = alpha.size() >= pixels;
    std::uint16_t const spp = channels + (with_alpha ? 1 : 0);
    std::string raw;
    if (with_alpha) {
        raw.resize(pixels * spp);
        for (std::uint64_t i = 0; i < pixels; ++i) {
            std::copy_n(colour.data() + i * channels, channels, raw.data() + i * spp);
            raw[i * spp + channels] = alpha[i];
        }
    } else {
        raw.assign(colour.substr(0, pixels * channels));
    }
    uLongf len = compressBound(raw.size());
    std::string z(len, '\0');
    if (compress2(reinterpret_cast<Bytef *>(z.data()), &len, reinterpret_cast<Bytef const *>(raw.data()), raw.size(),
                  3) != Z_OK) {
        return {};
    }
    z.resize(len);

    struct Entry
    {
        std::uint16_t tag, type;
        std::uint32_t count, value;
    };
    constexpr std::uint16_t SHORT = 3, LONG = 4;
    bool const cmyk = channels == 4;
    std::vector<Entry> e = {{256, LONG, 1, w},        {257, LONG, 1, h},
                            {258, SHORT, spp, 0},     {259, SHORT, 1, 8}, // Adobe Deflate
                            {262, SHORT, 1, cmyk ? 5u : 1u}, {273, LONG, 1, 0}, // separated, or black at 0
                            {277, SHORT, 1, spp},     {278, LONG, 1, h},
                            {279, LONG, 1, std::uint32_t(z.size())}, {284, SHORT, 1, 1}};
    if (cmyk) e.push_back({332, SHORT, 1, 1});                            // InkSet: CMYK
    if (with_alpha) e.push_back({338, SHORT, 1, 2});                      // unassociated alpha
    std::uint32_t const ifd = 8;
    std::uint32_t const bits = ifd + 2 + 12 * std::uint32_t(e.size()) + 4;
    std::uint32_t const strip = bits + (spp > 2 ? 2 * spp : 0);
    // Up to two shorts fit in the entry itself (TIFF 6.0, section 2).
    e[2].value = spp > 2 ? bits : spp == 1 ? 8u : 8u | (8u << 16);
    e[5].value = strip;

    std::string out = "II";
    auto put16 = [&](std::uint16_t v) {
        out.push_back(char(v & 0xff));
        out.push_back(char(v >> 8));
    };
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xff));
    };
    put16(42);
    put32(ifd);
    put16(std::uint16_t(e.size()));
    for (auto const &x : e) {
        put16(x.tag);
        put16(x.type);
        put32(x.count);
        if (x.type == SHORT && x.count == 1) {
            put16(std::uint16_t(x.value));
            put16(0);
        } else {
            put32(x.value);
        }
    }
    put32(0);
    if (spp > 2) for (int i = 0; i < spp; ++i) put16(8);
    out += z;
    return out;
}

/// Interior points u (0-1) where u^n is sampled so that the blend between them stays within
/// half a percent of the curve.
void curve_samples(double n, double u0, double u1, int depth, std::vector<double> &out)
{
    double const um = (u0 + u1) / 2;
    double const chord = (std::pow(u0, n) + std::pow(u1, n)) / 2;
    if (depth >= 7 || std::abs(std::pow(um, n) - chord) < 0.005) return;
    curve_samples(n, u0, um, depth + 1, out);
    out.push_back(um);
    curve_samples(n, um, u1, depth + 1, out);
}

int lcms_intent(Colors::RenderingIntent intent)
{
    using Colors::RenderingIntent;
    switch (intent) {
        case RenderingIntent::RELATIVE_COLORIMETRIC:
        case RenderingIntent::RELATIVE_COLORIMETRIC_NOBPC:
            return INTENT_RELATIVE_COLORIMETRIC;
        case RenderingIntent::SATURATION:
            return INTENT_SATURATION;
        case RenderingIntent::ABSOLUTE_COLORIMETRIC:
            return INTENT_ABSOLUTE_COLORIMETRIC;
        default:
            return INTENT_PERCEPTUAL;
    }
}

/// A gradient stop's colour: a process colour, or a tint of one of the document's inks.
struct StopPaint
{
    Color color;
    int ink = -1;
    double tint = 1.0;
};

/// The last part of a file name written with either kind of separator.
std::string base_name(std::string const &name)
{
    auto const at = name.find_last_of("/\\");
    return at == std::string::npos ? name : name.substr(at + 1);
}

/// A file name's bytes as text, in each encoding they may be in: Illustrator writes the
/// name the way the system that saved the document gave it.
std::vector<std::string> decodings(std::string const &bytes)
{
    std::vector<std::string> out;
    auto add = [&](std::string s) {
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
    };
    if (g_utf8_validate(bytes.data(), bytes.size(), nullptr)) add(bytes);
    gsize written = 0;
    if (auto *s = g_locale_to_utf8(bytes.data(), bytes.size(), nullptr, &written, nullptr)) {
        add(std::string(s, written));
        g_free(s);
    }
    add(text_of(bytes));
    return out;
}

class Builder
{
public:
    Builder(Document const &ai, SPDocument *doc, TextCheck text, Source const &source);
    ~Builder();
    Builder(Builder const &) = delete;
    Builder &operator=(Builder const &) = delete;

    /// Add every layer to the document.
    void run();

    std::vector<Geom::Path> guides; ///< In the document's user units.
    std::size_t hidden_text = 0;
    std::size_t empty_text = 0;
    std::size_t overflowing_text = 0;
    std::size_t optical_kerning = 0;
    std::size_t substituted_text = 0;   ///< Text objects set in a font standing in for theirs.
    /// Text objects with letters read from the right that are set from the left, as typed.
    std::size_t unordered_text = 0;
    /// Text objects painted with a gradient, which is black on type.
    std::size_t gradient_text = 0;
    std::size_t unused_gradient_end_stops = 0;
    std::size_t glyphs_missing_text = 0; ///< Text objects with characters their font lacks.
    std::size_t path_text = 0; ///< Parameter brackets expanded to editable path distances.
    std::size_t path_options = 0; ///< Source options retained; automatic curve spacing approximated.
    std::vector<XML::Node *> automatic_path_text; ///< Must have a supported live spacing metric after layout.
    /// Lines of justified area type, and how far each reaches: stretched once laid out (justify).
    std::vector<std::pair<XML::Node *, double>> justified_lines;
    std::size_t paragraph_styled_text = 0; ///< Named paragraph style links expanded inline.
    std::size_t mesh_outlines = 0; ///< Uniform mesh patches with extra boundary nodes.
    std::size_t mesh_cut = 0; ///< Mesh patches bent through anchors, cut into cells there.
    std::size_t mesh_split = 0; ///< Meshes drawn patch by patch: not one grid, or too many cells.
    std::size_t foreign_objects = 0;
    std::size_t hidden_foreign = 0;
    std::string foreign_error;
    /// The text objects that show and are set by a font other than their own.
    std::vector<XML::Node *> substituted_shown;
    std::size_t knockouts = 0;
    std::size_t images_left_out = 0;
    /// A linked file that shows and can't be placed: why. The first one found.
    std::string link_error;
    std::size_t hidden_links_missing = 0;
    /// The linked pictures that show and can't be found.
    std::vector<MissingLink> missing_links;
    /// The linked pictures that show, are found, and of which the document keeps a picture.
    std::vector<FoundLink> found_links;
    /// Linked CMYK TIFFs that carry a profile other than the document's: their file names, and
    /// the profiles' names. Their numbers are kept (Illustrator's "Preserve Numbers").
    std::map<std::string, std::string> other_profiles;
    /// The shares of linked files that weren't looked up, being on another computer
    /// (io/link-policy.h).
    std::set<std::string> elsewhere;

private:
    XML::Node *create(char const *name) { return _xml->createElement(name); }
    static void append(XML::Node *parent, XML::Node *child)
    {
        parent->appendChild(child);
        GC::release(child);
    }
    std::string unique(std::string const &stem);

    XML::Node *node(Node const &n);
    XML::Node *made(Node const &n);
    XML::Node *layer(Node const &n);
    XML::Node *group(Node const &n);
    XML::Node *shape(Node const &n, Geom::PathVector const &pv, std::string const &nodetypes);
    XML::Node *mesh(Node const &n);
    XML::Node *whole_mesh(Node const &n);
    XML::Node *mesh_cells(MeshPatch const &p);
    XML::Node *mesh_grid(std::size_t across, std::size_t up, std::vector<MeshPatch const *> const &grid,
                         std::vector<std::vector<double>> const &us, std::vector<std::vector<double>> const &vs);
    XML::Node *foreign(Node const &n);
    XML::Node *image(Node const &n);
    XML::Node *placed(Node const &n);
    std::optional<std::string> linked_file(Placed const &p);
    XML::Node *text(Node const &n);
    void children(XML::Node *parent, std::vector<Node> const &kids, bool clipped);
    std::string clip_path(Node const &clip);
    XML::Node *masked(XML::Node *art, std::vector<Node> const &mask, bool clipped);
    void common(XML::Node *e, Node const &n, std::string &style);

    Colors::Color process(Color const &c) const;
    std::string paint(Paint const &p);
    std::string ink(int index, double tint);
    std::string ink_swatch(Ink const &ink, double tint);
    std::string text_ink(TextInk const &used);
    std::string process_swatch(Ink const &ink, double tint);
    std::string gradient(Paint const &p);
    std::string gradient_vector(int index);
    std::string pattern(Paint const &p);
    std::string pattern_tile(int index);
    Colors::Color shown(StopPaint const &s) const;
    void add_stop(XML::Node *vector, double offset, StopPaint const *paint, Colors::Color const &color, double opacity,
                  bool sample);

    std::vector<std::uint8_t> screen_pixels(Image const &img, std::string &eight, int &channels);

    Document const &_ai;
    SPDocument *_doc;
    XML::Document *_xml;
    XML::Node *_defs;
    Geom::Affine _to_doc;
    std::shared_ptr<Colors::Space::AnySpace> _cmyk_space; ///< The assigned CMYK profile, if any.
    std::shared_ptr<Colors::CMS::Profile> _srgb;
    cmsHTRANSFORM _cmyk_to_rgb = nullptr;
    std::map<std::pair<int, double>, std::string> _inks; ///< (ink, tint) to its paint.
    std::vector<std::pair<TextInk, std::string>> _text_inks; ///< Inks only type uses, to their paint.
    std::map<std::pair<int, bool>, std::string> _vectors; ///< (Gradient, mask coverage) to vector id.
    std::map<int, std::string> _tiles;                 ///< Pattern to its tile's id.
    /// An image as the document holds it: for the screen, and its CMYK samples for print.
    struct Encoded
    {
        int image = -1; ///< The first image that had these samples.
        std::string screen, samples;
    };
    std::unordered_multimap<std::size_t, Encoded> _encoded; ///< By a digest of the samples.
    std::map<std::string, int> _counters;
    Fonts _text_fonts;
    std::set<StoryKey> _unsupported_hidden_text, _glyphs_missing, _substituted_shown;
    std::string _folder;       ///< The .ai file's folder, for linked pictures.
    std::size_t _hidden = 0;   ///< How many of the objects being built into don't show.
    std::size_t _in_tile = 0;  ///< How many pattern tiles are being built into.
    bool _in_opacity_mask = false; ///< Greys are coverage, not black printing ink.
    std::shared_ptr<Colors::Space::AnySpace> _mask_space;
};

Builder::Builder(Document const &ai, SPDocument *doc, TextCheck text, Source const &source)
    : _ai(ai)
    , _doc(doc)
    , _xml(doc->getReprDoc())
    , _defs(doc->getDefs()->getRepr())
    , _to_doc(ai.to_doc)
    , _text_fonts(std::move(text.fonts))
    , _unsupported_hidden_text(std::move(text.unsupported_hidden))
    , _glyphs_missing(std::move(text.glyphs_missing))
    , _substituted_shown(std::move(text.substituted_shown))
    , _folder(source.folder)
{
    // CMYK pictures are shown through the document's CMYK profile; in an RGB document, through
    // the one DeviceCMYK paint is shown through (DocumentColors::showDeviceCMYK), as Illustrator
    // shows them through its working CMYK space.
    auto space = Colors::DocumentColors::assignedSpace(doc);
    std::shared_ptr<Colors::CMS::Profile> shown_by;
    if (space && space->getComponentType() == Type::CMYK) {
        _cmyk_space = space;
        shown_by = space->getProfile();
    } else if (auto device = Colors::Manager::get().find(Type::CMYK)) {
        if (auto profile = device->getProfile(); profile && profile->getColorSpace() == cmsSigCmykData) {
            shown_by = profile;
        }
    }
    if (shown_by) {
        _srgb = Colors::CMS::Profile::create_srgb();
        auto const intent = Colors::DocumentColors::workingIntent();
        cmsUInt32Number const flags =
            intent == Colors::RenderingIntent::RELATIVE_COLORIMETRIC ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0;
        _cmyk_to_rgb = cmsCreateTransform(shown_by->getHandle(), TYPE_CMYK_8, _srgb->getHandle(), TYPE_RGB_8,
                                          lcms_intent(intent), flags);
    }
}

Builder::~Builder()
{
    if (_cmyk_to_rgb) cmsDeleteTransform(_cmyk_to_rgb);
}

std::string Builder::unique(std::string const &stem)
{
    for (;;) {
        auto id = stem + std::to_string(++_counters[stem]);
        if (!_doc->getObjectById(id)) return id;
    }
}

void Builder::run()
{
    auto *root = _doc->getReprRoot();
    // Keep the document's named inks even when no object uses them.
    for (std::size_t i = 0; i < _ai.inks.size(); ++i) ink(int(i), 1.0);
    for (auto const &l : _ai.layers) {
        if (auto e = node(l)) append(root, e);
    }
    // And its patterns. One nothing is painted with must not cost the document for
    // something its tile holds: such a tile is taken out again. Nor is what its tile holds
    // worth a note: nothing shows it.
    for (std::size_t i = 0; i < _ai.patterns.size(); ++i) {
        if (_tiles.count(int(i)) || !_ai.patterns[i].unread.empty()) continue;
        auto const images = images_left_out;
        auto const error = link_error;
        auto const foreign = foreign_error;
        auto const auto_paths = automatic_path_text.size();
        auto const justified = justified_lines.size();
        auto const counts = std::tuple(hidden_text, empty_text, overflowing_text, optical_kerning, knockouts,
                                       hidden_links_missing, substituted_text, glyphs_missing_text, unordered_text,
                                       gradient_text, paragraph_styled_text, path_text, path_options, mesh_outlines, mesh_cut, mesh_split,
                                       foreign_objects, hidden_foreign, unused_gradient_end_stops);
        auto const id = pattern_tile(int(i));
        automatic_path_text.resize(auto_paths); // These definitions are not painted with.
        justified_lines.resize(justified);
        bool const placed = images_left_out == images && link_error == error && foreign_error == foreign;
        images_left_out = images;
        link_error = error;
        foreign_error = foreign;
        std::tie(hidden_text, empty_text, overflowing_text, optical_kerning, knockouts, hidden_links_missing,
                 substituted_text, glyphs_missing_text, unordered_text, gradient_text, paragraph_styled_text, path_text, path_options, mesh_outlines,
                 mesh_cut, mesh_split, foreign_objects, hidden_foreign, unused_gradient_end_stops) = counts;
        if (placed) continue;
        for (auto *child = _defs->firstChild(); child; child = child->next()) {
            auto const *child_id = child->attribute("id");
            if (child_id && id == child_id) {
                _defs->removeChild(child);
                break;
            }
        }
    }
}

XML::Node *Builder::node(Node const &n)
{
    // What a hidden object holds doesn't show either.
    bool const hidden = !n.visible;
    if (hidden) ++_hidden;
    auto *e = made(n);
    if (e && n.opacity_mask) e = masked(e, *n.opacity_mask, n.opacity_mask_clipped);
    if (hidden) --_hidden;
    return e;
}

XML::Node *Builder::made(Node const &n)
{
    switch (n.kind) {
        case Node::Kind::Layer:
            return layer(n);
        case Node::Kind::Group:
            return group(n);
        case Node::Kind::Compound: {
            Geom::PathVector pv;
            std::string nodetypes;
            outline(n, pv, &nodetypes);
            return shape(n, pv, nodetypes);
        }
        case Node::Kind::Path:
            if (n.guide) {
                for (auto const &p : n.path) guides.push_back(p * _to_doc);
                return nullptr;
            }
            return shape(n, n.path, n.nodetypes);
        case Node::Kind::Image:
            return image(n);
        case Node::Kind::Placed:
            return placed(n);
        case Node::Kind::Text:
            return text(n);
        case Node::Kind::Mesh:
            return mesh(n);
        case Node::Kind::Foreign:
            return foreign(n);
    }
    return nullptr;
}

XML::Node *Builder::text(Node const &n)
{
    auto const *stories = _ai.texts ? (n.text_proxy ? &_ai.texts->drawn : &_ai.texts->stories) : nullptr;
    if (!stories || !n.story || *n.story >= stories->size() || !(*stories)[*n.story] ||
        _unsupported_hidden_text.count({n.text_proxy, *n.story})) {
        ++hidden_text; return nullptr;
    }
    auto const &story = *(*stories)[*n.story];
    if (story.empty) { ++empty_text; return nullptr; }
    for (auto const &line : story.lines) for (auto const &run : line.runs) {
        auto it = _text_fonts.find(run.style.font);
        if (it == _text_fonts.end() || !it->second) { ++hidden_text; return nullptr; }
    }
    auto e = create("svg:text");
    if (story.paragraph_styles_expanded) ++paragraph_styled_text;
    if (story.path) ++path_text;
    if (story.path && !story.path->source_options.empty()) ++path_options;
    e->setAttribute("xml:space", "preserve");
    // Type an appearance drew isn't the object's own text: its story is told apart.
    e->setAttribute(n.text_proxy ? "proof:ai-drawn-story" : "proof:ai-story", std::to_string(*n.story));
    e->setAttribute("transform", sp_svg_transform_write(story.to_art * _to_doc));
    // Area type's paragraphs: how each is aligned, whichever end its lines are anchored at, and
    // whether it is hyphenated. What every line has goes on the text, else on each line. A
    // justified one says how its last line is aligned too, as CSS's text-align-last would.
    auto align_of = [&](TextLine const &l) -> char const * {
        if (l.justification) return "justify";
        return l.paragraph_alignment == 2 ? "center" : (l.paragraph_alignment == 1) != story.rtl ? "end" : "start";
    };
    auto last_of = [](TextLine const &l) -> char const * {
        if (!l.justification) return "";
        return l.justification == 6 ? "justify" : l.justification == 5 ? "center" : l.justification == 4 ? "end" : "start";
    };
    bool one_alignment = true, one_hyphenation = true;
    for (auto const &l : story.lines) {
        one_alignment = one_alignment && std::string_view(align_of(l)) == align_of(story.lines.front()) &&
                        std::string_view(last_of(l)) == last_of(story.lines.front());
        one_hyphenation = one_hyphenation && l.hyphenate == story.lines.front().hyphenate;
    }
    if (story.area) {
        // Area type, set as Illustrator laid it out. Its frame is kept for what flows text
        // into it again; the lines don't move when the text is edited.
        e->setAttribute(TEXT_FRAME_ATTRIBUTE, sp_svg_write_path(story.frame));
        if (!story.overflow.empty()) {
            e->setAttribute(TEXT_OVERFLOW_ATTRIBUTE, story.overflow);
            ++overflowing_text;
        }
        if (one_hyphenation && !story.lines.empty() && story.lines.front().hyphenate) e->setAttribute(HYPHENATE_ATTRIBUTE, "true");
        if (story.first_baseline) {
            e->setAttribute(FIRST_BASELINE_ATTRIBUTE, std::to_string(story.first_baseline->first) + " " + num(story.first_baseline->second));
        }
    }
    // A run's style in parts, each inherited by spans. The text object carries the first
    // run's, so its fill, stroke and font are the object's own, as the Appearance controls
    // read and set them; a span carries only what its run has different.
    // Kerning and the substitutions a font makes unasked are the font's own unless a run
    // says otherwise; a story where none does says nothing about them.
    // Nor about a variable font's axes, unless one of its fonts is an instance of one: an
    // ordinary font beside it then says it has none, where it would inherit the instance's.
    // All Caps is the same: said for every run of a story that has any, and not otherwise.
    // So are small capitals, and superscript and subscript.
    bool features = false, optical = false, instances = false, capitals = false, small_capitals = false, stand_in = false;
    bool positions = false, gradients = false;
    for (auto const &line : story.lines) for (auto const &run : line.runs) {
        positions = positions || run.style.position;
        gradients = gradients || run.style.gradient;
        features = features || run.style.kerning == 0 || !run.style.ligatures || !run.style.contextual;
        optical = optical || run.style.kerning == 2;
        instances = instances || _text_fonts.at(run.style.font)->font.variable;
        capitals = capitals || run.style.all_caps;
        small_capitals = small_capitals || run.style.small_caps;
        stand_in = stand_in || _text_fonts.at(run.style.font)->missing;
    }
    if (optical) ++optical_kerning;
    if (gradients) ++gradient_text;
    if (stand_in) ++substituted_text;
    if (_glyphs_missing.count({n.text_proxy, *n.story})) {
        ++glyphs_missing_text;
        e->setAttribute(GLYPHS_MISSING_ATTRIBUTE, "true");
    }
    if (_substituted_shown.count({n.text_proxy, *n.story})) substituted_shown.push_back(e);
    // A run's style, and the font it stands in for when its own isn't installed.
    struct Parts
    {
        std::array<std::string, 13> css;
        std::string missing, axes;
        bool same_font(Parts const &o) const { return css[0] == o.css[0] && missing == o.missing && axes == o.axes; }
        void name(XML::Node *element) const
        {
            if (missing.empty()) return;
            element->setAttribute(FONT_MISSING_ATTRIBUTE, missing);
            if (!axes.empty()) element->setAttribute(FONT_MISSING_AXES_ATTRIBUTE, axes);
        }
    };
    // A pattern on type is placed as on a path, in art space. The text has a matrix of its
    // own, so the pattern is written for the space inside it.
    std::vector<std::pair<TextPattern, std::string>> patterns;
    auto patterned = [&](TextPattern const &used) {
        for (auto const &[known, url] : patterns) {
            if (known == used) return url;
        }
        Paint p;
        p.kind = Paint::Kind::Pattern;
        p.pattern_matrix = used.matrix;
        for (std::size_t i = 0; i < _ai.patterns.size(); ++i) {
            if (_ai.patterns[i].name == used.name) p.pattern = static_cast<int>(i);
        }
        auto const outer = std::exchange(_to_doc, story.to_art.inverse());
        auto url = pattern(p);
        _to_doc = outer;
        patterns.emplace_back(used, url);
        return url;
    };
    auto parts = [&](auto const &s) {
        auto color = [&](std::optional<Color> const &c, std::optional<TextPattern> const &pattern,
                         std::optional<TextInk> const &named) {
            if (c && pattern) return patterned(*pattern);
            if (c && named) return text_ink(*named);
            Paint p;
            if (c) p.color = *c;
            return c ? paint(p) : std::string("none");
        };
        std::string ligatures, kerning;
        if (features) {
            ligatures = std::string("font-variant-ligatures:") +
                        (s.ligatures ? (s.contextual ? "normal" : "no-contextual")
                                     : (s.contextual ? "no-common-ligatures" : "no-common-ligatures no-contextual")) + ";";
            kerning = std::string("font-feature-settings:") + (s.kerning == 0 ? "'kern' 0" : "normal") + ";";
        }
        auto const &text_font = *_text_fonts.at(s.font);
        auto const &font = text_font.font;
        Parts out;
        out.css = {
            font.css + (instances && !font.variable ? "font-variation-settings:normal;" : ""),
            // The type's own size: a superscript or subscript is drawn smaller by the layout.
            "font-size:" + num(s.size) + ";", "fill:" + color(s.fill, s.fill_pattern, s.fill_ink) + ";",
            "fill-opacity:" + num(s.fill_opacity) + ";", "stroke:" + color(s.stroke, s.stroke_pattern, s.stroke_ink) + ";",
            "stroke-opacity:" + num(s.stroke_opacity) + ";", "stroke-width:" + num(s.stroke_width) + ";",
            "letter-spacing:" + num(s.set_size() * s.tracking / 1000.0) + ";", ligatures, kerning,
            capitals ? std::string("text-transform:") + (s.all_caps ? "uppercase" : "none") + ";" : std::string(),
            small_capitals ? std::string("font-variant-caps:") + (s.small_caps ? "small-caps" : "normal") + ";" : std::string(),
            positions ? std::string("font-variant-position:") + (s.position == 1 ? "super" : s.position == 2 ? "sub" : "normal") + ";"
                      : std::string()};
        if (text_font.missing) {
            out.missing = s.font;
            for (auto const v : s.design) out.axes += (out.axes.empty() ? "" : " ") + num(v);
        }
        return out;
    };
    std::optional<Parts> base;
    for (auto const &line : story.lines) {
        if (!base && !line.runs.empty()) base = parts(line.runs.front().style);
    }
    std::string common_style = "text-anchor:start;";
    // Which only lines laid out in a frame follow: the lines here are placed by their anchors.
    if (story.area && one_alignment && !story.lines.empty()) {
        common_style += std::string("text-align:") + align_of(story.lines.front()) + ";";
        if (story.lines.front().justification) e->setAttribute(TEXT_ALIGN_LAST_ATTRIBUTE, last_of(story.lines.front()));
    }
    if (story.illustrator_alignment) e->setAttribute(Text::LINE_ALIGNMENT_ATTRIBUTE, "illustrator");
    e->setAttribute("proof:ai-text-composer", story.world_ready ? "middle-eastern" : "latin");
    if (story.rtl) common_style += "direction:rtl;";
    // Letters read from the right that Illustrator set from the left, as typed: the same here.
    if (story.unordered) {
        common_style += "direction:ltr;unicode-bidi:bidi-override;";
        ++unordered_text;
    }
    common(e, n, common_style);
    if (base) {
        for (auto const &part : base->css) common_style += part;
        base->name(e);
    }
    e->setAttribute("style", common_style);
    for (auto const &line : story.lines) {
        auto span = create(story.path ? "svg:textPath" : "svg:tspan");
        if (story.path) {
            auto p = create("svg:path");
            auto const id = unique("textPath");
            p->setAttribute("id", id);
            p->setAttribute("d", sp_svg_write_path(story.path->outline));
            append(_defs, p);
            span->setAttribute("xlink:href", "#" + id);
            auto const start = story.path->start, end = story.path->end;
            span->setAttribute("startOffset", num(line.alignment == 2 ? (start + end) / 2 : line.alignment == 1 ? end : start));
            span->setAttribute("proof:path-start", num(start));
            span->setAttribute("proof:path-end", num(end));
            if (story.path->automatic_spacing) {
                span->setAttribute("proof:path-spacing", "auto");
                automatic_path_text.push_back(e);
            }
            if (!story.path->source_options.empty()) {
                span->setAttribute("proof:ai-path-options", story.path->source_options);
            }
        }
        // Explicit ATE line positions must remain SVG positions. role="line"
        // asks Inkscape to reflow the line and discard a single x/y pair.
        if (!story.path) {
            span->setAttribute("x", num(line.origin.x())); span->setAttribute("y", num(line.origin.y()));
        }
        if (line.tabs) line.tabs->write(span);
        if (line.indents != std::array<double, 3>{}) {
            span->setAttribute(PARAGRAPH_INDENTS_ATTRIBUTE,
                               num(line.indents[0]) + " " + num(line.indents[1]) + " " + num(line.indents[2]));
        }
        // The line's origin is its left end, its right end or its middle. Which of those a
        // line starts at depends on the way it reads.
        char const *const anchor = line.alignment == 2 ? "middle" : (line.alignment == 1) != story.rtl ? "end" : "start";
        span->setAttribute("style", std::string("text-anchor:") + anchor + ";" +
                                        (story.area && !one_alignment ? std::string("text-align:") + align_of(line) + ";" : ""));
        if (story.area && !one_alignment && line.justification) span->setAttribute(TEXT_ALIGN_LAST_ATTRIBUTE, last_of(line));
        // A justified line is stretched across its measure once the document can lay it out.
        if (line.justified_width) justified_lines.emplace_back(span, *line.justified_width);
        if (line.end == TextLine::End::Wrap) span->setAttribute(LINE_END_ATTRIBUTE, "wrap");
        if (line.end == TextLine::End::Break) span->setAttribute(LINE_END_ATTRIBUTE, "break");
        if (story.area && !one_hyphenation && line.hyphenate) span->setAttribute(HYPHENATE_ATTRIBUTE, "true");
        std::string plain; // Text in the object's own style, not yet written.
        auto write_plain = [&] {
            if (!plain.empty()) append(span, _xml->createTextNode(plain.c_str()));
            plain.clear();
        };
        for (std::size_t i = 0; i < line.runs.size() + std::size_t(line.automatic_hyphen); ++i) {
            bool const automatic = i == line.runs.size();
            auto const &run = line.runs[automatic ? i - 1 : i];
            std::string const &contents = automatic ? std::string("-") : run.text;
            auto const own = parts(run.style);
            std::string style;
            // Another font, or another one that isn't installed, says its font again: the
            // name of a font that isn't installed goes with the declarations it stands for.
            bool const font_differs = !own.same_font(*base);
            for (std::size_t i = 0; i < own.css.size(); ++i) {
                if (i == 0 ? font_differs : own.css[i] != base->css[i]) style += own.css[i];
            }
            // A shift is taken from the baseline of what holds the span, and added to that
            // one's own: only the span the characters are in carries it. A superscript's or
            // subscript's own place is no part of it.
            if (run.style.baseline_shift != 0) style += "baseline-shift:" + num(run.style.baseline_shift) + ";";
            // So are the lines drawn under and through a run: they are the span's own.
            if (run.style.underline || run.style.strikethrough) {
                style += std::string("text-decoration-line:") + (run.style.underline ? "underline" : "") +
                         (run.style.underline && run.style.strikethrough ? " " : "") +
                         (run.style.strikethrough ? "line-through" : "") + ";";
            }
            if (style.empty() && !automatic) {
                plain += contents;
                continue;
            }
            write_plain();
            auto piece = create("svg:tspan");
            piece->setAttribute("style", style);
            if (automatic) piece->setAttribute(AUTOMATIC_HYPHEN_ATTRIBUTE, "true");
            if (font_differs) own.name(piece);
            append(piece, _xml->createTextNode(contents.c_str())); append(span, piece);
        }
        write_plain();
        append(e, span);
    }
    return e;
}

void Builder::common(XML::Node *e, Node const &n, std::string &style)
{
    if (!n.name.empty()) e->setAttribute("inkscape:label", n.name);
    if (n.locked) e->setAttribute("sodipodi:insensitive", "true");
    if (!n.visible) style += "display:none;";
    auto const &t = n.transparency;
    if (t.opacity < 1.0) style += "opacity:" + num(t.opacity) + ";";
    if (t.blend > 0 && t.blend < 16) style += std::string("mix-blend-mode:") + BLEND_MODES[t.blend] + ";";
    if (t.isolate) style += "isolation:isolate;";
    if (t.knockout) {
        e->setAttribute("proof:knockout", t.knockout == 1 ? "on" : "neutral");
        if (t.knockout == 1 && n.children.size() > 1) ++knockouts;
    }
    if (t.knockout_shape) e->setAttribute("proof:knockout-shape", "true");
}

XML::Node *Builder::layer(Node const &n)
{
    auto e = create("svg:g");
    e->setAttribute("inkscape:groupmode", "layer");
    e->setAttribute("inkscape:label", n.name.empty() ? std::string("Layer") : n.name);
    if (n.locked) e->setAttribute("sodipodi:insensitive", "true");
    if (!n.visible) e->setAttribute("style", "display:none");
    auto const &o = n.layer;
    char color[8];
    g_snprintf(color, sizeof color, "#%02x%02x%02x", o.color_rgb[0], o.color_rgb[1], o.color_rgb[2]);
    e->setAttribute("inkscape:highlight-color", color);
    if (!o.printable) e->setAttribute(LAYER_PRINT_ATTRIBUTE, "false");
    if (!o.preview) e->setAttribute("proof:layer-preview", "false");
    if (o.dim) e->setAttribute("proof:layer-dim", std::to_string(*o.dim));
    if (n.clipped) {
        // A layer's clipping path clips its other objects: a clip group inside the layer, marked so
        // that saving doesn't name it (object_name()).
        auto clip = create("svg:g");
        clip->setAttribute("inkscape:label", "Clip Group");
        clip->setAttribute("proof:layer-clip", "true");
        children(clip, n.children, true);
        append(e, clip);
    } else {
        children(e, n.children, false);
    }
    return e;
}

XML::Node *Builder::group(Node const &n)
{
    auto e = create("svg:g");
    std::string style;
    common(e, n, style);
    if (n.drawn_look) e->setAttribute("proof:drawn-look", "true");
    if (!n.plugin.empty() && !n.envelope) e->setAttribute("proof:plugin", n.plugin);
    if (!style.empty()) e->setAttribute("style", style);
    children(e, n.children, n.clipped);
    // The turned box of an object read as its drawn look: its paths share it.
    if (n.drawn_look && n.box_rotation) {
        auto const deg = std::remainder(-*n.box_rotation * 180.0 / M_PI, 360.0);
        sp_repr_visit_descendants(e, [&](XML::Node *d) {
            if (d->name() && std::string_view(d->name()) == "svg:path") d->setAttribute(BOX_ANGLE_ATTRIBUTE, num(deg));
            return true;
        });
    }
    return e;
}

void Builder::children(XML::Node *parent, std::vector<Node> const &kids, bool clipped)
{
    std::size_t start = 0;
    // A clipping path can be painted itself. Its fill lies under what it clips, as a
    // background. Its stroke lies over it, whole: what is clipped goes in a group of its
    // own then, with the stroked path after it.
    auto *into = parent;
    bool const stroked = clipped && !kids.empty() && kids.front().stroke;
    if (clipped && !kids.empty()) {
        if (stroked) into = create("svg:g");
        into->setAttribute("clip-path", "url(#" + clip_path(kids.front()) + ")");
        if (!kids.front().fill) start = 1;
    }
    auto const seamless = tiles(kids, start, clipped && !kids.empty() ? &kids.front() : nullptr, _to_doc);
    for (std::size_t i = start; i < kids.size(); ++i) {
        XML::Node *e = nullptr;
        if (i == 0 && stroked) {
            Node fill = kids[i];
            fill.stroke.reset();
            e = node(fill);
        } else {
            e = node(kids[i]);
        }
        if (e) {
            if (seamless.count(i)) {
                auto const style = e->attribute("style");
                e->setAttribute("style", std::string(style ? style : "") + "shape-rendering:crispEdges;");
            }
            append(into, e);
        }
    }
    if (stroked) {
        append(parent, into);
        Node line = kids.front();
        line.fill.reset();
        if (auto e = node(line)) append(parent, e);
    }
}

std::string Builder::clip_path(Node const &clip)
{
    Geom::PathVector pv;
    std::string nodetypes;
    outline(clip, pv, &nodetypes);
    auto cp = create("svg:clipPath");
    auto const id = unique("clipPath");
    cp->setAttribute("id", id);
    cp->setAttribute("clipPathUnits", "userSpaceOnUse");
    auto p = create("svg:path");
    p->setAttribute("d", sp_svg_write_path(pv * _to_doc));
    if (!nodetypes.empty()) p->setAttribute("sodipodi:nodetypes", nodetypes);
    if (clip.evenodd) p->setAttribute("style", "clip-rule:evenodd");
    if (!clip.name.empty()) p->setAttribute("inkscape:label", clip.name);
    append(cp, p);
    append(_defs, cp);
    return id;
}

XML::Node *Builder::masked(XML::Node *art, std::vector<Node> const &mask, bool clipped)
{
    if (!_mask_space) {
        auto const profile = Colors::CMS::Profile::create_srgb();
        auto &cms = _doc->getDocumentCMS();
        auto const name = cms.attachProfileToDoc(*profile, ColorProfileStorage::HREF_DATA,
                                                 Colors::RenderingIntent::RELATIVE_COLORIMETRIC);
        _mask_space = cms.getSpace(name);
    }
    auto const id = unique("mask");
    auto m = create("svg:mask");
    m->setAttribute("id", id);
    m->setAttribute("maskUnits", "userSpaceOnUse");
    m->setAttribute("maskContentUnits", "userSpaceOnUse");
    // Use actual painted bounds, including strokes, instead of clipping to an artboard.
    // The temporary definitions child is never displayed and retains its creation ref.
    _defs->appendChild(art);
    _doc->ensureUpToDate();
    auto const *item = dynamic_cast<SPItem *>(_doc->getObjectByRepr(art));
    auto const art_bounds = item ? item->visualBounds(item->transform) : Geom::OptRect();
    _defs->removeChild(art);
    Geom::PathVector pv;
    for (auto const &n : mask) outline(n, pv);
    auto bounds = (pv * _to_doc).boundsFast();
    if (art_bounds) bounds.unionWith(art_bounds);
    auto box = bounds.value_or(Geom::Rect(Geom::Point(0, 0), Geom::Point(1, 1)));
    if (!clipped) {
        // White is the unclipped mask's backdrop, including where its artwork is absent
        // or translucent. Extend beyond the target's painted edge to avoid multiplying
        // its antialiasing by a second coincident edge. The margin follows SVG's 10% guard.
        box.expandBy(std::max(1.0, std::max(box.width(), box.height()) * 0.1));
        auto background = create("svg:rect");
        background->setAttribute("x", num(box.left()));
        background->setAttribute("y", num(box.top()));
        background->setAttribute("width", num(box.width()));
        background->setAttribute("height", num(box.height()));
        background->setAttribute("style", "fill:" + Colors::Color(_mask_space, {1, 1, 1}).toString(false));
        background->setAttribute("proof:mask-backdrop", "white");
        append(m, background);
    }
    m->setAttribute("proof:ai-mask-clipping", clipped ? "true" : "false");
    m->setAttribute("x", num(box.left()));
    m->setAttribute("y", num(box.top()));
    m->setAttribute("width", num(box.width()));
    m->setAttribute("height", num(box.height()));
    auto const was_mask = std::exchange(_in_opacity_mask, true);
    children(m, mask, false);
    _in_opacity_mask = was_mask;
    append(_defs, m);
    // An image or text can carry its own transform. A wrapper keeps both the mask
    // and the already positioned artwork in the same document coordinates.
    auto wrapper = create("svg:g");
    wrapper->setAttribute("mask", "url(#" + id + ")");
    append(wrapper, art);
    return wrapper;
}

XML::Node *Builder::shape(Node const &n, Geom::PathVector const &pv, std::string const &nodetypes)
{
    if (pv.empty()) return nullptr;
    auto e = create("svg:path");
    e->setAttribute("d", sp_svg_write_path(pv * _to_doc));
    if (!nodetypes.empty()) e->setAttribute("sodipodi:nodetypes", nodetypes);
    std::string style = "fill:" + (n.fill ? paint(*n.fill) : std::string("none")) + ";";
    if (n.fill) style += std::string("fill-rule:") + (n.evenodd ? "evenodd" : "nonzero") + ";";
    if (n.stroke) {
        auto const &s = n.stroke_style;
        style += "stroke:" + paint(*n.stroke) + ";stroke-width:" + num(s.width) + ";";
        style += std::string("stroke-linecap:") + CAPS[std::clamp(s.cap, 0, 2)] + ";";
        style += std::string("stroke-linejoin:") + JOINS[std::clamp(s.join, 0, 2)] + ";";
        style += "stroke-miterlimit:" + num(s.miter) + ";";
        double total = 0;
        for (double d : s.dash) total += d;
        if (!s.dash.empty() && total > 0) {
            std::string dash;
            for (double d : s.dash) dash += (dash.empty() ? "" : ",") + num(d);
            style += "stroke-dasharray:" + dash + ";stroke-dashoffset:" + num(s.dash_offset) + ";";
        }
    } else {
        style += "stroke:none;";
    }
    common(e, n, style);
    e->setAttribute("style", style);
    if (n.overprint_fill || n.overprint_stroke) {
        e->setAttribute("proof:overprint", n.overprint_fill && n.overprint_stroke ? "fill stroke"
                                           : n.overprint_fill                     ? "fill"
                                                                                  : "stroke");
    }
    if (n.box_rotation) {
        // BBAccumRotation turns counterclockwise in art space (y up); the document's y points down.
        e->setAttribute(BOX_ANGLE_ATTRIBUTE, num(std::remainder(-*n.box_rotation * 180.0 / M_PI, 360.0)));
    }
    return e;
}

// ---- colours --------------------------------------------------------------------------------

XML::Node *Builder::foreign(Node const &n)
{
    if (n.foreign < 0 || n.foreign >= static_cast<int>(_ai.foreign.size())) {
        foreign_error = "a foreign object's embedded PDF is missing";
        return nullptr;
    }
#ifdef WITH_POPPLER
    auto const &source = _ai.foreign[n.foreign];
    std::string error;
    auto art = import_foreign_pdf(source, _doc, error);
    if (!art) {
        if (_hidden) ++hidden_foreign;
        else foreign_error = error;
        return nullptr;
    }
    auto e = create("svg:g");
    std::string style;
    common(e, n, style);
    if (!style.empty()) e->setAttribute("style", style);
    e->setAttribute("proof:ai-foreign", "pdf");
    auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(source.pdf.data()), source.pdf.size());
    e->setAttribute("proof:foreign-pdf", std::string("data:application/pdf;base64,") + encoded);
    g_free(encoded);
    art->setAttribute("transform", sp_svg_transform_write(source.to_art * _to_doc));
    // Page bounds are a clip, not a geometric fit: preserve blank margins, strokes,
    // rotations and shears even when the painted bounds are smaller than the page.
    auto clip = create("svg:clipPath");
    auto id = unique("foreignclip");
    clip->setAttribute("id", id);
    clip->setAttribute("clipPathUnits", "userSpaceOnUse");
    auto rect = create("svg:rect");
    rect->setAttribute("width", num(source.bounds.width()));
    rect->setAttribute("height", num(source.bounds.height()));
    append(clip, rect);
    append(_defs, clip);
    art->setAttribute("clip-path", "url(#" + id + ")");
    append(e, art);
    ++foreign_objects;
    return e;
#else
    foreign_error = "embedded foreign PDFs require PDF import support";
    return nullptr;
#endif
}

namespace {

/// A mesh patch's edge as a function of its parameter, from 0 to 1. Each cubic takes a share
/// as long as its control polygon: what Illustrator gives it (gradient mesh controls,
/// 2026-10-17: an anchor slid along an edge keeps its handles and moves its share so).
struct MeshEdge
{
    std::vector<std::array<Geom::Point, 4>> cubics;
    std::vector<double> knots;

    MeshEdge(std::vector<Geom::CubicBezier> const &edge, Geom::Affine const &to, bool backwards)
    {
        for (auto const &c : edge) cubics.push_back({c[0] * to, c[1] * to, c[2] * to, c[3] * to});
        if (backwards) {
            std::reverse(cubics.begin(), cubics.end());
            for (auto &c : cubics) std::reverse(c.begin(), c.end());
        }
        std::vector<double> lengths;
        double total = 0;
        for (auto const &c : cubics) {
            lengths.push_back(Geom::distance(c[0], c[1]) + Geom::distance(c[1], c[2]) + Geom::distance(c[2], c[3]));
            total += lengths.back();
        }
        knots.push_back(0);
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            knots.push_back(i + 1 == lengths.size() ? 1.0 :
                            total > 1e-12 ? knots.back() + lengths[i] / total : double(i + 1) / lengths.size());
        }
    }
    /// The cubic that holds `u`, a point inside a cell.
    std::size_t segment(double u) const
    {
        std::size_t s = 0;
        while (s + 2 < knots.size() && u > knots[s + 1]) ++s;
        return s;
    }
    Geom::Point at(double u, std::size_t s) const
    {
        auto const &c = cubics[s];
        double const t = (u - knots[s]) / (knots[s + 1] - knots[s]), r = 1 - t;
        return c[0] * (r * r * r) + c[1] * (3 * r * r * t) + c[2] * (3 * r * t * t) + c[3] * (t * t * t);
    }
    Geom::Point slope(double u, std::size_t s) const
    {
        auto const &c = cubics[s];
        double const span = knots[s + 1] - knots[s], t = (u - knots[s]) / span, r = 1 - t;
        return ((c[1] - c[0]) * (r * r) + (c[2] - c[1]) * (2 * r * t) + (c[3] - c[2]) * (t * t)) * (3 / span);
    }
};

/// What Illustrator draws of a mesh patch (gradient mesh controls, 2026-10-17): the Coons patch of
/// its edges, edge 0 running along u and edge 3 back along v, its colour the corners' blended by
/// the patch's parameters.
struct CoonsPatch
{
    MeshPatch const *patch;
    MeshEdge e0, e1, e2, e3;
    std::array<Geom::Point, 4> corner;

    CoonsPatch(MeshPatch const &p, Geom::Affine const &to)
        : patch(&p)
        , e0(p.edges[0], to, false)
        , e1(p.edges[1], to, false)
        , e2(p.edges[2], to, true)
        , e3(p.edges[3], to, true)
    {
        for (std::size_t k = 0; k < 4; ++k) corner[k] = p.corners[k].point * to;
    }
    /// The cubic of each edge a cell lies against.
    struct At
    {
        std::size_t s0, s1, s2, s3;
    };
    At at(double u0, double u1, double v0, double v1) const
    {
        return {e0.segment((u0 + u1) / 2), e1.segment((v0 + v1) / 2), e2.segment((u0 + u1) / 2),
                e3.segment((v0 + v1) / 2)};
    }
    Geom::Point point(double u, double v, At const &a) const
    {
        return e0.at(u, a.s0) * (1 - v) + e2.at(u, a.s2) * v + e3.at(v, a.s3) * (1 - u) + e1.at(v, a.s1) * u -
               (corner[0] * ((1 - u) * (1 - v)) + corner[1] * (u * (1 - v)) + corner[2] * (u * v) +
                corner[3] * ((1 - u) * v));
    }
    Geom::Point along_u(double u, double v, At const &a) const
    {
        return e0.slope(u, a.s0) * (1 - v) + e2.slope(u, a.s2) * v - e3.at(v, a.s3) + e1.at(v, a.s1) -
               ((corner[1] - corner[0]) * (1 - v) + (corner[2] - corner[3]) * v);
    }
    Geom::Point along_v(double u, double v, At const &a) const
    {
        return e2.at(u, a.s2) - e0.at(u, a.s0) + e3.slope(v, a.s3) * (1 - u) + e1.slope(v, a.s1) * u -
               ((corner[3] - corner[0]) * (1 - u) + (corner[2] - corner[1]) * u);
    }
};

/// Cuts from 0 to 1, each once: those of two edges across from each other, or of a row's.
std::vector<double> merged(std::vector<double> c)
{
    std::sort(c.begin(), c.end());
    c.erase(std::unique(c.begin(), c.end(), [](double x, double y) { return y - x < 1e-9; }), c.end());
    c.back() = 1.0;
    return c;
}

/// The cells one SVG mesh gradient may have: the reader's default limit of patches in a mesh.
constexpr std::size_t MAX_MESH_CELLS = 4096;

} // namespace

/**
 * Patches of a mesh as one SVG mesh gradient, cut into cells: column a of them at `us[a]`, row b
 * at `vs[b]`, each a list of cuts from 0 to 1 along the patch's u or v. Each cell's sides are exact
 * cubics of its patch's Coons surface, and that surface is the Coons patch of the cell's sides (it
 * is linear in u or v term by term), so the cells draw the patches exactly; each cell's corners
 * have the colour the patch has there. Patch [a b] of `grid` (at b * across + a) is row b, column
 * a, each corner and side numbered as Illustrator's (corner k, the side from it to corner k + 1).
 */
XML::Node *Builder::mesh_grid(std::size_t across, std::size_t up, std::vector<MeshPatch const *> const &grid,
                              std::vector<std::vector<double>> const &us, std::vector<std::vector<double>> const &vs)
{
    std::vector<CoonsPatch> surfaces;
    surfaces.reserve(grid.size());
    for (auto const *p : grid) surfaces.emplace_back(*p, _to_doc);
    auto stop_style = [&](MeshPatch const &p, double u, double v) {
        // Uncut patches can have a different model at each corner. At an original corner keep
        // that exact colour; only new points between corners need interpolation in one model.
        if ((u == 0 || u == 1) && (v == 0 || v == 1)) {
            auto const &c = p.corners[v == 0 ? (u == 0 ? 0 : 1) : (u == 0 ? 3 : 2)];
            return "stop-color:" + process(c.color).toString(false) + ";stop-opacity:" + num(c.opacity);
        }
        double const w[4] = {(1 - u) * (1 - v), u * (1 - v), u * v, (1 - u) * v};
        Color color = p.corners[0].color;
        color.v = {};
        double opacity = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            for (std::size_t i = 0; i < 4; ++i) color.v[i] += w[k] * p.corners[k].color.v[i];
            opacity += w[k] * p.corners[k].opacity;
        }
        return "stop-color:" + process(color).toString(false) + ";stop-opacity:" + num(opacity);
    };
    auto side = [&](Geom::Point h1, Geom::Point h2, Geom::Point b) {
        return "C " + num(h1.x()) + " " + num(h1.y()) + " " + num(h2.x()) + " " + num(h2.y()) + " " + num(b.x()) +
               " " + num(b.y());
    };

    auto gradient = create("svg:meshgradient");
    auto const id = unique("mesh");
    gradient->setAttribute("id", id);
    gradient->setAttribute("gradientUnits", "userSpaceOnUse");
    gradient->setAttribute("x", num(surfaces[0].corner[0].x()));
    gradient->setAttribute("y", num(surfaces[0].corner[0].y()));
    // Rows and columns as Inkscape writes them (SPMeshNodeArray::write): a top side in the
    // first row only, a left side in the first column only, each colour on the corner its
    // side starts from.
    bool first_row = true;
    for (std::size_t b = 0; b < up; ++b) {
        for (std::size_t i = 0; i + 1 < vs[b].size(); ++i) {
            auto row = create("svg:meshrow");
            double const v0 = vs[b][i], v1 = vs[b][i + 1], dv = (v1 - v0) / 3;
            bool first_column = true;
            for (std::size_t a = 0; a < across; ++a) {
                auto const &s = surfaces[b * across + a];
                auto const &p = *s.patch;
                for (std::size_t j = 0; j + 1 < us[a].size(); ++j) {
                    double const u0 = us[a][j], u1 = us[a][j + 1], du = (u1 - u0) / 3;
                    auto const at = s.at(u0, u1, v0, v1);
                    auto const p00 = s.point(u0, v0, at), p10 = s.point(u1, v0, at), p11 = s.point(u1, v1, at),
                               p01 = s.point(u0, v1, at);
                    auto patch = create("svg:meshpatch");
                    auto add = [&](std::string const &path, std::optional<std::pair<double, double>> colour) {
                        auto stop = create("svg:stop");
                        stop->setAttribute("path", path);
                        if (colour) stop->setAttribute("style", stop_style(p, colour->first, colour->second));
                        append(patch, stop);
                    };
                    if (first_row) {
                        add(side(p00 + s.along_u(u0, v0, at) * du, p10 - s.along_u(u1, v0, at) * du, p10),
                            first_column ? std::optional(std::pair(u0, v0)) : std::nullopt);
                    }
                    add(side(p10 + s.along_v(u1, v0, at) * dv, p11 - s.along_v(u1, v1, at) * dv, p11),
                        first_row ? std::optional(std::pair(u1, v0)) : std::nullopt);
                    add(side(p11 - s.along_u(u1, v1, at) * du, p01 + s.along_u(u0, v1, at) * du, p01), std::pair(u1, v1));
                    if (first_column) {
                        add(side(p01 - s.along_v(u0, v1, at) * dv, p00 + s.along_v(u0, v0, at) * dv, p00),
                            std::pair(u0, v1));
                    }
                    append(row, patch);
                    first_column = false;
                }
            }
            append(gradient, row);
            first_row = false;
        }
    }
    append(_defs, gradient);
    return gradient;
}

/** A patch on its own as an SVG mesh gradient: its Coons patch, cut wherever an edge has an
 * anchor (mesh_grid).
 */
XML::Node *Builder::mesh_cells(MeshPatch const &p)
{
    CoonsPatch const s(p, _to_doc);
    auto cuts = [](MeshEdge const &a, MeshEdge const &b) {
        std::vector<double> c(a.knots);
        c.insert(c.end(), b.knots.begin(), b.knots.end());
        return merged(std::move(c));
    };
    mesh_cut += std::any_of(p.edges.begin(), p.edges.end(), [](auto const &e) { return e.size() > 1; });
    return mesh_grid(1, 1, {&p}, {cuts(s.e0, s.e2)}, {cuts(s.e3, s.e1)});
}

/**
 * A mesh as Illustrator edits it, one grid: one SVG mesh gradient on its outline, which the writer
 * gives back as one mesh. Its patches are cut into cells where their edges have anchors: a
 * column at all of its patches' anchors along it, a row likewise, so neighbours share their cut
 * sides (mesh_grid). Neighbours must share their corners, colours and sides, as Illustrator writes
 * them; a patch that is cut, one colour model. Otherwise, or past MAX_MESH_CELLS cells, nothing,
 * and each patch is drawn on its own; so is one patch of one colour bent through anchors, which
 * is its outline.
 */
XML::Node *Builder::whole_mesh(Node const &n)
{
    auto const &m = n.mesh;
    auto const across = m.size[0], up = m.size[1];
    if (!across || !up || m.patches.size() != across * up) return nullptr;
    std::vector<MeshPatch const *> grid(across * up, nullptr);
    for (auto const &p : m.patches) {
        if (p.place[0] >= across || p.place[1] >= up) return nullptr;
        auto &slot = grid[p.place[1] * across + p.place[0]];
        if (slot) return nullptr;
        slot = &p;
    }
    if (grid.size() == 1 && grid[0]->outlined) return nullptr;
    auto at = [&](std::size_t a, std::size_t b) -> MeshPatch const & { return *grid[b * across + a]; };
    auto near = [](Geom::Point const &p, Geom::Point const &q) { return Geom::distance(p, q) <= 1e-4; };
    auto same_corner = [&](MeshCorner const &c, MeshCorner const &d) {
        return near(c.point, d.point) && c.color == d.color && std::abs(c.opacity - d.opacity) <= 1e-6;
    };
    // `y` is the same side as `x`, run the other way.
    auto same_side = [&](std::vector<Geom::CubicBezier> const &x, std::vector<Geom::CubicBezier> const &y) {
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.size(); ++i) {
            auto const &c = x[i], &d = y[y.size() - 1 - i];
            for (unsigned k = 0; k < 4; ++k) {
                if (!near(c[k], d[3 - k])) return false;
            }
        }
        return true;
    };
    for (std::size_t b = 0; b < up; ++b) {
        for (std::size_t a = 0; a < across; ++a) {
            auto const &p = at(a, b);
            if (a + 1 < across) {
                // Its side from corner 1 to 2 is the next one's from 3 to 0.
                auto const &q = at(a + 1, b);
                if (!same_corner(p.corners[1], q.corners[0]) || !same_corner(p.corners[2], q.corners[3]) ||
                    !same_side(p.edges[1], q.edges[3])) {
                    return nullptr;
                }
            }
            if (b + 1 < up) {
                // Its side from corner 2 to 3 is the one above's from 0 to 1.
                auto const &q = at(a, b + 1);
                if (!same_corner(p.corners[3], q.corners[0]) || !same_corner(p.corners[2], q.corners[1]) ||
                    !same_side(p.edges[2], q.edges[0])) {
                    return nullptr;
                }
            }
        }
    }
    // Where each column and row is cut: at every anchor of its patches' edges along it.
    std::vector<std::vector<double>> us(across), vs(up);
    for (std::size_t a = 0; a < across; ++a) {
        std::vector<double> c;
        for (std::size_t b = 0; b < up; ++b) {
            for (int k : {0, 2}) {
                auto const knots = MeshEdge(at(a, b).edges[k], _to_doc, k == 2).knots;
                c.insert(c.end(), knots.begin(), knots.end());
            }
        }
        us[a] = merged(std::move(c));
    }
    for (std::size_t b = 0; b < up; ++b) {
        std::vector<double> c;
        for (std::size_t a = 0; a < across; ++a) {
            for (int k : {1, 3}) {
                auto const knots = MeshEdge(at(a, b).edges[k], _to_doc, k == 3).knots;
                c.insert(c.end(), knots.begin(), knots.end());
            }
        }
        vs[b] = merged(std::move(c));
    }
    std::size_t columns = 0, rows = 0;
    for (auto const &u : us) columns += u.size() - 1;
    for (auto const &v : vs) rows += v.size() - 1;
    if (columns * rows > MAX_MESH_CELLS) return nullptr;
    // A cell's colour blends its patch's corners, in one model.
    for (std::size_t b = 0; b < up; ++b) {
        for (std::size_t a = 0; a < across; ++a) {
            auto const &c = at(a, b).corners;
            bool const cut = us[a].size() > 2 || vs[b].size() > 2;
            if (cut && std::any_of(c.begin(), c.end(), [&](MeshCorner const &k) { return k.color.model != c[0].color.model; })) {
                return nullptr;
            }
        }
    }

    auto gradient = mesh_grid(across, up, grid, us, vs);
    for (auto const *p : grid) {
        mesh_cut += std::any_of(p->edges.begin(), p->edges.end(), [](auto const &e) { return e.size() > 1; });
    }
    // The outline as Illustrator has it, anchors and all: the first row's edge 0, the last
    // column's edge 1, the last row's edge 2 and the first column's edge 3, around from the
    // first patch's corner 0.
    Geom::Path outline(at(0, 0).corners[0].point * _to_doc);
    auto edge = [&](MeshPatch const &p, std::size_t k) {
        for (auto const &c : p.edges[k]) outline.appendNew<Geom::CubicBezier>(c[1] * _to_doc, c[2] * _to_doc, c[3] * _to_doc);
    };
    for (std::size_t a = 0; a < across; ++a) edge(at(a, 0), 0);
    for (std::size_t b = 0; b < up; ++b) edge(at(across - 1, b), 1);
    for (std::size_t a = across; a-- > 0;) edge(at(a, up - 1), 2);
    for (std::size_t b = up; b-- > 0;) edge(at(0, b), 3);
    outline.close(true);

    auto path = create("svg:path");
    path->setAttribute("d", sp_svg_write_path(Geom::PathVector{outline}));
    std::string style = "fill:url(#" + std::string(gradient->attribute("id")) + ");stroke:none;";
    common(path, n, style);
    path->setAttribute("style", style);
    path->setAttribute("proof:ai-mesh", "true");
    path->setAttribute("proof:mesh-object", "true");
    return path;
}

XML::Node *Builder::mesh(Node const &n)
{
    if (auto whole = whole_mesh(n)) return whole;
    if (n.mesh.patches.size() > 1) ++mesh_split;
    std::vector<XML::Node *> paths;
    for (auto const &p : n.mesh.patches) {
        auto path = create("svg:path");
        path->setAttribute("d", sp_svg_write_path(p.boundary * _to_doc));
        std::string fill;
        if (p.outlined) {
            ++mesh_outlines;
            fill = process(p.corners[0].color).toString(false) + ";fill-opacity:" + num(p.corners[0].opacity);
            path->setAttribute("proof:mesh-outline", "true");
        } else {
            fill = "url(#" + std::string(mesh_cells(p)->attribute("id")) + ")";
            path->setAttribute("proof:mesh-object", "true");
        }
        path->setAttribute("style", "fill:" + fill + ";stroke:none;");
        paths.push_back(path);
    }
    // A mesh of one patch is that path, with the mesh's own state: no group about it, which
    // each save and reopen would add again.
    bool const single = paths.size() == 1;
    auto e = single ? paths.front() : create("svg:g");
    std::string style = single ? e->attribute("style") : "";
    common(e, n, style);
    if (!style.empty()) e->setAttribute("style", style);
    e->setAttribute("proof:ai-mesh", "true");
    if (!single) {
        for (auto *path : paths) append(e, path);
    }
    return e;
}

Colors::Color Builder::process(Color const &c) const
{
    if (_in_opacity_mask) {
        // The reader admits only neutral RGB, Gray, or K-only CMYK here. 100K in a
        // mask means zero coverage, regardless of the document's print profile.
        double const grey = c.model == Color::Model::Cmyk ? 1.0 - c.v[3] : c.v[0];
        return Colors::Color(_mask_space, {grey, grey, grey});
    }
    auto cmyk = [&](std::vector<double> v) {
        return _cmyk_space ? Colors::Color(_cmyk_space, std::move(v)) : Colors::Color(Type::CMYK, std::move(v));
    };
    switch (c.model) {
        case Color::Model::Cmyk:
            return cmyk({c.v[0], c.v[1], c.v[2], c.v[3]});
        case Color::Model::Gray:
            // Grey prints on the black plate in a CMYK document.
            if (_ai.cmyk) return cmyk({0.0, 0.0, 0.0, 1.0 - c.v[0]});
            return Colors::Color(Type::RGB, {c.v[0], c.v[0], c.v[0]});
        case Color::Model::Rgb:
            return Colors::Color(Type::RGB, {c.v[0], c.v[1], c.v[2]});
        case Color::Model::Lab:
            return definition(c);
    }
    return Colors::Color(Type::RGB, {0.0, 0.0, 0.0});
}

std::string Builder::paint(Paint const &p)
{
    if (p.kind == Paint::Kind::Gradient) return gradient(p);
    if (p.kind == Paint::Kind::Pattern) return pattern(p);
    if (p.ink >= 0 && p.ink < static_cast<int>(_ai.inks.size())) return ink(p.ink, p.tint);
    return process(p.color).toString(false);
}

std::string Builder::ink(int index, double tint)
{
    tint = std::clamp(tint, 0.0, 1.0);
    auto const key = std::pair(index, tint);
    if (auto it = _inks.find(key); it != _inks.end()) return it->second;
    return _inks[key] = ink_swatch(_ai.inks[index], tint);
}

std::string Builder::ink_swatch(Ink const &ink, double tint)
{
    std::string id;
    if (ink.kind == Ink::Kind::Process) {
        id = process_swatch(ink, tint);
    } else {
        auto const name = ink.kind == Ink::Kind::Registration ? std::string(REGISTRATION_INK) : ink.name;
        id = SpotInk::ensure(_xml, _defs, _doc, SpotInk::Ink{name, definition(ink.color), tint});
    }
    return "url(#" + id + ")";
}

std::string Builder::text_ink(TextInk const &used)
{
    // The Swatches panel's ink of that name, which paths painted with it share: the same
    // definition first, else the one of that name and kind.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < _ai.inks.size(); ++i) {
            auto const &known = _ai.inks[i];
            if (known.name == used.ink.name && known.kind == used.ink.kind && (pass == 1 || known.color == used.ink.color)) {
                return ink(static_cast<int>(i), used.tint);
            }
        }
    }
    // An ink only type uses.
    for (auto const &[known, url] : _text_inks) {
        if (known == used) return url;
    }
    return _text_inks.emplace_back(used, ink_swatch(used.ink, std::clamp(used.tint, 0.0, 1.0))).second;
}

std::string Builder::process_swatch(Ink const &ink, double tint)
{
    auto stem = "swatch-" + SpotInk::id_stem(ink.name).substr(4);
    if (tint < 1.0 - 1e-4) stem += "-t" + std::to_string(std::lround(tint * 100));
    auto id = stem;
    for (int n = 2; _doc->getObjectById(id); ++n) id = stem + "-" + std::to_string(n);
    auto grad = create("svg:linearGradient");
    grad->setAttribute("id", id);
    grad->setAttribute("inkscape:swatch", "solid");
    grad->setAttribute("inkscape:label", SpotInk::label_for(ink.name, tint));
    auto stop = create("svg:stop");
    stop->setAttribute("offset", "0");
    stop->setAttribute("style", "stop-color:" + process(tinted(ink.color, tint)).toString(false) + ";stop-opacity:1");
    append(grad, stop);
    append(_defs, grad);
    return id;
}

// ---- gradients ------------------------------------------------------------------------------

Colors::Color Builder::shown(StopPaint const &s) const
{
    if (s.ink >= 0 && s.ink < static_cast<int>(_ai.inks.size())) {
        auto const &ink = _ai.inks[s.ink];
        if (ink.kind == Ink::Kind::Process) return process(tinted(ink.color, s.tint));
        return SpotInk::display_color(definition(ink.color), s.tint);
    }
    return process(s.color);
}

void Builder::add_stop(XML::Node *vector, double offset, StopPaint const *paint, Colors::Color const &color,
                       double opacity, bool sample)
{
    auto stop = create("svg:stop");
    stop->setAttribute("offset", num(std::clamp(offset, 0.0, 1.0)));
    stop->setAttribute("style", "stop-color:" + color.toString(false) + ";stop-opacity:" + num(std::clamp(opacity, 0.0, 1.0)));
    if (paint && paint->ink >= 0 && paint->ink < static_cast<int>(_ai.inks.size())) {
        auto const &ink = _ai.inks[paint->ink];
        if (ink.kind != Ink::Kind::Process) {
            // The stop is a tint of a spot ink: keep which, as a swatch does.
            stop->setAttribute("proof:ink", ink.kind == Ink::Kind::Registration ? REGISTRATION_INK : ink.name.c_str());
            stop->setAttribute("proof:ink-alternate", definition(ink.color).toString(false));
            if (paint->tint < 1.0 - 1e-4) stop->setAttribute("proof:ink-tint", num(paint->tint));
        }
    }
    // A stop that only follows Illustrator's blend between two of its stops.
    if (sample) stop->setAttribute("proof:midpoint-sample", "true");
    append(vector, stop);
}

std::string Builder::gradient_vector(int index)
{
    auto const key = std::pair(index, _in_opacity_mask);
    if (auto it = _vectors.find(key); it != _vectors.end()) return it->second;
    auto const &g = _ai.gradients[index];
    auto vec = create("svg:linearGradient");
    auto const id = unique("gradient");
    vec->setAttribute("id", id);
    if (!g.name.empty()) vec->setAttribute("inkscape:label", g.name);
    bool const perceptual = g.options == std::vector<double>{1, 0};
    if (perceptual) vec->setAttribute("proof:gradient-method", "oklab");
    auto const &stops = g.stops;
    for (std::size_t i = 0; i < stops.size(); ++i) {
        auto const &a = stops[i];
        // Past its end Illustrator extends a gradient with the first of its stops there, where SVG
        // takes the last, and the others never show (Illustrator 29.2.1, linear and radial,
        // 2026-10-22; six gradients of the owner's have two at 100).
        if (i > 0 && stops[i - 1].offset >= 1.0 && a.offset >= 1.0) {
            ++unused_gradient_end_stops;
            continue;
        }
        StopPaint const pa{a.color, a.ink, a.tint};
        auto const ca = shown(pa);
        add_stop(vec, a.offset, &pa, ca, a.opacity, false);
        if (perceptual) {
            vec->lastChild()->setAttribute("proof:gradient-midpoint", num(a.midpoint));
            continue; // Rendering samples are computed from the editable originals.
        }
        if (i + 1 == stops.size() || std::abs(a.midpoint - 0.5) < 0.005) continue;
        // Illustrator's blend between two stops follows u^n, half-way at the midpoint.
        auto const &b = stops[i + 1];
        // Two stops at one place have nothing between them to blend.
        if (b.offset <= a.offset) continue;
        StopPaint const pb{b.color, b.ink, b.tint};
        auto const cb = shown(pb);
        double const n = std::log(0.5) / std::log(a.midpoint);
        std::vector<double> us;
        curve_samples(n, 0.0, 1.0, 0, us);
        for (double u : us) {
            double const v = std::pow(u, n);
            double const offset = a.offset + (b.offset - a.offset) * u;
            double const opacity = a.opacity + (b.opacity - a.opacity) * v;
            if (pa.ink >= 0 && pa.ink == pb.ink) {
                // Two tints of one ink blend as tints of it.
                StopPaint const mid{pa.color, pa.ink, pa.tint + (pb.tint - pa.tint) * v};
                add_stop(vec, offset, &mid, shown(mid), opacity, true);
            } else if (pa.ink < 0 && pb.ink < 0 && pa.color.model == pb.color.model) {
                StopPaint mid{pa.color};
                for (int k = 0; k < 4; ++k) mid.color.v[k] = pa.color.v[k] + (pb.color.v[k] - pa.color.v[k]) * v;
                add_stop(vec, offset, &mid, shown(mid), opacity, true);
            } else {
                // Different models (a spot ink and a process colour, RGB and CMYK): blend what they
                // show in the document's colour mode. Illustrator 29.2.1 blends process colours of
                // two models so (within 1 level in an RGB document), and its ramp of the owner's one
                // ink-to-RGB gradient follows one curve in R, G and B; an ink beside a process
                // colour it draws overprinting the process ramp, lighter mid-ramp than this
                // (PROOF-ai-interchange.md, "Gradient stops of different colour models").
                auto const mode = _ai.cmyk ? Type::CMYK : Type::RGB;
                auto const values = [&](Colors::Color const &c) -> std::optional<std::vector<double>> {
                    auto const space = c.getSpace();
                    if (space && space->getComponentType() == mode) return c.getValues();
                    // Into the document's CMYK profile, as an RGB end is read there (interpret).
                    auto converted = mode == Type::CMYK && _cmyk_space ? c.converted(_cmyk_space) : c.converted(mode);
                    if (!converted) return {};
                    return converted->getValues();
                };
                auto ra = values(ca), rb = values(cb);
                std::size_t const count = mode == Type::CMYK ? 4 : 3;
                if (!ra || !rb || ra->size() < count || rb->size() < count) continue;
                std::vector<double> mix(count);
                for (std::size_t k = 0; k < count; ++k) mix[k] = (*ra)[k] + ((*rb)[k] - (*ra)[k]) * v;
                auto const sample = mode == Type::CMYK ? (_cmyk_space ? Colors::Color(_cmyk_space, std::move(mix))
                                                                      : Colors::Color(Type::CMYK, std::move(mix)))
                                                       : Colors::Color(Type::RGB, std::move(mix));
                add_stop(vec, offset, nullptr, sample, opacity, true);
            }
        }
    }
    append(_defs, vec);
    return _vectors[key] = id;
}

std::string Builder::gradient(Paint const &p)
{
    auto const &pl = p.gradient;
    if (pl.gradient < 0 || pl.gradient >= static_cast<int>(_ai.gradients.size()) ||
        _ai.gradients[pl.gradient].stops.empty()) {
        return "none";
    }
    auto const &g = _ai.gradients[pl.gradient];
    // The gradient in its own space, and the matrix from there to the document. See
    // GradientPlacement: with Xm or Bm the space is the unit one and that matrix is all
    // of the placement; Bg's own values are then not to be applied as well.
    Geom::Affine m;
    Geom::Point start = pl.origin, end, focus = pl.origin + pl.hilight;
    double radius = std::abs(pl.length);
    if (pl.xm || pl.bm_given) {
        m = (pl.xm ? *pl.xm : pl.bm) * _to_doc;
        start = focus = {0.0, 0.0};
        end = {1.0, 0.0};
        radius = 1.0;
        if (g.radial) {
            if (std::abs(pl.hilight_length) > 1e-9) {
                double const a = pl.hilight_angle * M_PI / 180.0;
                focus = Geom::Point(std::cos(a), -std::sin(a)) * pl.hilight_length;
            } else if (Geom::L2(pl.hilight) > 1e-9) {
                focus = pl.hilight * (pl.xm ? *pl.xm : pl.bm).withoutTranslation().inverse();
            }
        } else if (!pl.xm && g.stops.size() > 1 && g.stops[1].offset - g.stops[0].offset > 1e-6) {
            // Bm alone, for a linear gradient: 0 to 1 is from the first stop to the second.
            double const lo = g.stops[0].offset, span = g.stops[1].offset - lo;
            start = {-lo / span, 0.0};
            end = {(1.0 - lo) / span, 0.0};
        }
    } else {
        m = pl.bg * _to_doc;
        double const a = pl.angle * M_PI / 180.0;
        end = pl.origin + Geom::Point(std::cos(a), std::sin(a)) * pl.length;
    }
    if (!finite(m) || m.isSingular()) {
        auto const &s = g.stops.front();
        return shown(StopPaint{s.color, s.ink, s.tint}).toString(false);
    }
    auto const vector = gradient_vector(pl.gradient);
    auto e = create(g.radial ? "svg:radialGradient" : "svg:linearGradient");
    auto const id = unique(g.radial ? "radialGradient" : "linearGradient");
    e->setAttribute("id", id);
    e->setAttribute("inkscape:collect", "always");
    e->setAttribute("xlink:href", "#" + vector);
    e->setAttribute("gradientUnits", "userSpaceOnUse");
    e->setAttribute("gradientTransform", sp_svg_transform_write(m));
    if (g.radial) {
        e->setAttributeSvgDouble("cx", start.x());
        e->setAttributeSvgDouble("cy", start.y());
        e->setAttributeSvgDouble("r", radius);
        if (Geom::L2(focus - start) > 1e-9) {
            e->setAttributeSvgDouble("fx", focus.x());
            e->setAttributeSvgDouble("fy", focus.y());
        }
    } else {
        e->setAttributeSvgDouble("x1", start.x());
        e->setAttributeSvgDouble("y1", start.y());
        e->setAttributeSvgDouble("x2", end.x());
        e->setAttributeSvgDouble("y2", end.y());
    }
    append(_defs, e);
    return "url(#" + id + ")";
}

// ---- patterns -------------------------------------------------------------------------------

/// A pattern's own space (y up) to its tile's: the tile's top-left corner at 0,0, y down.
Geom::Affine tile_space(Pattern const &p)
{
    return Geom::Affine(1, 0, 0, -1, -p.tile.left(), p.tile.bottom());
}

/// The pattern holding the tile and its art, once for the document.
std::string Builder::pattern_tile(int index)
{
    if (auto it = _tiles.find(index); it != _tiles.end()) return it->second;
    auto const &pat = _ai.patterns[index];
    auto e = create("svg:pattern");
    auto const id = unique("pattern");
    e->setAttribute("id", id);
    if (!pat.name.empty()) e->setAttribute("inkscape:label", pat.name);
    e->setAttribute("patternUnits", "userSpaceOnUse");
    e->setAttributeSvgDouble("width", pat.tile.width());
    e->setAttributeSvgDouble("height", pat.tile.height());
    // The art is built in the tile's space, and shows wherever the pattern does.
    auto const outer = std::exchange(_to_doc, tile_space(pat));
    auto const hidden = std::exchange(_hidden, 0);
    auto outer_guides = std::exchange(guides, {});
    ++_in_tile;
    children(e, pat.art, false);
    --_in_tile;
    guides = std::move(outer_guides);
    _hidden = hidden;
    _to_doc = outer;
    append(_defs, e);
    return _tiles[index] = id;
}

/**
 * The pattern as one object places it: the tile's, with that object's matrix.
 *
 * The matrix places the tile from its lower-left corner: that corner is the origin of what
 * it carries onto the art, wherever the tile's box lies in the pattern's own space. (The
 * PDF page of a file whose tiles start at 5.76, 5.76 has them as cells from 0,0 under the
 * same matrix, with the art moved to match.)
 */
std::string Builder::pattern(Paint const &p)
{
    if (p.pattern < 0 || p.pattern >= static_cast<int>(_ai.patterns.size())) return "none";
    auto const &pat = _ai.patterns[p.pattern];
    // The tile's space (top-left corner at 0,0, y down) to one with its lower-left corner
    // at 0,0 and y up, then onto the art.
    Geom::Affine const m = Geom::Affine(1, 0, 0, -1, 0, pat.tile.height()) * p.pattern_matrix * _to_doc;
    if (!finite(m) || m.isSingular()) return "none";
    auto const tile = pattern_tile(p.pattern);
    auto e = create("svg:pattern");
    auto const id = unique("pattern");
    e->setAttribute("id", id);
    e->setAttribute("inkscape:collect", "always");
    e->setAttribute("xlink:href", "#" + tile);
    e->setAttribute("patternTransform", sp_svg_transform_write(m));
    append(_defs, e);
    return "url(#" + id + ")";
}

// ---- images ---------------------------------------------------------------------------------

/// The image's colours for the screen, 8-bit RGB or RGBA (`channels`); `eight` gets its
/// samples at 8 bits.
std::vector<std::uint8_t> Builder::screen_pixels(Image const &img, std::string &eight, int &channels)
{
    std::size_t const w = img.width, h = img.height, ch = img.channels;
    if (img.bits == 8) {
        eight = img.samples;
    } else {
        // One bit a sample: 1 is full (white in grey, full ink in CMYK).
        std::size_t const row = (w * ch + 7) / 8;
        eight.assign(w * h * ch, '\0');
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w * ch; ++x) {
                auto const byte = static_cast<unsigned char>(img.samples[y * row + x / 8]);
                eight[y * w * ch + x] = ((byte >> (7 - x % 8)) & 1) ? char(255) : char(0);
            }
        }
    }
    std::vector<std::uint8_t> rgb(w * h * 3);
    auto const *s = reinterpret_cast<std::uint8_t const *>(eight.data());
    if (ch == 1) {
        for (std::size_t i = 0; i < w * h; ++i) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = s[i];
    } else if (ch == 3) {
        std::copy_n(s, w * h * 3, rgb.data());
    } else if (_cmyk_to_rgb) {
        // Row by row: cmsDoTransform takes a 32-bit pixel count.
        for (std::size_t y = 0; y < h; ++y) cmsDoTransform(_cmyk_to_rgb, s + y * w * 4, rgb.data() + y * w * 3, w);
    } else {
        for (std::size_t i = 0; i < w * h; ++i) {
            double const k = 1.0 - s[i * 4 + 3] / 255.0;
            for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = std::uint8_t(std::lround((255 - s[i * 4 + c]) * k));
        }
    }
    channels = 3;
    if (img.alpha.size() >= w * h) {
        std::vector<std::uint8_t> rgba(w * h * 4);
        for (std::size_t i = 0; i < w * h; ++i) {
            std::copy_n(rgb.data() + i * 3, 3, rgba.data() + i * 4);
            rgba[i * 4 + 3] = static_cast<std::uint8_t>(img.alpha[i]);
        }
        channels = 4;
        return rgba;
    }
    return rgb;
}

XML::Node *Builder::image(Node const &n)
{
    if (n.image < 0 || n.image >= static_cast<int>(_ai.images.size())) {
        ++images_left_out;
        return nullptr;
    }
    auto const &img = _ai.images[n.image];
    // The matrix is written for rows that count upwards, and the picture's count down: the
    // rows' direction is turned over before it, which for a picture that isn't upright is
    // not the same as turning over what comes out.
    auto const &r = img.matrix;
    Geom::Affine const m = Geom::Affine(r[0], r[1], -r[2], -r[3], r[4], r[5]) * _to_doc;
    if (!finite(m) || m.isSingular()) {
        ++images_left_out;
        return nullptr;
    }
    // A picture placed several times is in the records as many times, whole. Its samples
    // are converted and packed once.
    auto const same = [&](int other) {
        auto const &o = _ai.images[other];
        return o.width == img.width && o.height == img.height && o.bits == img.bits && o.channels == img.channels &&
               o.samples == img.samples && o.alpha == img.alpha;
    };
    auto const digest = std::hash<std::string_view>{}(img.samples) ^ (std::hash<std::string_view>{}(img.alpha) << 1) ^
                        (std::size_t(img.width) << 32) ^ std::size_t(img.height);
    Encoded const *encoded = nullptr;
    auto const [first, last] = _encoded.equal_range(digest);
    for (auto it = first; it != last && !encoded; ++it) {
        if (same(it->second.image)) encoded = &it->second;
    }
    if (!encoded) {
        std::string eight;
        int channels = 3;
        auto const pixels = screen_pixels(img, eight, channels);
        auto const data = png(pixels, img.width, img.height, channels);
        if (!data) {
            ++images_left_out;
            return nullptr;
        }
        Encoded made;
        made.image = n.image;
        made.screen = "data:image/png;base64," + base64(*data);
        if (img.channels == 4 || img.channels == 1) {
            // The screen sees RGB; the CMYK or grey samples stay for print, a grey picture on
            // the black plate alone, and Illustrator keeps it grey.
            if (auto tiff = samples_tiff(img.width, img.height, eight, img.channels, img.alpha)) {
                made.samples = "data:image/tiff;base64," + base64(*tiff);
            }
        }
        encoded = &_encoded.emplace(digest, std::move(made))->second;
    }
    auto e = create("svg:image");
    e->setAttributeSvgDouble("x", 0);
    e->setAttributeSvgDouble("y", 0);
    e->setAttributeSvgDouble("width", img.width);
    e->setAttributeSvgDouble("height", img.height);
    e->setAttribute("preserveAspectRatio", "none");
    e->setAttribute("transform", sp_svg_transform_write(m));
    e->setAttribute("xlink:href", encoded->screen);
    if (!encoded->samples.empty()) e->setAttribute("proof:samples", encoded->samples);
    std::string style;
    common(e, n, style);
    if (!style.empty()) e->setAttribute("style", style);
    return e;
}

/// The file a placed picture links to: where the document says it is, else in the folder
/// the document gives from its own, else beside the document. Nothing when it isn't there.
/// A place on another computer isn't looked up (io/link-policy.h): its share is noted.
std::optional<std::string> Builder::linked_file(Placed const &p)
{
    std::vector<std::string> tries;
    auto const folders = decodings(p.folder);
    for (auto const &name : decodings(p.path)) {
        if (g_path_is_absolute(name.c_str())) tries.push_back(name);
        auto const base = base_name(name);
        if (base.empty()) continue;
        auto join = [&](std::initializer_list<std::string> parts) {
            std::string out;
            for (auto const &part : parts) {
                if (!out.empty() && out.back() != '/' && out.back() != '\\') out += G_DIR_SEPARATOR;
                out += part;
            }
            tries.push_back(std::move(out));
        };
        for (auto const &folder : folders) {
            if (g_path_is_absolute(folder.c_str())) {
                join({folder, base});
            } else if (!_folder.empty()) {
                join({_folder, folder, base});
            }
        }
        if (!_folder.empty()) join({_folder, base});
    }
    std::set<std::string> passed;
    for (auto const &file : tries) {
        if (!Inkscape::IO::may_follow_link(file, _folder.c_str())) {
            auto const share = Inkscape::IO::network_share(file);
            passed.insert(share == "\\\\" ? std::string("a device") : share);
            continue;
        }
        if (g_file_test(file.c_str(), G_FILE_TEST_IS_REGULAR)) return file;
    }
    // Only a link that is found nowhere is named: one found beside the document needs no word.
    elsewhere.insert(passed.begin(), passed.end());
    return {};
}

XML::Node *Builder::placed(Node const &n)
{
    if (n.placed < 0 || n.placed >= static_cast<int>(_ai.placed.size())) {
        ++images_left_out;
        return nullptr;
    }
    auto const &p = _ai.placed[n.placed];
    Geom::Affine const m = p.matrix * _to_doc;
    if (!finite(m) || m.isSingular()) {
        ++images_left_out;
        return nullptr;
    }
    auto const names = decodings(p.path);
    auto const name = names.empty() ? std::string() : base_name(names.front());
    auto file = linked_file(p);
    bool gone = false;
    // A TIFF or a Photoshop file, which PROOF reads itself (display/raster-pixels.h), is read for
    // its header: whose CMYK profile it carries, and of a Photoshop file whether its merged image
    // can be read.
    auto const ends = [](std::string const &f, std::initializer_list<char const *> endings) {
        gchar *lower = g_utf8_strdown(f.c_str(), -1);
        bool const found = std::any_of(endings.begin(), endings.end(), [&](char const *e) { return g_str_has_suffix(lower, e); });
        g_free(lower);
        return found;
    };
    bool const cmyk = _cmyk_space && _cmyk_space->getProfile();
    std::optional<bool> photoshop_shows;
    std::string photoshop_why;
    if (file && (ends(*file, {".psd", ".psb"}) || (cmyk && ends(*file, {".tif", ".tiff"})))) {
        gchar *data = nullptr;
        gsize len = 0;
        if (g_file_get_contents(file->c_str(), &data, &len, nullptr)) {
            std::string_view const bytes(data, len);
            std::optional<std::string> own;
            if (looks_like_psd(bytes)) {
                photoshop_shows = psd_readable(bytes, &photoshop_why);
                own = cmyk_psd_profile(bytes);
            } else {
                own = cmyk_tiff_profile(bytes);
            }
            if (cmyk && own && !own->empty()) {
                auto profile = Colors::CMS::Profile::create_from_data(*own);
                auto const theirs = profile ? profile->getName() : std::string("an unreadable profile");
                if (theirs != _cmyk_space->getProfile()->getName()) other_profiles.emplace(name, theirs);
            }
            g_free(data);
        }
    }
    // A placed PDF page (a linked .pdf, or an .ai, a PDF too): the selected page cropped to its crop
    // box, which is drawn for the screen and goes to print as it is (pdfinput/placed-page.h).
    std::optional<bool> page_shows;
    std::string page_why;
#ifdef WITH_POPPLER
    if (file && ends(*file, {".pdf", ".ai"})) {
        auto const crop = placed_page_crop(*file, &page_why, p.page);
        auto same = [](double a, double b) { return std::abs(a - b) <= 0.01 + 1e-5 * std::abs(b); };
        if (crop && (!same(crop->width(), p.box.width()) || !same(crop->height(), p.box.height()))) {
            page_why = "it is cropped to another of its boxes than its crop box, which isn't shown yet";
        }
        page_shows = crop && page_why.empty();
    }
#endif
    bool const shows = photoshop_shows ? *photoshop_shows
                     : page_shows      ? *page_shows
                                       : bool(gdk_pixbuf_get_file_info(file ? file->c_str() : "", nullptr, nullptr));
    if (file && !shows) {
        // A file of another application's (EPS), a Photoshop file whose merged image isn't
        // read, or a page that can't be placed: nothing here draws it.
        if (!_hidden && link_error.empty()) {
            link_error = photoshop_shows ? "a linked Photoshop file can't be shown: " + photoshop_why + " (" + name + ")"
                       : page_shows      ? "a linked page can't be shown: " + page_why + " (" + name + ")"
                                         : "a linked file is of a kind PROOF can't show yet (" + name + ")";
        }
        file.reset();
    } else if (!file) {
        gone = true;
        if (!_hidden && link_error.empty()) {
            if (names.empty()) {
                link_error = "a linked picture's name can't be read";
            } else if (_in_tile) {
                // What a tile holds shows wherever its pattern paints: there is no one place
                // to leave out of a comparison, or to point to.
                link_error = "a linked picture in a pattern can't be found (" + name + ")";
            }
        }
    }
    // One that can't be found keeps its link as the document has it, to be found again later.
    if (!file) {
        if (_hidden) ++hidden_links_missing;
        if (names.empty()) return nullptr;
        file = names.front();
    }
    auto e = create("svg:image");
    if (p.page != 1) {
        e->setAttribute(SPImage::PDF_PAGE_ATTRIBUTE, std::to_string(p.page));
    }
    e->setAttributeSvgDouble("x", p.box.left());
    e->setAttributeSvgDouble("y", p.box.top());
    e->setAttributeSvgDouble("width", p.box.width());
    e->setAttributeSvgDouble("height", p.box.height());
    e->setAttribute("preserveAspectRatio", "none");
    e->setAttribute("transform", sp_svg_transform_write(m));
    gchar *uri = g_path_is_absolute(file->c_str()) ? g_filename_to_uri(file->c_str(), nullptr, nullptr) : nullptr;
    e->setAttribute("xlink:href", uri ? uri : file->c_str());
    g_free(uri);
    e->setAttribute("sodipodi:absref", *file);
    if (!gone && !_hidden && !_in_tile && p.preview.width > 0 && p.preview.height > 0) {
        FoundLink link;
        link.name = name;
        link.file = *file;
        link.preview_width = p.preview.width;
        link.preview_height = p.preview.height;
        link.preview = p.preview.pixels;
        for (unsigned i = 0; i < 4; ++i) link.box[i] = p.box.corner(i) * m;
        found_links.push_back(std::move(link));
    }
    if (gone) {
        // The picture Illustrator keeps of it for this stands in its place here too.
        bool preview = false;
        if (p.preview.width > 0 && p.preview.height > 0) {
            std::vector<std::uint8_t> const pixels(p.preview.pixels.begin(), p.preview.pixels.end());
            if (auto const data = png(pixels, p.preview.width, p.preview.height, 4)) {
                e->setAttribute(SPImage::LINK_PREVIEW_ATTRIBUTE, "data:image/png;base64," + base64(*data));
                preview = true;
            }
        }
        if (!_hidden && !_in_tile) {
            MissingLink link;
            link.name = name;
            link.preview = preview;
            for (unsigned i = 0; i < 4; ++i) link.box[i] = p.box.corner(i) * m;
            missing_links.push_back(std::move(link));
        }
    }
    std::string style;
    common(e, n, style);
    if (!style.empty()) e->setAttribute("style", style);
    return e;
}

/// A file's name short enough for a note: its start and its end, with dots between.
std::string shortened(std::string const &name, glong most = 48)
{
    if (!g_utf8_validate(name.c_str(), -1, nullptr)) return name;
    auto const length = g_utf8_strlen(name.c_str(), -1);
    if (length <= most) return name;
    glong const head = most * 2 / 3, tail = most - head - 3;
    auto const *start = name.c_str();
    std::size_t const cut = g_utf8_offset_to_pointer(start, head) - start;
    std::size_t const rest = g_utf8_offset_to_pointer(start, length - tail) - start;
    return name.substr(0, cut) + "..." + name.substr(rest);
}

/// Guides from guide paths: each straight segment is a guide; curves have none.
std::size_t add_guides(SPDocument *doc, std::vector<Geom::Path> const &paths)
{
    std::size_t curved = 0;
    std::vector<std::pair<Geom::Point, Geom::Point>> made;
    auto const scale = doc->getDocumentScale();
    for (auto const &path : paths) {
        for (auto const &seg : path) {
            if (!seg.isLineSegment()) {
                ++curved;
                continue;
            }
            auto a = seg.initialPoint() * scale, b = seg.finalPoint() * scale;
            if (Geom::distance(a, b) < 1e-6) continue;
            // A rectangle's opposite sides, or a guide drawn twice, are separate lines; the same
            // line twice is one guide.
            bool const same = std::any_of(made.begin(), made.end(), [&](auto const &g) {
                auto const d = Geom::unit_vector(g.second - g.first);
                auto off = [&](Geom::Point p) { return std::abs(Geom::cross(p - g.first, d)); };
                return off(a) < 1e-3 && off(b) < 1e-3;
            });
            if (same) continue;
            made.emplace_back(a, b);
            SPGuide::createSPGuide(doc, a, b);
        }
    }
    return curved;
}

/**
 * Each line of justified area type across its measure, as Illustrator justifies it: its word
 * spaces widened or narrowed alike, each by one share of its own width, its tracking included
 * (TextLine::justified_width). The layout says where the line's letters end, without the
 * tracking after the last, and how wide its word spaces are; word-spacing on the element that
 * holds them adds the rest. Illustrator 29.2.1's glyph places in its own control (2026-10-24)
 * show both. Nothing when every line then reaches its measure; why not otherwise.
 */
std::string justify(SPDocument *doc, std::vector<std::pair<XML::Node *, double>> const &lines)
{
    if (lines.empty()) return {};
    struct Laid
    {
        SPObject *line;
        double start, end;                                    ///< Where its letters start and end.
        std::map<SPObject *, std::pair<double, unsigned>> spaces; ///< Its word spaces' widths, by holder.
    };
    auto measure = [](SPObject *line) -> std::optional<Laid> {
        auto *text = line ? cast<SPText>(line->parent) : nullptr;
        if (!text) return {};
        Laid out{line, 0, 0, {}};
        auto const &layout = text->layout;
        std::vector<std::tuple<gunichar, SPObject *, Geom::Rect>> chars;
        for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
            SPObject *source = nullptr;
            Glib::ustring::iterator at;
            layout.getSourceOfCharacter(it, &source, &at);
            auto const *string = cast<SPString>(source);
            if (!string || !string->parent) continue;
            bool inside = false;
            for (auto *o = string->parent; o && o != text && !inside; o = o->parent) inside = o == line;
            if (inside) chars.emplace_back(*at, string->parent, layout.characterBoundingBox(it));
        }
        auto const last = std::find_if(chars.rbegin(), chars.rend(), [](auto const &c) { return !g_unichar_isspace(std::get<0>(c)); });
        if (last == chars.rend()) return {};
        out.start = std::get<2>(chars.front()).left();
        out.end = std::get<2>(*last).right() - std::get<1>(*last)->style->letter_spacing.computed;
        for (auto c = chars.begin(); c != last.base() - 1; ++c) {
            auto const [unichar, holder, box] = *c;
            if (unichar != ' ') continue;
            auto const *st = holder->style;
            auto &[width, count] = out.spaces[holder];
            width += box.width() - (st->word_spacing.normal ? 0.0 : st->word_spacing.computed);
            ++count;
        }
        return out;
    };
    for (auto const &[repr, width] : lines) {
        auto const laid = measure(doc->getObjectByRepr(repr));
        if (!laid) continue;
        double spaces = 0;
        for (auto const &[holder, sum] : laid->spaces) spaces += sum.first;
        if (spaces <= 0) continue; // A word alone, or none: its letters aren't spaced out.
        double const share = (width - (laid->end - laid->start)) / spaces;
        if (!std::isfinite(share) || share < -0.9) return "a justified line is too long for its frame";
        for (auto const &[holder, sum] : laid->spaces) {
            auto *own = holder->getRepr();
            auto const *style = own->attribute("style");
            own->setAttribute("style", std::string(style ? style : "") + "word-spacing:" + num(share * sum.first / sum.second) + ";");
        }
    }
    doc->ensureUpToDate();
    for (auto const &[repr, width] : lines) {
        auto const laid = measure(doc->getObjectByRepr(repr));
        if (laid && !laid->spaces.empty() && std::abs(laid->end - laid->start - width) > 0.01) {
            return "justified type its fonts here can't set across its frame";
        }
    }
    return {};
}

} // namespace

std::optional<Built> build(Document const &ai, std::string &error, Source const &source)
{
    error.clear();
    // These warnings describe artwork the reader replaced or omitted. A small
    // omission can fall below the visual tolerance, so refuse it explicitly.
    if (!ai.warnings.empty()) {
        error = ai.warnings.front();
        return {};
    }
    TextCheck text;
    error = check_text(ai, ai.layers, true, text);
    if (!error.empty()) return {};
    Built built;
    built.missing_fonts = text.missing;
    built.substituted_type = std::move(text.substituted_type);
    std::string stand_in;
    for (auto const &[name, font] : text.fonts) {
        if (font && font->missing && font->font.face) stand_in = sp_font_description_get_family(font->font.face->get_descr());
    }
    auto doc = SPDocument::createNewDoc(nullptr, true);
    if (!doc || !doc->getRoot()) {
        error = "a document couldn't be made";
        return {};
    }
    // Its pictures are loaded as they are built: the folder they are linked from decides which
    // may be looked up (io/link-policy.h). The file's own name is given it after the import.
    if (!source.folder.empty()) doc->setDocumentBase(source.folder.c_str());
    auto const undo = DocumentUndo::getUndoSensitive(doc.get());
    DocumentUndo::setUndoSensitive(doc.get(), false);

    for (auto const &a : ai.artboards) {
        auto rect = a.rect;
        rect *= ai.to_doc;
        built.artboards.push_back(rect);
    }
    Geom::Rect first(0, 0, 612, 792);
    if (!built.artboards.empty()) {
        first = built.artboards.front();
    } else if (ai.bbox) {
        first = *ai.bbox;
        first *= ai.to_doc;
    }

    auto *root = doc->getReprRoot();
    auto const unit = unit_of(ai.ruler_units);
    root->setAttribute("width", num(Util::Quantity::convert(first.width(), "pt", unit)) + unit);
    root->setAttribute("height", num(Util::Quantity::convert(first.height(), "pt", unit)) + unit);
    root->setAttribute("viewBox", num(first.left()) + " " + num(first.top()) + " " + num(first.width()) + " " +
                                      num(first.height()));
    if (auto nv = doc->getNamedView()) nv->getRepr()->setAttribute("inkscape:document-units", unit);

    // The document's colour mode and profile come first: process colours are written in it.
    // Its own profile when the file carries one, as Illustrator shows a document by the
    // profile it was saved with: adopt() keeps a profile of the mode that is already there,
    // and gives PROOF's working profile otherwise.
    if (!source.profile.empty()) {
        auto const profile = Colors::CMS::Profile::create_from_data(source.profile);
        if (profile && profile->getColorSpace() == (ai.cmyk ? cmsSigCmykData : cmsSigRgbData)) {
            doc->getDocumentCMS().attachProfileToDoc(*profile, ColorProfileStorage::HREF_DATA,
                                                     Colors::DocumentColors::workingIntent());
            built.profile = profile->getName();
        }
    }
    Colors::DocumentColors::adopt(doc.get(), ai.cmyk ? Type::CMYK : Type::RGB);
    // And how it sets superscripts, subscripts and small capitals, which its text is laid out by.
    if (ai.texts && ai.texts->error.empty()) ai.texts->type.write(doc.get());

    Builder b(ai, doc.get(), std::move(text), source);
    b.run();
    if (!b.foreign_error.empty()) {
        error = b.foreign_error;
        return {};
    }
    if (b.images_left_out) {
        error = "an embedded image couldn't be placed";
        return {};
    }
    if (!b.link_error.empty()) {
        error = b.link_error;
        return {};
    }

    // Artboards are pages.
    auto *defs = doc->getDefs()->getRepr();
    for (std::size_t i = 0; i < built.artboards.size(); ++i) {
        auto const &r = built.artboards[i];
        auto view = doc->getReprDoc()->createElement("svg:view");
        view->setAttribute("viewBox", num(r.left()) + " " + num(r.top()) + " " + num(r.width()) + " " + num(r.height()));
        view->setAttribute("inkscape:label", ai.artboards[i].name);
        defs->appendChild(view);
        GC::release(view);
    }
    doc->ensureUpToDate();
    auto const curved = add_guides(doc.get(), b.guides);
    for (auto *node : b.automatic_path_text) {
        auto const *text = cast<SPText>(doc->getObjectByRepr(node));
        if (!text || !text->layout.automaticPathSpacingSupported()) {
            error = "automatic type spacing on a sharp, very tight or collapsed path";
            return {};
        }
    }
    if (auto why = justify(doc.get(), b.justified_lines); !why.empty()) {
        error = std::move(why);
        return {};
    }

    auto &notes = built.notes;
    auto count = [](std::size_t n, char const *one, char const *many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    if (b.unused_gradient_end_stops) {
        notes.push_back(count(b.unused_gradient_end_stops, "unused gradient stop at 100% was left out",
                             "unused gradient stops at 100% were left out") +
                        "; Illustrator extends with the first stop there, where SVG uses the last");
    }
    if (ai.drawn_looks) {
        notes.push_back(count(ai.drawn_looks, "object", "objects") +
                        " with several fills or strokes, effects or a brush came in as groups of what they draw");
    }
    if (ai.envelopes) {
        notes.push_back(count(ai.envelopes, "object distorted by an envelope came in as the outlines it draws",
                              "objects distorted by envelopes came in as the outlines they draw") +
                        ": what was distorted, type or art, isn't kept");
    }
    if (!ai.plugin_objects.empty()) {
        // Compound shapes, blends and brush strokes: what a plug-in keeps live in Illustrator.
        std::size_t shapes = 0, blends = 0, strokes = 0;
        for (auto const &[name, n] : ai.plugin_objects) {
            (name == "Pathfinder Suite" ? shapes : name == "Adobe Path Blends" ? blends : strokes) += n;
        }
        std::vector<std::string> kinds;
        if (shapes) kinds.push_back(count(shapes, "compound shape", "compound shapes"));
        if (blends) kinds.push_back(count(blends, "blend", "blends"));
        if (strokes) kinds.push_back(count(strokes, "brush stroke", "brush strokes"));
        std::string list;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            list += (i == 0 ? "" : i + 1 == kinds.size() ? " and " : ", ") + kinds[i];
        }
        bool const one = shapes + blends + strokes == 1;
        notes.push_back(list + " came in as what " + (one ? "it draws" : "they draw") + ": the paths " +
                        (one ? "it was" : "they were") + " made from aren't kept");
    }
    if (b.hidden_text) {
        notes.push_back(count(b.hidden_text, "text object", "text objects") +
                        " that don't show were left out: their story, style or exact font isn't supported");
    }
    if (b.empty_text) {
        notes.push_back(count(b.empty_text, "text frame with nothing typed in it was", "text frames with nothing typed in them were") +
                        " left out");
    }
    if (b.paragraph_styled_text) {
        notes.push_back(count(b.paragraph_styled_text, "text object's", "text objects'") +
                        " paragraph styles came in as inline formatting; named style definitions and links aren't kept");
    }
    if (b.path_text) {
        notes.push_back(count(b.path_text, "path text object's", "path text objects'") +
                        " start and end brackets are kept as distances along the editable path; Illustrator's parameter brackets were left out");
    }
    if (b.path_options) {
        notes.push_back(count(b.path_options, "path text object's", "path text objects'") +
                        " source options are kept as metadata; requested automatic curve spacing uses a font-metric approximation");
    }
    if (b.overflowing_text) {
        notes.push_back(count(b.overflowing_text, "text frame holds", "text frames hold") +
                        " more text than fits; what doesn't show is kept with the frame as plain text");
    }
    if (b.gradient_text) {
        notes.push_back(count(b.gradient_text, "text object is", "text objects are") +
                        " painted with a gradient, which Illustrator doesn't draw on type: the characters are "
                        "black there, and here");
    }
    if (b.unordered_text) {
        notes.push_back(count(b.unordered_text, "text object has", "text objects have") +
                        " letters of a script read from the right that Illustrator set from the left, as they were "
                        "typed: they are set that way here too, and show backwards");
    }
    if (b.optical_kerning) {
        notes.push_back(count(b.optical_kerning, "text object is", "text objects are") +
                        " kerned optically in Illustrator; here the letters are spaced by their fonts' own kerning");
    }
    if (b.substituted_text) {
        std::string names;
        for (auto const &name : built.missing_fonts) names += (names.empty() ? "" : ", ") + name;
        notes.push_back(count(built.missing_fonts.size(), "font isn't", "fonts aren't") + " installed (" + names + "): " +
                        count(b.substituted_text, "text object is", "text objects are") + " set in " + stand_in +
                        " until " + (built.missing_fonts.size() == 1 ? "it is" : "they are"));
    }
    if (b.glyphs_missing_text) {
        notes.push_back(count(b.glyphs_missing_text, "text object has", "text objects have") +
                        " characters that " + (b.glyphs_missing_text == 1 ? "its font doesn't" : "their fonts don't") +
                        " have; another font draws those");
    }
    // Where that type lies here, with the font that sets it.
    for (auto *repr : b.substituted_shown) {
        auto *item = cast<SPItem>(doc->getObjectByRepr(repr));
        auto bounds = item ? item->documentVisualBounds() : Geom::OptRect();
        if (!bounds) continue;
        *bounds *= doc->getDocumentScale().inverse();
        built.substituted_type.push_back({bounds->corner(0), bounds->corner(1), bounds->corner(2), bounds->corner(3)});
    }
    if (!b.missing_links.empty()) {
        auto const &links = b.missing_links;
        // A few names say which; a document can link dozens, and one file many times.
        std::vector<std::string> distinct;
        std::size_t kept = 0;
        for (auto const &link : links) {
            kept += link.preview;
            if (std::find(distinct.begin(), distinct.end(), link.name) == distinct.end()) distinct.push_back(link.name);
        }
        std::string names;
        for (std::size_t i = 0; i < distinct.size() && i < 3; ++i) names += (i ? ", " : "") + shortened(distinct[i]);
        if (distinct.size() > 3) names += " and " + std::to_string(distinct.size() - 3) + " more";
        bool const one = links.size() == 1;
        std::string shows;
        if (!kept) {
            shows = one ? "a marked box shows in its place" : "a marked box shows in the place of each";
        } else {
            shows = std::string("the small picture the document keeps of ") + (one ? "it" : "each") + " shows in its place";
            if (kept < links.size()) shows += ", or a marked box where it keeps none,";
        }
        notes.push_back(count(links.size(), "linked picture can't", "linked pictures can't") + " be found (" + names +
                        "): " + shows + " until it is linked again");
        built.missing_links = links;
    }
    if (!b.elsewhere.empty()) {
        std::string where;
        for (auto const &share : b.elsewhere) where += (where.empty() ? "" : ", ") + share;
        notes.push_back("linked files on another computer weren't looked up (" + where +
                        "): Windows would offer it your sign-in. Relink them, or open the document from that share");
    }
    built.found_links = std::move(b.found_links);
    if (!b.other_profiles.empty()) {
        std::string names;
        for (auto const &[file, profile] : b.other_profiles) {
            names += (names.empty() ? "" : ", ") + file + ": " + profile;
        }
        notes.push_back(count(b.other_profiles.size(), "linked CMYK picture carries", "linked CMYK pictures carry") +
                        " a profile other than the document's (" + names + "); " +
                        (b.other_profiles.size() == 1 ? "its numbers are" : "their numbers are") +
                        " kept, and shown through the document's profile");
    }
    if (b.hidden_links_missing) {
        notes.push_back(count(b.hidden_links_missing, "linked picture that doesn't show wasn't",
                              "linked pictures that don't show weren't") +
                        " found; the links are kept as the document has them");
    }
    if (b.mesh_outlines) notes.push_back(count(b.mesh_outlines, "uniform mesh patch came in as the outlines",
                                             "uniform mesh patches came in as the outlines") + " they draw; mesh editing is not kept for them");
    if (b.mesh_cut) notes.push_back(count(b.mesh_cut, "mesh patch bent through anchors came in cut into cells there",
                                          "mesh patches bent through anchors came in cut into cells there") +
                                    ", each anchor a mesh point with the colour it had between its corners, on a mesh line across its mesh");
    if (b.mesh_split) notes.push_back(count(b.mesh_split, "gradient mesh came in patch by patch, each its own mesh",
                                            "gradient meshes came in patch by patch, each its own mesh") +
                                      ": not one grid, or more than 4096 cells as one");
    if (b.foreign_objects) notes.push_back(count(b.foreign_objects, "foreign object came in as what it draws, in an editable group",
                                               "foreign objects came in as what they draw, in editable groups") +
        "; vector paths and gradients are editable; the original embedded PDF is kept separately and is not updated by edits");
    if (b.hidden_foreign) notes.push_back(count(b.hidden_foreign, "hidden foreign object was left out", "hidden foreign objects were left out"));
    if (b.knockouts) notes.push_back(count(b.knockouts, "knockout group", "knockout groups") + " draw as ordinary groups");
    if (curved) notes.push_back(count(curved, "curved guide segment was", "curved guide segments were") + " left out");
    for (auto const &l : ai.left_out) notes.push_back("hidden layers hold " + l + ", left out of them");
    for (auto const &w : ai.warnings) notes.push_back(w);

    doc->ensureUpToDate();
    DocumentUndo::setUndoSensitive(doc.get(), undo);
    built.document = std::move(doc);
    return built;
}

} // namespace Inkscape::Extension::Internal::AiNative
