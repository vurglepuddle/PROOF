// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Which files a document may have looked up (PROOF).
 *
 * On Windows, looking up a path on another computer (\\server\share\…) makes Windows
 * contact it and offer the user's sign-in, so a received file whose links name such a
 * server could collect it. The owner, 2026-10-21: follow links on this computer and on
 * the share the document itself is on; others aren't looked up, show as missing, and
 * can be relinked. Read as a delta of VectorCraft's (`6e4fb6e4`, not their code).
 *
 * Copyright (C) 2026 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_INKSCAPE_IO_LINK_POLICY_H
#define SEEN_INKSCAPE_IO_LINK_POLICY_H

#include <string>

namespace Inkscape::IO {

/**
 * The network share a path names, `\\server\share` with both parts case-folded:
 * `\\server\share\…`, `//server/share/…` and `\\?\UNC\server\share\…`. Empty for a path
 * on a drive (`C:\…`, `\\?\C:\…`) or a relative one. `\\` alone for a path that names a
 * computer but no share, or a device (`\\.\pipe\…`): it matches no share.
 */
std::string network_share(std::string const &path);

/**
 * Whether a file a document names (a linked picture, a placed page, another document)
 * may be looked up. Followed: paths on this computer's drives, mapped network drives
 * included (the user chose those servers), relative paths, paths on the share the
 * document is on (`document_folder`, the document's folder; null or empty for one not
 * saved), and paths on a share the user chose a file from in this session. Not followed:
 * any other `\\server\share`, and devices. Elsewhere than Windows: always.
 */
bool may_follow_link(std::string const &file, char const *document_folder);

/**
 * The user chose this file (placed, imported or relinked it): links on its share are
 * followed for the rest of the session. Nothing for a file on a drive.
 */
void trust_link_share(std::string const &file);

} // namespace Inkscape::IO

#endif // SEEN_INKSCAPE_IO_LINK_POLICY_H
