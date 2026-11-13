#include "engine/ui/self_voicing.hpp"

#include <format>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/text/markup.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

namespace {

constexpr double kPercent = 100;

// The text without its tags; a text with a bad tag is spoken as written.
std::string untagged(const std::string& text) {
  try {
    return parse_markup(text).text;
  } catch (const MarkupError&) {
    return text;
  }
}

std::string_view role_name(NodeRole role) {
  switch (role) {
    case NodeRole::kGroup:
      return "group";
    case NodeRole::kText:
      return "text";
    case NodeRole::kImage:
      return "image";
    case NodeRole::kButton:
      return "button";
    case NodeRole::kToggle:
      return "toggle";
    case NodeRole::kSlider:
      return "slider";
    case NodeRole::kProgress:
      return "progress";
    case NodeRole::kTextInput:
      return "text input";
  }
  return "";
}

// "Label, role" and the state: on or off, the percent of a slider or bar, the text of an input,
// disabled.
std::string describe(const UiNode& node) {
  const std::string& caption = node.label.empty() ? node.text : node.label;
  std::string said = std::format("{}, {}", caption, role_name(node.role));
  switch (node.role) {
    case NodeRole::kToggle:
      said += node.checked ? ", on" : ", off";
      break;
    case NodeRole::kSlider:
    case NodeRole::kProgress: {
      const double range = node.max - node.min;
      said += std::format(", {:.0f} percent",
                          range > 0 ? (node.value - node.min) / range * kPercent : 0.0);
      break;
    }
    case NodeRole::kTextInput:
      if (!node.text.empty()) {
        said += ", " + node.text;
      }
      break;
    case NodeRole::kGroup:
    case NodeRole::kText:
    case NodeRole::kImage:
    case NodeRole::kButton:
      break;
  }
  if (node.state.disabled) {
    said += ", disabled";
  }
  return said;
}

const UiNode* focused_in(const UiNode& node) {
  if (node.state.focused && is_focusable(node)) {
    return &node;
  }
  for (const UiNode& child : node.children) {
    if (const UiNode* found = focused_in(child)) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

SelfVoicing::SelfVoicing(TtsBackend& tts, const ScriptRuntime& runtime,
                         const DialoguePresenter& presenter, const Playback& playback,
                         UiHost& ui)
    : tts_(&tts), runtime_(&runtime), presenter_(&presenter), playback_(&playback), ui_(&ui) {}

void SelfVoicing::set_enabled(bool enabled) {
  if (enabled == enabled_) {
    return;
  }
  enabled_ = enabled;
  if (enabled) {  // forget what was said, so that update() says it all again
    line_serial_ = 0;
    menu_position_.reset();
    focus_.clear();
    skipping_ = false;
  } else {
    tts_->speak("Self-voicing off");
  }
}

std::string SelfVoicing::focus_text() {
  const std::vector<const UiNode*> roots = ui_->roots();
  for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
    if (const UiNode* node = focused_in(**root)) {
      return describe(*node);
    }
  }
  return {};
}

void SelfVoicing::update() {
  if (!enabled_) {
    return;
  }
  const BlockingRequest* request = runtime_->request();
  const auto* menu = request != nullptr ? std::get_if<MenuRequest>(request) : nullptr;
  const std::string focus = focus_text();
  if (playback_->skip() != SkipMode::kOff) {  // skip mode is silent; note what passes
    if (!skipping_) {
      tts_->stop();
      skipping_ = true;
    }
    line_serial_ = presenter_->line_serial();
    menu_position_ = menu != nullptr ? std::optional{runtime_->position()} : std::nullopt;
    focus_ = focus;
    return;
  }
  skipping_ = false;

  // A line or a menu is spoken before the focus: the focus then follows silently.
  bool spoke = false;
  if (presenter_->line_serial() != line_serial_) {
    line_serial_ = presenter_->line_serial();
    if (const std::optional<DialogueLine>& line = presenter_->line()) {
      const std::string text = untagged(line->text);
      tts_->speak(line->speaker.empty() ? text : line->speaker + ": " + text);
      spoke = true;
    }
  }
  if (menu == nullptr) {
    menu_position_.reset();
  } else if (menu_position_ != runtime_->position()) {
    menu_position_ = runtime_->position();
    std::string said = "Choose";
    for (const std::string& option : menu->options) {
      said += ". " + (translator_ != nullptr
                          ? translator_->translate(runtime_->position().label, {}, option)
                          : option);
    }
    tts_->speak(said);
    spoke = true;
  }
  if (focus != focus_) {
    focus_ = focus;
    if (!spoke && !focus.empty()) {
      tts_->speak(focus);
    }
  }
}

}  // namespace vnmaki
