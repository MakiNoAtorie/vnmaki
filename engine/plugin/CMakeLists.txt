# Plugin API (task 48, docs/SPEC.md section 20): the context C++ plugins register in, and the
# manifest and the files of Lua plugins. docs/plugins.md describes both.
target_sources(vnmaki_engine PRIVATE lua_plugin.cpp lua_plugin.hpp plugin.cpp plugin.hpp
                                  plugin_value.hpp)
