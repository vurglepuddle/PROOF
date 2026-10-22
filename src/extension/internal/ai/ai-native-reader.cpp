// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: read Illustrator's native records into a document structure.
 * See ai-native-reader.h and doc/PROOF-ai-native-records.md.
 *
 * The operator meanings are ported from VectorCraft's reader
 * (crates/eps/src/import/ai/{read,paint,obj}.rs at 4cf912f), Copyright (c) 2026
 * ArtCraft Team and the VectorCraft contributors, MIT licence; see
 * LICENSES/MIT-VectorCraft.txt.
 */
#include "ai-native-reader.h"
#include "ai-native-text.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <glib.h>

#include <2geom/bezier-curve.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>

#include "ai-native-lexer.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

// ---- values and dictionaries (obj.rs) ---------------------------------------------------

struct Obj;
struct Mark {};
struct Value;
using Array = std::vector<Value>;
using ObjPtr = std::shared_ptr<Obj const>;

struct Value
{
    enum class Kind { Num, Str, Name, Arr, Dict, Mark } kind = Kind::Num;
    double num = 0.0;
    std::string str; ///< Str: bytes; Name: text.
    bool legacy_text = false; ///< The old file-format header declares Windows-1252 names.
    std::shared_ptr<Array> arr;
    ObjPtr obj;

    static Value number(double v) { Value x; x.kind = Kind::Num; x.num = v; return x; }
    static Value string(std::string s, bool legacy = false) { Value x; x.kind = Kind::Str; x.str = std::move(s); x.legacy_text = legacy; return x; }
    static Value name(std::string s) { Value x; x.kind = Kind::Name; x.str = std::move(s); return x; }
    static Value mark() { Value x; x.kind = Kind::Mark; return x; }

    std::optional<double> as_num() const { return kind == Kind::Num ? std::optional(num) : std::nullopt; }
    /// A string's or name's text.
    std::optional<std::string> text() const
    {
        if (kind == Kind::Str) return text_of(str, legacy_text);
        if (kind == Kind::Name) return str;
        return {};
    }
};

using Values = std::vector<Value>;

std::vector<double> nums_of(Values const &vals)
{
    std::vector<double> out;
    for (auto const &v : vals) {
        if (v.kind == Value::Kind::Num) out.push_back(v.num);
    }
    return out;
}

/// The last string or name among `vals`.
std::optional<std::string> last_text(Values const &vals)
{
    for (auto it = vals.rbegin(); it != vals.rend(); ++it) {
        if (auto t = it->text()) return t;
    }
    return {};
}

struct Obj
{
    std::string type;
    std::string_view foreign_data; ///< Raw ASCII85 after /Data, including its terminator.
    std::vector<Node> art; ///< Artwork inside a Mask dictionary, in art coordinates.
    bool unsupported_mask_art = false; ///< Includes dictionaries consumed by drawing operators.
    /// Entries in order: the key (none in an array) and the values before it.
    std::vector<std::pair<std::optional<std::string>, Values>> entries;
    /// Values closed without a key.
    Values content;

    Values const *get(std::string_view key) const
    {
        for (auto const &[k, v] : entries) {
            if (k && *k == key) return &v;
        }
        return nullptr;
    }
    std::vector<double> nums(std::string_view key) const
    {
        auto v = get(key);
        return v ? nums_of(*v) : std::vector<double>{};
    }
    std::optional<std::string> text(std::string_view key) const
    {
        if (auto v = get(key)) {
            for (auto const &x : *v) {
                if (x.kind == Value::Kind::Str) return x.text();
            }
        }
        return {};
    }
    Obj const *obj(std::string_view key) const
    {
        if (auto v = get(key)) {
            for (auto const &x : *v) {
                if (x.kind == Value::Kind::Dict) return x.obj.get();
            }
        }
        return nullptr;
    }
    /// The dictionaries an array holds, in order.
    std::vector<Obj const *> items() const
    {
        std::vector<Obj const *> out;
        for (auto const &[k, v] : entries) {
            for (auto const &x : v) {
                if (x.kind == Value::Kind::Dict) out.push_back(x.obj.get());
            }
        }
        for (auto const &x : content) {
            if (x.kind == Value::Kind::Dict) out.push_back(x.obj.get());
        }
        return out;
    }
    std::optional<std::string> content_text() const
    {
        for (auto const &x : content) {
            if (auto t = x.text()) return t;
        }
        return {};
    }
};

/// Add the values on `stack` above `base` to `o` as one entry (','): the last is the key,
/// except in an array.
void add_entry(Obj &o, Values &stack, std::size_t base)
{
    base = std::min(base, stack.size());
    Values vals(std::make_move_iterator(stack.begin() + base), std::make_move_iterator(stack.end()));
    stack.resize(base);
    if (o.type == "Array") {
        o.entries.emplace_back(std::nullopt, std::move(vals));
        return;
    }
    std::optional<std::string> key;
    if (!vals.empty() && (vals.back().kind == Value::Kind::Str || vals.back().kind == Value::Kind::Name)) {
        key = vals.back().text();
        vals.pop_back();
    }
    o.entries.emplace_back(std::move(key), std::move(vals));
}

/// Close `o` (';'): a last entry ending with a name is an entry, other values are its content.
void close_obj(Obj &o, Values &stack, std::size_t base)
{
    if (stack.size() <= base) return;
    if (o.type != "Array" && stack.back().kind == Value::Kind::Name) {
        add_entry(o, stack, base);
    } else {
        o.content.insert(o.content.end(), std::make_move_iterator(stack.begin() + base),
                         std::make_move_iterator(stack.end()));
        stack.resize(base);
    }
}

/// The dictionary of entry `key` in `o` or the dictionaries it holds.
Obj const *find_entry(Obj const &o, std::string_view key, int depth = 0)
{
    if (depth > 8) return nullptr;
    if (auto found = o.obj(key)) return found;
    for (auto const &[k, vals] : o.entries) {
        for (auto const &v : vals) {
            if (v.kind == Value::Kind::Dict) {
                if (auto found = find_entry(*v.obj, key, depth + 1)) return found;
            }
        }
    }
    return nullptr;
}

// ---- colours (paint.rs) -----------------------------------------------------------------

double unit(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

/// A named colour read from a colour operator.
struct Named
{
    std::string name;
    Ink::Kind kind = Ink::Kind::Spot;
    Color color;
    std::array<double, 4> cmyk{};
    double tint = 1.0;
};

/// `c m y k [v1 v2 v3] (name) tint [type]`: the name's position splits the numbers.
/// `force_rgb` forces the model when the operator has no type (gradient stops).
std::optional<Named> parse_named(Values const &vals, Ink::Kind kind, std::optional<bool> force_rgb)
{
    int at = -1;
    for (int i = static_cast<int>(vals.size()) - 1; i >= 0; --i) {
        if (vals[i].kind == Value::Kind::Str) {
            at = i;
            break;
        }
    }
    if (at < 0) return {};
    Named n;
    n.kind = kind;
    n.name = *vals[at].text();
    std::vector<double> before, after;
    for (int i = 0; i < at; ++i) {
        if (auto v = vals[i].as_num()) before.push_back(*v);
    }
    for (std::size_t i = at + 1; i < vals.size(); ++i) {
        if (auto v = vals[i].as_num()) after.push_back(*v);
    }
    if (after.empty()) return {};
    // Illustrator's tint operand: 0 is the full colour.
    n.tint = 1.0 - unit(after[0]);
    std::size_t const count = before.size();
    // The CMYK values come first; three more (RGB or Lab) may follow them.
    bool const extra = count >= 7;
    std::size_t const from = extra ? count - 7 : (count >= 4 ? count - 4 : SIZE_MAX);
    if (from == SIZE_MAX) return {};
    n.cmyk = {unit(before[from]), unit(before[from + 1]), unit(before[from + 2]), unit(before[from + 3])};
    // A type after the tint says which; without one, the operator or colour style does. Lab
    // inks' gradient stops are style 4, the 1998 specification's named RGB, with type 2: all
    // 22 of them in the owner's files, and Illustrator 29's (2026-10-21), which also writes
    // RGB inks' as style 4 with type 1.
    int type = 0;
    if (after.size() >= 2) {
        type = static_cast<int>(after[1]);
    } else if (force_rgb) {
        type = *force_rgb ? 1 : 0;
    }
    if (type == 1 && extra) {
        n.color = Color::rgb(unit(before[from + 4]), unit(before[from + 5]), unit(before[from + 6]));
    } else if (type == 2 && extra) {
        auto const l = std::clamp(before[from + 4], 0.0, 100.0);
        auto const a = std::clamp(before[from + 5], -128.0, 127.0);
        auto const b = std::clamp(before[from + 6], -128.0, 127.0);
        n.color = Color::lab(l, a, b);
    } else {
        n.color = Color::cmyk(n.cmyk[0], n.cmyk[1], n.cmyk[2], n.cmyk[3]);
    }
    return n;
}

/// The colour of a gradient stop's colour values and style (0 grey, 1 CMYK, 2 RGB with its
/// CMYK, 3 named CMYK, 4 named RGB, 5 named with its type, as Xx and Xk write a colour).
std::optional<std::pair<Color, std::optional<Named>>> stop_color(double style, Values const &comps)
{
    auto const nums = nums_of(comps);
    auto tail = [&](std::size_t n) -> double const * {
        return nums.size() >= n ? nums.data() + nums.size() - n : nullptr;
    };
    switch (static_cast<int>(style)) {
        case 0:
            if (nums.empty()) return {};
            return std::pair{Color::gray(unit(nums.back())), std::nullopt};
        case 1:
            if (auto t = tail(4)) return std::pair{Color::cmyk(unit(t[0]), unit(t[1]), unit(t[2]), unit(t[3])), std::nullopt};
            return {};
        case 2:
            if (auto t = tail(3)) return std::pair{Color::rgb(unit(t[0]), unit(t[1]), unit(t[2])), std::nullopt};
            return {};
        default: {
            auto const force = static_cast<int>(style) == 5 ? std::nullopt : std::optional(static_cast<int>(style) == 4);
            if (auto n = parse_named(comps, Ink::Kind::Spot, force)) {
                if (n->name == "[Registration]") n->kind = Ink::Kind::Registration;
                return std::pair{n->color, std::optional<Named>(*n)};
            }
            return {};
        }
    }
}

// ---- comments ---------------------------------------------------------------------------

/// Sections skipped whole, by their markers without the AI<version>_ prefix: begin -> end.
constexpr std::pair<std::string_view, std::string_view> SKIPPED[] = {
    {"BeginPattern", "EndPattern"},
    {"BeginBrushPattern", "EndBrushPattern"},
    {"BeginSVGFilter", "EndSVGFilter"},
    {"BeginSymbol", "EndSymbol"},
    {"BeginPluginObject", "EndPluginObject"},
    {"BeginArtStyles", "EndArtStyles"},
    {"BeginArtStyleList", "EndArtStyleList"},
    {"BeginSymbolList", "EndSymbolList"},
    {"BeginTextDocument", "EndTextDocument"},
    {"BeginEncoding", "EndEncoding"},
    {"Alternate_Content", "End_Versioned_Content"},
};

/// A section comment's marker without its AI<version>_ prefix and value
/// ("AI14_BeginSymbol" -> "BeginSymbol"); DSC comments ("%BeginProlog") keep their '%'.
std::string_view marker(std::string_view c)
{
    auto const end = c.find_first_of(": \t\r\n");
    auto word = c.substr(0, end);
    if (word.substr(0, 2) == "AI") {
        auto rest = word.substr(2);
        while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9') rest.remove_prefix(1);
        if (!rest.empty() && rest.front() == '_') return rest.substr(1);
    }
    return word;
}

std::vector<double> numbers_in(std::string_view text)
{
    std::vector<double> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        auto const start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        if (start == i) break;
        auto n = parse_number(text.substr(start, i - start));
        if (!n) break;
        out.push_back(*n);
    }
    return out;
}

constexpr double MAX_SIDE = 1e6;

bool sane(Geom::Rect const &r)
{
    for (double v : {r.left(), r.top(), r.right(), r.bottom()}) {
        if (!std::isfinite(v) || std::abs(v) > MAX_SIDE) return false;
    }
    return true;
}

std::optional<Geom::Rect> four(std::string_view text)
{
    auto n = numbers_in(text);
    if (n.size() < 4) return {};
    Geom::Rect r(Geom::Point(n[0], n[1]), Geom::Point(n[2], n[3]));
    return sane(r) ? std::optional(r) : std::nullopt;
}

/// Operators that change nothing the reader keeps. PROOF additions from the census:
/// Ar (output resolution), An (CS5, probably Align to Pixel Grid), XD (brush metadata).
constexpr std::string_view IGNORED[] = {
    "Ae", "AE", "Ap", "As", "Xr", "XG", "Xh", "XH", "XF", "D", "X=", "X+", "Bc", "Xm", "Np", "TE",
    "TZ", "Xt", "Xi", "XI", "`", "LB2", "Lc", "Xv", "XV", "Xq", "XQ", "Xg", "Xn", "Bn", "gsave", "grestore",
    "showpage", "annotatepage", "Ar", "An", "XD", "Br", "BS",
    "i", // flatness: how finely a printer splits curves into lines
};

bool ignored(std::string_view w)
{
    return std::find(std::begin(IGNORED), std::end(IGNORED), w) != std::end(IGNORED);
}

/// Operators that set how the next object is painted, and make nothing themselves.
constexpr std::string_view PAINT_STATE[] = {
    "O", "R", "XR", "w", "J", "j", "M", "d", "g", "G", "k", "K", "x", "X", "Xa", "XA",
    "Xx", "XX", "Xk", "XK", "Xs", "XS", "Xz", "XZ", "p", "P",
};

bool paint_state(std::string_view w)
{
    return std::find(std::begin(PAINT_STATE), std::end(PAINT_STATE), w) != std::end(PAINT_STATE);
}

/// The plug-ins whose objects are read as what they drew: those met in real files, each
/// written the same way (see Reader::plugin_object).
constexpr std::string_view LIVE_PLUGINS[] = {
    "Pathfinder Suite",               // A compound shape.
    "Adobe Path Blends",              // A blend.
    "Adobe Calligraphic Brush Tool",  // Brush strokes.
    "Adobe ArtOnPath Brush Tool",
    "Adobe PatternOnPath Brush Tool",
};

std::string join_and(std::set<std::string> const &items);

// ---- the reader (read.rs) ---------------------------------------------------------------

struct GState
{
    std::optional<Paint> fill = Paint{Paint::Kind::Solid, Color::gray(0.0)};
    std::optional<Paint> stroke = Paint{Paint::Kind::Solid, Color::gray(0.0)};
    StrokeStyle style;
    bool evenodd = false;
    Transparency transparency;
    bool overprint_fill = false;
    bool overprint_stroke = false;
    bool locked = false;
    bool hidden = false;
};

struct LayerAttrs
{
    std::string name;
    bool visible = true;
    bool locked = false;
    LayerOptions options;
};

enum class FrameKind { Layer, Group, Compound, Clip, Obj, Pattern };

struct Frame
{
    FrameKind kind;
    LayerAttrs layer;
    std::shared_ptr<Obj> obj;
    std::vector<Node> children;
    bool hidden = false;     ///< Opened on a %_ line.
    bool after = false;      ///< after_object when it opened.
    bool marked = false;     ///< Whether a style marker had followed the last object then.
    bool group_closed = false; ///< Whether a group had just closed then.
    GState gs;               ///< The state when it opened.
    std::size_t base = 0;    ///< The operand stack's height when it opened.
    std::vector<std::size_t> clips; ///< The clipping paths' indexes among the children.
};

struct GradientDef
{
    Gradient gradient;
    /// A "%_BS" stop: the process stand-in for the exact stop ("%_… Bs") that follows it.
    std::optional<Values> stand_in;
};

struct Instance
{
    bool stroke = false;
    std::optional<std::string> name;
    GradientPlacement placement;
};

/// A placed file being read, between %AI5_BeginPlace and %AI5_EndPlace.
struct Placing
{
    bool hidden = false; ///< Written on %_ lines.
    int file_type = -1;
    std::optional<Placed> placed; ///< Once its placement (`) has been read.
    Preview preview;
};

/// The longest side of a preview that is kept. Illustrator's own are 512 pixels at most.
constexpr std::size_t PREVIEW_SIDE = 2048;

/**
 * A placed file's preview, from what its section holds: "width height row-bytes 32 17 " and
 * then the rows, each pixel its alpha, red, green and blue. Every preview in the files seen
 * has 32 bits to a pixel and that last number 17; one written another way isn't guessed at.
 */
Preview preview_of(std::string_view data)
{
    Preview out;
    auto const head = preview_head(data);
    if (!head || head->bits != 32 || head->kind != 17 || head->width < 1 || head->height < 1 ||
        head->width > PREVIEW_SIDE || head->height > PREVIEW_SIDE || head->row_bytes < head->width * 4 ||
        head->row_bytes > head->width * 4 + 64) {
        return out;
    }
    data.remove_prefix(head->size);
    if (data.size() < head->row_bytes * head->height) return out;
    out.width = static_cast<int>(head->width);
    out.height = static_cast<int>(head->height);
    out.pixels.resize(head->width * head->height * 4);
    auto *to = out.pixels.data();
    for (std::size_t y = 0; y < head->height; ++y) {
        auto const *from = data.data() + y * head->row_bytes;
        for (std::size_t x = 0; x < head->width; ++x, from += 4, to += 4) {
            to[0] = from[1];
            to[1] = from[2];
            to[2] = from[3];
            to[3] = from[0];
        }
    }
    return out;
}

bool finite(Geom::Affine const &m)
{
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

struct MeshRead
{
    bool commented = false, version = false, cartesian = false, data = false;
    std::size_t rows = 0, columns = 0, points = 0;
    std::string space = "DeviceGray"; ///< Omitted /CS means Gray, as in the corpus.
    std::vector<std::array<double, 4>> inks; ///< /NChannel: each process ink's CMYK.
    Color swatch; ///< /Process: the named process colour a point's tint is of.
    std::optional<std::pair<std::size_t, std::size_t>> place;
    std::set<std::pair<std::size_t, std::size_t>> places;
    std::vector<MeshCorner> boundary;
    std::vector<std::size_t> corner_at; ///< Where in `boundary` each corner is.
    std::vector<MeshCorner> corners;
    Mesh mesh;
};

class Reader
{
public:
    Reader(std::string_view data, Limits const &limits)
        : _lex(data)
        , _limits(limits)
        , _layerless(data.find("%AI5_BeginLayer") == std::string_view::npos)
    {}

    std::optional<Document> run(std::string &error);

private:
    struct Stop : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    [[noreturn]] void fail(std::string const &why) { throw Stop(why); }

    void push(Value v)
    {
        if (_stack.size() >= _limits.max_stack) fail("it has too many values in a row");
        _stack.push_back(std::move(v));
    }

    Values operands()
    {
        std::size_t base = _frames.empty() ? 0 : _frames.back().base;
        base = std::min(base, _stack.size());
        Values out(std::make_move_iterator(_stack.begin() + base), std::make_move_iterator(_stack.end()));
        _stack.resize(base);
        return out;
    }

    bool shown() const
    {
        for (auto const &f : _frames) {
            if (f.kind == FrameKind::Layer && !f.layer.visible) return false;
        }
        return true;
    }

    void unreadable(std::string const &what)
    {
        // In a pattern's tile it only matters to what is painted with that pattern.
        if (_pattern_unread) {
            _pattern_unread->insert(what);
            return;
        }
        (shown() ? _unsupported : _hidden_unread).insert(what);
    }

    void warn(std::string const &w)
    {
        if (_doc.warnings.size() < 64 && std::find(_doc.warnings.begin(), _doc.warnings.end(), w) == _doc.warnings.end()) {
            _doc.warnings.push_back(w);
        }
    }

    // comments
    void comment(std::string_view c, bool hidden, std::string_view data);
    void skip_to(std::string_view end);
    void token(Token &t, bool hidden);

    // patterns
    void pattern_section();
    void pattern_paint(bool fill, Values const &vals);

    // placed files
    void place_file(Values const &vals);
    void end_place();

    // frames
    void open(Frame f);
    Frame close(char const *what, FrameKind kind);
    void add(Node node, bool commented);
    Node *last_object();
    Node new_node(Node::Kind kind, GState const &gs);
    void end_container(std::string_view what, bool hidden);
    std::pair<std::vector<Node>, bool> clip_first(std::vector<Node> kids, std::vector<std::size_t> const &clips);
    void end_layer();

    // operators
    void op(std::string_view w, bool hidden);
    void begin_obj(bool hidden);
    void end_obj();
    void art_dictionary(Obj const &o);
    void document(Obj const &o);
    void text_slot(Obj const &o);
    void foreign_art(Obj const &o);
    bool mesh_marker(Values const &vals, bool hidden);
    void mesh_entry(Values const &vals);
    void plugin_object(Values const &vals, bool hidden);
    void layer_attrs(Values const &vals);
    void color(std::string_view w, Values const &vals);
    int ink_index(Named const &n, bool by_name);
    void dash(Values const &vals);
    void transparency(Values const &vals);
    void style_marker(Values const &vals, bool hidden);
    void gradient_stop(Values const &vals);
    void set_instance(Instance const &i);

    // paths
    void path_op(std::string_view w, Values const &vals);
    void finish_path();
    void end_path_quietly();
    void paint(std::string_view op, bool hidden, bool guide);

    // images
    void image(std::string_view data, bool hidden);

    Document finish();
    std::optional<Geom::Rect> sized_board() const;

    Lexer _lex;
    Limits _limits;
    Document _doc;
    Values _stack;
    std::vector<std::size_t> _marks;
    GState _gs;
    std::vector<Frame> _frames;

    Geom::PathVector _path;
    std::optional<Geom::Path> _current;
    std::string _path_types, _current_types;
    std::size_t _points = 0;
    bool _clip_next = false;

    std::optional<Geom::Rect> _cropmarks;
    std::optional<Geom::Rect> _bbox, _hires, _template;
    std::optional<std::pair<double, double>> _art_size, _template_center;
    std::map<std::string, int> _gradient_index;
    std::optional<GradientDef> _gradient_def;
    std::set<std::size_t> _stop_inks; ///< Inks known only from a gradient stop so far.
    std::optional<Instance> _instance;
    std::optional<Paint> _pending_fill, _pending_stroke;
    bool _raster = false;
    std::string _raster_space;
    bool _raster_coloured = false; ///< `0 /DeviceGray XC`: a grey picture coloured black.
    std::optional<Placing> _place;
    std::map<std::string, int> _pattern_index;
    /// While a pattern's tile is read: what it holds that isn't.
    std::optional<std::set<std::string>> _pattern_unread;

    bool _after_object = false;
    /// A style marker (XW) has come since the last object. Its name can still follow, but
    /// transparency from here on is the next object's, not a just-closed group's.
    bool _marked = false;
    /// The last object is one a plug-in made, written on %_ lines: the paint its style gives
    /// it follows, before the style's own marker.
    bool _after_plugin = false;
    /// The last object is a group that has just closed: its own state can still follow (Xd).
    bool _group_closed = false;
    std::size_t _nodes = 0;
    std::size_t _foreign_bytes = 0;
    std::set<std::string> _unsupported, _unknown, _hidden_unread;
    std::optional<MeshRead> _mesh;
    std::size_t _cmyk_colors = 0, _rgb_colors = 0;
    std::optional<bool> _color_model;
    bool _done = false;
    /// The records have no layers (Illustrator 3, Photoshop's paths exported to Illustrator):
    /// what follows the setup is one layer, as Illustrator opens it.
    bool _layerless = false;
    bool _one_layer = false; ///< That layer is open.
    bool _obj_hidden = false;
    bool _legacy_strings = false; ///< FileFormat 1 to 3: names are Windows-1252, not guessed UTF-8.
    bool _palette = false; ///< In %AI5_BeginPalette: swatches register their inks.
};

std::optional<Document> Reader::run(std::string &error)
{
    try {
        while (auto t = _lex.next()) {
            if (_done) break;
            token(*t, t->hidden);
        }
        if (_one_layer && _frames.size() == 1 && _frames.back().kind == FrameKind::Layer) end_layer();
        for (auto const &f : _frames) {
            if (f.kind == FrameKind::Layer) fail("it ends inside a layer");
        }
        return finish();
    } catch (Stop const &e) {
        error = e.what();
        return {};
    }
}

/// Act on a token; `hidden` says whether what it makes counts as written on %_ lines.
void Reader::token(Token &t, bool hidden)
{
    switch (t.kind) {
        case Token::Kind::Number: push(Value::number(t.number)); break;
        case Token::Kind::String: push(Value::string(std::move(t.string), _legacy_strings)); break;
        case Token::Kind::Name: push(Value::name(text_of(t.view, _legacy_strings))); break;
        case Token::Kind::Word: op(t.view, hidden); break;
        case Token::Kind::Comment: comment(t.view, t.hidden, t.data); break;
        case Token::Kind::Data: image(t.view, hidden); break;
    }
}

// ---- comments --------------------------------------------------------------------------

void Reader::comment(std::string_view c, bool hidden, std::string_view data)
{
    while (!c.empty() && (c.back() == '\r' || c.back() == '\n' || c.back() == ' ' || c.back() == '\t')) {
        c.remove_suffix(1);
    }
    auto const m = marker(c);
    // An image or a placed file is written with its markers wherever it is, on %_ lines too.
    if (m == "BeginRaster") {
        _raster = true;
        _raster_space.clear();
        _raster_coloured = false;
        return;
    }
    if (m == "EndRaster") {
        _raster = false;
        return;
    }
    if (m == "BeginPlace") {
        _place = Placing{hidden};
        return;
    }
    if (m == "EndPlace") {
        end_place();
        return;
    }
    if (_place) {
        if (c.substr(0, 10) == "%FileType:") {
            auto const n = numbers_in(c.substr(10));
            if (!n.empty() && n[0] >= 0 && n[0] < 1000) _place->file_type = static_cast<int>(n[0]);
            return;
        }
        if (m == "BeginPlacedObjectPreview") {
            _place->preview = preview_of(data);
            return;
        }
        if (m == "EndPlacedRelativePath") {
            // The folder is the string between the two markers.
            auto const vals = operands();
            if (_place->placed) {
                for (auto const &v : vals) {
                    if (v.kind == Value::Kind::Str && v.str.size() <= 4096) _place->placed->folder = v.str;
                }
            }
            return;
        }
    }
    // Nothing else on a %_ line is a section of the document.
    if (hidden) return;
    if (m == "BeginPattern") {
        pattern_section();
        return;
    }
    for (auto const &[begin, end] : SKIPPED) {
        if (m == begin) {
            skip_to(end);
            return;
        }
    }
    if (m == "%BeginProlog") {
        skip_to("%EndProlog");
        return;
    }
    if (m == "%BeginResource") {
        skip_to("%EndResource");
        return;
    }
    if (m == "BeginPalette") {
        _palette = true;
        return;
    }
    if (m == "EndPalette") {
        _palette = false;
        return;
    }
    if (m == "%PageTrailer" || m == "%Trailer") {
        _done = true;
        return;
    }
    if (m == "%EndSetup" && _layerless && !_one_layer && _frames.empty()) {
        Frame f;
        f.kind = FrameKind::Layer;
        f.layer.name = "Layer 1";
        open(std::move(f));
        _one_layer = true;
        return;
    }
    auto starts = [&](std::string_view p) { return c.substr(0, p.size()) == p; };
    auto value = [&](std::string_view p) { return c.substr(p.size()); };
    if (starts("AI5_BeginLayer")) {
        Frame f;
        f.kind = FrameKind::Layer;
        open(std::move(f));
    } else if (_frames.empty() && starts("%HiResBoundingBox:")) {
        if (auto r = four(value("%HiResBoundingBox:"))) _hires = r;
    } else if (_frames.empty() && starts("%BoundingBox:")) {
        if (auto r = four(value("%BoundingBox:"))) _bbox = r;
    } else if (starts("AI3_Cropmarks:")) {
        if (auto r = four(value("AI3_Cropmarks:"))) _cropmarks = r;
    } else if (starts("AI5_ArtSize:")) {
        auto n = numbers_in(value("AI5_ArtSize:"));
        if (n.size() >= 2) _art_size = std::pair{n[0], n[1]};
    } else if (starts("AI3_TemplateBox:")) {
        auto n = numbers_in(value("AI3_TemplateBox:"));
        if (n.size() >= 2) _template_center = std::pair{n[0], n[1]};
        if (auto r = four(value("AI3_TemplateBox:"))) _template = r;
    } else if (_frames.empty() && (starts("AI5_FileFormat ") || starts("AI5_FileFormat:"))) {
        auto const n = numbers_in(value(starts("AI5_FileFormat:") ? "AI5_FileFormat:" : "AI5_FileFormat"));
        _legacy_strings = n.size() == 1 && n[0] >= 1 && n[0] <= 3 && n[0] == std::floor(n[0]);
    } else if (starts("AI9_ColorModel:")) {
        auto v = value("AI9_ColorModel:");
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        _color_model = v == "2";
    } else if (starts("AI5_RulerUnits:")) {
        auto n = numbers_in(value("AI5_RulerUnits:"));
        if (!n.empty() && n[0] >= 0 && n[0] < 16 && n[0] == std::floor(n[0])) _doc.ruler_units = static_cast<int>(n[0]);
    } else if (starts("%AI8_CreatorVersion:")) {
        auto v = value("%AI8_CreatorVersion:");
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        _doc.creator_version = std::string(v.substr(0, 64));
    }
}

void Reader::skip_to(std::string_view end)
{
    std::string_view begin;
    for (auto const &[b, e] : SKIPPED) {
        if (e == end) begin = b;
    }
    if (end == "End_Versioned_Content") begin = "Begin_Content_if_version_gt";
    std::size_t depth = 0;
    while (auto t = _lex.next()) {
        if (t->kind != Token::Kind::Comment || t->hidden) continue;
        auto const m = marker(t->view);
        if (!begin.empty() && m == begin) {
            ++depth;
        } else if (m == end) {
            if (depth == 0) return;
            --depth;
        }
    }
    fail("its section ending with %" + std::string(end) + " doesn't end");
}

// ---- frames ----------------------------------------------------------------------------

void Reader::open(Frame f)
{
    if (_frames.size() >= _limits.max_depth) fail("its objects are nested too deeply");
    f.base = f.kind == FrameKind::Obj ? _stack.size() : (_frames.empty() ? 0 : _frames.back().base);
    f.after = _after_object;
    f.marked = _marked;
    f.group_closed = _group_closed;
    f.gs = _gs;
    _frames.push_back(std::move(f));
    _after_object = false;
}

Frame Reader::close(char const *what, FrameKind kind)
{
    if (_frames.empty() || _frames.back().kind != kind) {
        fail(std::string("`") + what + "` closes something it didn't open");
    }
    Frame f = std::move(_frames.back());
    _frames.pop_back();
    return f;
}

void Reader::add(Node node, bool commented)
{
    if (++_nodes > _limits.max_nodes) fail("it has too many objects");
    node.commented = commented;
    if (_frames.empty()) fail("it has art outside its layers");
    _frames.back().children.push_back(std::move(node));
    _after_object = true;
    _marked = false;
    _after_plugin = false;
}

Node *Reader::last_object()
{
    if (!_after_object || _frames.empty() || _frames.back().children.empty()) return nullptr;
    return &_frames.back().children.back();
}

Node Reader::new_node(Node::Kind kind, GState const &gs)
{
    Node n;
    n.kind = kind;
    n.visible = !gs.hidden;
    n.locked = gs.locked;
    return n;
}

/// The last path among `nodes`, depth first, and in `group` the transparency of the nearest
/// group it lies in that has any. With `painted`, the last that has a fill or a stroke.
Node const *last_path(std::vector<Node> const &nodes, Transparency &group, bool painted)
{
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (it->kind == Node::Kind::Path) {
            if (!painted || it->fill || it->stroke) return &*it;
            continue;
        }
        if (auto const *inner = last_path(it->children, group, painted)) {
            if (group.is_default()) group = it->transparency;
            return inner;
        }
    }
    return nullptr;
}

void Reader::end_container(std::string_view what, bool hidden)
{
    FrameKind kind = what == "U" ? FrameKind::Group : what == "*U" ? FrameKind::Compound : FrameKind::Clip;
    Frame f = close(std::string(what).c_str(), kind);
    // A compound path of clipping paths clips its clipping group.
    bool const clipping = kind == FrameKind::Compound && !f.clips.empty();
    Node node;
    if (kind == FrameKind::Compound) {
        if (f.children.empty()) return;
        // Its paint is its last path's that has any. A gradient is written around the last
        // path alone (`Bb … Bg … f … BB` before `*U`), and the paths before it are painted
        // in a plain colour that stands in for readers that know no gradients: black, in
        // most files. Without a gradient every path is painted alike. A compound path with a
        // style of its own has its paths in a group inside it (`*u u … U n () XW *U`): the
        // path is in there, and the group has no paint to give.
        Transparency grouped;
        auto const *path = last_path(f.children, grouped, true);
        if (!path) {
            grouped = {};
            path = last_path(f.children, grouped, false);
        }
        auto const &first = path ? *path : f.children.front();
        node = new_node(Node::Kind::Compound, f.gs);
        node.fill = first.fill;
        node.stroke = first.stroke;
        node.stroke_style = first.stroke_style;
        node.overprint_fill = first.overprint_fill;
        node.overprint_stroke = first.overprint_stroke;
        node.transparency = first.transparency.is_default() ? grouped : first.transparency;
        node.evenodd = first.evenodd;
        node.clipping = clipping;
        for (auto &k : f.children) {
            k.transparency = {};
            k.visible = true;
            k.locked = false;
        }
        node.children = std::move(f.children);
    } else {
        // The clipping path of a clipping group can lie in a group of its own, written after
        // what it clips: a compound shape that clips, or a path that Illustrator wrapped.
        // That group holds nothing but the clipping path, and the clipping group has none
        // besides. The path clips all of the group, and its own paint lies under all of it,
        // as a clipping path's does. Left in its group it would clip nothing and paint on top.
        if (kind == FrameKind::Clip && f.clips.empty()) {
            // Nothing but clipping paths: one, or several, each in the group of the one before.
            auto wraps = [](Node const &n) {
                if (n.kind != Node::Kind::Group || !n.clipped || n.opacity_mask) return false;
                for (auto const *g = &n;;) {
                    if (g->children.size() == 1) return true;
                    if (g->children.size() != 2 || !g->children[1].clip_chain) return false;
                    g = &g->children[1];
                }
            };
            if (std::count_if(f.children.begin(), f.children.end(), wraps) == 1) {
                auto at = std::find_if(f.children.begin(), f.children.end(), wraps);
                auto const index = static_cast<std::size_t>(at - f.children.begin());
                Node wrapper = std::move(*at);
                std::vector<Node> clips;
                for (auto *g = &wrapper; g; g = g->children.size() == 2 ? &g->children[1] : nullptr) {
                    clips.push_back(std::move(g->children.front()));
                }
                if (clips.front().name.empty()) clips.front().name = wrapper.name;
                for (auto &c : clips) c.commented = wrapper.commented;
                f.children.erase(at);
                for (std::size_t i = 0; i < clips.size(); ++i) f.clips.push_back(index + i);
                f.children.insert(f.children.begin() + index, std::make_move_iterator(clips.begin()),
                                  std::make_move_iterator(clips.end()));
            }
        }
        // A group with a clipping path (q ... Q, or a W path in a group) is a clipping group.
        auto [children, clip] = clip_first(std::move(f.children), f.clips);
        node = new_node(Node::Kind::Group, f.gs);
        node.children = std::move(children);
        node.clipped = clip;
    }
    bool const commented = f.hidden || hidden;
    if (clipping && !_frames.empty()) _frames.back().clips.push_back(_frames.back().children.size());
    add(std::move(node), commented);
    _group_closed = kind != FrameKind::Compound;
}

std::pair<std::vector<Node>, bool> Reader::clip_first(std::vector<Node> kids, std::vector<std::size_t> const &clips)
{
    std::vector<Node> clip, rest;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        bool const is_clip = std::find(clips.begin(), clips.end(), i) != clips.end();
        (is_clip ? clip : rest).push_back(std::move(kids[i]));
    }
    if (clip.empty()) return {std::move(rest), false};
    // A clipping path with a fill and a stroke is written as the same path three times: to
    // be filled, to clip, to be stroked. They are one path, with the fill of the first that
    // has one, and the stroke.
    bool const same = clip.size() > 1 && std::all_of(clip.begin(), clip.end(), [&](Node const &k) {
        return k.kind == Node::Kind::Path && k.path == clip.front().path && k.evenodd == clip.front().evenodd;
    });
    std::vector<Node> out;
    if (clip.size() == 1) {
        out.push_back(std::move(clip.front()));
    } else if (same) {
        Node one = clip.front();
        one.fill.reset();
        one.stroke.reset();
        for (auto const &k : clip) {
            if (!one.fill && k.fill) {
                one.fill = k.fill;
                one.overprint_fill = k.overprint_fill;
            }
            if (!one.stroke && k.stroke) {
                one.stroke = k.stroke;
                one.stroke_style = k.stroke_style;
                one.overprint_stroke = k.overprint_stroke;
            }
            if (one.name.empty()) one.name = k.name;
        }
        out.push_back(std::move(one));
    } else {
        // Several clipping paths in one group clip together: what shows is where all of
        // them let it through, as with one clip set after another on a page. (A clipping
        // path of several pieces is a compound path, and is one of them.) The first clips
        // the group, and each of the others a group of its own inside the one before.
        for (std::size_t i = clip.size(); i-- > 1;) {
            Node inner;
            inner.kind = Node::Kind::Group;
            inner.clipped = true;
            inner.clip_chain = true;
            inner.children.push_back(std::move(clip[i]));
            for (auto &n : rest) inner.children.push_back(std::move(n));
            rest.clear();
            rest.push_back(std::move(inner));
        }
        out.push_back(std::move(clip.front()));
    }
    for (auto &n : rest) out.push_back(std::move(n));
    return {std::move(out), true};
}

void Reader::end_layer()
{
    Frame f = close("LB", FrameKind::Layer);
    auto [children, clip] = clip_first(std::move(f.children), f.clips);
    Node layer;
    layer.kind = Node::Kind::Layer;
    layer.name = f.layer.name;
    layer.visible = f.layer.visible;
    layer.locked = f.layer.locked;
    layer.layer = f.layer.options;
    layer.children = std::move(children);
    layer.clipped = clip;
    if (!_frames.empty()) {
        _frames.back().children.push_back(std::move(layer));
        _after_object = true;
        _marked = false;
    } else {
        _doc.layers.push_back(std::move(layer));
        _after_object = false;
    }
}

// ---- operators ---------------------------------------------------------------------------

void Reader::op(std::string_view w, bool hidden)
{
    if (_mesh && w != "[" && w != "]" && w != "X#" && w != "X!") {
        fail("it has gradient meshes with an unterminated or unsupported record");
    }
    if (w == "[") {
        // Arrays are built without recursion, but a chain of them is freed by recursion, so how
        // deeply they nest is bounded. Marks that operators have cut from the stack don't count.
        while (!_marks.empty() &&
               (_marks.back() >= _stack.size() || _stack[_marks.back()].kind != Value::Kind::Mark)) {
            _marks.pop_back();
        }
        if (_marks.size() >= _limits.max_depth) fail("its arrays are nested too deeply");
        _marks.push_back(_stack.size());
        push(Value::mark());
        return;
    }
    if (w == "]") {
        // The innermost '[' still on the stack (the stack is cut back by operators).
        std::size_t at = _stack.size();
        while (!_marks.empty()) {
            auto const m = _marks.back();
            _marks.pop_back();
            if (m < _stack.size() && _stack[m].kind == Value::Kind::Mark) {
                at = m;
                break;
            }
        }
        auto arr = std::make_shared<Array>();
        if (at < _stack.size()) {
            arr->assign(std::make_move_iterator(_stack.begin() + at + 1), std::make_move_iterator(_stack.end()));
            _stack.resize(at);
        }
        Value v;
        v.kind = Value::Kind::Arr;
        v.arr = std::move(arr);
        push(std::move(v));
        return;
    }
    if (w == ":") {
        begin_obj(hidden);
        return;
    }
    if (w == ",") {
        if (!_frames.empty() && _frames.back().kind == FrameKind::Obj) {
            auto &f = _frames.back();
            add_entry(*f.obj, _stack, f.base);
            // Binary data in ASCII85 follows, up to its ~>: lines that start with a percent
            // sign, some of them by chance with "%_". A foreign object's data is written
            // the same way after its /Data.
            auto const &key = f.obj->entries.empty() ? std::optional<std::string>() : f.obj->entries.back().first;
            if ((f.obj->type == "Binary" && key == "ASCII85Decode") || (f.obj->type == "ForeignObject" && key == "Data")) {
                auto const data = _lex.skip_past("~>");
                if (f.obj->type == "ForeignObject") f.obj->foreign_data = data;
            }
        }
        return;
    }
    if (w == ";") {
        end_obj();
        return;
    }
    // What follows an object (its name, transparency and style) ends with anything else.
    // An image is an object once its samples are read, and is written with an `XH` and an
    // `N` that paints no path after them: its name and style still follow those.
    bool follows = w == "Xy" || w == "Xd" || w == "XW" || w == "BB";
    if (!follows && (w == "XH" || ((w == "N" || w == "n") && !_current && _path.empty()))) {
        auto const *last = last_object();
        follows = last && (last->kind == Node::Kind::Image || last->kind == Node::Kind::Placed || last->kind == Node::Kind::Foreign);
    }
    if (!follows && _after_plugin) follows = paint_state(w);
    if (!follows) {
        _after_object = false;
        _after_plugin = false;
        // A stroke's width or a colour can come between a group's end and its own state.
        if (!paint_state(w)) _group_closed = false;
    }
    Values vals = operands();
    if (_palette) {
        // The Swatches panel: spot inks (Xx), global process colours (Xk) and named
        // colours (x) are the document's inks, used or not. Nothing else here is art.
        if (w == "Xx" || w == "Xk" || w == "x") {
            auto const kind = w == "Xk" ? Ink::Kind::Process : Ink::Kind::Spot;
            auto const force = w == "x" ? std::optional<bool>(false) : std::nullopt;
            if (auto n = parse_named(vals, kind, force)) {
                if (n->name == "[Registration]") n->kind = Ink::Kind::Registration;
                ink_index(*n, false);
            }
        }
        return;
    }
    if (_gradient_def) {
        // A stop is written twice: "c m y k 1 1 6 50 100 %_BS", in process colour for readers
        // that know no named colours, then "%_… Bs", exact. The stand-in counts only when no
        // exact stop follows it.
        auto stand_in = std::exchange(_gradient_def->stand_in, std::nullopt);
        if (w == "Bs") {
            gradient_stop(vals);
        } else if (w == "BS") {
            if (stand_in) gradient_stop(*stand_in);
            _gradient_def->stand_in = std::move(vals);
        } else if (w == "BD") {
            if (stand_in) gradient_stop(*stand_in);
            auto def = std::move(*_gradient_def);
            _gradient_def.reset();
            if (!def.gradient.stops.empty()) {
                // Illustrator lists a linear gradient's stops last first and takes them in that
                // order, so two at one place come in its order once the list is turned round
                // (its own copy of a hard stop drew red then blue as listed blue, red).
                if (!def.gradient.radial) std::reverse(def.gradient.stops.begin(), def.gradient.stops.end());
                std::stable_sort(def.gradient.stops.begin(), def.gradient.stops.end(),
                                 [](auto const &a, auto const &b) { return a.offset < b.offset; });
                _gradient_index[def.gradient.name] = static_cast<int>(_doc.gradients.size());
                _doc.gradients.push_back(std::move(def.gradient));
            }
        }
        return;
    }
    if (_mesh && w == "X#") {
        mesh_entry(vals);
        return;
    }
    auto const nums = nums_of(vals);
    auto last_is_one = [&] { return !nums.empty() && nums.back() == 1.0; };

    // Containers.
    if (w == "u" || w == "*u" || w == "q") {
        end_path_quietly();
        Frame f;
        f.kind = w == "u" ? FrameKind::Group : w == "*u" ? FrameKind::Compound : FrameKind::Clip;
        f.hidden = hidden;
        open(std::move(f));
    } else if (w == "U" || w == "*U" || w == "Q") {
        end_container(w, hidden);
    } else if (w == "Lb") {
        layer_attrs(vals);
    } else if (w == "Ln") {
        if (!_frames.empty() && _frames.back().kind == FrameKind::Layer) {
            _frames.back().layer.name = last_text(vals).value_or("");
        }
    } else if (w == "LB") {
        end_layer();
    }
    // Paths.
    else if (w == "m" || w == "l" || w == "L" || w == "c" || w == "C" || w == "v" || w == "V" || w == "y" || w == "Y") {
        path_op(w, vals);
    } else if (w == "h") {
        if (_current) _current->close(true);
    } else if (w == "H") {
        // Ends a path that stays open.
    } else if (w == "W") {
        _clip_next = true;
    } else if (w == "N" || w == "n" || w == "F" || w == "f" || w == "S" || w == "s" || w == "B" || w == "b") {
        paint(w, hidden, false);
    } else if (w == "*") {
        auto const op = last_text(vals).value_or("N");
        paint(op, hidden, true);
    }
    // Colours.
    else if (w == "g" || w == "G" || w == "k" || w == "K" || w == "x" || w == "X" || w == "Xa" || w == "XA" ||
             w == "Xx" || w == "XX" || w == "Xk" || w == "XK" || w == "Xs" || w == "XS" || w == "Xz" || w == "XZ") {
        color(w, vals);
    } else if (w == "p" || w == "P") {
        pattern_paint(w == "p", vals);
    } else if (w == "O") {
        _gs.overprint_fill = last_is_one();
    } else if (w == "R") {
        _gs.overprint_stroke = last_is_one();
    } else if (w == "XR") {
        _gs.evenodd = last_is_one();
    } else if (w == "w") {
        if (!nums.empty() && std::isfinite(nums.back()) && nums.back() >= 0) _gs.style.width = nums.back();
    } else if (w == "J") {
        int const v = nums.empty() ? 0 : static_cast<int>(nums.back());
        _gs.style.cap = (v == 1 || v == 2) ? v : 0;
    } else if (w == "j") {
        int const v = nums.empty() ? 0 : static_cast<int>(nums.back());
        _gs.style.join = (v == 1 || v == 2) ? v : 0;
    } else if (w == "M") {
        if (!nums.empty() && std::isfinite(nums.back()) && nums.back() >= 1.0) _gs.style.miter = std::min(nums.back(), 500.0);
    } else if (w == "d") {
        dash(vals);
    }
    // Transparency and state.
    else if (w == "Xy") {
        transparency(vals);
    } else if (w == "Xd") {
        // `Xd`, with `6 () XW` after it, ends the state a group is written with after it
        // closes. Its transparency is what holds then. An `Xy` comes before the two only when
        // that differs from what the last object was written with: a group as transparent as
        // the last thing in it sets none, and isn't opaque for that.
        if (_group_closed && !_marked && !_frames.empty() && !_frames.back().children.empty()) {
            auto &group = _frames.back().children.back();
            if (group.kind == Node::Kind::Group) group.transparency = _gs.transparency;
        }
        _group_closed = false;
    } else if (w == "Xw") {
        _gs.hidden = last_is_one();
    } else if (w == "A") {
        _gs.locked = last_is_one();
    } else if (w == "XW") {
        style_marker(vals, hidden);
    }
    // Gradients.
    else if (w == "Bd") {
        GradientDef def;
        for (auto const &v : vals) {
            if (v.kind == Value::Kind::Str) {
                def.gradient.name = *v.text();
                break;
            }
        }
        def.gradient.radial = !nums.empty() && nums.front() == 1.0;
        if (nums.size() > 2) def.gradient.options.assign(nums.begin() + 2, nums.end());
        if (!def.gradient.name.empty()) _gradient_def = std::move(def);
    } else if (w == "Bs" || w == "BD") {
    } else if (w == "Bb") {
        Instance i;
        i.stroke = last_is_one();
        _instance = i;
    } else if (w == "Bg") {
        if (_instance) {
            int at = -1;
            for (int i = static_cast<int>(vals.size()) - 1; i >= 0; --i) {
                if (vals[i].kind == Value::Kind::Str) {
                    at = i;
                    break;
                }
            }
            if (at >= 0) {
                _instance->name = vals[at].text();
                std::vector<double> n;
                for (std::size_t i = at + 1; i < vals.size(); ++i) {
                    if (auto v = vals[i].as_num()) n.push_back(*v);
                }
                if (n.size() >= 4) {
                    auto &p = _instance->placement;
                    p.origin = {n[0], n[1]};
                    p.angle = n[2];
                    p.length = n[3];
                    if (n.size() >= 10) {
                        Geom::Affine m(n[4], n[5], n[6], n[7], n[8], n[9]);
                        bool finite = true;
                        for (int k = 0; k < 6; ++k) finite = finite && std::isfinite(m[k]);
                        if (finite) p.bg = m;
                    }
                }
            }
        }
    } else if (w == "Bm" || w == "Xm") {
        if (_instance && nums.size() >= 6) {
            auto const *n = nums.data() + nums.size() - 6;
            Geom::Affine m(n[0], n[1], n[2], n[3], n[4], n[5]);
            bool finite = true;
            for (int i = 0; i < 6; ++i) finite = finite && std::isfinite(m[i]);
            if (finite && std::abs(m.det()) > 1e-12) {
                if (w == "Xm") {
                    _instance->placement.xm = m;
                } else if (!_instance->placement.bm_given) {
                    // A gradient of several ramps has a Bm for each; the first is the
                    // one between the first two stops.
                    _instance->placement.bm = m;
                    _instance->placement.bm_given = true;
                }
            }
        }
    } else if (w == "Bh") {
        if (_instance && nums.size() >= 2) _instance->placement.hilight = {nums[0], nums[1]};
        if (_instance && nums.size() >= 4 && std::isfinite(nums[2]) && std::isfinite(nums[3])) {
            _instance->placement.hilight_angle = nums[2];
            _instance->placement.hilight_length = nums[3];
        }
    } else if (w == "BB") {
        // A gradient is written around the paint operator of the path it paints, which has
        // taken it by now. One that closes with no path begun is a note that an object keeps
        // of its gradient, written with the object's own state (`Xd`, `Bb … Bg`, `BB`,
        // `6 () XW`): it paints nothing, and least of all the next path.
        if (_instance) {
            auto i = std::move(*_instance);
            _instance.reset();
            if (_current || !_path.empty()) set_instance(i);
        }
    }
    // Images.
    else if (w == "XN") {
        if (_raster) _raster_space = last_text(vals).value_or("");
    } else if (w == "XC" && _raster) {
        // A grey picture coloured with a paint, the paint's numbers and then its space, as
        // Illustrator 29 saves a grey picture it opened from a version 7 file:
        // `0 /DeviceGray XC`. In black it prints as the grey picture it is; another paint,
        // a spot ink in two of the owner's files, isn't drawn yet.
        bool const black = vals.size() == 2 && vals[0].as_num() == 0.0 && vals[1].kind == Value::Kind::Name &&
                           vals[1].str == "DeviceGray";
        if (black) {
            _raster_space = "DeviceGray";
            _raster_coloured = true;
        } else {
            unreadable("pictures coloured with a paint");
        }
    } else if (w == "XI" && _raster) {
        // Counted XI data is one lexer Data token, never this operator. A legacy raster
        // without byte-count brackets would otherwise be skipped as comments, losing the
        // picture silently in a standalone file with no PDF to compare against.
        unreadable("raster data without byte-count brackets");
    }
    // Type in the legacy format.
    else if (w == "To" || w == "TO" || w == "Tp" || w == "TP" || w == "Tx" || w == "TX" || w == "Tj" || w == "Tk") {
        unreadable("type in the legacy format");
    }
    // A mesh, and what a plug-in made.
    else if (w == "X!" && mesh_marker(vals, hidden)) {
    } else if (w == "XP") {
        plugin_object(vals, hidden);
    }
    // Placed files: ` places the file, ~ ends it.
    else if (w == "`" && _place) {
        place_file(vals);
    } else if (w == "~" && _place) {
    } else if (ignored(w)) {
    } else if (_frames.empty()) {
        // Before the art, the setup runs procedures of the printing format.
    } else {
        std::string const op(w.substr(0, 24));
        if (_pattern_unread) {
            if (_pattern_unread->size() < 16) _pattern_unread->insert("the operator `" + op + "`");
        } else if (!shown()) {
            if (_hidden_unread.size() < 16) _hidden_unread.insert("`" + op + "`");
        } else if (_unknown.size() < 16) {
            _unknown.insert(op);
        }
    }
}

// ---- patterns ----------------------------------------------------------------------------

/**
 * A pattern's definition, after its %AI3_BeginPattern:
 *
 *     (name) llx lly urx ury
 *     %_... the tile's art, every line of it hidden ...
 *     E
 *     %AI3_EndPattern
 *
 * Illustrator 8 and older write the art as procedures of the printing format instead
 * (`[ %AI3_Tile (...) @ ... ] E`); those aren't read, and only what is painted with such a
 * pattern is refused for it.
 */
void Reader::pattern_section()
{
    Pattern pattern;
    std::array<double, 4> box{};
    bool headed = false, ended = false;
    auto t = _lex.next();
    if (t && t->kind == Token::Kind::String && !t->hidden) {
        pattern.name = text_of(t->string, _legacy_strings);
        headed = true;
        for (auto &v : box) {
            t = _lex.next();
            if (!t || t->kind != Token::Kind::Number || t->hidden) {
                headed = false;
                break;
            }
            v = t->number;
        }
    }
    ended = t && t->kind == Token::Kind::Comment && !t->hidden && marker(t->view) == "EndPattern";
    if (headed) {
        pattern.tile = Geom::Rect(Geom::Point(box[0], box[1]), Geom::Point(box[2], box[3]));
        headed = sane(pattern.tile) && pattern.tile.width() > 1e-6 && pattern.tile.height() > 1e-6;
    }
    if (!headed) {
        if (!ended) skip_to("EndPattern");
        return;
    }

    // The tile's art is read like a layer's, into a frame of its own and with a state of
    // its own. Whatever goes wrong in it is the pattern's problem, not the document's.
    auto const depth = _frames.size();
    auto const stack = _stack.size();
    auto const gs = _gs;
    end_path_quietly();
    Frame frame;
    frame.kind = FrameKind::Pattern;
    open(std::move(frame));
    _gs = GState();
    _pattern_unread.emplace();
    std::string problem;
    bool closed = false;
    try {
        while ((t = _lex.next())) {
            if (!t->hidden) {
                if (t->kind == Token::Kind::Comment) {
                    if (marker(t->view) == "EndPattern") {
                        ended = true;
                        break;
                    }
                    continue;
                }
                if (t->kind == Token::Kind::Word && t->view == "E") {
                    closed = true;
                    continue;
                }
                problem = "art in the format of Illustrator 8 and older";
                break;
            }
            // Its lines are all hidden; what they make isn't a second copy of anything.
            if (!closed) token(*t, false);
        }
    } catch (Stop const &e) {
        problem = e.what();
    }
    if (problem.empty() && (_frames.size() != depth + 1 || _frames.back().kind != FrameKind::Pattern)) {
        problem = "art that doesn't end";
    }
    if (problem.empty()) {
        pattern.art = std::move(_frames.back().children);
        auto has_text = [](auto &&self, std::vector<Node> const &nodes) -> bool {
            for (auto const &n : nodes) {
                if (n.kind == Node::Kind::Text || self(self, n.children)) return true;
            }
            return false;
        };
        if (has_text(has_text, pattern.art)) _pattern_unread->insert("type");
        if (!_pattern_unread->empty()) problem = join_and(*_pattern_unread);
    }
    _frames.erase(_frames.begin() + depth, _frames.end());
    _stack.resize(std::min(stack, _stack.size()));
    _pattern_unread.reset();
    _gs = gs;
    end_path_quietly();
    _instance.reset();
    _pending_fill.reset();
    _pending_stroke.reset();
    _gradient_def.reset();
    _place.reset();
    _raster = false;
    _after_object = false;
    _marked = false;
    if (!problem.empty()) {
        pattern.art.clear();
        pattern.unread = problem;
    }
    if (!ended) skip_to("EndPattern");
    _pattern_index[pattern.name] = static_cast<int>(_doc.patterns.size());
    _doc.patterns.push_back(std::move(pattern));
}

/// `(name) px py sx sy angle rf r k ka [a b c d tx ty] p`: paint with a pattern. The matrix
/// carries the pattern's space onto the art. The numbers before it are how Illustrator 8
/// and older moved, scaled, turned, mirrored and slanted a pattern; later versions put all
/// of that in the matrix and leave them at rest (0 0 1 1 0 0 0 0 0).
void Reader::pattern_paint(bool fill, Values const &vals)
{
    int at = -1;
    for (int i = static_cast<int>(vals.size()) - 1; i >= 0; --i) {
        if (vals[i].kind == Value::Kind::Str) {
            at = i;
            break;
        }
    }
    std::vector<double> n, m;
    for (std::size_t i = at + 1; at >= 0 && i < vals.size(); ++i) {
        if (auto v = vals[i].as_num()) n.push_back(*v);
        if (vals[i].kind == Value::Kind::Arr) m = nums_of(*vals[i].arr);
    }
    auto &paint = fill ? _gs.fill : _gs.stroke;
    // Whatever happens, the colour before it no longer applies: unread, it paints nothing.
    paint.reset();
    if (at < 0 || m.size() != 6) {
        unreadable("pattern fills written in a way PROOF doesn't know");
        return;
    }
    auto const it = _pattern_index.find(*vals[at].text());
    if (it == _pattern_index.end()) {
        unreadable("pattern fills whose pattern isn't in the file");
        return;
    }
    auto const &pattern = _doc.patterns[it->second];
    if (!pattern.unread.empty()) {
        unreadable("pattern fills with " + pattern.unread);
        return;
    }
    constexpr double AT_REST[] = {0, 0, 1, 1, 0, 0, 0, 0, 0};
    bool rest = n.size() >= 9;
    for (std::size_t i = 0; rest && i < 9; ++i) rest = std::abs(n[i] - AT_REST[i]) < 1e-9;
    Geom::Affine const matrix(m[0], m[1], m[2], m[3], m[4], m[5]);
    if (!rest || !finite(matrix) || std::abs(matrix.det()) < 1e-12) {
        unreadable("pattern fills placed the way Illustrator 8 and older placed them");
        return;
    }
    Paint p;
    p.kind = Paint::Kind::Pattern;
    p.pattern = it->second;
    p.pattern_matrix = matrix;
    paint = p;
}

// ---- placed files ------------------------------------------------------------------------

/// `[a b c d tx ty] llx lly urx ury … (path)` then the operator: where a placed file lies.
void Reader::place_file(Values const &vals)
{
    std::vector<double> m, box;
    std::string path;
    bool after = false;
    for (auto const &v : vals) {
        if (v.kind == Value::Kind::Arr) {
            m = nums_of(*v.arr);
            after = true;
            box.clear();
        } else if (after && v.kind == Value::Kind::Num) {
            box.push_back(v.num);
        } else if (v.kind == Value::Kind::Str) {
            path = v.str;
        }
    }
    if (m.size() != 6 || box.size() < 4 || path.empty() || path.size() > 4096) return;
    Placed p;
    p.matrix = Geom::Affine(m[0], m[1], m[2], m[3], m[4], m[5]);
    p.box = Geom::Rect(Geom::Point(box[0], box[1]), Geom::Point(box[2], box[3]));
    if (!finite(p.matrix) || std::abs(p.matrix.det()) < 1e-12 || !sane(p.box * p.matrix) || p.box.width() < 1e-6 ||
        p.box.height() < 1e-6) {
        return;
    }
    p.path = std::move(path);
    p.file_type = _place->file_type;
    _place->placed = std::move(p);
}

void Reader::end_place()
{
    if (!_place) return;
    auto placing = std::move(*_place);
    _place.reset();
    if (!placing.placed) {
        // An embedded EPS, or a placement written some other way.
        unreadable("placed files of a kind PROOF doesn't read");
        return;
    }
    if (_frames.empty()) return;
    auto node = new_node(Node::Kind::Placed, _gs);
    node.transparency = _gs.transparency;
    node.placed = static_cast<int>(_doc.placed.size());
    placing.placed->preview = std::move(placing.preview);
    _doc.placed.push_back(std::move(*placing.placed));
    add(std::move(node), placing.hidden);
}

void Reader::begin_obj(bool hidden)
{
    auto obj = std::make_shared<Obj>();
    if (!_stack.empty() && _stack.back().kind == Value::Kind::Name) {
        obj->type = _stack.back().str;
        _stack.pop_back();
    }
    if (obj->type == "AI11Text" || obj->type == "SymbolInstance" || obj->type == "ForeignObject") {
        for (auto &frame : _frames) {
            if (frame.kind == FrameKind::Obj && frame.obj->type == "Mask") {
                frame.obj->unsupported_mask_art = true;
            }
        }
    }
    Frame f;
    f.kind = FrameKind::Obj;
    f.obj = std::move(obj);
    f.hidden = hidden;
    open(std::move(f));
    // The state carries on into a dictionary, and is put back when it ends. A mask's art,
    // the only art of a dictionary that is kept, is written like the art around it: only
    // what differs from the object before is set again. A mask whose first shape is painted
    // as that object was sets no colour, and a state begun afresh here would make it black,
    // which hides what it masks.
}

void Reader::end_obj()
{
    if (_frames.empty() || _frames.back().kind != FrameKind::Obj) return;
    Frame f = std::move(_frames.back());
    _frames.pop_back();
    close_obj(*f.obj, _stack, f.base);
    _obj_hidden = f.hidden;
    // Only a mask's art is needed outside its dictionary. Type's outlines and other
    // dictionary artwork remain metadata, not additional visible objects.
    _gs = f.gs;
    _after_object = f.after;
    // So is where the object stood: a group's own transparency after its dictionary is still
    // the group's, though a mask's art ended in a style marker of its own (`9 () XW`).
    _marked = f.marked;
    _group_closed = f.group_closed;
    auto const &type = f.obj->type;
    if (type == "Mask") {
        f.obj->art = std::move(f.children);
        auto const disabled = f.obj->nums("Disabled");
        bool const active = disabled.size() != 1 || disabled[0] != 1.0;
        auto const clipped = f.obj->nums("Clipping"), inverted = f.obj->nums("Inverted"), linked = f.obj->nums("Linked");
        if (active && (disabled.size() != 1 || disabled[0] != 0.0 ||
                clipped.size() != 1 || (clipped[0] != 0.0 && clipped[0] != 1.0) || inverted.size() != 1 || inverted[0] != 0.0 ||
                (!linked.empty() && (linked.size() != 1 || linked[0] != 1.0)))) {
            unreadable("opacity masks outside the supported non-inverted greyscale vector subset");
        }
        // A text/symbol can be a dictionary directly inside the mask,
        // without a group frame. Such artwork must not become an empty mask.
        // Remember them when opened: X+ and drawing operators consume unkeyed
        // dictionary values, so checking the remaining entries would miss them.
        if (active && f.obj->unsupported_mask_art) {
            unreadable("opacity masks containing unsupported artwork dictionaries");
        }
        if ((_frames.empty() || _frames.back().kind != FrameKind::Obj || _frames.back().obj->type != "ArtDictionary") &&
            (disabled.size() != 1 || disabled[0] != 1.0)) unreadable("opacity masks without an artwork dictionary");
    }
    if (type == "ForeignObject") {
        // Art of another format that Illustrator keeps as it came (from a placed or pasted
        // PDF, mostly) and can't edit either: a blob with where it goes.
        foreign_art(*f.obj);
        return;
    }
    // A mask dictionary holds real artwork, so an ArtDictionary inside it belongs
    // to its preceding child. Apply that attachment before retaining the dictionary
    // as metadata; otherwise a nested mask would silently disappear at X+.
    if (type == "ArtDictionary" && !_frames.empty() && _frames.back().kind == FrameKind::Obj &&
        _frames.back().obj->type == "Mask") art_dictionary(*f.obj);
    if (!_frames.empty() && _frames.back().kind == FrameKind::Obj) {
        Value v;
        v.kind = Value::Kind::Dict;
        v.obj = f.obj;
        push(std::move(v));
        return;
    }
    if (type == "ArtDictionary") {
        art_dictionary(*f.obj);
    } else if (type == "Document") {
        document(*f.obj);
    } else if (type == "AI11Text") {
        text_slot(*f.obj);
    } else if (type == "SymbolInstance") {
        unreadable("symbols");
    }
}

// Record layout reference: VectorCraft ai/read.rs::foreign_art at 0d0ea352 (MIT).
// Keep the full affine and PDF page margins rather than fitting painted bounding boxes.
void Reader::foreign_art(Obj const &o)
{
    auto bad = [&] { unreadable("art kept in another format (foreign objects) with unsupported or malformed records"); };
    std::set<std::string> keys;
    for (auto const &[key, values] : o.entries) {
        if (!key || !keys.insert(*key).second ||
            (*key != "Version" && *key != "RTransform" && *key != "Origin" && *key != "Bounds" && *key != "Data")) {
            bad(); return;
        }
    }
    auto numbers = [&](char const *key, std::size_t count) -> std::optional<std::vector<double>> {
        auto values = o.get(key);
        if (!values || values->size() != count) return {};
        std::vector<double> out;
        for (auto const &v : *values) {
            if (v.kind != Value::Kind::Num || !std::isfinite(v.num) || std::abs(v.num) > MAX_SIDE) return {};
            out.push_back(v.num);
        }
        return out;
    };
    auto version = numbers("Version", 1), rt = numbers("RTransform", 6), origin = numbers("Origin", 2), bounds = numbers("Bounds", 4);
    auto data = o.get("Data");
    bool const compound = std::any_of(_frames.begin(), _frames.end(), [](Frame const &f) {
        return f.kind == FrameKind::Compound || f.kind == FrameKind::Obj;
    });
    if (!version || (*version)[0] != 1 || !rt || !origin || !bounds || !data || !data->empty() ||
        !o.content.empty() || compound || _frames.empty() || (*bounds)[2] <= (*bounds)[0] || (*bounds)[3] <= (*bounds)[1]) {
        bad(); return;
    }
    Geom::Affine const matrix((*rt)[0], (*rt)[1], (*rt)[2], (*rt)[3], (*rt)[4], (*rt)[5]);
    if (matrix.isSingular()) { bad(); return; }
    // PDF's (0,0) is the low corner of Bounds. Origin flips the native canvas's
    // y axis before RTransform. Keep the full affine, including rotation
    // and shear; fitting transformed bounding boxes would destroy both.
    auto const to_art = Geom::Scale(1, -1) *
        Geom::Translate((*bounds)[0] + (*origin)[0], (*origin)[1] - (*bounds)[1]) * matrix;
    Geom::Rect box(0, 0, (*bounds)[2] - (*bounds)[0], (*bounds)[3] - (*bounds)[1]);
    box *= to_art;
    if (!std::isfinite(box.left()) || !std::isfinite(box.top()) || !std::isfinite(box.right()) || !std::isfinite(box.bottom()) ||
        std::max({std::abs(box.left()), std::abs(box.top()), std::abs(box.right()), std::abs(box.bottom())}) > MAX_SIDE) {
        bad(); return;
    }
    auto pdf = ascii85_decode(o.foreign_data, _limits.max_foreign_bytes - _foreign_bytes);
    if (!pdf || pdf->compare(0, 5, "%PDF-") != 0) { bad(); return; }
    auto n = new_node(Node::Kind::Foreign, _gs);
    n.transparency = _gs.transparency;
    n.foreign = static_cast<int>(_doc.foreign.size());
    _foreign_bytes += pdf->size();
    _doc.foreign.push_back({std::move(*pdf), Geom::Rect((*bounds)[0], (*bounds)[1], (*bounds)[2], (*bounds)[3]), to_art});
    add(std::move(n), _obj_hidden);
}

/**
 * `/Mesh X!` begins a mesh and `/End X!` ends it; everything between is its own (`X#`).
 *
 * An envelope keeps its non-art mesh the same way. Refuse unsupported meshes only after
 * plugin_object has removed those, so an envelope's controls cannot cost its drawn art.
 *
 * @return whether the marker was one of the two.
 */
bool Reader::mesh_marker(Values const &vals, bool hidden)
{
    std::string_view name;
    for (auto const &v : vals) {
        if (v.kind == Value::Kind::Name) name = v.str;
    }
    if (name == "Mesh") {
        if (_frames.empty()) fail("it has gradient meshes outside its layers");
        if (_mesh) {
            _mesh->mesh.unread = true;
            return true;
        }
        _mesh.emplace();
        _mesh->commented = hidden;
        return true;
    }
    if (name != "End" || !_mesh) return false;
    auto m = std::move(*_mesh);
    _mesh.reset();
    m.mesh.unread = m.mesh.unread || !m.version || !m.cartesian || !m.data || m.place ||
                    m.rows == 0 || m.columns == 0 || m.mesh.patches.size() != m.rows * m.columns ||
                    std::any_of(_frames.begin(), _frames.end(), [](Frame const &f) { return f.kind == FrameKind::Compound; });
    // In a pattern's tile it only matters to what is painted with that pattern.
    if (_pattern_unread && m.mesh.unread) {
        _pattern_unread->insert("a gradient mesh");
        return true;
    }
    auto n = new_node(Node::Kind::Mesh, _gs);
    m.mesh.size = {m.rows, m.columns};
    n.mesh = std::move(m.mesh);
    n.transparency = _gs.transparency;
    add(std::move(n), m.commented || hidden);
    return true;
}

/// A mesh's process inks, `[ [c m y k /DeviceCMYK (Cyan) /Separation] ... n /NChannel ]`: each
/// one's CMYK. Nothing for anything else, spot inks included.
static std::optional<std::vector<std::array<double, 4>>> process_inks(std::vector<Value> const &vals)
{
    if (vals.size() != 2 || vals[0].kind != Value::Kind::Arr) return {};
    auto const &a = *vals[0].arr;
    if (a.size() < 3 || a.back().kind != Value::Kind::Name || a.back().str != "NChannel" ||
        a[a.size() - 2].kind != Value::Kind::Num || a[a.size() - 2].num != double(a.size() - 2) || a.size() - 2 > 4) {
        return {};
    }
    static std::map<std::string, std::array<double, 4>> const process = {
        {"Cyan", {1, 0, 0, 0}}, {"Magenta", {0, 1, 0, 0}}, {"Yellow", {0, 0, 1, 0}}, {"Black", {0, 0, 0, 1}}};
    std::vector<std::array<double, 4>> inks;
    for (std::size_t i = 0; i + 2 < a.size(); ++i) {
        if (a[i].kind != Value::Kind::Arr) return {};
        auto const &s = *a[i].arr;
        if (s.size() != 7 || s[4].kind != Value::Kind::Name || s[4].str != "DeviceCMYK" ||
            s[5].kind != Value::Kind::Str || s[6].kind != Value::Kind::Name || s[6].str != "Separation") {
            return {};
        }
        auto const ink = process.find(s[5].str);
        if (ink == process.end()) return {};
        for (std::size_t j = 0; j < 4; ++j) {
            if (s[j].kind != Value::Kind::Num || s[j].num != ink->second[j]) return {};
        }
        inks.push_back(ink->second);
    }
    return inks;
}

/// A named process colour, `[r g b /DeviceRGB (name) /Process]` or `[c m y k /DeviceCMYK (name)
/// /Process]`, whose points give a tint of it.
static std::optional<Color> process_swatch(std::vector<Value> const &vals)
{
    if (vals.size() != 2 || vals[0].kind != Value::Kind::Arr) return {};
    auto const &a = *vals[0].arr;
    if (a.size() != 6 && a.size() != 7) return {};
    auto const n = a.size() - 3;
    if (a.back().kind != Value::Kind::Name || a.back().str != "Process" || a[n + 1].kind != Value::Kind::Str ||
        a[n].kind != Value::Kind::Name || a[n].str != (n == 4 ? "DeviceCMYK" : "DeviceRGB")) {
        return {};
    }
    double v[4] = {};
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i].kind != Value::Kind::Num || !(a[i].num >= 0 && a[i].num <= 1)) return {};
        v[i] = a[i].num;
    }
    return n == 4 ? Color::cmyk(v[0], v[1], v[2], v[3]) : Color::rgb(v[0], v[1], v[2]);
}

/** Mesh layout reference: VectorCraft ai/mesh.rs at 0d0ea352 (MIT, see the header).
 * Unlike that reader, keep /S and /A geometry and check every entry. A patch with extra
 * boundary nodes and uniform colour and opacity is its exact closed outline. One whose
 * colour changes is the Coons patch of its edges, as Illustrator draws it; the order its
 * anchors are listed in, and how it is drawn, were read from controls saved by Illustrator
 * 29.2.1 (`PROOF-ai-native-records.md`, "Gradient meshes whose edges bend").
 */
void Reader::mesh_entry(Values const &vals)
{
    auto &m = *_mesh;
    auto bad = [&] { m.mesh.unread = true; };
    if (vals.empty() || vals.back().kind != Value::Kind::Name) { bad(); return; }
    auto const &key = vals.back().str;
    auto array = [&]() -> std::optional<std::vector<double>> {
        if (vals.size() != 2 || vals[0].kind != Value::Kind::Arr) return {};
        std::vector<double> out;
        for (auto const &v : *vals[0].arr) {
            if (v.kind != Value::Kind::Num || !std::isfinite(v.num)) return {};
            out.push_back(v.num);
        }
        return out;
    };
    auto named = [&](std::string_view name) {
        return vals.size() == 2 && vals[0].kind == Value::Kind::Name && vals[0].str == name;
    };
    if (key == "Version") {
        m.version = vals.size() == 2 && vals[0].as_num() == 2.0;
        if (!m.version) bad();
    } else if (key == "Type") {
        m.cartesian = named("Cartesian");
        if (!m.cartesian) bad();
    } else if (key == "Size") {
        auto n = array();
        auto dim = [&](double v) { return v >= 1 && v <= _limits.max_mesh_patches && v == std::floor(v); };
        if (!n || n->size() != 2 || !dim((*n)[0]) || !dim((*n)[1]) ||
            (*n)[0] * (*n)[1] > _limits.max_mesh_patches || m.rows) { bad(); return; }
        m.rows = (*n)[0];
        m.columns = (*n)[1];
    } else if (key == "Data") {
        if (vals.size() != 1 || m.data) bad();
        m.data = true;
    } else if (key == "CS") {
        // A device space, or process inks as Illustrator 29 writes them, before each point:
        // [ [c m y k /DeviceCMYK (Cyan) /Separation] ... n /NChannel ]. Spot inks aren't read.
        m.inks.clear();
        if (vals.size() == 2 && vals[0].kind == Value::Kind::Name &&
            (vals[0].str == "DeviceGray" || vals[0].str == "DeviceRGB" || vals[0].str == "DeviceCMYK")) {
            m.space = vals[0].str;
        } else if (auto inks = process_inks(vals)) {
            m.space = "NChannel";
            m.inks = std::move(*inks);
        } else if (auto swatch = process_swatch(vals)) {
            m.space = "Process";
            m.swatch = *swatch;
        } else {
            bad(); return;
        }
    } else if (key == "P") {
        auto n = array();
        if (m.place || !m.boundary.empty() || !n || n->size() != 2 || !m.data ||
            (*n)[0] < 0 || (*n)[0] >= m.rows || (*n)[0] != std::floor((*n)[0]) ||
            (*n)[1] < 0 || (*n)[1] >= m.columns || (*n)[1] != std::floor((*n)[1])) { bad(); return; }
        m.place = std::pair<std::size_t, std::size_t>((*n)[0], (*n)[1]);
        if (!m.places.insert(*m.place).second) bad();
    } else if (key == "R") {
        // The patch's grid parameter. Patches are drawn independently, in file order.
        auto n = array();
        if (!m.place || !n || n->size() != 1 || (*n)[0] < -1 || (*n)[0] > 1) bad();
    } else if (key == "N" || key == "S" || key == "A") {
        auto n = array();
        bool const corner = key == "N";
        std::size_t const components = m.space == "NChannel" ? m.inks.size() : m.space == "DeviceCMYK" ? 4 :
                                       m.space == "DeviceRGB" ? 3 : 1; // Gray, and a Process tint
        if (!m.place || !n || n->size() != (corner ? components + 9 : 7) ||
            ++m.points > _limits.max_points || m.boundary.size() >= 4096 ||
            (corner && m.corners.size() >= 4)) { bad(); return; }
        auto const &v = *n;
        MeshCorner c;
        if (corner) {
            for (std::size_t i = 0; i <= components; ++i) {
                if (v[i] < 0 || v[i] > 1) { bad(); return; }
            }
            if (m.space == "NChannel") {
                std::array<double, 4> sum{};
                for (std::size_t i = 0; i < components; ++i) {
                    for (std::size_t j = 0; j < 4; ++j) sum[j] = std::min(1.0, sum[j] + v[i] * m.inks[i][j]);
                }
                c.color = Color::cmyk(sum[0], sum[1], sum[2], sum[3]);
            } else if (m.space == "Process") {
                c.color = tinted(m.swatch, v[0]);
            } else {
                c.color = components == 4 ? Color::cmyk(v[0], v[1], v[2], v[3]) :
                          components == 3 ? Color::rgb(v[0], v[1], v[2]) : Color::gray(v[0]);
            }
            c.opacity = v[components];
            c.point = Geom::Point(v[components + 1], v[components + 2]);
            c.next = Geom::Point(v[components + 3], v[components + 4]);
            c.previous = Geom::Point(v[components + 6], v[components + 7]);
            if (v[components + 5] != 1 || v[components + 8] != 1) { bad(); return; }
        } else {
            c.point = Geom::Point(v[0], v[1]);
            c.previous = Geom::Point(v[2], v[3]);
            c.next = Geom::Point(v[4], v[5]);
            // 1 a smooth point, 0 a corner (its handles apart): drawn alike.
            if (v[6] != 1 && v[6] != 0) { bad(); return; }
        }
        for (auto const &p : {c.point, c.next, c.previous}) {
            if (std::abs(p.x()) > MAX_SIDE || std::abs(p.y()) > MAX_SIDE) { bad(); return; }
        }
        if (corner) {
            m.corner_at.push_back(m.boundary.size());
            m.corners.push_back(c);
        }
        m.boundary.push_back(c);
    } else if (key == "E") {
        if (vals.size() != 1 || !m.place || m.corners.size() != 4 ||
            m.mesh.patches.size() >= _limits.max_mesh_patches) { bad(); return; }
        MeshPatch p;
        p.place = {m.place->first, m.place->second};
        std::copy(m.corners.begin(), m.corners.end(), p.corners.begin());
        // The corners run from the lower left with the patch's turn. An edge's anchors follow
        // the corner it starts from, the last edge's come before the first corner, and they
        // are listed along the mesh's grid, the first handle towards the one listed before:
        // with the turn on the first two edges, against it on the other two, which are read
        // back to front with their handles swapped (gradient mesh controls, 2026-10-17).
        auto const &b = m.boundary;
        auto const &at = m.corner_at;
        for (std::size_t k = 0; k < 4; ++k) {
            std::vector<MeshCorner> run(b.begin() + at[k] + 1, k < 3 ? b.begin() + at[k + 1] : b.end());
            if (k == 3) run.insert(run.end(), b.begin(), b.begin() + at[0]);
            if (k >= 2) {
                std::reverse(run.begin(), run.end());
                for (auto &a : run) std::swap(a.previous, a.next);
            }
            Geom::Point from = p.corners[k].point, out = p.corners[k].next;
            for (auto const &a : run) {
                p.edges[k].emplace_back(from, out, a.previous, a.point);
                from = a.point;
                out = a.next;
            }
            auto const &to = p.corners[(k + 1) % 4];
            p.edges[k].emplace_back(from, out, to.previous, to.point);
        }
        bool const bent = b.size() != 4;
        bool const uniform = std::all_of(m.corners.begin(), m.corners.end(), [&](MeshCorner const &c) {
            return c.color == m.corners[0].color && c.opacity == m.corners[0].opacity;
        });
        p.outlined = bent && uniform;
        if (bent && !uniform) {
            // Cut into cells at every anchor: bound how many, and blend one colour model.
            auto const across = p.edges[0].size() + p.edges[2].size(), down = p.edges[1].size() + p.edges[3].size();
            if (across * down > _limits.max_mesh_patches ||
                !std::all_of(m.corners.begin(), m.corners.end(),
                             [&](MeshCorner const &c) { return c.color.model == m.corners[0].color.model; })) {
                bad(); return;
            }
        }
        Geom::Path boundary(p.corners[0].point);
        for (auto const &edge : p.edges) {
            for (auto const &c : edge) boundary.appendNew<Geom::CubicBezier>(c[1], c[2], c[3]);
        }
        boundary.close(true);
        p.boundary.push_back(std::move(boundary));
        m.mesh.patches.push_back(std::move(p));
        m.boundary.clear();
        m.corner_at.clear();
        m.corners.clear();
        m.place.reset();
    } else {
        bad(); // A new mesh feature must never be silently dropped.
    }
}

/// Whether an object is nothing but meshes: one, or groups of them.
static bool only_meshes(Node const &n)
{
    if (n.kind == Node::Kind::Mesh) return true;
    return n.kind == Node::Kind::Group && !n.clipped &&
           std::all_of(n.children.begin(), n.children.end(), [](Node const &k) { return only_meshes(k); });
}

/**
 * `(a plug-in's name) … XP` ends an object that a plug-in keeps live: a compound shape, a
 * blend, a brush stroke, an envelope. Two groups come before the marker. The first is what
 * the object was made from (the paths of a compound shape, the ends and the spine of a
 * blend, the path a brush follows), always on %_ lines. The second is what the plug-in drew
 * of it, and only that is art: Illustrator draws and prints it, and so does the file's page.
 * The first group isn't kept. Drawn as well, a brush's path lies over its stroke, and the
 * paths of a compound shape over what they add up to, in colours of their own.
 *
 * An object with an appearance is on %_ lines altogether, its marker too, after its drawn
 * look. The paint that its style gives it then follows the marker, before the style's own
 * marker (`1 () XW`), which settles it as any object with a look.
 *
 * An envelope (Adobe Deform Plugin) has two things on its %_ lines: a group with the mesh it
 * distorts with, and in that group's dictionary the object it distorts, type or art. Art in
 * a dictionary is never kept. With the mesh gone too, what is left is the outlines the
 * envelope drew, which is all Illustrator draws of it.
 *
 * Objects of plug-ins that weren't met in a file are left as they have been read so far.
 */
void Reader::plugin_object(Values const &vals, bool hidden)
{
    if (_frames.empty()) return;
    auto const name = last_text(vals).value_or("");
    auto &kids = _frames.back().children;
    if (kids.size() < 2) return;
    auto const &drawn = kids[kids.size() - 1];
    auto const &source = kids[kids.size() - 2];
    if (drawn.kind != Node::Kind::Group || drawn.commented != hidden || !source.commented) return;
    bool const envelope = name == "Adobe Deform Plugin";
    if (envelope) {
        if (hidden || !only_meshes(source)) return;
    } else if (source.kind != Node::Kind::Group ||
               std::find(std::begin(LIVE_PLUGINS), std::end(LIVE_PLUGINS), name) == std::end(LIVE_PLUGINS)) {
        return;
    }
    kids.erase(kids.end() - 2);
    auto &made = kids.back();
    made.plugin = name;
    if (envelope) {
        made.drawn_look = true;
        made.envelope = true;
        return;
    }
    // The object's own name, transparency and style follow the marker.
    _after_object = true;
    _marked = false;
    _after_plugin = hidden;
    _group_closed = true;
}

void Reader::text_slot(Obj const &o)
{
    auto n = new_node(Node::Kind::Text, _gs);
    auto const story = o.nums("StoryIndex");
    if (!story.empty() && story.front() >= 0 && story.front() < 4294967295.0) n.story = static_cast<unsigned>(story.front());
    auto const drawn = o.nums("FreeUndo");
    n.text_proxy = !drawn.empty() && drawn.front() == 1.0;
    add(std::move(n), _obj_hidden);
}

void Reader::art_dictionary(Obj const &o)
{
    auto *n = last_object();
    for (auto const *item : o.items()) {
        if (item->type != "Mask") continue;
        auto const disabled = item->nums("Disabled");
        if (disabled.size() == 1 && disabled[0] == 1.0) continue;
        if (!n || n->kind == Node::Kind::Layer || item != o.obj("AI9_Mask")) {
            unreadable("opacity masks without a supported object attachment");
        }
    }
    if (!n || n->kind == Node::Kind::Layer) return;
    std::optional<std::string> name = o.text("AIArtName");
    if (!name) {
        if (auto uid = o.obj("AI10_ArtUID")) {
            if (auto id = uid->content_text()) name = xml_name(*id);
        }
    }
    if (name && !name->empty()) n->name = *name;
    if (auto r = o.text("BBAccumRotation")) {
        if (auto v = parse_number(*r); v && *v != 0.0) n->box_rotation = *v;
    }
    if (n->placed >= 0 && n->placed < static_cast<int>(_doc.placed.size())) {
        auto &placed = _doc.placed[n->placed];
        if (auto const page = o.nums("AI11 PlacedPDFPageIndex"); page.size() == 1) placed.page = int(page[0]);
        if (auto const crop = o.nums("AI11 PlacedPDFCropTo"); crop.size() == 1) placed.crop_to = int(crop[0]);
    }
    if (auto const *mask = o.obj("AI9_Mask")) {
        auto const disabled = mask->nums("Disabled");
        if (disabled.size() == 1 && disabled[0] == 1.0) return;
        auto const clipped = mask->nums("Clipping"), inverted = mask->nums("Inverted");
        // Non-clipping masks have an opaque white backdrop. Inversion applies to the
        // composite, and is still refused rather than reversing individual shapes.
        bool const supported_flags = mask->type == "Mask" && disabled.size() == 1 && disabled[0] == 0.0 &&
            clipped.size() == 1 && (clipped[0] == 0.0 || clipped[0] == 1.0) && inverted.size() == 1 && inverted[0] == 0.0;
        auto grey_color = [](Color const &c) {
            return c.model == Color::Model::Gray ||
                (c.model == Color::Model::Cmyk && c.v[0] == 0 && c.v[1] == 0 && c.v[2] == 0) ||
                (c.model == Color::Model::Rgb && c.v[0] == c.v[1] && c.v[1] == c.v[2]);
        };
        auto grey = [&](Paint const &p) {
            if (p.kind == Paint::Kind::Solid) return p.ink < 0 && grey_color(p.color);
            if (p.kind != Paint::Kind::Gradient || p.gradient.gradient < 0 ||
                p.gradient.gradient >= static_cast<int>(_doc.gradients.size())) return false;
            auto const &stops = _doc.gradients[p.gradient.gradient].stops;
            return !stops.empty() && std::all_of(stops.begin(), stops.end(), [&](GradientStop const &s) {
                return s.ink < 0 && grey_color(s.color);
            });
        };
        auto supported_art = [&](auto &&self, std::vector<Node> const &nodes) -> bool {
            for (auto const &k : nodes) {
                if ((k.kind != Node::Kind::Path && k.kind != Node::Kind::Compound && k.kind != Node::Kind::Group) ||
                    k.guide || (k.stroke && !grey(*k.stroke)) || (k.fill && !grey(*k.fill)) ||
                    (k.transparency.blend != 0 && k.transparency.blend != 1) || k.transparency.knockout == 1 ||
                    !self(self, k.children) || (k.opacity_mask && !self(self, *k.opacity_mask))) return false;
            }
            return true;
        };
        bool const in_compound = n->clipping || std::any_of(_frames.begin(), _frames.end(), [](Frame const &f) {
            return f.kind == FrameKind::Compound;
        });
        if (!supported_flags || in_compound || !supported_art(supported_art, mask->art)) {
            unreadable("opacity masks outside the supported non-inverted greyscale vector subset");
        } else {
            n->opacity_mask = mask->art;
            n->opacity_mask_clipped = clipped[0] == 1.0;
        }
    }
}

void Reader::document(Obj const &o)
{
    auto const *list = find_entry(o, "ArtboardArray");
    if (!list) return;
    int i = 0;
    for (auto const *ab : list->items()) {
        if (++i > 1000) break;
        auto const p1 = ab->nums("PositionPoint1");
        auto const p2 = ab->nums("PositionPoint2");
        if (p1.size() < 2 || p2.size() < 2) continue;
        Geom::Rect r(Geom::Point(p1[0], p1[1]), Geom::Point(p2[0], p2[1]));
        if (!sane(r) || r.area() <= 0) continue;
        auto name = ab->text("Name").value_or("Artboard " + std::to_string(i));
        _doc.artboards.push_back({std::move(name), r});
    }
}

void Reader::layer_attrs(Values const &vals)
{
    auto const n = nums_of(vals);
    if (_frames.empty() || _frames.back().kind != FrameKind::Layer) return;
    auto &a = _frames.back().layer;
    auto flag = [&](std::size_t i, bool def) { return i < n.size() ? n[i] != 0.0 : def; };
    a.visible = flag(0, true);
    a.options.preview = flag(1, true);
    a.locked = !flag(2, true);
    a.options.printable = flag(3, true);
    double dim = n.size() > 12 && std::isfinite(n[12]) ? std::clamp(n[12], 0.0, 100.0) : 50.0;
    if (flag(4, false)) a.options.dim = static_cast<int>(std::lround(dim));
    // The published Illustrator 7 form has ten operands. Modern files insert a field
    // before the colour index and append the dim percentage and reserved fields.
    std::size_t const color = n.size() == 10 ? 6 : 7;
    double const index = n.size() > color ? n[color] : 0.0;
    auto rgb = [&](std::size_t i) { return i < n.size() ? static_cast<int>(std::clamp(n[i], 0.0, 255.0)) : 0; };
    if (index >= 0 && index < 27 && index == std::floor(index)) {
        a.options.color_index = static_cast<int>(index);
    } else {
        a.options.color_index = -1;
    }
    a.options.color_rgb = {rgb(color + 1), rgb(color + 2), rgb(color + 3)};
}

int Reader::ink_index(Named const &n, bool by_name)
{
    if (!by_name) {
        // Gradients are defined before the Swatches panel is listed, so a stop may have met
        // the name first. Its guess becomes the swatch: the stop keeps pointing at it.
        for (auto const i : _stop_inks) {
            if (_doc.inks[i].name == n.name) {
                _doc.inks[i] = {n.name, n.kind, n.color, n.cmyk};
                _stop_inks.erase(i);
                return static_cast<int>(i);
            }
        }
    }
    for (std::size_t i = 0; i < _doc.inks.size(); ++i) {
        auto const &ink = _doc.inks[i];
        if (ink.name == n.name && ink.kind == n.kind && ink.color == n.color) return static_cast<int>(i);
    }
    // A gradient stop names its colour in CMYK form only, without saying whether it is
    // a spot ink or a global process colour: it is the swatch of that name.
    if (by_name) {
        for (std::size_t i = 0; i < _doc.inks.size(); ++i) {
            if (_doc.inks[i].name == n.name && _doc.inks[i].cmyk == n.cmyk) return static_cast<int>(i);
        }
        for (std::size_t i = 0; i < _doc.inks.size(); ++i) {
            if (_doc.inks[i].name == n.name) return static_cast<int>(i);
        }
    }
    if (_doc.inks.size() >= _limits.max_inks) fail("it has too many named colours");
    _doc.inks.push_back({n.name, n.kind, n.color, n.cmyk});
    if (by_name) _stop_inks.insert(_doc.inks.size() - 1);
    return static_cast<int>(_doc.inks.size() - 1);
}

void Reader::color(std::string_view w, Values const &vals)
{
    // Fill operators are lower case, or start with X and continue in lower case.
    bool const fill = w.size() == 1 ? (w[0] >= 'a' && w[0] <= 'z') : (w[1] >= 'a' && w[1] <= 'z');
    auto const nums = nums_of(vals);
    std::optional<Paint> p;
    auto solid = [](Color c) { return Paint{Paint::Kind::Solid, c}; };
    auto const lower = w.size() == 1 ? std::string(1, static_cast<char>(g_ascii_tolower(w[0])))
                                     : std::string("X") + static_cast<char>(g_ascii_tolower(w[1]));
    if (lower == "g") {
        if (!nums.empty()) p = solid(Color::gray(unit(nums.back())));
    } else if (lower == "k") {
        if (nums.size() >= 4) {
            auto const *n = nums.data() + nums.size() - 4;
            p = solid(Color::cmyk(unit(n[0]), unit(n[1]), unit(n[2]), unit(n[3])));
        }
    } else if (lower == "Xa") {
        if (nums.size() >= 3) {
            auto const *n = nums.data() + nums.size() - 3;
            p = solid(Color::rgb(unit(n[0]), unit(n[1]), unit(n[2])));
        }
    } else {
        Ink::Kind kind = Ink::Kind::Spot;
        std::optional<bool> force;
        if (lower == "x" || lower == "Xs") force = false; // c m y k (name) tint: no type
        if (lower == "Xk") kind = Ink::Kind::Process;
        if (lower == "Xs" || lower == "Xz") kind = Ink::Kind::Registration;
        if (auto n = parse_named(vals, kind, force)) {
            if (n->name == "[Registration]") n->kind = Ink::Kind::Registration;
            Paint paint{Paint::Kind::Solid, n->color};
            paint.ink = ink_index(*n, false);
            paint.tint = n->tint;
            p = paint;
        }
    }
    if (!p) return;
    if (p->color.model == Color::Model::Rgb) ++_rgb_colors;
    if (p->color.model == Color::Model::Cmyk) ++_cmyk_colors;
    (fill ? _gs.fill : _gs.stroke) = *p;
}

void Reader::gradient_stop(Values const &vals)
{
    auto &g = _gradient_def->gradient;
    if (g.stops.size() >= _limits.max_gradient_stops) return;
    auto const n = vals.size();
    auto num = [&](std::size_t back) -> std::optional<double> {
        return back <= n ? vals[n - back].as_num() : std::nullopt;
    };
    auto const ramp = num(1);
    auto const mid = num(2);
    if (!ramp || !mid) return;
    // Newer stops have an opacity and the marker 6 before the midpoint.
    bool const newer = n >= 5 && num(3) == 6.0;
    std::size_t const style_back = newer ? 5 : 3;
    double const opacity = newer ? num(4).value_or(1.0) : 1.0;
    auto const style = num(style_back);
    if (!style) return;
    Values const comps(vals.begin(), vals.begin() + (n - style_back));
    auto c = stop_color(*style, comps);
    if (!c) return;
    GradientStop stop;
    stop.offset = std::clamp(*ramp / 100.0, 0.0, 1.0);
    stop.midpoint = std::clamp(*mid / 100.0, 0.13, 0.87);
    stop.opacity = unit(opacity);
    stop.color = c->first;
    if (c->second) {
        stop.ink = ink_index(*c->second, true);
        stop.tint = c->second->tint;
    }
    g.stops.push_back(stop);
}

void Reader::dash(Values const &vals)
{
    std::vector<double> pattern;
    for (auto it = vals.rbegin(); it != vals.rend(); ++it) {
        if (it->kind == Value::Kind::Arr) {
            pattern = nums_of(*it->arr);
            break;
        }
    }
    auto const nums = nums_of(vals);
    double const offset = nums.empty() ? 0.0 : nums.back();
    bool ok = !pattern.empty() && pattern.size() <= 64;
    for (double v : pattern) ok = ok && std::isfinite(v) && v >= 0;
    _gs.style.dash = ok ? pattern : std::vector<double>{};
    _gs.style.dash_offset = std::isfinite(offset) ? offset : 0.0;
}

void Reader::transparency(Values const &vals)
{
    auto const n = nums_of(vals);
    if (n.size() < 2) return;
    auto &t = _gs.transparency;
    t.blend = (n[0] >= 0 && n[0] < 16) ? static_cast<int>(n[0]) : 0;
    t.opacity = std::isfinite(n[1]) ? unit(n[1]) : 1.0;
    t.isolate = n.size() > 2 && n[2] != 0.0;
    int const ko = n.size() > 3 ? static_cast<int>(n[3]) : 0;
    t.knockout = (ko == 1 || ko == 2) ? ko : 0;
    t.knockout_shape = n.size() > 4 && n[4] != 0.0;
    // Straight after a group or mesh closes, it is that object's. Illustrator writes a mesh's
    // own opacity both before its records and after /End, before its style marker.
    // A compound path has the transparency
    // of its paths, set inside it, and no state of its own after it: what follows its end is
    // the next object's.
    if (auto *obj = last_object(); obj && !_marked &&
        (obj->kind == Node::Kind::Group || obj->kind == Node::Kind::Mesh)) obj->transparency = t;
}

/// Remove the children read from %_ lines.
void strip_commented(Node &n)
{
    auto &c = n.children;
    c.erase(std::remove_if(c.begin(), c.end(), [](Node const &k) { return k.commented; }), c.end());
    for (auto &k : c) strip_commented(k);
}

/// The type an appearance drew, in `n` and what it holds, in order.
void proxies_in(Node &n, std::vector<Node *> &out)
{
    if (n.kind == Node::Kind::Text && n.text_proxy) out.push_back(&n);
    for (auto &k : n.children) proxies_in(k, out);
}

/// The stories of the type that was typed, in `n` and what it holds, in order.
void stories_in(Node const &n, std::vector<unsigned> &out)
{
    if (n.kind == Node::Kind::Text && n.story && !n.text_proxy) out.push_back(*n.story);
    for (auto const &k : n.children) stories_in(k, out);
}

/// Whether `n` is type and nothing else; with `plain`, in groups that add nothing of their
/// own either (no name, no transparency, not hidden, locked or clipped).
bool only_type(Node const &n, bool plain)
{
    if (n.opacity_mask) return false;
    if (n.kind == Node::Kind::Text) return true;
    if (n.kind != Node::Kind::Group || n.children.empty()) return false;
    if (plain && (!n.transparency.is_default() || !n.visible || n.locked || !n.name.empty() || n.clipped)) return false;
    return std::all_of(n.children.begin(), n.children.end(), [&](Node const &k) { return only_type(k, plain); });
}

void uncomment(Node &n)
{
    n.commented = false;
    for (auto &k : n.children) uncomment(k);
}

/// The type in `n` and what it holds, in order.
void type_in(Node const &n, std::vector<Node const *> &out)
{
    if (n.kind == Node::Kind::Text) out.push_back(&n);
    for (auto const &k : n.children) type_in(k, out);
}

/**
 * Settle the looks that hold type, now that the stories are read (`texts`, or none).
 *
 * A look holding type was kept with the object it stands for. When the look is nothing but
 * type in groups that add nothing, and each piece sets what the object's own type sets
 * (the same characters, styles and places; a drawn story that isn't there or can't be read
 * counts as the same), the object stands in the look's place, as it was typed. Otherwise
 * the look stands, drawing its own stories. Drawn type whose story isn't in the records
 * takes the object's typed story instead, when there is one a piece or one for all: files
 * without the second text document, and the fixtures of the tests, are read that way.
 */
void settle_type(Node &n, TextDocument const *texts)
{
    for (auto &k : n.children) settle_type(k, texts);
    if (n.typed.empty()) return;
    Node object = std::move(n.typed.front());
    n.typed.clear();
    settle_type(object, texts);

    auto drawn_story = [&](Node const &p) -> std::optional<PointText> const * {
        return (texts && p.story && *p.story < texts->drawn.size()) ? &texts->drawn[*p.story] : nullptr;
    };
    auto typed_story = [&](Node const &t) -> std::optional<PointText> const * {
        return (texts && t.story && *t.story < texts->stories.size()) ? &texts->stories[*t.story] : nullptr;
    };
    std::vector<Node *> drawn;
    proxies_in(n, drawn);
    std::vector<Node const *> typed;
    type_in(object, typed);
    bool stands = only_type(n, true) && only_type(object, false) && drawn.size() == typed.size();
    for (std::size_t i = 0; stands && i < drawn.size(); ++i) {
        if (typed[i]->text_proxy) {
            stands = false;
            break;
        }
        auto const *d = drawn_story(*drawn[i]);
        auto const *t = typed_story(*typed[i]);
        if (!d || !*d) continue; // Nothing to tell them apart by.
        stands = t && *t && same_drawing(**d, **t);
    }
    if (stands) {
        bool const commented = n.commented;
        if (!commented) uncomment(object);
        n = std::move(object);
        n.commented = commented;
        return;
    }
    std::vector<unsigned> stories;
    stories_in(object, stories);
    bool every = true;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        if (drawn_story(*drawn[i])) {
            every = false;
        } else if (stories.size() == 1 || stories.size() == drawn.size()) {
            drawn[i]->story = stories[stories.size() == 1 ? 0 : i];
            drawn[i]->text_proxy = false;
        } else {
            every = false;
        }
    }
    // A group around the object's own type and nothing else isn't a look: it draws nothing
    // the type doesn't.
    if (every && only_type(n, false)) n.drawn_look = false;
}

void Reader::style_marker(Values const &vals, bool hidden)
{
    auto const nums = nums_of(vals);
    double const code = nums.empty() ? 0.0 : nums.back();
    auto const style = last_text(vals).value_or("");
    bool const after = _after_object;
    _marked = true;
    _after_plugin = false;
    // `1 (style) XW` after the object written on %_ lines; its drawn look comes before it.
    // The style's name can be empty. An object with an appearance inside one that has an
    // appearance too is all on %_ lines: its look, itself and this marker (`nested`). It is
    // settled the same way, and stays part of the object it is in.
    if (code != 1.0 || !after || _frames.empty()) return;
    auto &kids = _frames.back().children;
    if (kids.size() < 2) return;
    auto &object = kids[kids.size() - 1];
    auto &look = kids[kids.size() - 2];
    bool const nested = hidden;
    if (!object.commented || look.commented != nested || look.kind != Node::Kind::Group || look.clipped) return;
    Node obj = std::move(object);
    if (obj.opacity_mask) unreadable("opacity masks on styled source objects");
    kids.pop_back();
    auto &l = kids.back();
    if (!nested) strip_commented(l);
    if (!obj.name.empty()) l.name = obj.name;
    l.box_rotation = obj.box_rotation;
    l.drawn_look = true;
    if (!obj.plugin.empty()) l.plugin = obj.plugin;
    // Type among what the appearance drew has stories of its own, in the text document's
    // second part. Whether the look is anything more than the object's own type can only be
    // told from those, once they are read: the object is kept until then (settle_type). A
    // look that holds type keeps the state it was written with; the object's is on the
    // object.
    std::vector<Node *> drawn;
    proxies_in(l, drawn);
    if (!drawn.empty()) {
        l.typed.clear();
        l.typed.push_back(std::move(obj));
        return;
    }
    l.visible = obj.visible;
    l.locked = obj.locked;
}

void Reader::set_instance(Instance const &i)
{
    if (!i.name) return;
    auto it = _gradient_index.find(*i.name);
    if (it == _gradient_index.end()) {
        warn("a gradient isn't defined in the file: a plain colour stands in for it");
        return;
    }
    Paint p;
    p.kind = Paint::Kind::Gradient;
    p.gradient = i.placement;
    p.gradient.gradient = it->second;
    (i.stroke ? _pending_stroke : _pending_fill) = p;
}

// ---- paths -------------------------------------------------------------------------------

void Reader::path_op(std::string_view w, Values const &vals)
{
    auto const n = nums_of(vals);
    auto p = [&](std::size_t back) -> std::optional<Geom::Point> {
        if (n.size() < back) return {};
        return Geom::Point(n[n.size() - back], n[n.size() - back + 1]);
    };
    if (++_points > _limits.max_points) fail("a path has too many points");
    if (w == "m") {
        auto a = p(2);
        if (!a) return;
        finish_path();
        _current = Geom::Path(*a);
        _current_types = "c"; // m has no smooth/corner variant.
        return;
    }
    if (!_current) return;
    auto const before = _current->size_open();
    if (w == "l" || w == "L") {
        if (auto a = p(2)) _current->appendNew<Geom::LineSegment>(*a);
    } else if (w == "c" || w == "C") {
        auto a = p(6), b = p(4), c = p(2);
        if (a && b && c) _current->appendNew<Geom::CubicBezier>(*a, *b, *c);
    } else if (w == "v" || w == "V") {
        auto b = p(4), c = p(2);
        if (b && c) _current->appendNew<Geom::CubicBezier>(_current->finalPoint(), *b, *c);
    } else {
        auto a = p(4), c = p(2);
        if (a && c) _current->appendNew<Geom::CubicBezier>(*a, *c, *c);
    }
    if (_current->size_open() > before) _current_types += g_ascii_islower(w[0]) ? 's' : 'c';
}

void Reader::finish_path()
{
    if (!_current) return;
    auto const count = _current->empty() ? 0 : _current->size() + 1;
    if (_current->closed() && count) {
        // An explicit segment back to m supplies the seam's type. An implicit
        // closing line has no operator and leaves m's default corner in force.
        char const seam = _current_types.size() >= count ? _current_types.back() : 'c';
        _current_types.resize(count, 'c');
        _current_types.front() = _current_types.back() = seam;
    } else {
        _current_types.resize(count, 'c');
    }
    _path_types += _current_types;
    _current_types.clear();
    _path.push_back(std::move(*_current));
    _current.reset();
}

void Reader::end_path_quietly()
{
    _path.clear();
    _current.reset();
    _path_types.clear();
    _current_types.clear();
    _points = 0;
    _clip_next = false;
}

void Reader::paint(std::string_view op, bool hidden, bool guide)
{
    bool const close = !op.empty() && op[0] >= 'a' && op[0] <= 'z';
    if (_current && close && !_current->closed()) _current->close(true);
    finish_path();
    Geom::PathVector pv = std::move(_path);
    _path.clear();
    _points = 0;
    bool const clip = std::exchange(_clip_next, false);
    if (_instance) {
        auto i = std::move(*_instance);
        _instance.reset();
        set_instance(i);
    }
    auto fill_paint = std::exchange(_pending_fill, std::nullopt);
    auto stroke_paint = std::exchange(_pending_stroke, std::nullopt);
    if (pv.empty()) return;
    char const k = op.empty() ? 'n' : static_cast<char>(g_ascii_tolower(op[0]));
    bool const do_fill = k == 'f' || k == 'b';
    bool const do_stroke = k == 's' || k == 'b';
    auto node = new_node(Node::Kind::Path, _gs);
    node.path = std::move(pv);
    node.nodetypes = std::exchange(_path_types, {});
    node.evenodd = _gs.evenodd;
    node.clipping = clip;
    node.guide = guide;
    // W clips the following artwork; it doesn't cancel this path's own paint.
    if (do_fill) node.fill = fill_paint ? fill_paint : _gs.fill;
    if (do_stroke) node.stroke = stroke_paint ? stroke_paint : _gs.stroke;
    // The visual gate cannot identify unsupported method flags reliably. Only the
    // qualified Perceptual form is interpreted; other settings remain explicit refusals.
    auto check_gradient = [&](std::optional<Paint> const &paint) {
        if (!paint || paint->kind != Paint::Kind::Gradient || paint->gradient.gradient < 0 ||
            paint->gradient.gradient >= static_cast<int>(_doc.gradients.size())) return;
        auto const &options = _doc.gradients[paint->gradient.gradient].options;
        if (!options.empty() && options != std::vector<double>{1, 0}) {
            unreadable("unrecognised gradient interpolation settings");
        } else if (!options.empty()) {
            auto const &stops = _doc.gradients[paint->gradient.gradient].stops;
            if (std::any_of(stops.begin(), stops.end(), [](GradientStop const &stop) { return stop.ink >= 0; })) {
                unreadable("Perceptual gradients with spot inks");
            }
        }
    };
    if (!guide) {
        check_gradient(node.fill);
        check_gradient(node.stroke);
    }
    node.stroke_style = _gs.style;
    node.overprint_fill = _gs.overprint_fill && do_fill;
    node.overprint_stroke = _gs.overprint_stroke && do_stroke;
    node.transparency = _gs.transparency;
    if (clip && !_frames.empty()) _frames.back().clips.push_back(_frames.back().children.size());
    add(std::move(node), hidden);
}

// ---- images ------------------------------------------------------------------------------

void Reader::image(std::string_view data, bool hidden)
{
    Values vals = operands();
    std::vector<double> m;
    for (auto const &v : vals) {
        if (v.kind == Value::Kind::Arr) {
            m = nums_of(*v.arr);
            break;
        }
    }
    auto const n = nums_of(vals);
    if (m.size() < 6 || n.size() < 11) return;
    auto const w = static_cast<std::uint64_t>(std::max(0.0, n[4]));
    auto const h = static_cast<std::uint64_t>(std::max(0.0, n[5]));
    double const bits = n[6], kind = n[7], alpha = n[8], binary = n[10];
    if (w == 0 || h == 0 || w * h > _limits.max_pixels || (bits != 1.0 && bits != 8.0)) {
        warn("an image that can't be read was left out");
        return;
    }
    if (_raster_coloured && kind != 1.0) {
        unreadable("pictures coloured with a paint");
        return;
    }
    int channels = 3;
    if (_raster_space == "DeviceGray") channels = 1;
    else if (_raster_space == "DeviceCMYK") channels = 4;
    else if (_raster_space == "DeviceRGB") channels = 3;
    else channels = kind == 1.0 ? 1 : kind == 4.0 ? 4 : 3;
    std::string decoded;
    std::string_view bytes = data;
    if (binary == 0.0) {
        std::string digits;
        digits.reserve(data.size());
        for (char c : data) {
            if (g_ascii_isxdigit(c)) digits.push_back(c);
        }
        decoded = hex_decode(digits).value_or("");
        bytes = decoded;
    }
    std::uint64_t const row = bits == 1.0 ? (w * channels + 7) / 8 : w * channels;
    std::uint64_t const need = row * h;
    std::uint64_t const alpha_len = alpha > 0 ? w * h : 0;
    if (bytes.size() < need) {
        warn("an image whose data is cut short was left out");
        return;
    }
    Image img;
    double const *a = m.data();
    img.matrix = Geom::Affine(a[0], a[1], a[2], a[3], a[4], a[5]);
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.bits = static_cast<int>(bits);
    img.channels = channels;
    img.space = _raster_space;
    img.samples.assign(bytes.substr(0, need));
    if (alpha_len && bytes.size() >= need + alpha_len) img.alpha.assign(bytes.substr(need, alpha_len));
    auto node = new_node(Node::Kind::Image, _gs);
    node.transparency = _gs.transparency;
    node.image = static_cast<int>(_doc.images.size());
    _doc.images.push_back(std::move(img));
    add(std::move(node), hidden);
}

// ---- the document ------------------------------------------------------------------------

std::optional<Geom::Rect> Reader::sized_board() const
{
    if (!_art_size || !_template_center) return {};
    auto const [w, h] = *_art_size;
    auto const [cx, cy] = *_template_center;
    double const x0 = std::floor(cx - w / 2), y1 = std::ceil(cy + h / 2);
    Geom::Rect r(Geom::Point(x0, y1 - h), Geom::Point(x0 + w, y1));
    return (sane(r) && w > 0 && h > 0) ? std::optional(r) : std::nullopt;
}

/// The text objects written twice, plainly and on %_ lines: the commented copy of a story
/// that also has a plain one goes.
void drop_commented_copies(std::vector<Node> &layers)
{
    std::set<unsigned> plain;
    // Type an appearance drew counts its stories apart: it is no copy of typed text.
    auto collect = [&](auto &&self, std::vector<Node> const &nodes) -> void {
        for (auto const &n : nodes) {
            if (n.kind == Node::Kind::Text) {
                if (n.story && !n.commented && !n.text_proxy) plain.insert(*n.story);
            } else {
                self(self, n.children);
            }
        }
    };
    collect(collect, layers);
    if (plain.empty()) return;
    auto drop = [&](auto &&self, std::vector<Node> &nodes) -> void {
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                                   [&](Node const &n) {
                                       return n.kind == Node::Kind::Text && n.commented && !n.text_proxy && n.story &&
                                              plain.count(*n.story);
                                   }),
                    nodes.end());
        for (auto &n : nodes) self(self, n.children);
    };
    drop(drop, layers);
}

std::size_t count_commented(std::vector<Node> const &nodes)
{
    std::size_t n = 0;
    for (auto const &k : nodes) n += (k.commented ? 1 : 0) + count_commented(k.children);
    return n;
}

std::string join_and(std::set<std::string> const &items)
{
    std::string out;
    std::size_t i = 0;
    for (auto const &s : items) {
        if (i) out += (i + 1 == items.size()) ? " and " : ", ";
        out += s;
        ++i;
    }
    return out;
}

Document Reader::finish()
{
    // Envelopes have dropped their control meshes. Refuse only unread artwork meshes.
    auto meshes = [&](auto &&self, std::vector<Node> &nodes, bool shown) -> void {
        for (auto &n : nodes) self(self, n.children, shown && (n.kind != Node::Kind::Layer || n.visible));
        auto const kept = std::remove_if(nodes.begin(), nodes.end(), [](Node const &n) {
            return n.kind == Node::Kind::Mesh && (n.mesh.unread || n.mesh.patches.empty());
        });
        if (kept == nodes.end()) return;
        nodes.erase(kept, nodes.end());
        (shown ? _unsupported : _hidden_unread).insert("gradient meshes outside the supported four-corner or uniform-outline subset");
    };
    meshes(meshes, _doc.layers, true);
    if (_mesh) _unsupported.insert("gradient meshes");
    if (!_unknown.empty()) {
        std::string ops;
        for (auto const &o : _unknown) ops += (ops.empty() ? "`" : ", `") + o + "`";
        _unsupported.insert("operators PROOF doesn't know (" + ops + ")");
    }
    if (!_unsupported.empty()) fail("it has " + join_and(_unsupported) + ", which PROOF doesn't read from it yet");
    if (_doc.layers.empty()) fail("it has no layers");

    std::optional<Geom::Rect> first;
    if (!_doc.artboards.empty()) first = _doc.artboards.front().rect;
    if (!first) first = _cropmarks;
    if (!first) first = sized_board();
    Geom::Point const top_left = first ? Geom::Point(first->left(), first->bottom()) : Geom::Point(0, 792);
    _doc.to_doc = Geom::Affine(1, 0, 0, -1, -top_left.x(), top_left.y());
    if (_doc.artboards.empty()) {
        if (auto r = _cropmarks ? _cropmarks : sized_board()) {
            _doc.artboards.push_back({"Artboard 1", *r});
            _doc.board_guessed = !_cropmarks;
        }
    }
    _doc.bbox = _hires ? _hires : _bbox;
    if (_template_center) _doc.template_center = Geom::Point(_template_center->first, _template_center->second);
    _doc.cmyk = _color_model.value_or(_cmyk_colors > _rgb_colors);
    for (auto const &h : _hidden_unread) _doc.left_out.push_back(h);
    return std::move(_doc);
}

} // namespace

// ---- public ------------------------------------------------------------------------------

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

bool place_board_by_page(Document &doc, Geom::Rect const &page, Geom::Rect const &art)
{
    if (!doc.board_guessed || doc.artboards.size() != 1 || !doc.bbox) return false;
    auto const board = doc.artboards.front().rect;
    auto const &bounds = *doc.bbox;
    if (!sane(page) || !sane(art)) return false;
    Geom::Point origin = board.min();
    bool moved = false;
    for (auto d : {Geom::X, Geom::Y}) {
        // Another page than this artboard's says nothing about it.
        if (std::abs(page.dimensions()[d] - board.dimensions()[d]) > 0.5) return false;
    }
    for (auto d : {Geom::X, Geom::Y}) {
        // Bounds of another size are of other art (hidden art counts in one and not the
        // other), or the page cut them: no telling where the page lies from those.
        if (std::abs(art.dimensions()[d] - bounds.dimensions()[d]) > 0.05) continue;
        double const at = bounds.min()[d] - (art.min()[d] - page.min()[d]);
        if (!std::isfinite(at) || std::abs(at - origin[d]) < 1e-9) continue;
        origin[d] = at;
        moved = true;
    }
    if (!moved) return false;
    auto const placed = Geom::Rect::from_xywh(origin, board.dimensions());
    if (!sane(placed)) return false;
    doc.artboards.front().rect = placed;
    doc.to_doc = Geom::Affine(1, 0, 0, -1, -placed.left(), placed.bottom());
    return true;
}

std::string text_of(std::string_view bytes, bool windows_1252)
{
    if (!windows_1252 && bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xfe && static_cast<unsigned char>(bytes[1]) == 0xff) {
        auto *s = g_convert(bytes.data() + 2, bytes.size() - 2, "UTF-8", "UTF-16BE", nullptr, nullptr, nullptr);
        if (s) {
            std::string out(s);
            g_free(s);
            return out;
        }
    }
    if (!windows_1252 && g_utf8_validate(bytes.data(), bytes.size(), nullptr)) return std::string(bytes);
    auto *s = g_convert(bytes.data(), bytes.size(), "UTF-8", "WINDOWS-1252", nullptr, nullptr, nullptr);
    if (s) {
        std::string out(s);
        g_free(s);
        return out;
    }
    // Windows-1252 leaves five bytes undefined: Latin-1 for those.
    std::string out;
    for (unsigned char c : bytes) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back(static_cast<char>(0xc0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

std::string xml_name(std::string_view id)
{
    // The suffix that makes ids unique: "_" then 20 or more digits, then "_".
    if (id.size() > 2 && id.back() == '_') {
        auto const body = id.substr(0, id.size() - 1);
        auto const at = body.rfind('_');
        if (at != std::string_view::npos) {
            auto const digits = body.substr(at + 1);
            if (digits.size() >= 20 && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                id = body.substr(0, at);
            }
        }
    }
    std::string out;
    std::size_t i = 0;
    while (i < id.size()) {
        if (id[i] == '_') {
            // _xHHHH_: a character.
            if (id.substr(i, 2) == "_x") {
                auto const end = id.find('_', i + 2);
                if (end != std::string_view::npos) {
                    auto const hex = id.substr(i + 2, end - i - 2);
                    if (!hex.empty() && hex.size() <= 6 && std::all_of(hex.begin(), hex.end(), [](char c) { return g_ascii_isxdigit(c); })) {
                        auto const cp = static_cast<gunichar>(std::stoul(std::string(hex), nullptr, 16));
                        if (g_unichar_validate(cp)) {
                            char buf[8];
                            auto const len = g_unichar_to_utf8(cp, buf);
                            out.append(buf, len);
                            i = end + 1;
                            continue;
                        }
                    }
                }
            }
            out.push_back(' ');
            ++i;
        } else {
            out.push_back(id[i]);
            ++i;
        }
    }
    return out;
}

std::optional<Document> read(std::string_view records, std::string &error, Limits const &limits)
{
    Reader r(records, limits);
    auto doc = r.run(error);
    if (!doc) return doc;
    // Type can also be in the objects that looks holding type were kept with.
    auto has_text = [&](auto &&self, std::vector<Node> const &nodes) -> bool {
        for (auto const &n : nodes) {
            if (n.kind == Node::Kind::Text || self(self, n.children) || self(self, n.typed)) return true;
        }
        return false;
    };
    if (has_text(has_text, doc->layers)) doc->texts = read_text_document(records, doc->template_center);
    for (auto &layer : doc->layers) settle_type(layer, doc->texts.get());
    for (auto &pattern : doc->patterns) {
        for (auto &n : pattern.art) settle_type(n, nullptr);
    }
    // Type an appearance drew whose story isn't in the records, and that no typed story
    // could stand in for: there is nothing to say what it sets.
    bool lost = false;
    auto unmatched = [&](auto &&self, std::vector<Node> &nodes, bool shown) -> void {
        for (auto &n : nodes) {
            bool const shows = shown && n.visible;
            if (n.kind == Node::Kind::Text && n.text_proxy &&
                (!n.story || !doc->texts || *n.story >= doc->texts->drawn.size())) {
                lost = lost || shows;
                n.story.reset();
            }
            self(self, n.children, shows);
        }
    };
    unmatched(unmatched, doc->layers, true);
    if (lost) {
        error = "it has type drawn by an appearance whose text isn't in the file, which PROOF doesn't read from it yet";
        return {};
    }
    drop_commented_copies(doc->layers);
    doc->commented_kept = count_commented(doc->layers);
    auto looks = [](auto &&self, std::vector<Node> const &nodes) -> std::size_t {
        std::size_t n = 0;
        for (auto const &k : nodes) n += (k.drawn_look && !k.envelope ? 1 : 0) + self(self, k.children);
        return n;
    };
    doc->drawn_looks = looks(looks, doc->layers);
    auto envelopes = [](auto &&self, std::vector<Node> const &nodes) -> std::size_t {
        std::size_t n = 0;
        for (auto const &k : nodes) n += (k.envelope ? 1 : 0) + self(self, k.children);
        return n;
    };
    doc->envelopes = envelopes(envelopes, doc->layers);
    auto plugins = [&](auto &&self, std::vector<Node> const &nodes) -> void {
        for (auto const &k : nodes) {
            // A look counts as one object, with whatever it holds.
            if (k.drawn_look) continue;
            if (!k.plugin.empty()) ++doc->plugin_objects[k.plugin];
            self(self, k.children);
        }
    };
    plugins(plugins, doc->layers);
    return doc;
}

} // namespace Inkscape::Extension::Internal::AiNative
