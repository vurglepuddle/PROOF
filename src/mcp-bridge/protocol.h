// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_MCP_PROTOCOL_H
#define INKSCAPE_MCP_PROTOCOL_H

#include <functional>
#include <memory>
#include "json.h"

namespace Inkscape::MCP {

Json tool_result(Json data, std::string text, bool error = false);

class Protocol : public std::enable_shared_from_this<Protocol>
{
public:
    using Reply = std::function<void(Json, bool close)>;
    using Consent = std::function<void(std::string, std::string, std::function<void(bool)>)>;
    using ToolCall = std::function<Json(std::string const &, Json const &)>;
    Protocol(std::string token, std::string nonce, std::string version, int pid);
    void dispatch(Json const &request, bool look, bool closing, ToolCall const &tool,
                  Consent const &consent, Reply reply);
    bool authorized() const { return _authorized; }

private:
    std::string _token, _nonce, _version;
    int _pid;
    bool _authorized = false;
    bool _hello = false;
};
} // namespace Inkscape::MCP
#endif
