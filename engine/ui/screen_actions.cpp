#include "engine/ui/screen_actions.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/dialogue/playback.hpp"
#include "engine/ui/backlog_screen.hpp"
#include "engine/ui/ui_build.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kQuitQuestion = "Quit the game?";
constexpr std::string_view kBrokenTitle = "Damaged save";

ScreenNodeDef text_args(std::vector<std::pair<std::string, ScreenValue>> props) {
  return {.kind = "args", .props = std::move(props)};
}

}  // namespace

ScreenActions::ScreenActions(ScriptRuntime& runtime, UiHost& host, Saves& saves,
                             ScreenActionTargets targets)
    : runtime_(&runtime), host_(&host), saves_(&saves), targets_(std::move(targets)) {}

bool ScreenActions::is_action(const ScreenValue& value) {
  const auto* text = std::get_if<std::string>(&value);
  return text != nullptr && !text->empty() && text->front() == '@';
}

void ScreenActions::warn(std::string_view message) const {
  if (targets_.on_warning) {
    targets_.on_warning(message);
  }
}

ScreenNodeDef ScreenActions::slot_args(bool saving) {
  ScreenNodeDef args = text_args({{"mode", std::string{saving ? "save" : "load"}}});
  const std::vector<SlotInfo> list = saves_->list();
  thumbnails_.clear();
  const auto add = [&](const std::string& slot) {
    const auto found = std::ranges::find(list, slot, &SlotInfo::slot);
    const bool used = found != list.end();
    const bool broken = used && !found->error.empty();
    const bool has_image = used && !broken && !found->thumbnail.rgba.empty();
    const std::string image =
        has_image ? "gen:save:" + slot + "@" + std::to_string(found->time) : std::string{};
    std::string title;
    if (used) {
      title = broken ? std::string{kBrokenTitle} : found->title;
    }
    args.children.push_back(text_args({{"slot", slot},
                                       {"title", title},
                                       {"time", used ? found->time : std::int64_t{0}},
                                       {"empty", !used},
                                       {"broken", broken},
                                       {"thumbnail", image}}));
    if (has_image) {
      thumbnails_.insert_or_assign(image, found->thumbnail);
    }
  };
  for (int slot = 1; slot <= kScreenSlotCount; ++slot) {
    add(std::to_string(slot));
  }
  if (!saving) {  // the slots the engine writes by itself
    for (const std::string_view extra : {kAutoSlot, kQuickSlot}) {
      if (saves_->exists(extra)) {
        add(std::string{extra});
      }
    }
  }
  return args;
}

std::optional<Image> ScreenActions::thumbnail(const std::string& image_name) const {
  const auto found = thumbnails_.find(image_name);
  if (found == thumbnails_.end()) {
    return std::nullopt;
  }
  return Image{.size = {.width = found->second.width, .height = found->second.height},
               .pixels = found->second.rgba};
}

std::string ScreenActions::open(std::string_view name, const ScreenNodeDef& args, bool modal) {
  ScreenNodeDef tree;
  std::string problem = runtime_->build_screen(name, args, tree);
  if (problem.empty()) {
    try {
      host_->show(std::string{name}, build_ui_tree(tree), modal);
    } catch (const UiBuildError& error) {
      problem = "screen '" + std::string{name} + "': " + error.what();
    }
  }
  if (!problem.empty()) {
    warn(problem);
    return problem;
  }
  std::erase(opened_, name);
  if (modal) {
    opened_.emplace_back(name);
    opened_args_.insert_or_assign(std::string{name}, args);
  }
  return {};
}

void ScreenActions::ask(std::string_view message, std::string yes, std::string no) {
  static_cast<void>(open("confirm", text_args({{"message", std::string{message}},
                                               {"yes", std::move(yes)},
                                               {"no", std::move(no)}})));
}

void ScreenActions::close_all() {
  for (const std::string& name : opened_) {
    host_->hide(name);
  }
  opened_.clear();
}

void ScreenActions::do_save(const std::string& slot) {
  const std::string problem = saves_->save(slot);
  if (problem.empty()) {
    close_all();
  } else {
    warn("save " + slot + ": " + problem);
  }
}

bool ScreenActions::handle(const ScreenActivation& activation) {
  if (!is_action(activation.value)) {
    return false;
  }
  const std::string_view action =
      std::string_view{std::get<std::string>(activation.value)}.substr(1);
  const std::size_t split = action.find_first_of(":!");
  const std::string_view verb = action.substr(0, split);
  const bool confirmed = split != std::string_view::npos && action[split] == '!';
  const std::string argument{split == std::string_view::npos ? std::string_view{}
                                                             : action.substr(split + 1)};
  if (verb == "open") {
    static_cast<void>(open(argument, argument == "save" || argument == "load"
                                         ? slot_args(argument == "save")
                                         : ScreenNodeDef{}));
  } else if (verb == "view") {
    static_cast<void>(open("gallery_view", text_args({{"image", argument}}), true));
  } else if (verb == "close") {
    host_->hide(activation.screen);
    std::erase(opened_, activation.screen);
  } else if (verb == "save") {
    if (confirmed || !saves_->exists(argument)) {
      do_save(argument);
    } else {
      ask("Replace the save in slot " + argument + "?", "@save!" + argument, "@close");
    }
  } else if (verb == "load") {
    const std::string problem = saves_->load(argument);
    if (problem.empty()) {
      close_all();
    } else {
      warn("load " + argument + ": " + problem);
    }
  } else if (verb == "auto") {
    if (targets_.playback != nullptr) {
      targets_.playback->set_auto(!targets_.playback->automatic());
    }
  } else if (verb == "skip") {
    if (targets_.playback != nullptr) {
      const ScreenValue unseen = runtime_->binding("prefs", "skip_unseen");
      const auto* skip_unseen = std::get_if<bool>(&unseen);
      targets_.playback->toggle_skip(skip_unseen != nullptr && *skip_unseen ? SkipMode::kAll
                                                                            : SkipMode::kSeen);
    }
  } else if (verb == "backlog") {
    if (targets_.backlog != nullptr) {
      targets_.backlog->open();
    }
  } else if (verb == "language") {
    if (const std::string problem = runtime_->set_binding("prefs", "language", argument);
        !problem.empty()) {
      warn(problem);
    } else if (targets_.language) {
      targets_.language(argument);
      // The screen of the button is built again, so that the styles of the language apply to it.
      if (const auto args = opened_args_.find(activation.screen); args != opened_args_.end()) {
        const ScreenNodeDef again = args->second;
        static_cast<void>(open(activation.screen, again));
      }
    }
  } else if (verb == "quit") {
    if (confirmed) {
      if (targets_.quit) {
        targets_.quit();
      }
    } else {
      ask(kQuitQuestion, "@quit!", "@close");
    }
  } else {
    warn("unknown screen action '" + std::string{action} + "'");
  }
  return true;
}

}  // namespace vnmaki
