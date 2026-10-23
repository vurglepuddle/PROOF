// SPDX-License-Identifier: GPL-2.0-or-later
// PROOF's local CapyPDF overlay: optional CFF metadata must not break subsetting.
#include <cffsubsetter.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

} // namespace
