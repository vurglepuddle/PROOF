// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: open an .ai file from Illustrator's own copy of its art. See ai-native-open.h.
 */
#include "ai-native-open.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <Catalog.h>
#include <Dict.h>
#include <GlobalParams.h>
#include <Object.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Stream.h>
#include <cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <goo/GooString.h>

#ifdef HAVE_POPPLER_CAIRO
#include <glib/poppler-document.h>
#include <glib/poppler-page.h>
#include <glib/poppler.h>
#endif

#include <2geom/rect.h>
#include <2geom/transforms.h>

#include "ai-private-data.h"
#include "display/cairo-utils.h"
#include "display/drawing-context.h"
#include "display/drawing-item.h"
#include "display/drawing.h"
#include "document.h"
#include "extension/internal/ai/ai-native-import.h"
#include "extension/internal/ai/ai-native-reader.h"
#include "io/resource.h"
#include "io/sys.h"
#include "object/sp-image.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "poppler-transition-api.h"
#include "svg/css-ostringstream.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal {
namespace {

/// The most decoded records read. They hold every embedded image uncompressed: a brochure
/// of photographs for print runs to several hundred megabytes from a file of twenty.
constexpr std::size_t RECORDS_LIMIT = std::size_t(2048) << 20;
/// The note of a file opened from its records alone, with no page to weigh them against.
constexpr char const *UNCHECKED_NOTE =
    "it is a PostScript file of Illustrator 8 or older, with no PDF page: its art wasn't checked against a page";

/// Whether the file at `path` is a PDF (any .ai of Illustrator 9 and later).
bool pdf_file(std::string const &path)
{
    auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(Inkscape::IO::fopen_utf8name(path.c_str(), "rb"),
                                                              std::fclose);
    char head[5] = {};
    return file && std::fread(head, 1, sizeof(head), file.get()) == sizeof(head) &&
           std::string_view(head, sizeof(head)) == "%PDF-";
}

/// The longest side, in pixels, of an artboard as it is weighed.
constexpr double LONGEST_SIDE = 600.0;
/// An artboard that differs is weighed again from pictures drawn this many times larger and
/// averaged back down.
constexpr int FINER = 4;
/// From this share of what is allowed, an artboard is weighed that second way as well, and
/// the better of the two counts. One that only just passes at first sight is mostly thin
/// lines too: weighed once, it was reported at nearly the limit where a file a shade worse
/// was reported at next to nothing.
constexpr double FINER_FROM = 0.5;
/// Pixels this far apart (of 255) in lightness, or in any channel, look different.
constexpr int LIGHTNESS_STEP = 48;
constexpr int COLOUR_STEP = 80;
/// Differences worth a note.
constexpr double NOTABLE = 0.002;
/// A page that takes Poppler longer than this many seconds to draw is kept for the next opening
/// of the same file, and when it passes at first sight it isn't drawn again finer: there that
/// changes only the figure reported, and one page of 18 placements took Poppler about 100 s at
/// any size (PROOF-ai-native-records.md, "Pages slow to draw: weighed once and kept").
constexpr double SLOW_PAGE = 10.0;
/// A page's first drawing runs this long alone; then its finer drawing starts beside it
/// (FinerBeside), the page being heavy.
constexpr double FINER_HEAD_START = 3.0;
/// Files whose slow pages an ordinary opening keeps; the folder least lately written goes first.
constexpr std::size_t KEPT_FILES = 64;
/// What a kept page depends on beside the file, Poppler and Cairo: raise it when draw_page or
/// the comparison sizes change.
constexpr int PAGE_STORE_FORMAT = 1;

std::string num(double v)
{
    CSSOStringStream os;
    os << v;
    return os.str();
}

std::string percent(double share)
{
    char buf[32];
    g_snprintf(buf, sizeof buf, "%.1f%%", share * 100.0);
    return buf;
}

/// A picture as seen on white: 8-bit RGB.
struct Picture
{
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgb;
};

std::string checksum(std::string_view data)
{
    auto *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                            reinterpret_cast<guchar const *>(data.data()), data.size());
    std::string out = sum ? sum : "";
    g_free(sum);
    return out;
}

// Cache files are disposable local test artifacts. Check their size and digest before
// reading them as records or pictures; a truncated/corrupt entry must cause a fresh run.
std::optional<std::string> read_cached(std::string const &path, std::size_t limit)
{
    GStatBuf stat;
    if (g_stat(path.c_str(), &stat) || stat.st_size <= 0 || std::uint64_t(stat.st_size) > limit) return {};
    if (g_stat((path + ".sha256").c_str(), &stat) || stat.st_size != 64) return {};
    gchar *data = nullptr, *digest = nullptr;
    gsize size = 0, digest_size = 0;
    bool const ok = g_file_get_contents(path.c_str(), &data, &size, nullptr) &&
                    g_file_get_contents((path + ".sha256").c_str(), &digest, &digest_size, nullptr);
    std::optional<std::string> out;
    if (ok && digest_size == 64 && checksum({data, size}) == std::string_view(digest, digest_size)) {
        out.emplace(data, size);
    }
    g_free(data);
    g_free(digest);
    return out;
}

void write_cached(std::string const &path, std::string_view data)
{
    if (g_file_set_contents(path.c_str(), data.data(), data.size(), nullptr)) {
        auto const digest = checksum(data);
        g_file_set_contents((path + ".sha256").c_str(), digest.data(), digest.size(), nullptr);
    }
}

void write_picture(Picture const &p, std::string const &path)
{
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, p.w, p.h);
    auto *data = cairo_image_surface_get_data(surface);
    auto const stride = cairo_image_surface_get_stride(surface);
    if (data) for (int y = 0; y < p.h; ++y) {
        auto *row = reinterpret_cast<std::uint32_t *>(data + y * stride);
        for (int x = 0; x < p.w; ++x) {
            auto const *rgb = &p.rgb[(std::size_t(y) * p.w + x) * 3];
            row[x] = (std::uint32_t(rgb[0]) << 16) | (std::uint32_t(rgb[1]) << 8) | rgb[2];
        }
    }
    cairo_surface_mark_dirty(surface);
    cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);
}

/// Takes the surface (premultiplied ARGB32) and frees it.
Picture seen_on_white(cairo_surface_t *s)
{
    Picture p;
    cairo_surface_flush(s);
    p.w = cairo_image_surface_get_width(s);
    p.h = cairo_image_surface_get_height(s);
    int const stride = cairo_image_surface_get_stride(s);
    auto const *data = cairo_image_surface_get_data(s);
    bool const opaque = cairo_image_surface_get_format(s) == CAIRO_FORMAT_RGB24;
    p.rgb.resize(std::size_t(p.w) * p.h * 3);
    for (int y = 0; y < p.h && data; ++y) {
        auto const *row = reinterpret_cast<std::uint32_t const *>(data + std::size_t(y) * stride);
        for (int x = 0; x < p.w; ++x) {
            std::uint32_t const px = row[x];
            int const paper = opaque ? 0 : 255 - int(px >> 24);
            auto *out = &p.rgb[(std::size_t(y) * p.w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                out[c] = std::uint8_t(std::min(255, int((px >> (16 - 8 * c)) & 0xff) + paper));
            }
        }
    }
    cairo_surface_destroy(s);
    return p;
}

/// `p` made `factor` times smaller: each pixel the mean of a factor x factor block.
Picture averaged(Picture const &p, int factor)
{
    Picture out;
    out.w = p.w / factor;
    out.h = p.h / factor;
    out.rgb.resize(std::size_t(out.w) * out.h * 3);
    int const n = factor * factor;
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            int sum[3] = {0, 0, 0};
            for (int dy = 0; dy < factor; ++dy) {
                auto const *block = &p.rgb[(std::size_t(y * factor + dy) * p.w + std::size_t(x) * factor) * 3];
                for (int i = 0; i < factor * 3; ++i) sum[i % 3] += block[i];
            }
            auto *pixel = &out.rgb[(std::size_t(y) * out.w + x) * 3];
            for (int c = 0; c < 3; ++c) pixel[c] = std::uint8_t((sum[c] + n / 2) / n);
        }
    }
    return out;
}

std::optional<Picture> read_reference(std::string const &path, int w, int h)
{
    // An RGB picture is tiny at the comparison resolution. Bound compressed input too.
    auto bytes = read_cached(path, 8u << 20);
    if (!bytes) return {};
    // Inspect the PNG IHDR before Cairo allocates the decoded image.
    if (bytes->size() < 24 || bytes->compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) return {};
    auto dimension = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = at; i < at + 4; ++i) value = (value << 8) | std::uint8_t((*bytes)[i]);
        return value;
    };
    if (dimension(16) != std::uint32_t(w) || dimension(20) != std::uint32_t(h)) return {};
    struct Input { std::string_view bytes; std::size_t at = 0; } input{*bytes};
    auto reader = [](void *closure, unsigned char *data, unsigned length) -> cairo_status_t {
        auto &in = *static_cast<Input *>(closure);
        if (length > in.bytes.size() - in.at) return CAIRO_STATUS_READ_ERROR;
        std::copy_n(in.bytes.data() + in.at, length, data);
        in.at += length;
        return CAIRO_STATUS_SUCCESS;
    };
    auto *surface = cairo_image_surface_create_from_png_stream(reader, &input);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_width(surface) != w || cairo_image_surface_get_height(surface) != h ||
        (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_RGB24 &&
         cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)) {
        cairo_surface_destroy(surface);
        return {};
    }
    return seen_on_white(surface);
}

void cache_reference(Picture const &picture, std::string const &path)
{
    write_picture(picture, path);
    gchar *bytes = nullptr;
    gsize size = 0;
    if (g_file_get_contents(path.c_str(), &bytes, &size, nullptr)) {
        auto const digest = checksum({bytes, size});
        g_file_set_contents((path + ".sha256").c_str(), digest.data(), digest.size(), nullptr);
    }
    g_free(bytes);
}

/// SLOW_PAGE, or the test suite's own (PROOF_AI_SLOW_PAGE_SECONDS) so it can try the rule on
/// pages that draw in an instant.
double slow_page()
{
    if (auto const *value = g_getenv("PROOF_AI_SLOW_PAGE_SECONDS")) {
        double const seconds = g_ascii_strtod(value, nullptr);
        if (std::isfinite(seconds) && seconds >= 0) return seconds;
    }
    return SLOW_PAGE;
}

/// FINER_HEAD_START, or the test suite's own (PROOF_AI_FINER_HEAD_START: 0 starts every page's
/// finer drawing beside its first).
double finer_head_start()
{
    if (auto const *value = g_getenv("PROOF_AI_FINER_HEAD_START")) {
        double const seconds = g_ascii_strtod(value, nullptr);
        if (std::isfinite(seconds) && seconds >= 0) return seconds;
    }
    return FINER_HEAD_START;
}

/// The SHA-256 of a file's bytes, read a megabyte at a time; empty when it can't be read.
std::string file_checksum(std::string const &path)
{
    std::unique_ptr<FILE, int (*)(FILE *)> file(g_fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) return {};
    auto *sum = g_checksum_new(G_CHECKSUM_SHA256);
    std::vector<guchar> block(1u << 20);
    for (std::size_t n; (n = std::fread(block.data(), 1, block.size(), file.get())) > 0;) {
        g_checksum_update(sum, block.data(), gssize(n));
    }
    std::string const digest = std::ferror(file.get()) ? "" : g_checksum_get_string(sum);
    g_checksum_free(sum);
    return digest;
}

#ifdef HAVE_POPPLER_CAIRO
/**
 * PROOF: the pages of a file drawn for the comparison, kept for its next opening. The test
 * runner passes a directory of its own, keyed by content, where everything is kept, as before.
 * An ordinary opening keeps only pages slow to draw, in the user's cache: a folder per file,
 * found by its path, size and time, Poppler's and Cairo's versions and PAGE_STORE_FORMAT, and
 * used only while the file's content is still the one it was kept for.
 */
class PageStore
{
public:
    PageStore(std::string const &path, AiNativeCache const *cache)
        : _path(path)
    {
        if (cache && !cache->reference_directory.empty()) {
            _directory = cache->reference_directory;
            _test = true;
            return;
        }
        GStatBuf stat;
        if (g_stat(path.c_str(), &stat)) return;
        auto const key = checksum(path + "|" + std::to_string(stat.st_size) + "|" + std::to_string(stat.st_mtime) +
                                  "|" + poppler_get_version() + "|" + cairo_version_string() + "|" +
                                  std::to_string(PAGE_STORE_FORMAT));
        // The test suite gives a store of its own (the user cache folder is fixed once a process asks).
        auto const *own = g_getenv("PROOF_AI_PAGE_STORE");
        _base = own ? own : IO::Resource::get_path_string(IO::Resource::CACHE, IO::Resource::NONE, "proof-ai-pages");
        _directory = _base + G_DIR_SEPARATOR_S + key.substr(0, 32);
    }

    /// The page drawn at an earlier opening at this size, and the seconds it took to draw.
    std::optional<std::pair<Picture, double>> find(std::string const &name, int w, int h)
    {
        if (_directory.empty() || !g_file_test(_directory.c_str(), G_FILE_TEST_IS_DIR)) return {};
        if (!_test) {
            auto const kept_for = read_cached(_directory + G_DIR_SEPARATOR_S "source", 64);
            if (!kept_for || *kept_for != digest()) return {};
        }
        auto picture = read_reference(_directory + G_DIR_SEPARATOR_S + name + ".png", w, h);
        if (!picture) return {};
        double seconds = 0;
        if (auto const text = read_cached(_directory + G_DIR_SEPARATOR_S + name + ".seconds", 32)) {
            seconds = g_ascii_strtod(text->c_str(), nullptr);
            if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
        }
        return std::pair(std::move(*picture), seconds);
    }

    /// Keep a page that took `seconds` to draw; an ordinary opening keeps slow ones only.
    void keep(std::string const &name, Picture const &picture, double seconds)
    {
        if (_directory.empty() || (!_test && seconds <= slow_page())) return;
        if (!_test && !g_file_test(_directory.c_str(), G_FILE_TEST_IS_DIR)) {
            if (digest().empty()) return;
            make_room();
        }
        g_mkdir_with_parents(_directory.c_str(), 0700);
        if (!_test) write_cached(_directory + G_DIR_SEPARATOR_S "source", digest());
        cache_reference(picture, _directory + G_DIR_SEPARATOR_S + name + ".png");
        char text[32];
        g_ascii_formatd(text, sizeof text, "%.3f", seconds);
        write_cached(_directory + G_DIR_SEPARATOR_S + name + ".seconds", text);
    }

private:
    std::string const &digest()
    {
        if (!_digest) _digest = file_checksum(_path);
        return *_digest;
    }

    /// Let the folders least lately written go until there is room for one more. Only folders
    /// named as these are, and only the files this store writes, are removed.
    void make_room()
    {
        GDir *dir = g_dir_open(_base.c_str(), 0, nullptr);
        if (!dir) return;
        std::vector<std::pair<std::int64_t, std::string>> folders;
        while (auto const *entry = g_dir_read_name(dir)) {
            std::string_view const name = entry;
            if (name.size() != 32 || name.find_first_not_of("0123456789abcdef") != std::string_view::npos) continue;
            auto const folder = _base + G_DIR_SEPARATOR_S + entry;
            GStatBuf stat;
            if (!g_stat(folder.c_str(), &stat) && g_file_test(folder.c_str(), G_FILE_TEST_IS_DIR)) {
                folders.emplace_back(std::int64_t(stat.st_mtime), folder);
            }
        }
        g_dir_close(dir);
        if (folders.size() < KEPT_FILES) return;
        std::sort(folders.begin(), folders.end());
        for (std::size_t i = 0; i + KEPT_FILES <= folders.size(); ++i) {
            auto const &folder = folders[i].second;
            GDir *files = g_dir_open(folder.c_str(), 0, nullptr);
            if (!files) continue;
            std::vector<std::string> names;
            while (auto const *entry = g_dir_read_name(files)) {
                std::string_view const name = entry;
                bool const ours = name.starts_with("page-") || name.starts_with("source");
                if (ours) names.emplace_back(entry);
            }
            g_dir_close(files);
            for (auto const &name : names) g_remove((folder + G_DIR_SEPARATOR_S + name).c_str());
            g_rmdir(folder.c_str());
        }
    }

    std::string _path, _base, _directory;
    bool _test = false;
    std::optional<std::string> _digest;
};
#endif

int lightness(std::uint8_t const *p)
{
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
}

/// Whether pixel (x, y) of `a` looks like some pixel of `b` within one pixel of it.
template <typename Same>
bool matched(Picture const &a, Picture const &b, int x, int y, Same const &same)
{
    auto const *pa = &a.rgb[(std::size_t(y) * a.w + x) * 3];
    for (int dy = -1; dy <= 1; ++dy) {
        int const yy = y + dy;
        if (yy < 0 || yy >= b.h) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            int const xx = x + dx;
            if (xx < 0 || xx >= b.w) continue;
            if (same(pa, &b.rgb[(std::size_t(yy) * b.w + xx) * 3])) return true;
        }
    }
    return false;
}

AiPageDifference weigh(Picture const &a, Picture const &b)
{
    if (std::abs(a.w - b.w) > 2 || std::abs(a.h - b.h) > 2) return {1.0, 1.0};
    int const w = std::min(a.w, b.w), h = std::min(a.h, b.h);
    auto light = [](std::uint8_t const *p, std::uint8_t const *q) {
        return std::abs(lightness(p) - lightness(q)) <= LIGHTNESS_STEP;
    };
    auto colour = [](std::uint8_t const *p, std::uint8_t const *q) {
        return std::abs(p[0] - q[0]) <= COLOUR_STEP && std::abs(p[1] - q[1]) <= COLOUR_STEP &&
               std::abs(p[2] - q[2]) <= COLOUR_STEP;
    };
    std::size_t by_light = 0, by_colour = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Art missing from either side counts: each picture is matched against the other.
            if (!matched(a, b, x, y, light) || !matched(b, a, x, y, light)) ++by_light;
            if (!matched(a, b, x, y, colour) || !matched(b, a, x, y, colour)) ++by_colour;
        }
    }
    double const n = std::max(1.0, double(w) * h);
    return {by_light / n, by_colour / n};
}

/**
 * Paint white, in a picture of `board` at `scale` pixels a point, each of `boxes` (in the
 * document's user units) and two pixels around it: what isn't to be weighed.
 *
 * @return how many pixels that is.
 */
std::size_t leave_out(Picture &p, std::vector<std::array<Geom::Point, 4>> const &boxes, Geom::Rect const &board, double scale)
{
    constexpr double MARGIN = 2.0;
    std::vector<bool> out(std::size_t(p.w) * p.h, false);
    for (auto const &box : boxes) {
        Geom::Point q[4];
        bool sane = true;
        for (int i = 0; i < 4; ++i) {
            q[i] = (box[i] - board.min()) * scale;
            sane = sane && std::isfinite(q[i].x()) && std::isfinite(q[i].y()) && std::abs(q[i].x()) < 1e7 && std::abs(q[i].y()) < 1e7;
        }
        if (!sane) continue;
        double left = q[0].x(), right = q[0].x(), top = q[0].y(), bottom = q[0].y(), area = 0;
        for (int i = 0; i < 4; ++i) {
            left = std::min(left, q[i].x());
            right = std::max(right, q[i].x());
            top = std::min(top, q[i].y());
            bottom = std::max(bottom, q[i].y());
            area += Geom::cross(q[i], q[(i + 1) % 4]);
        }
        double const turn = area < 0 ? -1.0 : 1.0;
        int const x0 = std::max(0, int(std::floor(left - MARGIN))), x1 = std::min(p.w, int(std::ceil(right + MARGIN)));
        int const y0 = std::max(0, int(std::floor(top - MARGIN))), y1 = std::min(p.h, int(std::ceil(bottom + MARGIN)));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                Geom::Point const c(x + 0.5, y + 0.5);
                bool inside = true;
                for (int i = 0; i < 4 && inside; ++i) {
                    auto const edge = q[(i + 1) % 4] - q[i];
                    double const length = edge.length();
                    // A box of no width or height is still its line.
                    if (length < 1e-9) continue;
                    inside = turn * Geom::cross(edge, c - q[i]) / length >= -MARGIN;
                }
                if (inside) out[std::size_t(y) * p.w + x] = true;
            }
        }
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!out[i]) continue;
        ++count;
        p.rgb[i * 3] = p.rgb[i * 3 + 1] = p.rgb[i * 3 + 2] = 255;
    }
    return count;
}

double difference_score(AiPageDifference const &d)
{
    return std::max(d.lightness / AI_NATIVE_MAX_LIGHTNESS, d.colour / AI_NATIVE_MAX_COLOUR);
}

/// The built document, shown once and drawn artboard by artboard.
class NativeView
{
public:
    explicit NativeView(SPDocument *doc)
        : _doc(doc)
        , _key(SPItem::display_key_new(1))
    {
        doc->ensureUpToDate();
        _drawing.setRoot(doc->getRoot()->invoke_show(_drawing, _key, SP_ITEM_SHOW_DISPLAY));
        _drawing.setExact();
    }
    ~NativeView() { _doc->getRoot()->invoke_hide(_key); }
    NativeView(NativeView const &) = delete;
    NativeView &operator=(NativeView const &) = delete;

    /// `board` (user units) at `scale` pixels a point, into a w x h picture. With
    /// `thin_lines_shown`, a line thinner than a pixel is drawn a pixel wide, as Poppler
    /// draws it on a screen.
    Picture draw(Geom::Rect const &board, double scale, int w, int h, bool thin_lines_shown = false)
    {
        _doc->ensureUpToDate();
        // User units are points; the drawing is in the document's pixels.
        auto const px = _doc->getDocumentScale();
        auto const area = board * px;
        _drawing.root()->setTransform(Geom::Translate(-area.min()) * Geom::Scale(scale / px[Geom::X], scale / px[Geom::Y]));
        auto const box = Geom::IntRect::from_xywh(0, 0, w, h);
        _drawing.update(box);
        auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        {
            Inkscape::DrawingContext dc(surface, Geom::Point(0, 0));
            _drawing.render(dc, box, Inkscape::DrawingItem::RENDER_BYPASS_CACHE |
                                         (thin_lines_shown ? Inkscape::DrawingItem::RENDER_VISIBLE_HAIRLINES : 0));
        }
        return seen_on_white(surface);
    }

private:
    SPDocument *_doc;
    unsigned _key;
    Inkscape::Drawing _drawing;
};

/// The layers that don't print, hidden while the view draws.
class NonPrintingHidden
{
public:
    explicit NonPrintingHidden(SPDocument *doc)
    {
        sp_repr_visit_descendants(doc->getReprRoot(), [&](XML::Node *n) {
            auto const print = n->attribute(AiNative::LAYER_PRINT_ATTRIBUTE);
            if (print && std::string_view(print) == "false") {
                auto const style = n->attribute("style");
                _saved.emplace_back(n, style ? std::optional<std::string>(style) : std::nullopt);
                n->setAttribute("style", std::string(style ? style : "") + ";display:none");
                return false;
            }
            return true;
        });
    }
    ~NonPrintingHidden()
    {
        for (auto const &[n, style] : _saved) n->setAttribute("style", style ? style->c_str() : nullptr);
    }
    bool any() const { return !_saved.empty(); }

private:
    std::vector<std::pair<XML::Node *, std::optional<std::string>>> _saved;
};

bool any_non_printing(SPDocument *doc)
{
    bool found = false;
    sp_repr_visit_descendants(doc->getReprRoot(), [&](XML::Node *n) {
        auto const print = n->attribute(AiNative::LAYER_PRINT_ATTRIBUTE);
        found = found || (print && std::string_view(print) == "false");
        return !found;
    });
    return found;
}

#ifdef HAVE_POPPLER_CAIRO
/// The page's trim box at `scale` pixels a point, into a w x h picture.
Picture draw_page(PopplerPage *page, PDFRectangle const &crop, PDFRectangle const &trim, double scale, int w, int h)
{
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    auto *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    // Poppler draws the crop box with its top-left corner at 0,0.
    cairo_translate(cr, -(trim.x1 - crop.x1), -(crop.y2 - trim.y2));
    poppler_page_render_full(page, cr, false, POPPLER_RENDER_ANNOTS_NONE);
    cairo_destroy(cr);
    return seen_on_white(surface);
}

struct Unref
{
    void operator()(gpointer p) const { g_object_unref(p); }
};

/**
 * PROOF: a page's finer drawing, started on a thread of its own once the first drawing has run
 * FINER_HEAD_START seconds. A page that heavy may well need it (the collaborator's 2.7 m page
 * fails at first sight and passes finer), and Poppler can't draw a page faster than its pictures
 * decode, at any size: the two drawings then take the time of one. The thread opens the file
 * in a Poppler document of its own; nothing is shared with the first drawing. A page that
 * draws within the head start never starts it. A page that doesn't need it waits only for
 * the rest of it, begun seconds after the first.
 */
class FinerBeside
{
public:
    FinerBeside(std::string uri, int index, PDFRectangle crop, PDFRectangle trim, double scale, int w, int h)
    {
        _thread = std::thread([this, uri = std::move(uri), index, crop, trim, scale, w, h] {
            {
                std::unique_lock lock(_mutex);
                if (_cv.wait_for(lock, std::chrono::duration<double>(finer_head_start()), [this] { return _first_done; })) {
                    return;
                }
                _started = true;
            }
            auto const start = std::chrono::steady_clock::now();
            std::optional<Picture> picture;
            GError *error = nullptr;
            std::unique_ptr<PopplerDocument, Unref> document(poppler_document_new_from_file(uri.c_str(), nullptr, &error));
            if (error) g_error_free(error);
            if (document) {
                std::unique_ptr<PopplerPage, Unref> page(poppler_document_get_page(document.get(), index));
                if (page) {
                    picture = averaged(draw_page(page.get(), crop, trim, scale * FINER, w * FINER, h * FINER), FINER);
                }
            }
            double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::lock_guard lock(_mutex);
            _picture = std::move(picture);
            _seconds = seconds;
            _finished = true;
            _cv.notify_all();
        });
    }

    FinerBeside(FinerBeside const &) = delete;
    FinerBeside &operator=(FinerBeside const &) = delete;

    ~FinerBeside()
    {
        first_done();
        if (_thread.joinable()) _thread.join();
    }

    /// The first drawing is done: a finer one not yet started never starts.
    void first_done()
    {
        std::lock_guard lock(_mutex);
        _first_done = true;
        _cv.notify_all();
    }

    /// The finer drawing and the seconds it took, waited for if it was started; nothing if it
    /// wasn't (call first_done before).
    std::optional<std::pair<Picture, double>> take()
    {
        std::unique_lock lock(_mutex);
        if (!_started) return {};
        _cv.wait(lock, [this] { return _finished; });
        if (!_picture) return {};
        return std::pair(std::move(*_picture), _seconds);
    }

private:
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _first_done = false, _started = false, _finished = false;
    std::optional<Picture> _picture;
    double _seconds = 0;
};
#endif

std::optional<AiNativeOpen> open_checked(std::string const &path, std::string &reason, AiKeepDiffering keep,
                                         std::string const &diagnostic_directory, AiNativeCache *cache);

/// The largest profile taken from a file. Those in use are under three megabytes.
constexpr std::size_t PROFILE_LIMIT = 32u << 20;

/**
 * The ICC profile a colour space is based on, when it is one for process colours of the
 * document's model: the space itself, or what a spot colour falls back to. A Lab profile
 * has three channels like an RGB one, so the profile's own header says which it is.
 */
std::string icc_profile(Object const &space, bool cmyk, int depth = 0)
{
    if (!space.isArray() || space.arrayGetLength() < 2 || depth > 2) return {};
    auto const kind = space.arrayGet(0, 0);
    if ((kind.isName("Separation") || kind.isName("DeviceN")) && space.arrayGetLength() >= 3) {
        return icc_profile(space.arrayGet(2, 0), cmyk, depth + 1);
    }
    if (!kind.isName("ICCBased")) return {};
    auto block = space.arrayGet(1, 0);
    if (!block.isStream()) return {};
    auto *stream = block.getStream();
    auto const channels = stream->getDict() ? stream->getDict()->lookup("N") : Object();
    if (!channels.isInt() || channels.getInt() != (cmyk ? 4 : 3)) return {};
#if POPPLER_CHECK_VERSION(26, 0, 0)
    if (!stream->rewind()) return {};
#else
    stream->reset();
#endif
    std::string bytes;
    unsigned char buf[65536];
    int got;
    while ((got = stream->doGetChars(sizeof(buf), buf)) > 0) {
        if (bytes.size() + got > PROFILE_LIMIT) {
            bytes.clear();
            break;
        }
        bytes.append(reinterpret_cast<char const *>(buf), got);
    }
    stream->close();
    if (bytes.size() < 132 || bytes.compare(36, 4, "acsp") != 0 || bytes.compare(16, 4, cmyk ? "CMYK" : "RGB ") != 0) return {};
    return bytes;
}

/**
 * The document's own colour profile. Illustrator, saving with the profile embedded, draws
 * every process colour of the page in a colour space based on it. Nothing when the pages
 * name none (the document is then in the working space of whoever opens it), or name more
 * than one, which Illustrator doesn't write and isn't guessed at.
 */
std::string document_profile(PDFDoc &pdf, bool cmyk)
{
    std::string found;
    int const pages = std::min(pdf.getNumPages(), 16);
    for (int number = 1; number <= pages; ++number) {
        auto *page = pdf.getCatalog()->getPage(number);
        auto *resources = page ? page->getResourceDict() : nullptr;
        if (!resources) continue;
        auto const spaces = resources->lookup("ColorSpace");
        if (!spaces.isDict()) continue;
        for (int i = 0; i < spaces.dictGetLength(); ++i) {
            auto profile = icc_profile(spaces.dictGetVal(i), cmyk);
            if (profile.empty()) continue;
            if (found.empty()) {
                found = std::move(profile);
            } else if (found != profile) {
                return {};
            }
        }
    }
    return found;
}

} // namespace

std::optional<AiNativeOpen> open_ai_native(std::string const &path, std::string &reason, AiKeepDiffering keep,
                                           std::string const &diagnostic_directory, AiNativeCache *cache)
{
    // Poppler's glib side, which draws the page, makes Poppler's settings for the first document
    // it opens and drops them when the last one closes. The caller may go on to import the page
    // with Poppler's core, which needs them: without, a file that fell back crashed in the import
    // dialog, reading a font. They are made again.
    struct KeepSettings
    {
        ~KeepSettings()
        {
            if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
        }
    } keep_settings;
    // The records hold every image uncompressed. A machine that runs out of memory over
    // them still has the page to open.
    try {
        return open_checked(path, reason, keep, diagnostic_directory, cache);
    } catch (std::bad_alloc const &) {
        reason = "there isn't enough memory to read its own art";
        return {};
    }
}

namespace {

/// More of a kept picture than this differing from its file, or its shape more than this
/// different, and the file changed since the document was saved. Measured on the collaborator's
/// files (2026-10-16): unchanged links differ in at most 6.4% of their kept picture, one
/// replaced after saving in 25-31% and its shape by 24%.
constexpr double CHANGED_LINK_SHARE = 0.15;
constexpr double CHANGED_LINK_SHAPE = 0.10;

/// Whether `link`'s file, as the document shows it (`picture`), no longer looks like the
/// picture the document keeps of it: both on white, the file drawn at the kept picture's
/// size, each pixel allowed to match any pixel next to it, as the pages are weighed.
bool link_changed(AiNative::FoundLink const &link, Inkscape::Pixbuf const &picture)
{
    int const w = link.preview_width, h = link.preview_height;
    int const iw = picture.width(), ih = picture.height();
    if (w < 1 || h < 1 || iw < 1 || ih < 1 || link.preview.size() < std::size_t(w) * h * 4) return false;
    if (std::abs((double(w) / h) / (double(iw) / ih) - 1.0) > CHANGED_LINK_SHAPE) return true;
    // The file at the kept picture's size, on white: each pixel the mean of a 4 x 4 grid of
    // samples over the part of the file it covers. A picture of thousands of pixels shrinks
    // in a few million reads; averaging every pixel of it took seconds.
    auto *surface = const_cast<cairo_surface_t *>(picture.getSurfaceRaw());
    cairo_surface_flush(surface);
    auto const *source = cairo_image_surface_get_data(surface);
    int const source_stride = cairo_image_surface_get_stride(surface);
    constexpr int GRID = 4;
    std::vector<std::array<int, 3>> small(std::size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::array<int, 3> sum{};
            for (int sy = 0; sy < GRID; ++sy) {
                int const py = std::min(ih - 1, int((y + (sy + 0.5) / GRID) * ih / h));
                auto const *row = reinterpret_cast<std::uint32_t const *>(source + std::size_t(py) * source_stride);
                for (int sx = 0; sx < GRID; ++sx) {
                    auto const px = row[std::min(iw - 1, int((x + (sx + 0.5) / GRID) * iw / w))];
                    int const a = px >> 24;
                    // Premultiplied: on white, a channel is its value and the white left showing.
                    for (int k = 0; k < 3; ++k) sum[k] += int((px >> (16 - 8 * k)) & 0xff) + 255 - a;
                }
            }
            for (int k = 0; k < 3; ++k) small[std::size_t(y) * w + x][k] = (sum[k] + GRID * GRID / 2) / (GRID * GRID);
        }
    }
    auto file_on_white = [&](int x, int y, int k) { return small[std::size_t(y) * w + x][k]; };
    std::size_t differing = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto const *kept = reinterpret_cast<unsigned char const *>(link.preview.data()) + (std::size_t(y) * w + x) * 4;
            int const a = kept[3];
            int best = 255;
            for (int dy = -1; dy <= 1 && best > LIGHTNESS_STEP; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int const nx = std::clamp(x + dx, 0, w - 1), ny = std::clamp(y + dy, 0, h - 1);
                    int most = 0;
                    for (int k = 0; k < 3; ++k) {
                        int const want = (kept[k] * a + 255 * (255 - a) + 127) / 255;
                        most = std::max(most, std::abs(file_on_white(nx, ny, k) - want));
                    }
                    best = std::min(best, most);
                }
            }
            if (best > LIGHTNESS_STEP) ++differing;
        }
    }
    return differing > CHANGED_LINK_SHARE * w * h;
}

/// The picture the document shows for the file `file`, when it found it.
std::shared_ptr<Inkscape::Pixbuf const> shown_picture(SPDocument *doc, std::string const &file)
{
    for (auto *object : doc->getObjectsByElement("image")) {
        auto *image = cast<SPImage>(object);
        if (image && !image->missing && image->pixbuf && image->linkedFile() == file) return image->pixbuf;
    }
    return nullptr;
}

std::optional<AiNativeOpen> open_checked(std::string const &path, std::string &reason, AiKeepDiffering keep,
                                         std::string const &diagnostic_directory, AiNativeCache *cache)
{
    reason.clear();
    if (cache) cache->records_reused = cache->references_reused = 0;
#ifndef HAVE_POPPLER_CAIRO
    reason = "this build can't draw the PDF page to check the art against";
    return {};
#else
    std::string error;
    std::vector<std::pair<std::string, double>> timings;
    auto clock = std::chrono::steady_clock::now();
    auto timed = [&](char const *part) {
        auto const now = std::chrono::steady_clock::now();
        timings.emplace_back(part, std::chrono::duration<double>(now - clock).count());
        clock = now;
    };
    std::optional<AiNativeRecords> records;
    if (cache && !cache->records_directory.empty()) {
        if (auto text = read_cached(cache->records_directory + "/records.txt", RECORDS_LIMIT)) {
            records.emplace();
            records->text = std::move(*text);
            ++cache->records_reused;
        }
    }
    if (!records) {
        records = read_ai_native_records(path, "", RECORDS_LIMIT, error);
        if (records && cache && !cache->records_directory.empty()) {
            g_mkdir_with_parents(cache->records_directory.c_str(), 0700);
            write_cached(cache->records_directory + "/records.txt", records->text);
        }
    }
    if (!records) {
        reason = error.empty() ? "it has no data of Illustrator's own" : error;
        return {};
    }
    // Illustrator 8 and older wrote the file as PostScript, with no PDF part: there is no page
    // to weigh the art against, and nothing else here opens such a file. Its records, read
    // in full, are what opens; what they hold that isn't read still refuses it.
    bool const postscript = !pdf_file(path);
    if (!postscript && records->text.find("%AI5_BeginLayer") == std::string::npos) {
        reason = "its own data has no layers";
        return {};
    }
    timed("records");
    auto ai = AiNative::read(records->text, error);
    records.reset();
    if (!ai) {
        reason = error;
        return {};
    }
    timed("read");
    AiNative::Source source;
    {
        gchar *folder = g_path_get_dirname(path.c_str());
        source.folder = folder;
        g_free(folder);
    }
    if (postscript) {
        auto built = AiNative::build(*ai, error, source);
        if (!built) {
            reason = error;
            return {};
        }
        timed("build");
        AiNativeOpen result;
        result.missing_fonts = built->missing_fonts;
        result.profile = built->profile;
        for (auto const &link : built->missing_links) result.missing_links.push_back(link.name);
        result.notes.push_back(UNCHECKED_NOTE);
        for (auto &n : built->notes) result.notes.push_back(std::move(n));
        result.document = std::move(built->document);
        // Said again when it opens in a window (ui/dialog/font-substitution.cpp).
        result.document->getReprRoot()->setAttribute(AI_UNCHECKED_ATTRIBUTE, "postscript");
        result.timings = std::move(timings);
        return result;
    }
    // The page: Poppler's core for its boxes and the document's profile, its glib side to
    // draw it.
    if (!globalParams) {
        globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    }
    auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
    if (!pdf || !pdf->isOk()) {
        reason = "its PDF part can't be read to check the art against";
        return {};
    }
    source.profile = document_profile(*pdf, ai->cmyk);
    // An artboard the records only let one guess at lies where the page is.
    if (ai->board_guessed) {
        if (auto *first = pdf->getCatalog()->getPage(1)) {
            auto const &trim = first->getTrimBox();
            auto const &art = first->getArtBox();
            AiNative::place_board_by_page(*ai, Geom::Rect(Geom::Point(trim.x1, trim.y1), Geom::Point(trim.x2, trim.y2)),
                                          Geom::Rect(Geom::Point(art.x1, art.y1), Geom::Point(art.x2, art.y2)));
        }
    }
    auto built = AiNative::build(*ai, error, source);
    ai.reset();
    if (!built) {
        reason = error;
        return {};
    }
    timed("build");
    auto *doc = built->document.get();
    auto const &boards = built->artboards;
    GError *gerror = nullptr;
    gchar *absolute = g_canonicalize_filename(path.c_str(), nullptr);
    gchar *uri = g_filename_to_uri(absolute, nullptr, &gerror);
    g_free(absolute);
    std::unique_ptr<PopplerDocument, Unref> pages;
    std::string page_uri; // For a heavy page's finer drawing, in a document of its own.
    if (uri) {
        pages.reset(poppler_document_new_from_file(uri, nullptr, &gerror));
        page_uri = uri;
        g_free(uri);
    }
    if (gerror) {
        g_error_free(gerror);
    }
    if (!pages) {
        reason = "its PDF part can't be drawn to check the art against";
        return {};
    }
    int const page_count = std::min(pdf->getNumPages(), poppler_document_get_n_pages(pages.get()));
    if (boards.empty() || page_count < 1) {
        reason = "it has no artboard to check against its page";
        return {};
    }
    if (boards.size() != std::size_t(page_count)) {
        reason = "its artboard count doesn't match its PDF page count";
        return {};
    }
    std::size_t const compared = boards.size();

    AiNativeOpen result;
    result.missing_fonts = built->missing_fonts;
    result.profile = built->profile;
    // Type set by a font other than its own can't look like the page: where it lies isn't
    // weighed, on either side. Nor is where a linked picture lies whose file is gone.
    auto unweighed = built->substituted_type;
    for (auto const &link : built->missing_links) {
        result.missing_links.push_back(link.name);
        unweighed.push_back(link.box);
    }
    // Nor where a linked picture lies that changed since the file was saved: the page keeps it
    // as it was (Illustrator: "Some of the linked objects have been updated").
    std::set<std::string> changed;
    for (auto const &link : built->found_links) {
        if (auto picture = shown_picture(doc, link.file); picture && link_changed(link, *picture)) {
            unweighed.push_back(link.box);
            changed.insert(link.name);
        }
    }
    double most_unweighed = 0;
    std::size_t most_unweighed_board = 0;
    auto const doc_pages = doc->getPageManager().getPages();
    bool const non_printing = any_non_printing(doc);
    std::optional<NativeView> view;
    view.emplace(doc);
    std::size_t worst = 0;
    if (!diagnostic_directory.empty()) g_mkdir_with_parents(diagnostic_directory.c_str(), 0700);
    PageStore store(path, cache);
    double slowest_once = 0; // The longest a page took that was weighed once, being slow.
    for (std::size_t i = 0; i < compared; ++i) {
        auto *page = pdf->getCatalog()->getPage(int(i) + 1);
        if (!page || page->getRotate() % 360 != 0) {
            reason = "its page " + std::to_string(i + 1) + " is turned or missing";
            return {};
        }
        auto const &crop = page->getCropBox();
        auto const &trim = page->getTrimBox();
        auto const &bleed = page->getBleedBox();
        double const tw = trim.x2 - trim.x1, th = trim.y2 - trim.y1;
        auto const &board = boards[i];
        if (!std::isfinite(tw) || !std::isfinite(th) || tw <= 0 || th <= 0 ||
            std::abs(tw - board.width()) > 1.0 || std::abs(th - board.height()) > 1.0) {
            reason = "its page " + std::to_string(i + 1) + " isn't the size of its artboard";
            return {};
        }
        // The page carries the bleed the artboard prints with.
        double const top = bleed.y2 - trim.y2, right = bleed.x2 - trim.x2;
        double const bottom = trim.y1 - bleed.y1, left = trim.x1 - bleed.x1;
        if (i < doc_pages.size() && std::max({top, right, bottom, left}) > 0.01) {
            doc_pages[i]->getRepr()->setAttribute("inkscape:bleed", num(std::max(0.0, top)) + " " +
                                                                        num(std::max(0.0, right)) + " " +
                                                                        num(std::max(0.0, bottom)) + " " +
                                                                        num(std::max(0.0, left)));
        }

        double const scale = std::min(4.0, LONGEST_SIDE / std::max(tw, th));
        int const w = std::max(1, int(std::ceil(tw * scale - 1e-6)));
        int const h = std::max(1, int(std::ceil(th * scale - 1e-6)));
        std::unique_ptr<PopplerPage, Unref> drawn(poppler_document_get_page(pages.get(), int(i)));
        if (!drawn) {
            reason = "its page " + std::to_string(i + 1) + " can't be drawn";
            return {};
        }
        // The page as drawn at an earlier opening, or drawn now and kept when slow (PageStore).
        auto const page_name = "page-" + std::to_string(i + 1);
        auto const draw_timed = [&](double factor, double &seconds) {
            auto const start = std::chrono::steady_clock::now();
            auto picture = factor == 1 ? draw_page(drawn.get(), crop, trim, scale, w, h)
                                       : averaged(draw_page(drawn.get(), crop, trim, scale * factor,
                                                            int(w * factor), int(h * factor)), int(factor));
            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            return picture;
        };
        Picture page_picture;
        double page_seconds = 0;
        std::optional<FinerBeside> beside;
        if (auto kept = store.find(page_name, w, h)) {
            std::tie(page_picture, page_seconds) = std::move(*kept);
            if (cache) ++cache->references_reused;
            ++result.pages_reused;
        } else {
            // A heavy page's finer drawing starts beside the first (FinerBeside).
            if (!page_uri.empty()) beside.emplace(page_uri, int(i), crop, trim, scale, w, h);
            page_picture = draw_timed(1, page_seconds);
            if (beside) beside->first_done();
            store.keep(page_name, page_picture, page_seconds);
        }
        if (!unweighed.empty()) {
            double const share = double(leave_out(page_picture, unweighed, board, scale)) / std::max(1.0, double(w) * h);
            if (share > most_unweighed) {
                most_unweighed = share;
                most_unweighed_board = i;
            }
        }
        Picture native_picture;
        AiPageDifference d;
        // The art against `page`, each drawn by `draw`; also without the layers that don't
        // print, which a page may leave out.
        auto weigh_art = [&](Picture const &page, auto const &draw_art) {
            auto draw = [&] {
                auto picture = draw_art();
                if (!unweighed.empty()) leave_out(picture, unweighed, board, scale);
                return picture;
            };
            auto picture = draw();
            auto difference = weigh(page, picture);
            if (difference_score(difference) > 1.0 && non_printing) {
                NonPrintingHidden hidden(doc);
                auto hidden_picture = draw();
                auto e = weigh(page, hidden_picture);
                if (difference_score(e) < difference_score(difference)) {
                    difference = e;
                    picture = std::move(hidden_picture);
                }
            }
            return std::pair(difference, std::move(picture));
        };
        std::tie(d, native_picture) = weigh_art(page_picture, [&] { return view->draw(board, scale, w, h); });
        // A page slow to draw that passes at first sight isn't drawn again finer: there that
        // changes only the figure reported, not whether the art opens.
        bool const slow_and_passing = page_seconds > slow_page() && difference_score(d) <= 1.0;
        if (difference_score(d) > FINER_FROM && slow_and_passing) {
            slowest_once = std::max(slowest_once, page_seconds);
        } else if (difference_score(d) > FINER_FROM) {
            // Lines thinner than a pixel are where the two renderers part: Poppler draws them
            // a whole pixel wide and on the pixel grid, the drawing as thin as they are. A
            // page of hairlines then seems to differ everywhere. Drawn FINER times larger and
            // averaged down, with the drawing keeping thin lines a pixel wide as well, both
            // show the same ink for the same line; art that really differs still does.
            auto const fine_name = page_name + "-finer";
            Picture fine_page;
            if (auto kept = store.find(fine_name, w, h)) {
                fine_page = std::move(kept->first);
                ++result.pages_reused;
            } else if (auto drawn_beside = beside ? beside->take() : std::nullopt) {
                fine_page = std::move(drawn_beside->first);
                store.keep(fine_name, fine_page, drawn_beside->second);
            } else {
                double fine_seconds = 0;
                fine_page = draw_timed(FINER, fine_seconds);
                store.keep(fine_name, fine_page, fine_seconds);
            }
            if (!unweighed.empty()) leave_out(fine_page, unweighed, board, scale);
            auto [e, fine_native] = weigh_art(fine_page, [&] {
                return averaged(view->draw(board, scale * FINER, w * FINER, h * FINER, true), FINER);
            });
            if (difference_score(e) < difference_score(d)) {
                d = e;
                page_picture = std::move(fine_page);
                native_picture = std::move(fine_native);
            }
        }
        if (!diagnostic_directory.empty()) {
            auto stem = diagnostic_directory + "/page-" + std::to_string(i + 1);
            write_picture(page_picture, stem + "-pdf.png");
            write_picture(native_picture, stem + "-native.png");
        }
        result.differences.push_back(d);
        if (difference_score(d) > difference_score(result.differences[worst])) worst = i;
        // A differing artboard settles it: the remaining ones needn't be drawn.
        if (difference_score(d) > 1.0 && keep != AiKeepDiffering::All) break;
    }
    view.reset();

    auto const &d = result.differences[worst];
    std::string const where = boards.size() > 1 ? " on artboard " + std::to_string(worst + 1) : "";
    if (d.lightness > AI_NATIVE_MAX_LIGHTNESS || d.colour > AI_NATIVE_MAX_COLOUR) {
        reason = "its own art draws differently from its page" + where + " (" + percent(d.lightness) +
                 " by lightness, " + percent(d.colour) + " by colour)";
        if (keep == AiKeepDiffering::No) return {};
        result.differing = AiArtDiffering{worst, boards.size(), std::max(d.lightness, d.colour)};
    }
    if (std::max(d.lightness, d.colour) > NOTABLE) {
        result.notes.push_back("its own art differs from its page" + where + " in about " +
                               percent(std::max(d.lightness, d.colour)) + " of it");
    }
    if (slowest_once > 0) {
        result.notes.push_back("its page took " + std::to_string(int(std::lround(slowest_once))) +
                               " s to draw, so it was compared once and not again more finely; it is kept for "
                               "the next opening");
    }
    if (!changed.empty()) {
        std::string names;
        for (auto const &name : changed) names += (names.empty() ? "" : ", ") + name;
        result.notes.push_back(std::to_string(changed.size()) +
                               (changed.size() == 1 ? " linked picture looks different from what the document kept of it"
                                                    : " linked pictures look different from what the document kept of them") +
                               " when it was saved (" + names + "): the page shows " +
                               (changed.size() == 1 ? "it" : "them") + " as before, so where " +
                               (changed.size() == 1 ? "it lies" : "they lie") + " wasn't compared with the page");
    }
    if (most_unweighed > 0.25) {
        std::vector<std::string> kinds;
        if (!built->substituted_type.empty()) kinds.emplace_back("type set in another font than its own");
        if (!built->missing_links.empty()) kinds.emplace_back("linked pictures that can't be found");
        if (!changed.empty()) kinds.emplace_back("linked pictures that changed since it was saved");
        std::string what;
        for (std::size_t k = 0; k < kinds.size(); ++k) {
            what += (k == 0 ? "" : k + 1 == kinds.size() ? " or " : ", ") + kinds[k];
        }
        result.notes.push_back("about " + percent(most_unweighed) + " of " +
                               (boards.size() > 1 ? "artboard " + std::to_string(most_unweighed_board + 1) : "the artboard") +
                               " is " + what + ", and wasn't compared with the page");
    }
    for (auto &n : built->notes) result.notes.push_back(std::move(n));
    result.document = std::move(built->document);
    timed("compare");
    result.timings = std::move(timings);
    return result;
#endif
}

} // namespace

} // namespace Inkscape::Extension::Internal
