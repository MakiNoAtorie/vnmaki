#include "engine/assets/import_settings.hpp"

#include <cmath>
#include <optional>

#include <toml++/toml.hpp>

namespace vnmaki {

namespace {

constexpr double kMinTargetDb = -120;

std::string at(const toml::node& node) {
  const toml::source_position position = node.source().begin;
  return "project.toml:" + std::to_string(position.line) + ":" + std::to_string(position.column) +
         ": ";
}

}  // namespace

ImportSettings parse_import_settings(std::string_view toml_text) {
  toml::table root;
  try {
    root = toml::parse(toml_text);
  } catch (const toml::parse_error& error) {
    throw ImportSettingsError{std::string{"project.toml: "} + std::string{error.description()}};
  }
  ImportSettings settings;
  const auto audio = root["build"]["audio"];
  if (!audio) {
    return settings;
  }
  if (!audio.is_table()) {
    throw ImportSettingsError{at(*audio.node()) + "'build.audio' must be a table"};
  }
  if (const auto mode = audio["normalize"]) {
    const std::optional<std::string> text = mode.value<std::string>();
    if (text == "none") {
      settings.normalize = AudioNormalize::kNone;
    } else if (text == "peak") {
      settings.normalize = AudioNormalize::kPeak;
    } else if (text == "rms") {
      settings.normalize = AudioNormalize::kRms;
    } else {
      throw ImportSettingsError{at(*mode.node()) +
                                "'build.audio.normalize' must be \"none\", \"peak\" or \"rms\""};
    }
  }
  settings.target_db =
      settings.normalize == AudioNormalize::kRms ? kDefaultRmsTargetDb : kDefaultPeakTargetDb;
  if (const auto target = audio["target_db"]) {
    const std::optional<double> value = target.value<double>();
    if (!value || !(*value >= kMinTargetDb && *value <= 0)) {
      throw ImportSettingsError{at(*target.node()) +
                                "'build.audio.target_db' must be a number from -120 to 0"};
    }
    settings.target_db = *value;
  }
  return settings;
}

}  // namespace vnmaki
