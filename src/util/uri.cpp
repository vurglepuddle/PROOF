// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * URI functions as per 4.3.4 of CSS 2.1
 *   http://www.w3.org/TR/CSS21/syndata.html#uri
 *
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2006-2024 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstring>
#include <glib.h>
#include <optional>

#include "uri.h"

std::string extract_uri(char const *s, char const **endptr)
{
    std::string result;

    if (!s)
        return result;

    gchar const *sb = s;
    if ( strlen(sb) < 4 || strncmp(sb, "url", 3) != 0 ) {
        return result;
    }

    sb += 3;

    if ( endptr ) {
        *endptr = nullptr;
    }

    // This first whitespace technically is not allowed.
    // Just left in for now for legacy behavior.
    while ( ( *sb == ' ' ) ||
            ( *sb == '\t' ) )
    {
        sb++;
    }

    if ( *sb == '(' ) {
        sb++;
        while ( ( *sb == ' ' ) ||
                ( *sb == '\t' ) )
        {
            sb++;
        }

        gchar delim = ')';
        if ( (*sb == '\'' || *sb == '"') ) {
            delim = *sb;
            sb++;
        }

        if (!*sb) {
            return result;
        }

        gchar const* se = sb;
        while ( *se && (*se != delim) ) {
            se++;
        }

        // we found the delimiter
        if ( *se ) {
            if ( delim == ')' ) {
                if ( endptr ) {
                    *endptr = se + 1;
                }

                // back up for any trailing whitespace
                while (se > sb && g_ascii_isspace(se[-1]))
                {
                    se--;
                }

                result = std::string(sb, se);
            } else {
                gchar const* tail = se + 1;
                while ( ( *tail == ' ' ) ||
                        ( *tail == '\t' ) )
                {
                    tail++;
                }
                if ( *tail == ')' ) {
                    if ( endptr ) {
                        *endptr = tail + 1;
                    }
                    result = std::string(sb, se);
                }
            }
        }
    }

    return result;
}

std::optional<std::string> try_extract_uri(const char* url) {
    auto link = extract_uri(url);
    return link.empty() ? std::nullopt : std::make_optional(link);
}

std::optional<std::string> try_extract_uri_id(const char *url) {
    if (auto ret = try_extract_uri(url)) {
        if (!ret->empty() && (*ret)[0] == '#') {
            ret->erase(0, 1);
            return ret;
        }
    }
    return std::nullopt;
}

std::tuple<char const *, Base64Data> extract_uri_data(char const *uri_data)
{
    bool data_is_base64 = false;
    bool data_is_image = false;
    bool data_is_svg = false;
    bool data_has_mime = false;

    gchar const *data = uri_data;

    if ((*data) && g_ascii_strncasecmp(data, "data:", 5) == 0) {
        data += 5;
    }

    while (*data) {
        if (g_ascii_strncasecmp(data, "base64", 6) == 0) {
            /* base64-encoding */
            data_is_base64 = true;
            // Illustrator produces embedded images without MIME type, so we assume it's image if no mime found
            data_is_image = !data_has_mime;
            data += 6;
        }
        else if (g_ascii_strncasecmp(data, "image/png", 9) == 0
              || g_ascii_strncasecmp(data, "image/jpg", 9) == 0
              || g_ascii_strncasecmp(data, "image/jp2", 9) == 0
              || g_ascii_strncasecmp(data, "image/bmp", 9) == 0) {
            /* PNGi, JPEG, JPEG200, BMP image */
            data_is_image = true;
            data += 9;
        }
        else if (g_ascii_strncasecmp(data, "image/jpeg", 10) == 0
              || g_ascii_strncasecmp(data, "image/tiff", 10) == 0) {
            /* JPEG, TIFF image */
            data_is_image = true;
            data += 10;
        }
        else if (g_ascii_strncasecmp(data, "image/vnd.adobe.photoshop", 25) == 0) {
            data_is_image = true;
            data += 25;
        }
        else if (g_ascii_strncasecmp(data, "image/svg+xml", 13) == 0) {
            /* SVG image */
            data_is_svg = true;
            data_is_image = true;
            data += 13;
        }
        else { /* unrecognized option; skip it */
            while (*data) {
                if (((*data) == ';') || ((*data) == ',')) {
                    break;
                }
                if ((*data) == '/') {
                    data_has_mime = true;
                }
                data++;
            }
        }
        if ((*data) == ';') {
            data++;
            continue;
        }
        if ((*data) == ',') {
            data++;
            break;
        }
    }
    if (data_is_base64 && data_is_image) {
        return {data, data_is_svg ? Base64Data::SVG : Base64Data::RASTER};
    }
    // No other data format yet
    return std::tuple{data, Base64Data::NONE};
}


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
