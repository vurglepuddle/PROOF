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
    std::shared_ptr<Array> arr;
    ObjPtr obj;

    static Value number(double v) { Value x; x.kind = Kind::Num; x.num = v; return x; }
    static Value string(std::string s) { Value x; x.kind = Kind::Str; x.str = std::move(s); return x; }
    static Value name(std::string s) { Value x; x.kind = Kind::Name; x.str = std::move(s); return x; }
    static Value mark() { Value x; x.kind = Kind::Mark; return x; }

    std::optional<double> as_num() const { return kind == Kind::Num ? std::optional(num) : std::nullopt; }
    /// A string's or name's text.
    std::optional<std::string> text() const
    {
        if (kind == Kind::Str) return text_of(str);
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
    int type = 0;
    if (force_rgb) {
        type = *force_rgb ? 1 : 0;
    } else if (after.size() >= 2) {
        type = static_cast<int>(after[1]);
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
/// CMYK, 3 named CMYK, 4 named RGB).
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
        default:
            if (auto n = parse_named(comps, Ink::Kind::Spot, static_cast<int>(style) == 4)) {
                if (n->name == "[Registration]") n->kind = Ink::Kind::Registration;
                return std::pair{n->color, std::optional<Named>(*n)};
            }
            return {};
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
    "Ae", "AE", "Ap", "As", "Xd", "Xr", "XG", "Xh", "XH", "XF", "D", "X=", "X+", "Bc", "Xm", "XP", "Np", "TE",
    "TZ", "Xt", "Xi", "XI", "`", "LB2", "Lc", "Xv", "XV", "Xq", "XQ", "Xg", "Xn", "Bn", "gsave", "grestore",
    "showpage", "annotatepage", "Ar", "An", "XD",
};

bool ignored(std::string_view w)
{
    return std::find(std::begin(IGNORED), std::end(IGNORED), w) != std::end(IGNORED);
}

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

enum class FrameKind { Layer, Group, Compound, Clip, Obj };

struct Frame
{
    FrameKind kind;
    LayerAttrs layer;
    std::shared_ptr<Obj> obj;
    std::vector<Node> children;
    bool hidden = false;     ///< Opened on a %_ line.
    bool after = false;      ///< after_object when it opened.
    GState gs;               ///< The state when it opened.
    std::size_t base = 0;    ///< The operand stack's height when it opened.
    std::vector<std::size_t> clips; ///< The clipping paths' indexes among the children.
};

struct GradientDef
{
    Gradient gradient;
};

struct Instance
{
    bool stroke = false;
    std::optional<std::string> name;
    GradientPlacement placement;
};

class Reader
{
public:
    Reader(std::string_view data, Limits const &limits)
        : _lex(data)
        , _limits(limits)
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
        (shown() ? _unsupported : _hidden_unread).insert(what);
    }

    void warn(std::string const &w)
    {
        if (_doc.warnings.size() < 64 && std::find(_doc.warnings.begin(), _doc.warnings.end(), w) == _doc.warnings.end()) {
            _doc.warnings.push_back(w);
        }
    }

    // comments
    void comment(std::string_view c);
    void skip_to(std::string_view end);

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
    void end_path_quietly();
    void paint(std::string_view op, bool hidden, bool guide);

    // images
    void image(std::string_view data);

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
    std::size_t _points = 0;
    bool _clip_next = false;

    std::optional<Geom::Rect> _cropmarks;
    std::optional<Geom::Rect> _bbox, _hires, _template;
    std::optional<std::pair<double, double>> _art_size, _template_center;
    std::map<std::string, int> _gradient_index;
    std::optional<GradientDef> _gradient_def;
    std::optional<Instance> _instance;
    std::optional<Paint> _pending_fill, _pending_stroke;
    bool _raster = false;
    std::string _raster_space;

    bool _after_object = false;
    std::size_t _nodes = 0;
    std::set<std::string> _unsupported, _unknown, _hidden_unread;
    std::size_t _cmyk_colors = 0, _rgb_colors = 0;
    std::optional<bool> _color_model;
    bool _done = false;
    bool _obj_hidden = false;
    bool _palette = false; ///< In %AI5_BeginPalette: swatches register their inks.
};

std::optional<Document> Reader::run(std::string &error)
{
    try {
        while (auto t = _lex.next()) {
            if (_done) break;
            switch (t->kind) {
                case Token::Kind::Number: push(Value::number(t->number)); break;
                case Token::Kind::String: push(Value::string(std::move(t->string))); break;
                case Token::Kind::Name: push(Value::name(text_of(t->view))); break;
                case Token::Kind::Word: op(t->view, t->hidden); break;
                case Token::Kind::Comment: comment(t->view); break;
                case Token::Kind::Data: image(t->view); break;
            }
        }
        for (auto const &f : _frames) {
            if (f.kind == FrameKind::Layer) fail("it ends inside a layer");
        }
        return finish();
    } catch (Stop const &e) {
        error = e.what();
        return {};
    }
}

// ---- comments --------------------------------------------------------------------------

void Reader::comment(std::string_view c)
{
    while (!c.empty() && (c.back() == '\r' || c.back() == '\n' || c.back() == ' ' || c.back() == '\t')) {
        c.remove_suffix(1);
    }
    auto const m = marker(c);
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
    auto starts = [&](std::string_view p) { return c.substr(0, p.size()) == p; };
    auto value = [&](std::string_view p) { return c.substr(p.size()); };
    if (starts("AI5_BeginLayer")) {
        Frame f;
        f.kind = FrameKind::Layer;
        open(std::move(f));
    } else if (starts("AI5_BeginRaster")) {
        _raster = true;
        _raster_space.clear();
    } else if (starts("AI5_EndRaster")) {
        _raster = false;
    } else if (starts("AI5_BeginPlace")) {
        unreadable("placed files");
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
        if (t->kind != Token::Kind::Comment) continue;
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

void Reader::end_container(std::string_view what, bool hidden)
{
    FrameKind kind = what == "U" ? FrameKind::Group : what == "*U" ? FrameKind::Compound : FrameKind::Clip;
    Frame f = close(std::string(what).c_str(), kind);
    // A compound path of clipping paths clips its clipping group.
    bool const clipping = kind == FrameKind::Compound && !f.clips.empty();
    Node node;
    if (kind == FrameKind::Compound) {
        if (f.children.empty()) return;
        auto const &first = f.children.front();
        node = new_node(Node::Kind::Compound, f.gs);
        node.fill = first.fill;
        node.stroke = first.stroke;
        node.stroke_style = first.stroke_style;
        node.overprint_fill = first.overprint_fill;
        node.overprint_stroke = first.overprint_stroke;
        node.transparency = first.transparency;
        node.evenodd = first.evenodd;
        node.clipping = clipping;
        for (auto &k : f.children) {
            k.transparency = {};
            k.visible = true;
            k.locked = false;
        }
        node.children = std::move(f.children);
    } else {
        // A group with a clipping path (q ... Q, or a W path in a group) is a clipping group.
        auto [children, clip] = clip_first(std::move(f.children), f.clips);
        node = new_node(Node::Kind::Group, f.gs);
        node.children = std::move(children);
        node.clipped = clip;
    }
    bool const commented = f.hidden || hidden;
    if (clipping && !_frames.empty()) _frames.back().clips.push_back(_frames.back().children.size());
    add(std::move(node), commented);
}

std::pair<std::vector<Node>, bool> Reader::clip_first(std::vector<Node> kids, std::vector<std::size_t> const &clips)
{
    std::vector<Node> clip, rest;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        bool const is_clip = std::find(clips.begin(), clips.end(), i) != clips.end();
        (is_clip ? clip : rest).push_back(std::move(kids[i]));
    }
    if (clip.empty()) return {std::move(rest), false};
    std::vector<Node> out;
    if (clip.size() == 1) {
        out.push_back(std::move(clip.front()));
    } else {
        Node compound;
        compound.kind = Node::Kind::Compound;
        compound.clipping = true;
        compound.evenodd = clip.front().evenodd;
        compound.children = std::move(clip);
        out.push_back(std::move(compound));
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
    } else {
        _doc.layers.push_back(std::move(layer));
        _after_object = false;
    }
}

// ---- operators ---------------------------------------------------------------------------

void Reader::op(std::string_view w, bool hidden)
{
    if (w == "[") {
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
            // Binary data in ASCII85 follows, up to its ~>.
            if (f.obj->type == "Binary" && !f.obj->entries.empty() && f.obj->entries.back().first == "ASCII85Decode") {
                _lex.skip_past("~>");
            }
        }
        return;
    }
    if (w == ";") {
        end_obj();
        return;
    }
    // What follows an object (its name, transparency and style) ends with anything else.
    if (w != "Xy" && w != "Xd" && w != "XW" && w != "BB") _after_object = false;
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
        if (w == "Bs") {
            gradient_stop(vals);
        } else if (w == "BD") {
            auto def = std::move(*_gradient_def);
            _gradient_def.reset();
            if (!def.gradient.stops.empty()) {
                std::stable_sort(def.gradient.stops.begin(), def.gradient.stops.end(),
                                 [](auto const &a, auto const &b) { return a.offset < b.offset; });
                _gradient_index[def.gradient.name] = static_cast<int>(_doc.gradients.size());
                _doc.gradients.push_back(std::move(def.gradient));
            }
        }
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
        unreadable("pattern fills");
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
    } else if (w == "Bm") {
        if (_instance && nums.size() >= 6) {
            auto const *n = nums.data() + nums.size() - 6;
            Geom::Affine m(n[0], n[1], n[2], n[3], n[4], n[5]);
            bool finite = true;
            for (int i = 0; i < 6; ++i) finite = finite && std::isfinite(m[i]);
            if (finite && std::abs(m.det()) > 1e-12) _instance->placement.bm = m;
        }
    } else if (w == "Bh") {
        if (_instance && nums.size() >= 2) _instance->placement.hilight = {nums[0], nums[1]};
    } else if (w == "BB") {
        if (_instance) {
            auto i = std::move(*_instance);
            _instance.reset();
            set_instance(i);
        }
    }
    // Images.
    else if (w == "XN") {
        if (_raster) _raster_space = last_text(vals).value_or("");
    }
    // Type in the legacy format.
    else if (w == "To" || w == "TO" || w == "Tp" || w == "TP" || w == "Tx" || w == "TX" || w == "Tj" || w == "Tk") {
        unreadable("type in the legacy format");
    } else if (ignored(w)) {
    } else if (_frames.empty()) {
        // Before the art, the setup runs procedures of the printing format.
    } else {
        std::string const op(w.substr(0, 24));
        if (!shown()) {
            if (_hidden_unread.size() < 16) _hidden_unread.insert("`" + op + "`");
        } else if (_unknown.size() < 16) {
            _unknown.insert(op);
        }
    }
}

void Reader::begin_obj(bool hidden)
{
    auto obj = std::make_shared<Obj>();
    if (!_stack.empty() && _stack.back().kind == Value::Kind::Name) {
        obj->type = _stack.back().str;
        _stack.pop_back();
    }
    Frame f;
    f.kind = FrameKind::Obj;
    f.obj = std::move(obj);
    f.hidden = hidden;
    open(std::move(f));
    // A dictionary's state starts afresh (styles set theirs inside).
    _gs = GState();
}

void Reader::end_obj()
{
    if (_frames.empty() || _frames.back().kind != FrameKind::Obj) return;
    Frame f = std::move(_frames.back());
    _frames.pop_back();
    close_obj(*f.obj, _stack, f.base);
    _obj_hidden = f.hidden;
    // Art made inside a dictionary (type's paths, a mask's art) isn't kept.
    _gs = f.gs;
    _after_object = f.after;
    auto const &type = f.obj->type;
    if (type == "Mask") {
        // An opacity mask: its art is in the dictionary. Disabled ones change nothing.
        auto const disabled = f.obj->nums("Disabled");
        if (disabled.empty() || disabled.back() != 1.0) unreadable("opacity masks");
    }
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

void Reader::text_slot(Obj const &o)
{
    auto n = new_node(Node::Kind::Text, _gs);
    auto const story = o.nums("StoryIndex");
    if (!story.empty() && story.front() >= 0 && story.front() < 4294967295.0) n.story = static_cast<unsigned>(story.front());
    add(std::move(n), _obj_hidden);
}

void Reader::art_dictionary(Obj const &o)
{
    auto *n = last_object();
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
    double const index = n.size() > 7 ? n[7] : 0.0;
    auto rgb = [&](std::size_t i) { return i < n.size() ? static_cast<int>(std::clamp(n[i], 0.0, 255.0)) : 0; };
    if (index >= 0 && index < 27 && index == std::floor(index)) {
        a.options.color_index = static_cast<int>(index);
    } else {
        a.options.color_index = -1;
    }
    a.options.color_rgb = {rgb(8), rgb(9), rgb(10)};
}

int Reader::ink_index(Named const &n, bool by_name)
{
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
    // After a group or compound path closes, it is the group's.
    if (auto *obj = last_object(); obj && (obj->kind == Node::Kind::Group || obj->kind == Node::Kind::Compound)) {
        obj->transparency = t;
    }
}

/// Remove the children read from %_ lines.
void strip_commented(Node &n)
{
    auto &c = n.children;
    c.erase(std::remove_if(c.begin(), c.end(), [](Node const &k) { return k.commented; }), c.end());
    for (auto &k : c) strip_commented(k);
}

void Reader::style_marker(Values const &vals, bool hidden)
{
    auto const nums = nums_of(vals);
    double const code = nums.empty() ? 0.0 : nums.back();
    auto const style = last_text(vals).value_or("");
    bool const after = _after_object;
    _after_object = false;
    // `1 (style) XW` after the object written on %_ lines; its drawn look comes before it.
    if (hidden || code != 1.0 || style.empty() || !after || _frames.empty()) return;
    auto &kids = _frames.back().children;
    if (kids.size() < 2) return;
    auto &object = kids[kids.size() - 1];
    auto &look = kids[kids.size() - 2];
    if (!object.commented || look.commented || look.kind != Node::Kind::Group || look.clipped) return;
    Node obj = std::move(object);
    kids.pop_back();
    auto &l = kids.back();
    strip_commented(l);
    if (!obj.name.empty()) l.name = obj.name;
    l.visible = obj.visible;
    l.locked = obj.locked;
    l.box_rotation = obj.box_rotation;
    l.drawn_look = true;
    ++_doc.drawn_looks;
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
        if (_current) _path.push_back(std::move(*_current));
        _current = Geom::Path(*a);
        return;
    }
    if (!_current) return;
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
}

void Reader::end_path_quietly()
{
    _path.clear();
    _current.reset();
    _points = 0;
    _clip_next = false;
}

void Reader::paint(std::string_view op, bool hidden, bool guide)
{
    if (_current) _path.push_back(std::move(*_current));
    _current.reset();
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
    // Lower-case painting closes the path.
    if (!op.empty() && op[0] >= 'a' && op[0] <= 'z' && !pv.back().closed()) pv.back().close(true);
    char const k = op.empty() ? 'n' : static_cast<char>(g_ascii_tolower(op[0]));
    bool const do_fill = k == 'f' || k == 'b';
    bool const do_stroke = k == 's' || k == 'b';
    auto node = new_node(Node::Kind::Path, _gs);
    node.path = std::move(pv);
    node.evenodd = _gs.evenodd;
    node.clipping = clip;
    node.guide = guide;
    // W clips the following artwork; it doesn't cancel this path's own paint.
    if (do_fill) node.fill = fill_paint ? fill_paint : _gs.fill;
    if (do_stroke) node.stroke = stroke_paint ? stroke_paint : _gs.stroke;
    node.stroke_style = _gs.style;
    node.overprint_fill = _gs.overprint_fill && do_fill;
    node.overprint_stroke = _gs.overprint_stroke && do_stroke;
    node.transparency = _gs.transparency;
    if (clip && !_frames.empty()) _frames.back().clips.push_back(_frames.back().children.size());
    add(std::move(node), hidden);
}

// ---- images ------------------------------------------------------------------------------

void Reader::image(std::string_view data)
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
    add(std::move(node), false);
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
    auto collect = [&](auto &&self, std::vector<Node> const &nodes) -> void {
        for (auto const &n : nodes) {
            if (n.kind == Node::Kind::Text) {
                if (n.story && !n.commented) plain.insert(*n.story);
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
                                       return n.kind == Node::Kind::Text && n.commented && n.story &&
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
    if (!_unknown.empty()) {
        std::string ops;
        for (auto const &o : _unknown) ops += (ops.empty() ? "`" : ", `") + o + "`";
        _unsupported.insert("operators PROOF doesn't know (" + ops + ")");
    }
    if (!_unsupported.empty()) fail("it has " + join_and(_unsupported) + ", which PROOF doesn't read from it yet");
    if (_doc.layers.empty()) fail("it has no layers");
    drop_commented_copies(_doc.layers);
    _doc.commented_kept = count_commented(_doc.layers);

    std::optional<Geom::Rect> first;
    if (!_doc.artboards.empty()) first = _doc.artboards.front().rect;
    if (!first) first = _cropmarks;
    if (!first) first = sized_board();
    Geom::Point const top_left = first ? Geom::Point(first->left(), first->bottom()) : Geom::Point(0, 792);
    _doc.to_doc = Geom::Affine(1, 0, 0, -1, -top_left.x(), top_left.y());
    if (_doc.artboards.empty()) {
        if (auto r = _cropmarks ? _cropmarks : sized_board()) _doc.artboards.push_back({"Artboard 1", *r});
    }
    _doc.bbox = _hires ? _hires : _bbox;
    if (_template_center) _doc.template_center = Geom::Point(_template_center->first, _template_center->second);
    _doc.cmyk = _color_model.value_or(_cmyk_colors > _rgb_colors);
    for (auto const &h : _hidden_unread) _doc.left_out.push_back(h);
    return std::move(_doc);
}

} // namespace

// ---- public ------------------------------------------------------------------------------

std::string text_of(std::string_view bytes)
{
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xfe && static_cast<unsigned char>(bytes[1]) == 0xff) {
        auto *s = g_convert(bytes.data() + 2, bytes.size() - 2, "UTF-8", "UTF-16BE", nullptr, nullptr, nullptr);
        if (s) {
            std::string out(s);
            g_free(s);
            return out;
        }
    }
    if (g_utf8_validate(bytes.data(), bytes.size(), nullptr)) return std::string(bytes);
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
    if (doc) {
        auto has_text = [&](auto &&self, std::vector<Node> const &nodes) -> bool {
            for (auto const &n : nodes) if (n.kind == Node::Kind::Text || self(self, n.children)) return true;
            return false;
        };
        if (has_text(has_text, doc->layers)) doc->texts = read_text_document(records, doc->template_center);
    }
    return doc;
}

} // namespace Inkscape::Extension::Internal::AiNative
