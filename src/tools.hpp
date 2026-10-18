// The MCP tool surface: what an AI client can ask the node to do.
#pragma once

#include "node.hpp"

namespace maas {

json tool_definitions();
// Runs a tool and returns an MCP CallToolResult: {"content": [...], "isError": bool}.
json call_tool(Context& context, const Principal& caller, const std::string& name, const json& arguments);
std::string server_instructions(Context& context);
json node_capabilities(Context& context);

}  // namespace maas
