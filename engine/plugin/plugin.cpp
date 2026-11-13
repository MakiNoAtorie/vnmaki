#include "engine/plugin/plugin.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

#include "engine/render/image.hpp"

namespace vnmaki {

namespace {

constexpr auto kLuaKeywords = std::to_array<std::string_view>(
    {"and",      "break",  "do",   "else", "elseif", "end",  "false", "for",
     "function", "goto",   "if",   "in",   "local",  "nil",  "not",   "or",
     "repeat",   "return", "then", "true", "until",  "while"});

bool is_lua_name(std::string_view name) {
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
    return false;
  }
  const bool characters = std::ranges::all_of(
      name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
  return characters && std::ranges::find(kLuaKeywords, name) == kLuaKeywords.end();
}

std::string quoted(std::string_view what, std::string_view name, std::string_view rest) {
  std::string text{what};
  text += " '";
  text += name;
  text += "' ";
  text += rest;
  return text;
}

}  // namespace

const PluginValue* find_param(const PluginParams& params, std::string_view name) {
  const auto found = std::ranges::find(params, name, &PluginParams::value_type::first);
  return found == params.end() ? nullptr : &found->second;
}

double number_param(const PluginParams& params, std::string_view name, double fallback) {
  const PluginValue* value = find_param(params, name);
  if (value == nullptr) {
    return fallback;
  }
  if (const auto* integer = std::get_if<std::int64_t>(value)) {
    return static_cast<double>(*integer);
  }
  if (const auto* real = std::get_if<double>(value)) {
    return *real;
  }
  throw PluginError{quoted("param", name, "must be a number")};
}

std::string string_param(const PluginParams& params, std::string_view name,
                         std::string_view fallback) {
  const PluginValue* value = find_param(params, name);
  if (value == nullptr) {
    return std::string{fallback};
  }
  if (const auto* text = std::get_if<std::string>(value)) {
    return *text;
  }
  throw PluginError{quoted("param", name, "must be a string")};
}

PluginContext& PluginContext::linked() {
  static PluginContext context;
  return context;
}

void PluginContext::add_function(std::string name, PluginFunction function) {
  if (!is_lua_name(name)) {
    throw PluginError{quoted("function", name, "is not a Lua name")};
  }
  if (!function) {
    throw PluginError{quoted("function", name, "has no body")};
  }
  if (!functions_.emplace(name, std::move(function)).second) {
    throw PluginError{quoted("function", name, "is registered twice")};
  }
}

void PluginContext::add_displayable(std::string type, PluginDisplayableFactory factory) {
  if (type.empty() || !factory) {
    throw PluginError{quoted("displayable type", type, "needs a name and a factory")};
  }
  if (!displayables_.emplace(type, std::move(factory)).second) {
    throw PluginError{quoted("displayable type", type, "is registered twice")};
  }
}

void PluginContext::add_image_loader(std::string extension, PluginImageLoader loader) {
  if (!loader) {
    throw PluginError{quoted("image format", extension, "has no loader")};
  }
  try {
    register_image_format(
        extension, [loader = std::move(loader)](std::span<const std::byte> bytes) -> Image {
          try {
            PluginImage decoded = loader(bytes);
            return Image{.size = {.width = decoded.width, .height = decoded.height},
                         .pixels = std::move(decoded.rgba)};
          } catch (const PluginError& error) {
            throw ImageError{error.what()};
          }
        });
  } catch (const std::invalid_argument& error) {
    throw PluginError{error.what()};
  }
}

const PluginDisplayableFactory* PluginContext::find_displayable(std::string_view type) const {
  const auto found = displayables_.find(type);
  return found == displayables_.end() ? nullptr : &found->second;
}

std::vector<std::string> PluginContext::displayable_types() const {
  std::vector<std::string> types;
  types.reserve(displayables_.size());
  for (const auto& entry : displayables_) {
    types.push_back(entry.first);
  }
  return types;
}

}  // namespace vnmaki
