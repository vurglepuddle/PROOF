// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: make a document from Illustrator's native records. See ai-native-import.h.
 */
#include "ai-native-import.h"
#include "ai-native-text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
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
#include "colors/document-colors.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/enum.h"
#include "colors/spaces/lab.h"
#include "document-undo.h"
#include "document.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "object/sp-defs.h"
#include "object/sp-guide.h"
#include "object/sp-namedview.h"
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

/// A tint of a colour given at full strength: inks scale, other models blend towards paper.
Color tinted(Color c, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    switch (c.model) {
        case Color::Model::Cmyk:
            for (int i = 0; i < 4; ++i) c.v[i] *= t;
            break;
        case Color::Model::Gray:
            c.v[0] = 1.0 - t * (1.0 - c.v[0]);
            break;
        case Color::Model::Rgb:
            for (int i = 0; i < 3; ++i) c.v[i] = 1.0 - t * (1.0 - c.v[i]);
            break;
        case Color::Model::Lab:
            c.v[0] = 100.0 - t * (100.0 - c.v[0]);
            c.v[1] *= t;
            c.v[2] *= t;
            break;
    }
    return c;
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

std::string font_style(std::string const &name, std::shared_ptr<FontInstance> &face)
{
    auto *desc = FontFactory::get().parsePostscriptName(name, false);
    if (!desc) return {};
    Glib::ustring family = sp_font_description_get_family(desc);
    css_font_family_quote(family);
    auto *spec = pango_font_description_to_string(desc);
    Glib::ustring specification = spec ? spec : "";
    g_free(spec); css_quote(specification);
    auto css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "font-family", family.c_str());
    sp_repr_css_set_property(css, "-inkscape-font-specification", specification.c_str());
    sp_repr_css_set_property(css, "font-weight", std::to_string(pango_font_description_get_weight(desc)).c_str());
    auto const slant = pango_font_description_get_style(desc);
    sp_repr_css_set_property(css, "font-style", slant == PANGO_STYLE_ITALIC ? "italic" : slant == PANGO_STYLE_OBLIQUE ? "oblique" : "normal");
    constexpr char const *stretch[] = {"ultra-condensed", "extra-condensed", "condensed", "semi-condensed", "normal",
                                      "semi-expanded", "expanded", "extra-expanded", "ultra-expanded"};
    sp_repr_css_set_property(css, "font-stretch", stretch[std::clamp(int(pango_font_description_get_stretch(desc)), 0, 8)]);
    Glib::ustring result;
    sp_repr_css_write_string(css, result);
    sp_repr_css_attr_unref(css);
    try { face = FontFactory::get().Face(desc, false); }
    catch (std::runtime_error const &) { result.clear(); }
    pango_font_description_free(desc);
    return result.empty() ? std::string{} : result.raw() + ";";
}

std::string check_text(Document const &ai, std::vector<Node> const &nodes, bool shown,
                       std::map<std::string, std::string> &fonts,
                       std::map<std::string, std::shared_ptr<FontInstance>> &faces,
                       std::set<std::size_t> &unsupported_hidden)
{
    for (auto const &n : nodes) {
        bool const visible = shown && n.visible;
        if (n.kind == Node::Kind::Text) {
            std::string error;
            if (!ai.texts) error = "the text document is missing";
            else if (!ai.texts->error.empty()) error = ai.texts->error;
            else if (!n.story || *n.story >= ai.texts->stories.size()) error = "a text object has no matching story";
            else if (!ai.texts->stories[*n.story]) error = ai.texts->errors[*n.story];
            else for (auto const &line : ai.texts->stories[*n.story]->lines) for (auto const &run : line.runs) {
                auto const &name = run.style.font;
                if (!fonts.count(name)) fonts[name] = font_style(name, faces[name]);
                if (fonts[name].empty() || !faces[name]) error = "the exact font '" + name + "' isn't installed or couldn't be loaded";
                else for (auto const *at = run.text.data(); at < run.text.data() + run.text.size(); at = g_utf8_next_char(at)) {
                    auto const cp = g_utf8_get_char(at);
                    if (g_unichar_type(cp) != G_UNICODE_FORMAT && !faces[name]->MapUnicodeChar(cp)) {
                        error = "the exact font '" + name + "' doesn't contain every text character";
                        break;
                    }
                }
            }
            if (visible && !error.empty()) return "its visible text can't be reconstructed: " + error;
            if (!visible && !error.empty() && n.story) unsupported_hidden.insert(*n.story);
        } else if (auto error = check_text(ai, n.children, visible, fonts, faces, unsupported_hidden); !error.empty()) return error;
    }
    return {};
}

/// Every path of an object and its children, in art space.
void outline(Node const &n, Geom::PathVector &out)
{
    for (auto const &p : n.path) out.push_back(p);
    for (auto const &k : n.children) outline(k, out);
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
        if (n.kind != Node::Kind::Path || n.guide || !n.visible || n.stroke || !n.fill ||
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
    bool const ok = gdk_pixbuf_save_to_buffer(pb, &buf, &len, "png", &err, "compression", "6", nullptr);
    g_object_unref(pb);
    if (!ok) {
        if (err) g_error_free(err);
        return {};
    }
    std::string out(buf, len);
    g_free(buf);
    return out;
}

/// A CMYK TIFF (Deflate) of 8-bit samples, with unassociated alpha when given.
std::optional<std::string> cmyk_tiff(std::uint32_t w, std::uint32_t h, std::string_view cmyk, std::string_view alpha)
{
    std::uint64_t const pixels = std::uint64_t(w) * h;
    if (cmyk.size() < pixels * 4) return {};
    bool const with_alpha = alpha.size() >= pixels;
    std::uint16_t const spp = with_alpha ? 5 : 4;
    std::string raw;
    if (with_alpha) {
        raw.resize(pixels * 5);
        for (std::uint64_t i = 0; i < pixels; ++i) {
            std::copy_n(cmyk.data() + i * 4, 4, raw.data() + i * 5);
            raw[i * 5 + 4] = alpha[i];
        }
    } else {
        raw.assign(cmyk.substr(0, pixels * 4));
    }
    uLongf len = compressBound(raw.size());
    std::string z(len, '\0');
    if (compress2(reinterpret_cast<Bytef *>(z.data()), &len, reinterpret_cast<Bytef const *>(raw.data()), raw.size(),
                  6) != Z_OK) {
        return {};
    }
    z.resize(len);

    struct Entry
    {
        std::uint16_t tag, type;
        std::uint32_t count, value;
    };
    constexpr std::uint16_t SHORT = 3, LONG = 4;
    std::vector<Entry> e = {{256, LONG, 1, w},        {257, LONG, 1, h},
                            {258, SHORT, spp, 0},     {259, SHORT, 1, 8}, // Adobe Deflate
                            {262, SHORT, 1, 5},       {273, LONG, 1, 0},  // separated (CMYK)
                            {277, SHORT, 1, spp},     {278, LONG, 1, h},
                            {279, LONG, 1, std::uint32_t(z.size())}, {284, SHORT, 1, 1},
                            {332, SHORT, 1, 1}};                          // InkSet: CMYK
    if (with_alpha) e.push_back({338, SHORT, 1, 2});                      // unassociated alpha
    std::uint32_t const ifd = 8;
    std::uint32_t const bits = ifd + 2 + 12 * std::uint32_t(e.size()) + 4;
    std::uint32_t const strip = bits + 2 * spp;
    e[2].value = bits;
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
    for (int i = 0; i < spp; ++i) put16(8);
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

class Builder
{
public:
    Builder(Document const &ai, SPDocument *doc, std::map<std::string, std::string> fonts,
            std::set<std::size_t> unsupported_hidden);
    ~Builder();
    Builder(Builder const &) = delete;
    Builder &operator=(Builder const &) = delete;

    /// Add every layer to the document.
    void run();

    std::vector<Geom::Path> guides; ///< In the document's user units.
    std::size_t hidden_text = 0;
    std::size_t knockouts = 0;
    std::size_t images_left_out = 0;

private:
    XML::Node *create(char const *name) { return _xml->createElement(name); }
    static void append(XML::Node *parent, XML::Node *child)
    {
        parent->appendChild(child);
        GC::release(child);
    }
    std::string unique(std::string const &stem);

    XML::Node *node(Node const &n);
    XML::Node *layer(Node const &n);
    XML::Node *group(Node const &n);
    XML::Node *shape(Node const &n, Geom::PathVector const &pv);
    XML::Node *image(Node const &n);
    XML::Node *text(Node const &n);
    void children(XML::Node *parent, std::vector<Node> const &kids, bool clipped);
    std::string clip_path(Node const &clip);
    void common(XML::Node *e, Node const &n, std::string &style);

    Colors::Color process(Color const &c) const;
    std::string paint(Paint const &p);
    std::string ink(int index, double tint);
    std::string process_swatch(Ink const &ink, double tint);
    std::string gradient(Paint const &p);
    std::string gradient_vector(int index);
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
    std::map<int, std::string> _vectors;               ///< Gradient to its vector's id.
    std::map<std::string, int> _counters;
    std::map<std::string, std::string> _text_fonts;
    std::set<std::size_t> _unsupported_hidden_text;
};

Builder::Builder(Document const &ai, SPDocument *doc, std::map<std::string, std::string> fonts,
                 std::set<std::size_t> unsupported_hidden)
    : _ai(ai)
    , _doc(doc)
    , _xml(doc->getReprDoc())
    , _defs(doc->getDefs()->getRepr())
    , _to_doc(ai.to_doc)
    , _text_fonts(std::move(fonts))
    , _unsupported_hidden_text(std::move(unsupported_hidden))
{
    auto space = Colors::DocumentColors::assignedSpace(doc);
    if (space && space->getComponentType() == Type::CMYK) {
        _cmyk_space = space;
        if (auto profile = space->getProfile()) {
            _srgb = Colors::CMS::Profile::create_srgb();
            auto const intent = Colors::DocumentColors::workingIntent();
            cmsUInt32Number const flags =
                intent == Colors::RenderingIntent::RELATIVE_COLORIMETRIC ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0;
            _cmyk_to_rgb = cmsCreateTransform(profile->getHandle(), TYPE_CMYK_8, _srgb->getHandle(), TYPE_RGB_8,
                                              lcms_intent(intent), flags);
        }
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
}

XML::Node *Builder::node(Node const &n)
{
    switch (n.kind) {
        case Node::Kind::Layer:
            return layer(n);
        case Node::Kind::Group:
            return group(n);
        case Node::Kind::Compound: {
            Geom::PathVector pv;
            outline(n, pv);
            return shape(n, pv);
        }
        case Node::Kind::Path:
            if (n.guide) {
                for (auto const &p : n.path) guides.push_back(p * _to_doc);
                return nullptr;
            }
            return shape(n, n.path);
        case Node::Kind::Image:
            return image(n);
        case Node::Kind::Text:
            return text(n);
    }
    return nullptr;
}

XML::Node *Builder::text(Node const &n)
{
    if (!_ai.texts || !n.story || *n.story >= _ai.texts->stories.size() || !_ai.texts->stories[*n.story] ||
        _unsupported_hidden_text.count(*n.story)) {
        ++hidden_text; return nullptr;
    }
    auto const &story = *_ai.texts->stories[*n.story];
    for (auto const &line : story.lines) for (auto const &run : line.runs) {
        auto it = _text_fonts.find(run.style.font);
        if (it == _text_fonts.end() || it->second.empty()) { ++hidden_text; return nullptr; }
    }
    auto e = create("svg:text");
    e->setAttribute("xml:space", "preserve");
    e->setAttribute("proof:ai-story", std::to_string(*n.story));
    e->setAttribute("transform", sp_svg_transform_write(story.to_art * _to_doc));
    // A run's style in parts, each inherited by spans. The text object carries the first
    // run's, so its fill, stroke and font are the object's own, as the Appearance controls
    // read and set them; a span carries only what its run has different.
    auto parts = [&](auto const &s) {
        auto color = [&](std::optional<Color> const &c) { Paint p; if (c) p.color = *c; return c ? paint(p) : std::string("none"); };
        return std::array<std::string, 8>{
            _text_fonts.at(s.font), "font-size:" + num(s.size) + ";", "fill:" + color(s.fill) + ";",
            "fill-opacity:" + num(s.fill_opacity) + ";", "stroke:" + color(s.stroke) + ";",
            "stroke-opacity:" + num(s.stroke_opacity) + ";", "stroke-width:" + num(s.stroke_width) + ";",
            "letter-spacing:" + num(s.size * s.tracking / 1000.0) + ";"};
    };
    std::optional<std::array<std::string, 8>> base;
    for (auto const &line : story.lines) {
        if (!base && !line.runs.empty()) base = parts(line.runs.front().style);
    }
    std::string common_style = "text-anchor:start;";
    common(e, n, common_style);
    if (base) for (auto const &part : *base) common_style += part;
    e->setAttribute("style", common_style);
    for (auto const &line : story.lines) {
        auto span = create("svg:tspan");
        // Explicit ATE line positions must remain SVG positions. role="line"
        // asks Inkscape to reflow the line and discard a single x/y pair.
        span->setAttribute("x", num(line.origin.x())); span->setAttribute("y", num(line.origin.y()));
        span->setAttribute("style", std::string("text-anchor:") +
                                   (line.alignment == 1 ? "end" : line.alignment == 2 ? "middle" : "start") + ";");
        std::string plain; // Text in the object's own style, not yet written.
        auto write_plain = [&] {
            if (!plain.empty()) append(span, _xml->createTextNode(plain.c_str()));
            plain.clear();
        };
        for (auto const &run : line.runs) {
            auto const own = parts(run.style);
            std::string style;
            for (std::size_t i = 0; i < own.size(); ++i) {
                if (own[i] != (*base)[i]) style += own[i];
            }
            if (style.empty()) {
                plain += run.text;
                continue;
            }
            write_plain();
            auto piece = create("svg:tspan");
            piece->setAttribute("style", style);
            append(piece, _xml->createTextNode(run.text.c_str())); append(span, piece);
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
        // A layer's clipping path clips its other objects: a clip group inside the layer.
        auto clip = create("svg:g");
        clip->setAttribute("inkscape:label", "Clip Group");
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
    if (clipped && !kids.empty()) {
        parent->setAttribute("clip-path", "url(#" + clip_path(kids.front()) + ")");
        // A clipping path can also be a painted background (W f / W B).
        if (!kids.front().fill && !kids.front().stroke) start = 1;
    }
    auto const seamless = tiles(kids, start, clipped && !kids.empty() ? &kids.front() : nullptr, _to_doc);
    for (std::size_t i = start; i < kids.size(); ++i) {
        if (auto e = node(kids[i])) {
            if (seamless.count(i)) {
                auto const style = e->attribute("style");
                e->setAttribute("style", std::string(style ? style : "") + "shape-rendering:crispEdges;");
            }
            append(parent, e);
        }
    }
}

std::string Builder::clip_path(Node const &clip)
{
    Geom::PathVector pv;
    outline(clip, pv);
    auto cp = create("svg:clipPath");
    auto const id = unique("clipPath");
    cp->setAttribute("id", id);
    cp->setAttribute("clipPathUnits", "userSpaceOnUse");
    auto p = create("svg:path");
    p->setAttribute("d", sp_svg_write_path(pv * _to_doc));
    if (clip.evenodd) p->setAttribute("style", "clip-rule:evenodd");
    if (!clip.name.empty()) p->setAttribute("inkscape:label", clip.name);
    append(cp, p);
    append(_defs, cp);
    return id;
}

XML::Node *Builder::shape(Node const &n, Geom::PathVector const &pv)
{
    if (pv.empty()) return nullptr;
    auto e = create("svg:path");
    e->setAttribute("d", sp_svg_write_path(pv * _to_doc));
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

Colors::Color Builder::process(Color const &c) const
{
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
    if (p.ink >= 0 && p.ink < static_cast<int>(_ai.inks.size())) return ink(p.ink, p.tint);
    return process(p.color).toString(false);
}

std::string Builder::ink(int index, double tint)
{
    tint = std::clamp(tint, 0.0, 1.0);
    auto const key = std::pair(index, tint);
    if (auto it = _inks.find(key); it != _inks.end()) return it->second;
    auto const &ink = _ai.inks[index];
    std::string id;
    if (ink.kind == Ink::Kind::Process) {
        id = process_swatch(ink, tint);
    } else {
        auto const name = ink.kind == Ink::Kind::Registration ? std::string(REGISTRATION_INK) : ink.name;
        id = SpotInk::ensure(_xml, _defs, _doc, SpotInk::Ink{name, definition(ink.color), tint});
    }
    return _inks[key] = "url(#" + id + ")";
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
    if (auto it = _vectors.find(index); it != _vectors.end()) return it->second;
    auto const &g = _ai.gradients[index];
    auto vec = create("svg:linearGradient");
    auto const id = unique("gradient");
    vec->setAttribute("id", id);
    if (!g.name.empty()) vec->setAttribute("inkscape:label", g.name);
    auto const &stops = g.stops;
    for (std::size_t i = 0; i < stops.size(); ++i) {
        auto const &a = stops[i];
        StopPaint const pa{a.color, a.ink, a.tint};
        auto const ca = shown(pa);
        add_stop(vec, a.offset, &pa, ca, a.opacity, false);
        if (i + 1 == stops.size() || std::abs(a.midpoint - 0.5) < 0.005) continue;
        // Illustrator's blend between two stops follows u^n, half-way at the midpoint.
        auto const &b = stops[i + 1];
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
                // Different models: blend what they show.
                auto ra = ca.converted(Type::RGB), rb = cb.converted(Type::RGB);
                if (!ra || !rb) continue;
                std::vector<double> rgb(3);
                for (int k = 0; k < 3; ++k) rgb[k] = (*ra)[k] + ((*rb)[k] - (*ra)[k]) * v;
                add_stop(vec, offset, nullptr, Colors::Color(Type::RGB, std::move(rgb)), opacity, true);
            }
        }
    }
    append(_defs, vec);
    return _vectors[index] = id;
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
    // The matrix maps pixels (y down) into art space with its y flipped.
    auto const &r = img.matrix;
    Geom::Affine const m = Geom::Affine(r[0], r[1], r[2], r[3], r[4], -r[5]) * Geom::Scale(1, -1) * _to_doc;
    if (!finite(m) || m.isSingular()) {
        ++images_left_out;
        return nullptr;
    }
    std::string eight;
    int channels = 3;
    auto const pixels = screen_pixels(img, eight, channels);
    auto const data = png(pixels, img.width, img.height, channels);
    if (!data) {
        ++images_left_out;
        return nullptr;
    }
    auto e = create("svg:image");
    e->setAttributeSvgDouble("x", 0);
    e->setAttributeSvgDouble("y", 0);
    e->setAttributeSvgDouble("width", img.width);
    e->setAttributeSvgDouble("height", img.height);
    e->setAttribute("preserveAspectRatio", "none");
    e->setAttribute("transform", sp_svg_transform_write(m));
    e->setAttribute("xlink:href", "data:image/png;base64," + base64(*data));
    if (img.channels == 4) {
        // The screen sees RGB; the CMYK samples stay for print.
        if (auto tiff = cmyk_tiff(img.width, img.height, eight, img.alpha)) {
            e->setAttribute("proof:samples", "data:image/tiff;base64," + base64(*tiff));
        }
    }
    std::string style;
    common(e, n, style);
    if (!style.empty()) e->setAttribute("style", style);
    return e;
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

} // namespace

std::optional<Built> build(Document const &ai, std::string &error)
{
    error.clear();
    // These warnings describe artwork the reader replaced or omitted. A small
    // omission can fall below the visual tolerance, so refuse it explicitly.
    if (!ai.warnings.empty()) {
        error = ai.warnings.front();
        return {};
    }
    std::map<std::string, std::string> fonts;
    std::map<std::string, std::shared_ptr<FontInstance>> faces;
    std::set<std::size_t> unsupported_hidden;
    error = check_text(ai, ai.layers, true, fonts, faces, unsupported_hidden);
    if (!error.empty()) return {};
    auto doc = SPDocument::createNewDoc(nullptr, true);
    if (!doc || !doc->getRoot()) {
        error = "a document couldn't be made";
        return {};
    }
    auto const undo = DocumentUndo::getUndoSensitive(doc.get());
    DocumentUndo::setUndoSensitive(doc.get(), false);

    Built built;
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
    Colors::DocumentColors::adopt(doc.get(), ai.cmyk ? Type::CMYK : Type::RGB);

    Builder b(ai, doc.get(), std::move(fonts), std::move(unsupported_hidden));
    b.run();
    if (b.images_left_out) {
        error = "an embedded image couldn't be placed";
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

    auto &notes = built.notes;
    auto count = [](std::size_t n, char const *one, char const *many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    if (ai.drawn_looks) {
        notes.push_back(count(ai.drawn_looks, "object", "objects") +
                        " with several fills or strokes, effects or a brush came in as groups of what they draw");
    }
    if (b.hidden_text) {
        notes.push_back(count(b.hidden_text, "text object", "text objects") +
                        " that don't show were left out: their story, style or exact font isn't supported");
    }
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
