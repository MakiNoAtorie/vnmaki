#include "engine/ui/preferences.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <variant>

#include "engine/audio/audio.hpp"
#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/platform/platform.hpp"

namespace vnmaki {

namespace {

double number(const ScriptRuntime& runtime, std::string_view key, double fallback) {
  const ScreenValue value = runtime.binding("prefs", key);
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    return static_cast<double>(*integer);
  }
  if (const auto* real = std::get_if<double>(&value)) {
    return *real;
  }
  return fallback;
}

bool flag(const ScriptRuntime& runtime, std::string_view key, bool fallback) {
  const ScreenValue value = runtime.binding("prefs", key);
  const auto* held = std::get_if<bool>(&value);
  return held != nullptr ? *held : fallback;
}

std::string text(const ScriptRuntime& runtime, std::string_view key) {
  const ScreenValue value = runtime.binding("prefs", key);
  const auto* held = std::get_if<std::string>(&value);
  return held != nullptr ? *held : std::string{};
}

double unit_volume(const ScriptRuntime& runtime, std::string_view key, double fallback) {
  return std::clamp(number(runtime, key, fallback), 0.0, 1.0);
}

}  // namespace

Preferences read_preferences(const ScriptRuntime& runtime) {
  const Preferences defaults;
  return {
      .text_speed = std::clamp(number(runtime, "text_speed", defaults.text_speed), kMinTextSpeed,
                               kMaxTextSpeed),
      .auto_delay =
          std::clamp(number(runtime, "auto_delay", defaults.auto_delay), 0.0, kMaxAutoDelay),
      .volume_music = unit_volume(runtime, "volume_music", defaults.volume_music),
      .volume_sound = unit_volume(runtime, "volume_sound", defaults.volume_sound),
      .volume_voice = unit_volume(runtime, "volume_voice", defaults.volume_voice),
      .volume_ambient = unit_volume(runtime, "volume_ambient", defaults.volume_ambient),
      .volume_ui = unit_volume(runtime, "volume_ui", defaults.volume_ui),
      .fullscreen = flag(runtime, "fullscreen", defaults.fullscreen),
      .skip_unseen = flag(runtime, "skip_unseen", defaults.skip_unseen),
      .window_opacity = std::clamp(number(runtime, "window_opacity", defaults.window_opacity),
                                   kMinWindowOpacity, 1.0),
      .reduce_motion = flag(runtime, "reduce_motion", defaults.reduce_motion),
      .language = text(runtime, "language"),
  };
}

PreferenceApplier::PreferenceApplier(const ScriptRuntime& runtime, PreferenceTargets targets)
    : runtime_(&runtime), targets_(targets) {}

void PreferenceApplier::update() {
  const Preferences now = read_preferences(*runtime_);
  if (now == applied_) {
    return;
  }
  if (targets_.presenter != nullptr && now.text_speed != applied_.text_speed) {
    targets_.presenter->set_characters_per_second(now.text_speed);
  }
  if (targets_.playback != nullptr && now.auto_delay != applied_.auto_delay) {
    targets_.playback->set_auto_base_delay(now.auto_delay);
  }
  if (targets_.audio != nullptr) {
    const auto set_volume = [&](std::string_view channel, double level, double before) {
      if (level != before) {
        static_cast<void>(targets_.audio->set_preference_volume(channel, level));
      }
    };
    set_volume(kMusicChannel, now.volume_music, applied_.volume_music);
    set_volume(kSoundChannel, now.volume_sound, applied_.volume_sound);
    set_volume(kVoiceChannel, now.volume_voice, applied_.volume_voice);
    set_volume(kAmbientChannel, now.volume_ambient, applied_.volume_ambient);
    set_volume(kUiChannel, now.volume_ui, applied_.volume_ui);
  }
  if (targets_.platform != nullptr) {
    if (now.fullscreen != applied_.fullscreen) {
      targets_.platform->set_fullscreen(now.fullscreen);
    }
    if (now.window_opacity != applied_.window_opacity) {
      targets_.platform->set_window_opacity(static_cast<float>(now.window_opacity));
    }
  }
  if (targets_.language && now.language != applied_.language) {
    targets_.language(now.language);
  }
  applied_ = now;
}

}  // namespace vnmaki
