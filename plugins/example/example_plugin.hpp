#pragma once

// The example C++ plugin (docs/plugins.md). It adds:
//   example_add(a, b)           Lua function: the sum of two numbers
//   example_greet(name)         Lua function: "Hello, <name>!"
//   displayable "example_badge" a coloured square: show("b", { displayable = "example_badge",
//                               params = { size = 100, color = "#ff8800" } })
//   image format ".ppm"         binary PPM (P6) pictures: images/<name>.ppm

#include "engine/plugin/plugin.hpp"

namespace vnmaki {

void register_plugin_example(PluginContext& context);

}  // namespace vnmaki
