#pragma once

// The values that cross between Lua and a C++ plugin (engine/plugin/plugin.hpp). This header is
// light: the script runtime includes it.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vnmaki {

/// A plugin broke a rule of the API, or a plugin function failed. As the error of a Lua call it
/// reaches the script at the line of the call.
class PluginError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// What Lua code and a plugin exchange: nil, a boolean, an integer, a float or a string.
/// (A table does not cross.)
using PluginValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

/// The `params` table of `show(tag, { displayable = type, params = { ... } })`, sorted by name.
using PluginParams = std::vector<std::pair<std::string, PluginValue>>;

/// The value of `name`, or nullptr.
[[nodiscard]] const PluginValue* find_param(const PluginParams& params, std::string_view name);

/// The numeric value of a param (an integer or a float), or `fallback` if it is not there.
/// Throws PluginError if it is there and is not a number.
[[nodiscard]] double number_param(const PluginParams& params, std::string_view name,
                                  double fallback);
/// The string value of a param, or `fallback`. Throws PluginError if it is not a string.
[[nodiscard]] std::string string_param(const PluginParams& params, std::string_view name,
                                       std::string_view fallback);

}  // namespace vnmaki
