// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_MCP_TOOLS_H
#define INKSCAPE_MCP_TOOLS_H

#include <2geom/point.h>
#include "json.h"

class SPDocument;
class SPObject;
namespace Inkscape { class ObjectSet; }

namespace Inkscape::MCP {

// Lives only for one main-thread request; nothing here owns or retains a target.
struct LookContext {
    SPDocument &document;
    ObjectSet *selection = nullptr;
    SPObject *current_layer = nullptr;
    Geom::Point anchor{0, 0};
    Json identity;
};

Json::Array look_tools();
bool known_tool(std::string const &name);
void validate_arguments(std::string const &name, Json const &arguments);
Json call_look_tool(LookContext const &context, std::string const &name, Json const &arguments);

} // namespace Inkscape::MCP
#endif
