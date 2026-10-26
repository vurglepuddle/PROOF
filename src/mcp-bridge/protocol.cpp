// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol.h"
#include "tools.h"

#include <cmath>
#include <stdexcept>

namespace Inkscape::MCP {
using Object = Json::Object;
using Array = Json::Array;

namespace {
std::string text(Json const &v) { return v.get<std::string>(); }
Json error(Json id, int code, std::string message)
{
    return Object{{"id", std::move(id)}, {"error", Object{{"code", code}, {"message", std::move(message)}}}};
}
Json result(Json id, Json data) { return Object{{"id", std::move(id)}, {"result", std::move(data)}}; }
}

Json tool_result(Json data, std::string text, bool error)
{
    return Object{{"content", Array{Object{{"type", "text"}, {"text", std::move(text)}}}},
                  {"structuredContent", std::move(data)}, {"isError", error}};
}

Protocol::Protocol(std::string token, std::string nonce, std::string version, int pid)
    : _token(std::move(token)), _nonce(std::move(nonce)), _version(std::move(version)), _pid(pid) {}

void Protocol::dispatch(Json const &request, bool look, bool closing, ToolCall const &tool,
                        Consent const &consent, Reply reply)
{
    Json id;
    try {
        id = request.at("id");
        if (!id.is<std::string>() && (!id.is<double>() || std::floor(id.get<double>()) != id.get<double>() ||
                                      std::abs(id.get<double>()) > 9007199254740991.0)) {
            throw std::runtime_error("Invalid request id");
        }
        auto method = text(request.at("method"));
        auto const &params = request.at("params");
        if (!params.is<Object>()) throw std::runtime_error("Invalid parameters");
        if (closing) { reply(result(id, tool_result(Object{}, "PROOF is closing", true)), !_authorized); return; }
        if (!_authorized) {
            if (_hello || method != "hello") { reply(error(id, -32001, "unauthorized: hello required"), true); return; }
            _hello = true;
            auto token = text(params.at("token"));
            unsigned difference = token.size() == _token.size() ? 0 : 1;
            for (std::size_t i = 0; i < _token.size(); ++i) difference |= _token[i] ^ (i < token.size() ? token[i] : 0);
            if (difference) { reply(error(id, -32001, "unauthorized"), true); return; }
            if (params.at("bridge_protocol") != Json(1)) { reply(error(id, -32002, "Unsupported bridge protocol"), true); return; }
            auto name = text(params.at("client").at("name"));
            auto version = text(params.at("client").at("version"));
            auto control = [](std::string const &s) {
                for (unsigned char c : s) if (c < 32 || c == 127) return true;
                return false;
            };
            if (name.empty() || name.size() > 256 || version.size() > 128 ||
                control(name) || control(version)) {
                throw std::runtime_error("Invalid client identity");
            }
            consent(std::move(name), std::move(version), [self = shared_from_this(), id, look, reply](bool allowed) {
                self->_authorized = allowed;
                if (!allowed) { reply(error(id, -32003, "denied"), true); return; }
                reply(result(id, Object{{"bridge_protocol", 1}, {"proof", Object{{"version", self->_version}}},
                    {"instance", Object{{"pid", self->_pid}, {"nonce", self->_nonce}}},
                    {"permissions", look ? Array{Json("look")} : Array{}}}), false);
            });
            return;
        }
        if (method == "ping") { reply(result(id, Object{}), false); return; }
        if (method == "tools/list") {
            Array tools;
            if (look) tools = look_tools();
            reply(result(id, Object{{"tools", std::move(tools)}}), false);
            return;
        }
        if (method == "tools/call") {
            try {
                auto name = text(params.at("name"));
                auto const &args = params.at("arguments");
                if (!known_tool(name)) throw std::runtime_error("Unknown tool");
                if (!look) throw std::runtime_error("Look permission is off");
                reply(result(id, tool(name, args)), false);
            } catch (std::exception const &e) { reply(result(id, tool_result(Object{}, e.what(), true)), false); }
            catch (...) { reply(result(id, tool_result(Object{}, "The tool failed", true)), false); }
            return;
        }
        reply(error(id, -32601, "Unknown method"), false);
    } catch (std::exception const &) { reply(error(id, -32600, "Invalid bridge request"), !_authorized); }
    catch (...) { reply(error(id, -32603, "Bridge request failed"), true); }
}
} // namespace Inkscape::MCP
