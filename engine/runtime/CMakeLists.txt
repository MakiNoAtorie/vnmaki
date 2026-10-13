# Script runtime (ticket 06): Lua VM, script loader, label registry and the
# coroutine scheduler, plus project.toml parsing and the headless runner.
target_sources(
  vnmaki_engine
  PRIVATE headless_runner.cpp
          headless_runner.hpp
          project_config.cpp
          project_config.hpp
          script_runtime.cpp
          script_runtime.hpp)

# Lua 5.4 (PUC). vcpkg's lua port and the system package are both found by
# CMake's FindLua module. Lua is compiled as C; sol2 includes it through
# <lua.hpp>, which wraps the C headers in extern "C".
find_package(Lua 5.4 REQUIRED)
# vcpkg's FindLua wrapper sets no version variables, so read lua.h.
file(STRINGS "${LUA_INCLUDE_DIR}/lua.h" vnmaki_lua_version_num
     REGEX "^#define[ \t]+LUA_VERSION_NUM[ \t]+[0-9]+")
string(REGEX MATCH "[0-9]+$" vnmaki_lua_version_num "${vnmaki_lua_version_num}")
if(NOT vnmaki_lua_version_num EQUAL 504)
  message(FATAL_ERROR "VNMaki needs Lua 5.4; ${LUA_INCLUDE_DIR}/lua.h has "
                      "LUA_VERSION_NUM '${vnmaki_lua_version_num}'")
endif()
find_package(sol2 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

# Lua, sol2 and toml++ are private to the engine: no engine header includes
# them. SYSTEM keeps their warnings out of -Werror.
target_include_directories(vnmaki_engine SYSTEM PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(vnmaki_engine PRIVATE ${LUA_LIBRARIES} sol2::sol2
                                            tomlplusplus::tomlplusplus)

# sol2: check every argument type at the Lua/C++ boundary (scripts are
# untrusted input) and never print errors itself; the runtime reports them.
set_property(
  SOURCE script_runtime.cpp TARGET_DIRECTORY vnmaki_engine
  APPEND
  PROPERTY COMPILE_DEFINITIONS SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)
