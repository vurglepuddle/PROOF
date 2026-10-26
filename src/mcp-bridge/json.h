// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_MCP_JSON_H
#define INKSCAPE_MCP_JSON_H

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Inkscape::MCP {

class Json
{
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Json() : value(nullptr) {}
    Json(std::nullptr_t) : value(nullptr) {}
    Json(bool v) : value(v) {}
    Json(double v) : value(v) {}
    Json(int v) : value(double(v)) {}
    Json(char const *v) : value(std::string(v)) {}
    Json(std::string v) : value(std::move(v)) {}
    Json(Array v) : value(std::move(v)) {}
    Json(Object v) : value(std::move(v)) {}

    template <typename T> bool is() const { return std::holds_alternative<T>(value); }
    template <typename T> T const &get() const { return std::get<T>(value); }
    Json const &at(std::string_view key) const;
    bool contains(std::string_view key) const;
    bool operator==(Json const &) const = default;
    Value value;
};

constexpr std::size_t REQUEST_LIMIT = 1024 * 1024;
constexpr std::size_t RESPONSE_LIMIT = 32 * 1024 * 1024;
Json parse(std::string_view input);
std::string serialize(Json const &value);

} // namespace Inkscape::MCP
#endif
