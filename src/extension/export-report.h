// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_EXTENSION_EXPORT_REPORT_H
#define INKSCAPE_EXTENSION_EXPORT_REPORT_H

#include <algorithm>
#include <string>
#include <vector>
#include <glibmm/i18n.h>

namespace Inkscape::Extension {

enum class Severity { Warning, Error };
enum class MissingLinkAction { RequirePresent, Ignore, Cancel };
struct Finding {
    std::string object;
    std::string reason;
    Severity severity;
};

// An outcome of writing, not a preflight assessment of the document.
struct ExportReport {
    std::vector<Finding> findings;
    bool written = false;

    void add(std::string object, std::string reason, Severity severity)
    {
        if (std::none_of(findings.begin(), findings.end(), [&](auto const &f) {
                return f.object == object && f.reason == reason && f.severity == severity;
            })) findings.push_back({std::move(object), std::move(reason), severity});
    }
    void error(std::string object, std::string reason) { add(std::move(object), std::move(reason), Severity::Error); }
    void missing_link(std::string object, std::string const &path, MissingLinkAction action) {
        add(std::move(object), std::string(_("Linked picture is missing: ")) + path,
            action == MissingLinkAction::Ignore ? Severity::Warning : Severity::Error);
    }
    bool has_errors() const
    {
        return std::any_of(findings.begin(), findings.end(), [](auto const &f) { return f.severity == Severity::Error; });
    }
    std::string details() const
    {
        std::string text;
        for (auto const &f : findings) {
            text += "\n" + std::string(f.severity == Severity::Error ? _("Error") : _("Warning")) +
                    " [" + f.object + "]: " + f.reason;
        }
        return text;
    }
    std::string summary() const
    {
        return std::string(!written ? _("Export failed.") : findings.empty() ? _("Export completed.") :
            _("PDF written with export findings. Some content may be missing or not converted.")) + details();
    }
};

// Fault injection for automated tests only. No preference or command-line option.
inline bool export_test_failure(char const *kind, std::string const &object)
{
    auto const value = g_getenv("PROOF_TEST_PDF_FAILURE");
    return value && std::string(value) == std::string(kind) + ":" + object;
}

} // namespace Inkscape::Extension
#endif
