# SPDX-License-Identifier: GPL-2.0-or-later
# PROOF's small overlay for the pinned CapyPDF: select PDF 1.7 per document, and
# open and write files by their full Unicode names on Windows, and subset CFF
# fonts without assuming optional metadata entries are present. Serialize the
# TrueType maxp payload without the C++ variant's discriminator/padding.
# Keep the submodule untouched, and fail on an upstream change needing review.
set(CAPYPDF_PROOF_DIR "${CMAKE_CURRENT_BINARY_DIR}/capypdf-proof")
file(MAKE_DIRECTORY "${CAPYPDF_PROOF_DIR}")

# Replace one exact, unique passage of a CapyPDF source in its PROOF copy.
# Several calls for the same file apply in turn.
function(proof_capypdf_overlay name needle replacement)
    get_property(overlaid GLOBAL PROPERTY PROOF_CAPYPDF_OVERLAID)
    if(name IN_LIST overlaid)
        set(source "${CAPYPDF_PROOF_DIR}/${name}")
    else()
        set(source "${CMAKE_CURRENT_LIST_DIR}/capypdf/src/${name}")
        set_property(GLOBAL APPEND PROPERTY PROOF_CAPYPDF_OVERLAID "${name}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    endif()
    file(READ "${source}" text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(FIND "${text}" "${needle}" first)
    string(FIND "${text}" "${needle}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "Review PROOF's CapyPDF overlay: ${name} changed")
    endif()
    string(REPLACE "${needle}" "${replacement}" text "${text}")
    file(CONFIGURE OUTPUT "${CAPYPDF_PROOF_DIR}/${name}" CONTENT "${text}" @ONLY)
endfunction()

proof_capypdf_overlay(ft_subsetter.hpp
    [==[    void set_num_glyphs(uint16_t glyph_count);
    void swap_endian();]==]
    [==[    void set_num_glyphs(uint16_t glyph_count);
    void swap_endian();
    std::span<const std::byte> serialized_data() const;]==])
proof_capypdf_overlay(ft_subsetter.cpp
    [==[void TTMaxp::swap_endian() {
    std::visit([](auto &d) { d.swap_endian(); }, data);
}]==]
    [==[void TTMaxp::swap_endian() {
    std::visit([](auto &d) { d.swap_endian(); }, data);
}

std::span<const std::byte> TTMaxp::serialized_data() const {
    return std::visit([](auto const &d) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte *>(&d), sizeof(d));
    }, data);
}]==])
proof_capypdf_overlay(ft_subsetter.cpp
    [==[odata, "maxp", std::span<const std::byte>((const std::byte *)&tf.maxp, sizeof(tf.maxp)))]==]
    [==[odata, "maxp", tf.maxp.serialized_data())]==])

proof_capypdf_overlay(document.hpp
    "std::variant<std::monostate, CapyPDF_PDFX_Type, CapyPDF_PDFA_Type> subtype;"
    "std::variant<std::monostate, CapyPDF_PDFX_Type, CapyPDF_PDFA_Type> subtype;\n    // PROOF: explicit compatibility for ordinary documents; PDF/X and PDF/A retain their versions.\n    std::optional<PdfVersion> pdf_version;")
proof_capypdf_overlay(document.cpp
    "return PdfVersion::v20;\n}\n\nbool DocumentProperties::use_rdf_metadata()"
    "return pdf_version.value_or(PdfVersion::v20);\n}\n\nbool DocumentProperties::use_rdf_metadata()")

# Unicode file names on Windows. Names arrive as UTF-8, as GLib gives them; the
# narrow C runtime and the -A Windows calls read them in the ANSI code page, so
# a file or folder named in Cyrillic, Hebrew, Chinese, with emoji or with an
# umlaut outside that code page could not be written or read (2026-10-18).
proof_capypdf_overlay(utils.hpp
    [==[#include <cstdio>
]==]
    [==[#include <cstdio>
#include <filesystem>
]==])
proof_capypdf_overlay(utils.hpp
    [==[rvoe<std::string> load_file_as_string(const char *fname);]==]
    [==[// PROOF: open by a UTF-8 name, with the wide calls on Windows.
std::filesystem::path utf8_path(const char *fname);
FILE *path_fopen(const std::filesystem::path &path, const char *mode);
FILE *utf8_fopen(const char *fname, const char *mode);

rvoe<std::string> load_file_as_string(const char *fname);]==])

proof_capypdf_overlay(utils.cpp
    [==[rvoe<std::string> load_file_as_string(const char *fname) {
    FILE *f = fopen(fname, "rb");]==]
    [==[std::filesystem::path utf8_path(const char *fname) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(fname)));
}

FILE *path_fopen(const std::filesystem::path &path, const char *mode) {
#ifdef _WIN32
    std::wstring wide_mode(mode, mode + strlen(mode));
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

FILE *utf8_fopen(const char *fname, const char *mode) { return path_fopen(utf8_path(fname), mode); }

rvoe<std::string> load_file_as_string(const char *fname) {
    FILE *f = utf8_fopen(fname, "rb");]==])
proof_capypdf_overlay(utils.cpp
    [==[    FILE *f = fopen(fname.c_str(), "rb");]==]
    [==[    FILE *f = utf8_fopen(fname.c_str(), "rb");]==])
proof_capypdf_overlay(utils.cpp
    [==[    FILE *f = fopen(ofname, "wb");]==]
    [==[    FILE *f = utf8_fopen(ofname, "wb");]==])

proof_capypdf_overlay(imagefileops.cpp
    [==[rvoe<RasterImage> load_png_file(const char *fname) {
    FILE *f = fopen(fname, "rb");]==]
    [==[rvoe<RasterImage> load_png_file(const char *fname) {
    FILE *f = utf8_fopen(fname, "rb");]==])
proof_capypdf_overlay(imagefileops.cpp
    [==[    TIFF *tif = TIFFOpen(fname, "rb");]==]
    [==[#ifdef _WIN32
    TIFF *tif = TIFFOpenW(utf8_path(fname).c_str(), "rb");
#else
    TIFF *tif = TIFFOpen(fname, "rb");
#endif]==])
proof_capypdf_overlay(imagefileops.cpp
    [==[rvoe<jpg_image> load_jpg_file(const char *fname) {
    FILE *f = fopen(fname, "rb");]==]
    [==[rvoe<jpg_image> load_jpg_file(const char *fname) {
    FILE *f = utf8_fopen(fname, "rb");]==])
proof_capypdf_overlay(imagefileops.cpp
    [==[    std::filesystem::path fobj(fname);]==]
    [==[    auto fobj = utf8_path(fname);]==])
proof_capypdf_overlay(imagefileops.cpp
    [==[    FILE *f = fopen(fname, "rb");
    auto rc = fread(buf, 1, bufsize, f);]==]
    [==[    FILE *f = utf8_fopen(fname, "rb");
    if(!f) {
        RETERR(CouldNotOpenFile);
    }
    auto rc = fread(buf, 1, bufsize, f);]==])

proof_capypdf_overlay(mmapper.cpp
    [==[    HANDLE file_handle = CreateFile(fname,]==]
    [==[    HANDLE file_handle = CreateFileW(utf8_path(fname).c_str(),]==])

proof_capypdf_overlay(pdfwriter.cpp
    [==[    std::filesystem::path tempfname(ofilename);
    tempfname.replace_extension(".pdf~");
    FILE *out_file = fopen(tempfname.string().c_str(), "wb");]==]
    [==[    auto const target = utf8_path(ofilename);
    auto tempfname = target;
    tempfname.replace_extension(".pdf~");
    FILE *out_file = path_fopen(tempfname, "wb");]==])
proof_capypdf_overlay(pdfwriter.cpp
    [==[    std::filesystem::rename(tempfname, ofilename, ec);
    if(ec) {]==]
    [==[    std::filesystem::rename(tempfname, target, ec);
    if(ec) {
        std::error_code ignored;
        std::filesystem::remove(tempfname, ignored);]==])

proof_capypdf_overlay(capi.cpp
    [==[    std::filesystem::path fspath(path);
    auto pathless_name = fspath.filename().string();]==]
    [==[    auto const fspath = utf8_path(path);
    auto const u8name = fspath.filename().u8string();
    std::string pathless_name(reinterpret_cast<const char *>(u8name.data()), u8name.size());]==])

# FreeType opens a named font through the ANSI code page on Windows. Map and
# parse the file first (UTF-8 aware) and give FreeType those bytes; the face is
# declared after them, so it is destroyed first.
proof_capypdf_overlay(document.hpp
    [==[struct TtfFont {
    std::unique_ptr<FT_FaceRec_, FT_Error (*)(FT_Face)> face;
    TrueTypeFontFile fontdata;
};]==]
    [==[struct TtfFont {
    TrueTypeFontFile fontdata;
    std::unique_ptr<FT_FaceRec_, FT_Error (*)(FT_Face)> face;
};]==])
proof_capypdf_overlay(document.cpp
    [==[    TtfFont ttf{std::unique_ptr<FT_FaceRec_, FT_Error (*)(FT_Face)>{nullptr, guarded_face_close},
                {}};
    auto error = FT_New_Face(ft, fname, props.subfont, &face);]==]
    [==[    TtfFont ttf{{}, std::unique_ptr<FT_FaceRec_, FT_Error (*)(FT_Face)>{nullptr, guarded_face_close}};
    ERC(fontdata, load_and_parse_font_file(fname, props));
    if(!std::holds_alternative<TrueTypeFontFile>(fontdata)) {
        RETERR(UnsupportedFormat);
    }
    ttf.fontdata = std::move(std::get<TrueTypeFontFile>(fontdata));
    ERC(font_bytes, span_of_source(ttf.fontdata.original_data));
    auto error = FT_New_Memory_Face(ft,
                                    reinterpret_cast<const FT_Byte *>(font_bytes.data()),
                                    (FT_Long)font_bytes.size(),
                                    props.subfont,
                                    &face);]==])
proof_capypdf_overlay(document.cpp
    [==[    ERC(fontdata, load_and_parse_font_file(fname, props));
    assert(std::holds_alternative<TrueTypeFontFile>(fontdata));
    ttf.fontdata = std::move(std::get<TrueTypeFontFile>(fontdata));
]==]
    "")

# CFF Top DICT metadata is optional. The pinned subsetter copies it conditionally
# but then patches four offsets by fixed entry numbers, throwing or corrupting
# the output when an entry is absent. Record each placeholder's byte position
# as it is written instead. Input fonts and their glyph programs stay unchanged.
proof_capypdf_overlay(cffsubsetter.cpp
    [==[    create_topdict();]==]
    [==[    ERCV(create_topdict());]==])
proof_capypdf_overlay(cffsubsetter.cpp
    [==[    if(source.is_cid) {
        ERCV(copy_dict_item(topdict, DictOperator::CIDFontVersion));
        ERCV(copy_dict_item(topdict, DictOperator::CIDCount));
        ERCV(copy_dict_item(topdict, DictOperator::FDArray));  // offset needs to be fixed in post.
        ERCV(copy_dict_item(topdict, DictOperator::FDSelect)); // offset needs to be fixed in post.
    } else {
        std::vector<int32_t> operand;
        operand.push_back(-1);
        topdict.append_command(operand, DictOperator::CIDFontVersion);
        operand.clear();
        operand.push_back(65535);
        topdict.append_command(operand, DictOperator::CIDCount);
        operand.clear();
        operand.push_back(-1);
        topdict.append_command(operand, DictOperator::FDArray);
        topdict.append_command(operand, DictOperator::FDSelect);
    }
    ERCV(copy_dict_item(topdict, DictOperator::Charset)); // offset needs to be fixed in post.
    ERCV(copy_dict_item(topdict,
                        DictOperator::CharStrings)); // offset needs to be fixed in post.
]==]
    [==[    // These CID metadata entries also have defaults (CFF spec, Table 10).
    if(source.is_cid) {
        copy_dict_item_if_exists(topdict, DictOperator::CIDFontVersion);
    }
    // The output charset assigns consecutive CIDs to the subset's glyphs.
    topdict.append_command({static_cast<int32_t>(sub.size())}, DictOperator::CIDCount);
    auto placeholder = [&](DictOperator op) {
        // CFFDictWriter writes a 29 byte followed by a four-byte integer.
        auto const offset = topdict.current_size() + 1;
        topdict.append_command({-1}, op);
        return offset;
    };
    auto const fdarray_offset = placeholder(DictOperator::FDArray);
    auto const fdselect_offset = placeholder(DictOperator::FDSelect);
    auto const charset_offset = placeholder(DictOperator::Charset);
    auto const charstrings_offset = placeholder(DictOperator::CharStrings);
]==])
proof_capypdf_overlay(cffsubsetter.cpp
    [==[    fixups.fdarray.offset = serialization.offsets.at(8) + 1 + dict_start;
    fixups.fdselect.offset = serialization.offsets.at(9) + 1 + dict_start;
    fixups.charsets.offset = serialization.offsets.at(10) + 1 + dict_start;
    fixups.charstrings.offset = serialization.offsets.at(11) + 1 + dict_start;]==]
    [==[    fixups.fdarray.offset = fdarray_offset + dict_start;
    fixups.fdselect.offset = fdselect_offset + dict_start;
    fixups.charsets.offset = charset_offset + dict_start;
    fixups.charstrings.offset = charstrings_offset + dict_start;]==])
