// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Which files a document may have looked up (PROOF). See link-policy.h.
 *
 * Copyright (C) 2026 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "io/link-policy.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <string_view>

#include <glib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Inkscape::IO {

namespace {

std::string const no_share = "\\\\";

std::string folded(std::string_view s)
{
    gchar *f = g_utf8_casefold(s.data(), static_cast<gssize>(s.size()));
    std::string out = f ? f : "";
    g_free(f);
    return out;
}

std::mutex trusted_lock;
std::set<std::string> trusted;

#ifdef _WIN32
/// The share a document's folder is on: named in its path, or behind its mapped drive.
std::string document_share(char const *folder)
{
    if (!folder || !*folder) return {};
    if (auto share = network_share(folder); !share.empty()) return share == no_share ? std::string() : share;
    std::string const f = folder;
    if (f.size() < 2 || !g_ascii_isalpha(f[0]) || f[1] != ':') return {};
    wchar_t const root[] = {static_cast<wchar_t>(f[0]), L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_REMOTE) return {};
    // A mapped drive: the folder's final path names the share behind it (\\?\UNC\server\share\…).
    // This asks the server the user mapped, never one a document names.
    gunichar2 *wide = g_utf8_to_utf16(f.c_str(), -1, nullptr, nullptr, nullptr);
    if (!wide) return {};
    HANDLE h = CreateFileW(reinterpret_cast<wchar_t const *>(wide), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    g_free(wide);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring buffer(4096, L'\0');
    DWORD const n = GetFinalPathNameByHandleW(h, buffer.data(), static_cast<DWORD>(buffer.size()),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= buffer.size()) return {};
    gchar *utf8 = g_utf16_to_utf8(reinterpret_cast<gunichar2 const *>(buffer.c_str()), n, nullptr, nullptr, nullptr);
    if (!utf8) return {};
    std::string const final_path = utf8;
    g_free(utf8);
    auto share = network_share(final_path);
    return share == no_share ? std::string() : share;
}
#endif

} // namespace

std::string network_share(std::string const &path)
{
    std::string p = path;
    std::replace(p.begin(), p.end(), '/', '\\');
    if (p.size() < 2 || p[0] != '\\' || p[1] != '\\') return {};
    std::string_view rest(p);
    rest.remove_prefix(2);
    if (rest.starts_with("?\\") || rest.starts_with(".\\")) {
        // Windows' file and device namespaces: a drive there (\\?\C:\…) is this computer's,
        // a UNC path (\\?\UNC\server\share\…) is read as one, anything else is a device.
        rest.remove_prefix(2);
        if (rest.size() >= 2 && g_ascii_isalpha(rest[0]) && rest[1] == ':') return {};
        if (rest.size() < 4 || g_ascii_strncasecmp(rest.data(), "UNC\\", 4) != 0) return no_share;
        rest.remove_prefix(4);
    }
    auto const server_end = rest.find('\\');
    if (server_end == 0 || server_end == std::string_view::npos) return no_share;
    auto const share_end = rest.find('\\', server_end + 1);
    auto const share = rest.substr(server_end + 1, share_end == std::string_view::npos ? std::string_view::npos
                                                                                       : share_end - server_end - 1);
    if (share.empty()) return no_share;
    return no_share + folded(rest.substr(0, server_end)) + "\\" + folded(share);
}

bool may_follow_link(std::string const &file, char const *document_folder)
{
#ifdef _WIN32
    auto const share = network_share(file);
    if (share.empty()) return true;
    if (share == no_share) return false;
    {
        std::lock_guard lock(trusted_lock);
        if (trusted.contains(share)) return true;
    }
    return share == document_share(document_folder);
#else
    (void)file;
    (void)document_folder;
    return true;
#endif
}

void trust_link_share(std::string const &file)
{
    auto share = network_share(file);
    if (share.empty() || share == no_share) return;
    std::lock_guard lock(trusted_lock);
    trusted.insert(std::move(share));
}

} // namespace Inkscape::IO
