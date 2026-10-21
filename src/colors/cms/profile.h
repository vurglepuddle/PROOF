// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 *
 * Authors: see git history
 *
 * Copyright (C) 2023 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_COLORS_CMS_PROFILE_H
#define SEEN_COLORS_CMS_PROFILE_H

#include <cassert>
#include <lcms2.h> // cmsHPROFILE
#include <memory>
#include <string>
#include <vector>

namespace Inkscape::Colors::CMS {

class Profile
{
public:
    static std::shared_ptr<Profile> create(cmsHPROFILE handle, std::string path = "", bool in_home = false);
    static std::shared_ptr<Profile> create_from_copy(cmsHPROFILE handle);
    static std::shared_ptr<Profile> create_from_uri(std::string path, bool in_home = false);
    static std::shared_ptr<Profile> create_from_data(std::string const &contents);

    /* LittleCMS based identity profiles */
    static std::shared_ptr<Profile> create_gray();
    static std::shared_ptr<Profile> create_srgb();
    static std::shared_ptr<Profile> create_linearrgb();
    static std::shared_ptr<Profile> create_xyz65();
    static std::shared_ptr<Profile> create_xyz50();
    static std::shared_ptr<Profile> create_lab();

    static bool sortByName(std::shared_ptr<Profile> const &p1, std::shared_ptr<Profile> const &p2)
    {
        return p1->getName() < p2->getName();
    }
    static bool sortById(std::shared_ptr<Profile> const &p1, std::shared_ptr<Profile> const &p2)
    {
        return p1->_id < p2->_id;
    }

    Profile(cmsHPROFILE handle, std::string path, bool in_home);
    ~Profile() { cmsCloseProfile(_handle); }
    Profile(Profile const &) = delete;
    Profile &operator=(Profile const &) = delete;
    bool operator==(Profile const &other) const { return _checksum == other._checksum; }

    cmsHPROFILE getHandle() const { return _handle; }
    std::string const &getPath() const { return _path; }
    bool inHome() const { return _in_home; }

    bool isForDisplay() const;
    const std::string &getId() const { return _id; }
    const std::string &getChecksum() const { return _checksum; }
    std::string getName(bool sanitize = false) const;
    unsigned int getSize() const;
    cmsColorSpaceSignature getColorSpace() const;
    cmsProfileClassSignature getProfileClass() const;

    static bool isIccFile(std::string const &filepath);
    std::string dumpBase64() const;
    /// PROOF: the profile as it came, byte for byte, when it came from data or a file that still
    /// holds it; else lcms's encoding of it.
    std::vector<unsigned char> dumpData() const;
    /// lcms's encoding of a profile. It writes the tags it has parsed in its own encoding (a
    /// press profile's 8-bit tables as 16-bit), which can convert differently from the original.
    static std::vector<unsigned char> dumpData(cmsHPROFILE profile);

private:
    cmsHPROFILE _handle;
    std::string _path;
    std::string _id;
    std::string _checksum;
    bool _in_home = false;
    /// The bytes it was read from, when it came from data.
    std::vector<unsigned char> _data;
    static std::string checksum_of(std::vector<unsigned char> data);

public:
    std::string get_id() const;
    std::string generate_id() const;
    std::string generate_checksum() const;
};

class CmsProfileError : public std::exception
{
public:
    CmsProfileError(std::string &&msg)
        : _msg(std::move(msg))
    {}
    char const *what() const noexcept override { return _msg.c_str(); }

private:
    std::string _msg;
};

} // namespace Inkscape::Colors::CMS

#endif // SEEN_COLORS_CMS_PROFILE_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
