// SPDX-License-Identifier: GPL-2.0-or-later
// Regression tests for PROOF's local CapyPDF overlay.
#include <cffsubsetter.hpp>
#include <capypdf.hpp>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace capypdf::internal;

namespace {

constexpr std::array metadata = {DictOperator::Notice, DictOperator::FullName,
                                DictOperator::FamilyName, DictOperator::Weight,
                                DictOperator::FontBBox};

struct SyntheticFont {
    std::string name = "PROOF-CFF-Test";
    // .notdef, two used Type 2 programs and an unused glyph; no installed font is needed.
    std::array<std::byte, 1> notdef = {std::byte{14}};
    std::array<std::byte, 4> first = {std::byte{139}, std::byte{139}, std::byte{21}, std::byte{14}};
    std::array<std::byte, 4> second = {std::byte{149}, std::byte{159}, std::byte{21}, std::byte{14}};
    std::array<std::byte, 4> unused = {std::byte{169}, std::byte{179}, std::byte{21}, std::byte{14}};
    CFFont font{};

    SyntheticFont(unsigned mask, bool cid)
    {
        font.name.entries.emplace_back(reinterpret_cast<std::byte *>(name.data()), name.size());
        font.char_strings.entries = {notdef, first, second, unused};
        // A nonempty Private DICT, so the round trip exercises its offset too.
        font.pdict.entries.entries.push_back({{600}, DictOperator::DefaultWidthX});
        append_ros_strings(font); // The two strings also serve the synthetic CID font.
        font.is_cid = cid;
        for (unsigned i = 0; i < metadata.size(); ++i) {
            if (mask & (1u << i)) {
                std::vector<int32_t> operands = metadata[i] == DictOperator::FontBBox
                    ? std::vector<int32_t>{0, 0, 500, 700} : std::vector<int32_t>{0};
                font.top_dict.entries.push_back({std::move(operands), metadata[i]});
            }
        }
        if (cid) {
            font.top_dict.entries.push_back({{391, 392, 0}, DictOperator::ROS});
            font.fdarray.push_back({{}, font.pdict});
            font.fdselect.push_back({0, 0});
            // CIDFontVersion and CIDCount intentionally absent: both have defaults.
        }
    }
};

CFFDictItem const *find(CFFont const &font, DictOperator op)
{
    auto const &entries = font.top_dict.entries;
    auto it = std::find_if(entries.begin(), entries.end(), [op](auto const &e) { return e.opr == op; });
    return it == entries.end() ? nullptr : &*it;
}

void check_subset(unsigned mask, bool cid)
{
    SCOPED_TRACE(::testing::Message() << "metadata mask " << mask << ", CID " << cid);
    SyntheticFont source(mask, cid);
    // Reorder glyphs as well as subsetting, so a merely copied font cannot pass.
    std::vector<SubsetGlyphs> glyphs{{0, 0}, {'B', 2}, {'A', 1}};
    CFFWriter writer(source.font, glyphs);
    ASSERT_TRUE(writer.create().has_value());
    auto output = writer.steal();
    auto parsed = parse_cff_data(std::move(output));
    ASSERT_TRUE(parsed.has_value()) << static_cast<int>(parsed.error());
    ASSERT_EQ(parsed->char_strings.size(), 3);
    auto equals = [](auto a, auto b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); };
    EXPECT_TRUE(equals(parsed->char_strings.entries[0], source.notdef));
    EXPECT_TRUE(equals(parsed->char_strings.entries[1], source.second));
    EXPECT_TRUE(equals(parsed->char_strings.entries[2], source.first));
    ASSERT_EQ(parsed->fdarray.size(), 1);
    ASSERT_TRUE(parsed->fdarray[0].priv.has_value());
    EXPECT_EQ(parsed->fdarray[0].priv->entries.entries.front().operand, std::vector<int32_t>{600});
    for (unsigned i = 0; i < metadata.size(); ++i) {
        EXPECT_EQ(find(*parsed, metadata[i]) != nullptr, (mask & (1u << i)) != 0);
    }
    auto count = find(*parsed, DictOperator::CIDCount);
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->operand, std::vector<int32_t>{3});
}

TEST(CffSubsetter, EveryCombinationOfOptionalNameMetadata)
{
    for (unsigned mask = 0; mask < (1u << metadata.size()); ++mask) {
        check_subset(mask, false);
    }
}

TEST(CffSubsetter, CidFontsMayOmitOptionalMetadataAndUseDefaultVersionAndCount)
{
    for (unsigned mask = 0; mask < (1u << metadata.size()); ++mask) {
        check_subset(mask, true);
    }
}

TEST(CffSubsetter, MissingRequiredRosIsReportedByCreate)
{
    SyntheticFont source(31, true);
    auto &entries = source.font.top_dict.entries;
    std::erase_if(entries, [](auto const &e) { return e.opr == DictOperator::ROS; });
    std::vector<SubsetGlyphs> glyphs{{0, 0}, {'A', 1}};
    CFFWriter writer(source.font, glyphs);
    auto result = writer.create();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::MissingCFFDictItem);
}

TEST(CapyPdfOverlay, GraphicsStateOverprintKeys)
{
    // This target already links the patched CapyPDF. Keep the generated PDF for
    // inspection, in an atomically created directory that cannot overwrite input.
    auto const dir = std::filesystem::path(::testing::TempDir()) /
        ("proof-capypdf-overprint-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(dir));
    auto const path = dir / "states.pdf";
    SCOPED_TRACE(path.string());
    auto const utf8 = path.u8string();
    {
        capypdf::DocumentProperties properties;
        capypdf::Generator pdf(reinterpret_cast<char const *>(utf8.c_str()), properties);
        auto page = pdf.new_page_context();
        for (int fill = 0; fill != 2; ++fill) {
            capypdf::GraphicsState state;
            state.set_op(fill);
            state.set_OP(1 - fill);
            state.set_OPM(fill);
            page.cmd_gs(pdf.add_graphics_state(state));
            page.cmd_re(10 + 20 * fill, 10, 10, 10);
            page.cmd_f();
        }
        pdf.add_page(page);
        pdf.write();
    }
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    std::string const bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ASSERT_TRUE(bytes.starts_with("%PDF-"));
    EXPECT_EQ(bytes.find("/op2"), std::string::npos);

    // CapyPDF compresses the dictionaries into one object stream. Inspect its
    // decoded bytes as well: checking compressed file bytes alone proves nothing.
    auto const object_stream = bytes.find("/Type /ObjStm");
    ASSERT_NE(object_stream, std::string::npos);
    auto const length_at = bytes.find("/Length ", object_stream);
    ASSERT_NE(length_at, std::string::npos);
    auto const size = std::stoul(bytes.substr(length_at + 8));
    auto const stream_at = bytes.find("stream\n", length_at);
    ASSERT_NE(stream_at, std::string::npos);
    auto const data_at = stream_at + 7;
    ASSERT_LE(size, bytes.size() - data_at);
    std::array<unsigned char, 65536> decoded;
    uLongf decoded_size = decoded.size();
    ASSERT_EQ(uncompress(decoded.data(), &decoded_size,
                         reinterpret_cast<Bytef const *>(bytes.data() + data_at), size), Z_OK);
    std::string const objects(reinterpret_cast<char const *>(decoded.data()), decoded_size);
    EXPECT_EQ(objects.find("/op2"), std::string::npos);
    std::size_t next = 0;
    for (int fill = 0; fill != 2; ++fill) {
        auto const state_at = objects.find("/Type /ExtGState", next);
        ASSERT_NE(state_at, std::string::npos);
        auto const end = objects.find(">>", state_at);
        ASSERT_NE(end, std::string::npos);
        auto const state = objects.substr(state_at, end - state_at);
        EXPECT_NE(state.find(fill ? "/op true" : "/op false"), std::string::npos);
        EXPECT_NE(state.find(fill ? "/OP false" : "/OP true"), std::string::npos);
        EXPECT_NE(state.find(fill ? "/OPM 1" : "/OPM 0"), std::string::npos);
        next = end + 2;
    }
    EXPECT_EQ(objects.find("/Type /ExtGState", next), std::string::npos);
}

} // namespace
