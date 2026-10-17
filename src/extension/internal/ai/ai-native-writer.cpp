// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Illustrator's own records for a PROOF document. See ai-native-writer.h.
 *
 * The record forms are the ones Illustrator 29.2.1 opened as editable art in PROOF's spikes
 * (tools/native-ai-spike.py, 2026-10-02; the writer spike, 2026-10-17), in the coordinates
 * of the writer recipe (doc/PROOF-ai-interchange.md): the first artboard's top-left corner
 * is 0,0, y negative downward, in points.
 */

#include "ai-native-writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include <glib.h>
#include <2geom/bezier-curve.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>

#include "colors/color.h"
#include "colors/document-colors.h"
#include "colors/spaces/base.h"
#include "document.h"
#include "helper/geom.h"
#include "object/sp-clippath.h"
#include "object/sp-gradient.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-page.h"
#include "object/sp-pattern.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "spot-ink.h"
#include "style.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;

constexpr double PX_TO_PT = 0.75;

/// A number as Illustrator writes it: up to four decimals, no trailing zeros.
std::string num(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

/// A PostScript string: parentheses and backslashes escaped, other bytes outside ASCII in octal.
std::string ps_string(std::string const &s)
{
    std::string out = "(";
    for (unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            out += std::format("\\{:03o}", c);
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + ")";
}

double scale24(unsigned value)
{
    return value / double(SP_SCALE24_MAX);
}

class Writer
{
public:
    explicit Writer(SPDocument *doc)
        : _doc(doc)
    {}

    WrittenRecords write();

private:
    void layer(SPGroup const *group);
    void item(SPItem const *item);
    void contents(SPObject const *parent);
    void shape(SPShape const *shape);
    bool open_clip(SPItem const *item);
    void path_data(Geom::Path const &path);
    void state(std::string &cache, std::string const &line);
    std::optional<std::string> paint(SPIPaint const &p, SPStyle const *style, bool fill);
    std::optional<std::string> colour(Colors::Color const &c, bool fill);
    void refuse(std::string what) { _refused.insert(std::move(what)); }

    SPDocument *_doc;
    Geom::Affine _to_art;
    std::ostringstream _body;
    std::set<std::string> _refused;
    std::map<std::string, std::array<double, 4>> _spots;
    std::size_t _layers = 0;
    // What has been set already, so each operator is written only when it changes.
    std::string _fill_over, _fill, _stroke_over, _stroke, _stroke_style, _rule;
};

std::optional<std::string> Writer::colour(Colors::Color const &c, bool fill)
{
    if (c.getOpacity() < 1.0 - 1e-6) {
        refuse("transparency");
        return {};
    }
    if (c.getSpace() && c.getSpace()->getComponentType() == Type::CMYK) {
        return std::format("{} {} {} {} {}", num(c[0]), num(c[1]), num(c[2]), num(c[3]), fill ? "k" : "K");
    }
    if (auto rgb = c.converted(Type::RGB)) {
        return std::format("{} {} {} {}", num((*rgb)[0]), num((*rgb)[1]), num((*rgb)[2]), fill ? "Xa" : "XA");
    }
    refuse("colours that are neither CMYK nor RGB");
    return {};
}

/// The operator that sets this paint, or nothing for none (and for what is refused).
std::optional<std::string> Writer::paint(SPIPaint const &p, SPStyle const *style, bool fill)
{
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL || p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
        refuse("context paint");
        return {};
    }
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CURRENT_COLOR) return colour(style->color.getColor(), fill);
    if (p.isNone()) return {};
    if (p.isColor()) return colour(p.getColor(), fill);
    auto server = p.isPaintserver() && p.href ? p.href->getObject() : nullptr;
    auto swatch = cast<SPGradient>(server);
    if (swatch && swatch->isSolid()) {
        auto ink = SpotInk::read(swatch->getRepr());
        if (!ink && swatch->getVector()) ink = SpotInk::read(swatch->getVector()->getRepr());
        if (!ink) return colour(const_cast<SPGradient *>(swatch)->getPreviewAverageColor(), fill);
        auto const &a = ink->alternate;
        if (!a.getSpace() || a.getSpace()->getComponentType() != Type::CMYK) {
            refuse("spot inks defined in RGB or Lab");
            return {};
        }
        std::array<double, 4> cmyk{a[0], a[1], a[2], a[3]};
        auto [known, added] = _spots.emplace(ink->name, cmyk);
        if (!added && known->second != cmyk) {
            refuse("one ink name with two definitions");
            return {};
        }
        auto const head = std::format("{} {} {} {} {}", num(cmyk[0]), num(cmyk[1]), num(cmyk[2]), num(cmyk[3]),
                                      ps_string(ink->name));
        // The operand is the tint taken from 1: 0 is the ink at full strength.
        if (ink->tint >= 1.0 - 1e-6) return head + " 0 " + (fill ? "x" : "X");
        return head + " " + num(1.0 - ink->tint) + " 0 " + (fill ? "Xx" : "XX");
    }
    refuse(is<SPMeshGradient>(server) ? "mesh gradients" : is<SPPattern>(server) ? "patterns" : "gradients");
    return {};
}

void Writer::state(std::string &cache, std::string const &line)
{
    if (cache == line) return;
    cache = line;
    _body << line << '\n';
}

void Writer::path_data(Geom::Path const &path)
{
    auto const start = path.initialPoint();
    _body << num(start.x()) << ' ' << num(start.y()) << " m\n";
    auto segment = [&](Geom::Curve const &c) {
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&c)) {
            _body << std::format("{} {} {} {} {} {} C\n", num((*cubic)[1].x()), num((*cubic)[1].y()),
                                 num((*cubic)[2].x()), num((*cubic)[2].y()), num((*cubic)[3].x()),
                                 num((*cubic)[3].y()));
        } else {
            _body << num(c.finalPoint().x()) << ' ' << num(c.finalPoint().y()) << " L\n";
        }
    };
    for (auto const &c : path) segment(c);
    // A closed path ends on its start point, so that its last corner stays a corner.
    if (path.closed() && Geom::distance(path.finalPoint(), start) > 1e-4) segment(path.closingSegment());
}

/// Opens a clipping group for the item's clipping path: `q`, the path, `W n`. Whether it did.
bool Writer::open_clip(SPItem const *item)
{
    auto clip = item->getClipObject();
    if (!clip) return false;
    std::vector<SPShape const *> shapes;
    for (auto const &child : clip->children) {
        if (auto s = cast<SPShape>(&child)) {
            shapes.push_back(s);
        } else if (is<SPItem>(&child)) {
            shapes.clear();
            break;
        }
    }
    if (clip->clippath_units() || shapes.size() != 1 || !shapes[0]->curve()) {
        refuse("clipping paths other than one path");
        return false;
    }
    auto const pv = pathv_to_linear_and_cubic_beziers(*shapes[0]->curve() * shapes[0]->transform *
                                                      item->i2doc_affine() * _to_art);
    if (pv.size() != 1) {
        refuse("clipping paths other than one path");
        return false;
    }
    _body << "q\n";
    path_data(pv[0]);
    _body << "h\nW\nn\n";
    return true;
}

void Writer::shape(SPShape const *s)
{
    auto const *style = s->style;
    if (!s->curve() || s->curve()->empty()) return;
    if (s->hasMarkers()) refuse("markers");
    auto fill = paint(style->fill, style, true);
    auto stroke = style->stroke_width.computed > 0 ? paint(style->stroke, style, false) : std::nullopt;
    if (scale24(style->fill_opacity.value) < 1.0 - 1e-6 && fill) refuse("transparency");
    if (scale24(style->stroke_opacity.value) < 1.0 - 1e-6 && stroke) refuse("transparency");

    auto const affine = s->i2doc_affine() * _to_art;
    if (stroke) {
        if (style->vector_effect.stroke) refuse("non-scaling strokes");
        if (!affine.withoutTranslation().preservesAngles(1e-6)) refuse("strokes on stretched or skewed objects");
        for (auto const layer : style->paint_order.layer) {
            if (layer == SP_CSS_PAINT_ORDER_FILL) break;
            if (layer == SP_CSS_PAINT_ORDER_STROKE && fill) {
                refuse("strokes painted under their fill");
                break;
            }
        }
    }
    if (!_refused.empty()) return;

    auto const overprint = s->getAttribute("proof:overprint") ? std::string(s->getAttribute("proof:overprint")) : "";
    state(_fill_over, overprint.find("fill") != std::string::npos ? "1 O" : "0 O");
    if (fill) state(_fill, *fill);
    state(_stroke_over, overprint.find("stroke") != std::string::npos ? "1 R" : "0 R");
    if (stroke) {
        state(_stroke, *stroke);
        double const scale = std::sqrt(std::abs(affine.det()));
        std::string dash = "[";
        for (auto const &d : style->stroke_dasharray.values) {
            dash += (dash.size() > 1 ? " " : "") + num(d.computed * scale);
        }
        dash += "]" + num(style->stroke_dashoffset.computed * scale) + " d";
        int const cap = style->stroke_linecap.computed == SP_STROKE_LINECAP_ROUND    ? 1
                        : style->stroke_linecap.computed == SP_STROKE_LINECAP_SQUARE ? 2 : 0;
        int const join = style->stroke_linejoin.computed == SP_STROKE_LINEJOIN_ROUND   ? 1
                         : style->stroke_linejoin.computed == SP_STROKE_LINEJOIN_BEVEL ? 2 : 0;
        state(_stroke_style, std::format("{} w {} J {} j {} M {}", num(style->stroke_width.computed * scale), cap,
                                         join, num(style->stroke_miterlimit.value), dash));
    }
    state(_rule, style->fill_rule.computed == SP_WIND_RULE_EVENODD ? "1 XR" : "0 XR");

    auto const pv = pathv_to_linear_and_cubic_beziers(*s->curve() * affine);
    auto op = [&](Geom::Path const &p) {
        char c = fill && stroke ? 'b' : fill ? 'f' : stroke ? 's' : 'n';
        return p.closed() ? c : static_cast<char>(g_ascii_toupper(c));
    };
    if (pv.size() == 1) {
        path_data(pv[0]);
        _body << op(pv[0]) << '\n';
        return;
    }
    _body << "*u\n";
    for (auto const &p : pv) {
        path_data(p);
        _body << op(p) << '\n';
    }
    _body << "*U\n";
}

void Writer::item(SPItem const *it)
{
    auto const *style = it->style;
    if (scale24(style->opacity.value) < 1.0 - 1e-6) refuse("transparency");
    if (style->mix_blend_mode.set && style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) refuse("blend modes");
    if (it->getMaskObject()) refuse("opacity masks");
    if (const_cast<SPStyle *>(style)->getFilter()) refuse("filters");

    // Only the object's own: a locked layer's objects aren't each locked.
    bool const hidden = it->isHidden();
    bool const locked = it->getAttribute("sodipodi:insensitive") != nullptr;
    if (hidden) _body << "1 Xw\n";
    if (locked) _body << "1 A\n";
    bool const clipped = open_clip(it);

    if (auto use = cast<SPUse>(it)) {
        // A clone is written as a copy of what it shows.
        if (use->child) item(use->child);
    } else if (auto group = cast<SPGroup>(it)) {
        if (!clipped) _body << "u\n";
        contents(group);
        if (!clipped) _body << "U\n";
    } else if (auto s = cast<SPShape>(it)) {
        shape(s);
    } else if (is<SPImage>(it)) {
        refuse("pictures");
    } else {
        refuse("type");
    }

    if (clipped) _body << "Q\n";
    if (locked) _body << "0 A\n";
    if (hidden) _body << "0 Xw\n";
}

void Writer::contents(SPObject const *parent)
{
    for (auto const &child : parent->children) {
        auto it = cast<SPItem>(&child);
        if (!it) continue;
        auto group = cast<SPGroup>(it);
        if (group && group->isLayer()) {
            layer(group);
        } else {
            item(it);
        }
    }
}

void Writer::layer(SPGroup const *group)
{
    ++_layers;
    auto attr = [&](char const *name) {
        auto v = group->getAttribute(name);
        return v ? std::string(v) : std::string();
    };
    int r = 79, g = 128, b = 255;
    auto const color = attr("inkscape:highlight-color");
    if (color.size() >= 7 && color[0] == '#') {
        auto hex = [&](std::size_t at) { return static_cast<int>(std::strtol(color.substr(at, 2).c_str(), nullptr, 16)); };
        r = hex(1), g = hex(3), b = hex(5);
    }
    // Illustrator shows a layer in its preset colour by index, whatever RGB follows it. The
    // presets as the owner's files pair them (census, 2026-10-17; 4 to 6 vary between files and
    // are left out): the nearest one.
    static constexpr std::array<std::array<int, 4>, 18> presets{{
        {0, 79, 128, 255}, {1, 255, 79, 79},   {2, 79, 255, 79},   {3, 79, 79, 255},  {7, 128, 128, 128},
        {8, 0, 0, 0},      {9, 255, 102, 0},   {10, 0, 85, 0},     {11, 0, 153, 153}, {12, 204, 153, 102},
        {13, 153, 51, 0},  {14, 153, 51, 255}, {15, 255, 153, 0},  {16, 0, 0, 136},   {17, 255, 153, 204},
        {19, 153, 0, 0},   {21, 255, 153, 153}, {0, 79, 128, 255}}};
    auto const nearest = *std::min_element(presets.begin(), presets.end(), [&](auto const &p, auto const &q) {
        auto d = [&](auto const &c) { return (c[1] - r) * (c[1] - r) + (c[2] - g) * (c[2] - g) + (c[3] - b) * (c[3] - b); };
        return d(p) < d(q);
    });
    auto const dim = attr("proof:layer-dim");
    // visible, preview, unlocked, printing, dimmed, 0, 1, colour index, its RGB, 0, dim %, 0: as
    // Illustrator 29 writes it (`1 1 1 1 0 0 1 0 79 128 255 0 50 0 Lb`).
    _body << "%AI5_BeginLayer\n"
          << std::format("{} {} {} {} {} 0 1 {} {} {} {} 0 {} 0 Lb\n", group->isHidden() ? 0 : 1,
                         attr("proof:layer-preview") == "false" ? 0 : 1, attr("sodipodi:insensitive").empty() ? 1 : 0,
                         attr("proof:layer-print") == "false" ? 0 : 1, dim.empty() ? 0 : 1, nearest[0], nearest[1],
                         nearest[2], nearest[3], dim.empty() ? std::string("50") : dim)
          << ps_string(group->label() ? group->label() : group->defaultLabel()) << " Ln\n0 A\n0 Xw\n";
    if (group->style && scale24(group->style->opacity.value) < 1.0 - 1e-6) refuse("transparency");
    bool const clipped = open_clip(group);
    contents(group);
    if (clipped) _body << "Q\n";
    _body << "LB\n%AI5_EndLayer--\n";
}

WrittenRecords Writer::write()
{
    // Shapes such as rectangles have their path only once the document is up to date.
    _doc->ensureUpToDate();
    // Artboards: the document's pages, or its own size when it has none.
    std::vector<std::pair<std::string, Geom::Rect>> boards;
    for (auto const *page : _doc->getPageManager().getPages()) {
        boards.emplace_back(page->getLabel(), page->getDocumentRect());
    }
    if (boards.empty()) boards.emplace_back("Artboard 1", Geom::Rect(Geom::Point(0, 0), _doc->getDimensions()));
    auto const origin = boards.front().second.corner(0);
    _to_art = Geom::Translate(-origin) * Geom::Scale(PX_TO_PT, -PX_TO_PT);

    // Items outside any layer go to a layer of their own, as Illustrator keeps every object in one.
    auto const *root = _doc->getRoot();
    bool loose = false;
    for (auto const &child : root->children) {
        auto group = cast<SPGroup>(&child);
        if (is<SPItem>(&child) && !(group && group->isLayer())) loose = true;
    }
    for (auto const &child : root->children) {
        auto group = cast<SPGroup>(&child);
        if (group && group->isLayer()) layer(group);
    }
    if (loose) {
        ++_layers;
        _body << "%AI5_BeginLayer\n1 1 1 1 0 0 1 0 79 128 255 0 50 0 Lb\n(Layer) Ln\n0 A\n0 Xw\n";
        for (auto const &child : root->children) {
            auto group = cast<SPGroup>(&child);
            if (auto it = cast<SPItem>(&child); it && !(group && group->isLayer())) item(it);
        }
        _body << "LB\n%AI5_EndLayer--\n";
    }

    WrittenRecords out;
    if (!_refused.empty()) {
        out.refused.assign(_refused.begin(), _refused.end());
        return out;
    }

    // The document's extent from the first artboard's top-left, and Illustrator's canvas around it.
    Geom::Rect extent = boards.front().second;
    for (auto const &[name, rect] : boards) extent.unionWith(rect);
    double const w = (extent.right() - origin.x()) * PX_TO_PT, h = (extent.bottom() - origin.y()) * PX_TO_PT;
    double const cx = w / 2 + 0.5, cy = -h / 2 - 0.5;
    bool const cmyk = Colors::DocumentColors::mode(_doc) == Type::CMYK;

    std::ostringstream head;
    head << "%!PS-Adobe-3.0 \n%%Creator: PROOF\n%%AI8_CreatorVersion: 29.2.1\n%%For: () ()\n"
         << "%%Title: " << ps_string(_doc->getDocumentName() ? _doc->getDocumentName() : "Untitled") << '\n'
         << "%%Canvassize: 16383\n"
         << std::format("%%BoundingBox: 0 {} {} 0\n%%HiResBoundingBox: 0 {} {} 0\n", num(std::floor(-h)),
                        num(std::ceil(w)), num(-h), num(w))
         << "%%DocumentProcessColors: Cyan Magenta Yellow Black\n";
    bool first = true;
    for (auto const &[name, c] : _spots) {
        head << (first ? "%%DocumentCustomColors: " : "%%+ ") << ps_string(name) << '\n';
        first = false;
    }
    first = true;
    for (auto const &[name, c] : _spots) {
        head << (first ? "%%CMYKCustomColor: " : "%%+ ")
             << std::format("{} {} {} {} {}\n", num(c[0]), num(c[1]), num(c[2]), num(c[3]), ps_string(name));
        first = false;
    }
    head << "%AI5_FileFormat 14.0\n%AI12_BuildNumber: 116\n%AI3_ColorUsage: Color\n"
         << std::format("%AI3_Cropmarks: 0 {} {} 0\n", num(-h), num(w))
         << std::format("%AI3_TemplateBox: {} {} {} {}\n", num(cx), num(cy), num(cx), num(cy))
         << std::format("%AI3_TileBox: 0 {} {} 0\n", num(-h), num(w))
         << "%AI3_DocumentPreview: None\n%AI5_ArtSize: 14400 14400\n%AI5_RulerUnits: 2\n"
         << "%AI24_LargeCanvasScale: 1\n"
         << "%AI9_ColorModel: " << (cmyk ? 2 : 1) << '\n'
         << "%AI5_ArtFlags: 0 0 0 1 0 0 1 0 0\n%AI5_TargetResolution: 800\n"
         << "%AI5_NumLayers: " << _layers << '\n';
    // Where Illustrator's window looks when the file opens: the first artboard, fitted. Without
    // it the window opened on empty canvas (the owner, 2026-10-17). Its top-left corner in art
    // space, the zoom, then a 1910 by 914 window and constants as Illustrator 29 writes them.
    auto const &first_board = boards.front().second;
    double const bw = first_board.width() * PX_TO_PT, bh = first_board.height() * PX_TO_PT;
    double const zoom = std::clamp(0.9 * std::min(1910 / std::max(bw, 1.0), 914 / std::max(bh, 1.0)), 0.03125, 64.0);
    auto const view = std::format("{} {} {}", num(bw / 2 - 1910 / (2 * zoom)), num(-bh / 2 + 914 / (2 * zoom)), num(zoom));
    head << "%AI17_Begin_Content_if_version_gt:24 4\n"
         << "%AI10_OpenToVie: " << view << " 0 8191 8191 1910 914 18 0 0 5 121 0 0 0 1 1 0 1 1 0 1\n"
         << "%AI17_Alternate_Content\n"
         << "%AI9_OpenToView: " << view << " 1910 914 18 0 0 5 121 0 0 0 1 1 0 1 1 0 1\n"
         // No %AI5_OpenViewLayers: it holds view states per layer that override `Lb`'s, and a
         // copied "7" moved the hidden and locked flags onto other layers.
         << "%AI17_End_Versioned_Content\n"
         << std::format("%%PageOrigin:0 {}\n", num(-h)) << "%%EndComments\n"
         << "%%BeginProlog\n%%EndProlog\n%%BeginSetup\n";

    // The artboards, as the writer recipe's document data.
    head << "%AI9_BeginDocumentData\n%_/Document :\n%_/Dictionary :\n%_0 /Int (CropAreaActive) ,\n%_/Array :\n";
    for (auto const &[name, rect] : boards) {
        double const l = (rect.left() - origin.x()) * PX_TO_PT, t = -(rect.top() - origin.y()) * PX_TO_PT;
        double const r = (rect.right() - origin.x()) * PX_TO_PT, bot = -(rect.bottom() - origin.y()) * PX_TO_PT;
        auto uuid = g_uuid_string_random();
        head << "%_/Dictionary :\n"
             << std::format("%_{} {} /RealPointRelToROrigin\n%_ (PositionPoint1) ,\n", num(l), num(t))
             << std::format("%_{} {} /RealPointRelToROrigin\n%_ (PositionPoint2) ,\n", num(r), num(bot))
             << "%_0 /Bool (IsArtboardDefaultName) ,\n%_0 /Bool (IsArtboardSelected) ,\n%_0 /Int (DisplayMark) ,\n"
             << std::format("%_{} {} /RealPoint\n%_ (RulerOrigin) ,\n", num(std::floor(8191.5 - cx + l)),
                            num(std::floor(8191.5 + cy - t)))
             << "%_(" << uuid << ") /String (ArtboardUUID) ,\n%_1 /Real (PAR) ,\n"
             << "%_" << ps_string(name) << " /UnicodeString (Name) ,\n%_; ,\n";
        g_free(uuid);
    }
    head << "%_; (ArtboardArray) ,\n%_; /Recorded ,\n%_;\n%AI9_EndDocumentData\n%%EndSetup\n";

    out.records = head.str() + _body.str() +
                  "%%PageTrailer\ngsave annotatepage grestore showpage\n%%Trailer\n%%EOF\n";
    return out;
}

} // namespace

WrittenRecords write_records(SPDocument *document)
{
    return Writer(document).write();
}

} // namespace Inkscape::Extension::Internal::AiNative
