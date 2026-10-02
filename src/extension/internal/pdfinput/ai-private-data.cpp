// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: read Adobe Illustrator's native records from an .ai file. See ai-private-data.h.
 */
#include "ai-private-data.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include <glib.h>
#include <zlib.h>
#include <zstd.h>

#include <Object.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Stream.h>
#include <goo/GooString.h>

#include "io/sys.h"
#include "poppler-transition-api.h"

namespace Inkscape::Extension::Internal {
namespace {

constexpr std::string_view ZSTD_MARKER = "%AI24_ZStandard_Data";
constexpr std::string_view ZLIB_MARKER = "%AI12_CompressedData";
constexpr std::size_t MAX_COMPRESSED = 512u * 1024 * 1024;

/// Append decoded bytes and report whether the stop marker has now been produced.
bool append_and_check(AiNativeRecords &out, char const *data, std::size_t size, std::string_view stop_marker)
{
    auto const before = out.text.size();
    out.text.append(data, size);
    if (stop_marker.empty()) {
        return false;
    }
    // Search only the new bytes plus enough overlap to catch a marker split across chunks.
    auto const from = before > stop_marker.size() ? before - stop_marker.size() : 0;
    return out.text.find(stop_marker, from) != std::string::npos;
}

/// Skip whitespace between a compression marker and its payload.
std::size_t payload_start(std::string const &data, std::size_t after_marker)
{
    while (after_marker < data.size() && g_ascii_isspace(data[after_marker])) {
        ++after_marker;
    }
    return after_marker;
}

bool decode_zstd(std::string const &data, std::size_t start, AiNativeRecords &out, std::string_view stop_marker,
                 std::size_t limit, std::string &error)
{
    auto stream = std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)>(ZSTD_createDStream(), ZSTD_freeDStream);
    if (!stream) {
        error = "Zstandard decoder unavailable";
        return false;
    }
    ZSTD_initDStream(stream.get());
    std::vector<char> buffer(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in{data.data() + start, data.size() - start, 0};
    while (in.pos < in.size) {
        ZSTD_outBuffer chunk{buffer.data(), buffer.size(), 0};
        auto const result = ZSTD_decompressStream(stream.get(), &chunk, &in);
        if (ZSTD_isError(result)) {
            error = std::string("Zstandard data is damaged: ") + ZSTD_getErrorName(result);
            return false;
        }
        if (out.text.size() + chunk.pos > limit) {
            error = "Illustrator data exceeds the size limit";
            return false;
        }
        if (append_and_check(out, buffer.data(), chunk.pos, stop_marker)) {
            out.stopped_early = true;
            return true;
        }
        if (result == 0) {
            break; // Illustrator writes one frame; ignore any padding after it
        }
    }
    return true;
}

bool decode_zlib(std::string const &data, std::size_t start, AiNativeRecords &out, std::string_view stop_marker,
                 std::size_t limit, std::string &error)
{
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        error = "zlib decoder unavailable";
        return false;
    }
    std::vector<char> buffer(256 * 1024);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data() + start));
    zs.avail_in = static_cast<uInt>(data.size() - start);
    int status = Z_OK;
    while (status == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef *>(buffer.data());
        zs.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&zs, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&zs);
            error = "zlib data is damaged";
            return false;
        }
        auto const produced = buffer.size() - zs.avail_out;
        if (out.text.size() + produced > limit) {
            inflateEnd(&zs);
            error = "Illustrator data exceeds the size limit";
            return false;
        }
        if (append_and_check(out, buffer.data(), produced, stop_marker)) {
            out.stopped_early = true;
            break;
        }
        if (produced == 0 && zs.avail_in == 0) {
            break;
        }
    }
    inflateEnd(&zs);
    return true;
}

/// Join page 1's AIPrivateDataN (or AIPDFPrivateDataN) streams in numeric order.
std::optional<std::string> pdf_private_data(std::string const &path, std::string &error)
{
    auto doc = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
    if (!doc || !doc->isOk() || doc->getNumPages() < 1) {
        error = "not a readable PDF-based Illustrator file";
        return {};
    }
    auto page = doc->getPage(1);
    auto piece_info = page ? page->getPieceInfo() : nullptr;
    if (!piece_info) {
        error = "no Illustrator data (saved without PDF compatibility, or not from Illustrator)";
        return {};
    }
    auto illustrator = piece_info->lookup("Illustrator");
    if (!illustrator.isDict()) {
        error = "no Illustrator data in this PDF";
        return {};
    }
    auto priv = illustrator.getDict()->lookup("Private");
    if (!priv.isDict()) {
        error = "no Illustrator private data in this PDF";
        return {};
    }
    for (auto prefix : {"AIPrivateData", "AIPDFPrivateData"}) {
        std::string joined;
        int found = 0;
        for (int index = 1; index <= 100000; ++index) {
            auto block = priv.getDict()->lookup(std::string(prefix) + std::to_string(index));
            if (!block.isStream()) {
                break;
            }
            auto stream = block.getStream();
#if POPPLER_CHECK_VERSION(26, 0, 0)
            if (!stream->rewind()) {
                error = "Illustrator data block could not be read";
                return {};
            }
#else
            stream->reset();
#endif
            unsigned char buf[65536];
            int got;
            while ((got = stream->doGetChars(sizeof(buf), buf)) > 0) {
                if (joined.size() + got > MAX_COMPRESSED) {
                    error = "Illustrator data exceeds the size limit";
                    return {};
                }
                joined.append(reinterpret_cast<char const *>(buf), got);
            }
            stream->close();
            ++found;
        }
        if (found) {
            return joined;
        }
    }
    error = "no Illustrator data blocks found";
    return {};
}

} // namespace

std::optional<AiNativeRecords> read_ai_native_records(std::string const &path, std::string_view stop_marker,
                                                      std::size_t limit, std::string &error)
{
    // UTF-8 safe open: plain std::ifstream cannot open non-ASCII paths on Windows.
    auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(Inkscape::IO::fopen_utf8name(path.c_str(), "rb"),
                                                              std::fclose);
    char head[8] = {};
    if (!file || std::fread(head, 1, sizeof(head), file.get()) != sizeof(head)) {
        error = "file could not be read";
        return {};
    }
    AiNativeRecords out;

    if (std::string_view(head, 5) != "%PDF-") {
        // Illustrator 8 and older: the file is PostScript records.
        if (std::string_view(head, 4) != "%!PS") {
            error = "not an Illustrator file";
            return {};
        }
        out.text.assign(head, sizeof(head));
        std::vector<char> buffer(256 * 1024);
        std::size_t got;
        while ((got = std::fread(buffer.data(), 1, buffer.size(), file.get())) > 0) {
            if (out.text.size() + got > limit) {
                error = "Illustrator data exceeds the size limit";
                return {};
            }
            if (append_and_check(out, buffer.data(), got, stop_marker)) {
                out.stopped_early = true;
                break;
            }
        }
        out.encoding = "PostScript";
        return out;
    }
    file.reset();

    auto data = pdf_private_data(path, error);
    if (!data) {
        return {};
    }
    if (data->compare(0, ZSTD_MARKER.size(), ZSTD_MARKER) == 0) {
        out.encoding = "AI24 Zstandard";
        if (!decode_zstd(*data, payload_start(*data, ZSTD_MARKER.size()), out, stop_marker, limit, error)) {
            return {};
        }
    } else if (data->compare(0, ZLIB_MARKER.size(), ZLIB_MARKER) == 0) {
        out.encoding = "AI12 zlib";
        if (!decode_zlib(*data, payload_start(*data, ZLIB_MARKER.size()), out, stop_marker, limit, error)) {
            return {};
        }
    } else {
        if (data->size() > limit) {
            error = "Illustrator data exceeds the size limit";
            return {};
        }
        out.encoding = "plain";
        out.text = std::move(*data);
    }
    return out;
}

} // namespace Inkscape::Extension::Internal
