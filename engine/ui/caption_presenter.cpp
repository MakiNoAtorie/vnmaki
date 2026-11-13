#include "engine/ui/caption_presenter.hpp"

#include <algorithm>
#include <utility>

#include "engine/ui/ui_build.hpp"

namespace vnmaki {

CaptionPresenter::CaptionPresenter(ScriptRuntime& runtime, UiHost& host,
                                   std::function<void(std::string_view)> on_warning)
    : runtime_(&runtime), host_(&host), on_warning_(std::move(on_warning)) {}

std::vector<std::string> CaptionPresenter::shown() const {
  std::vector<std::string> texts;
  texts.reserve(active_.size());
  for (const AudioCaption& caption : active_) {
    texts.push_back(caption.text);
  }
  return texts;
}

void CaptionPresenter::update(std::vector<AudioCaption> fresh, double seconds, bool enabled) {
  if (!enabled) {
    active_.clear();
  } else {
    for (AudioCaption& caption : active_) {
      caption.seconds -= seconds;
    }
    std::erase_if(active_, [](const AudioCaption& caption) { return !(caption.seconds > 0); });
    for (AudioCaption& caption : fresh) {
      active_.push_back(std::move(caption));
    }
    if (active_.size() > kMaxCaptions) {
      active_.erase(active_.begin(), active_.end() - static_cast<std::ptrdiff_t>(kMaxCaptions));
    }
  }
  if (shown() != on_screen_) {
    on_screen_ = shown();
    show_screen();
  }
}

void CaptionPresenter::show_screen() {
  if (on_screen_.empty()) {
    host_->hide(kCaptionsScreenName);
    return;
  }
  ScreenNodeDef args{.kind = "args"};
  for (const std::string& text : on_screen_) {
    args.children.push_back({.kind = "args", .props = {{"text", text}}});
  }
  ScreenNodeDef tree;
  std::string problem = runtime_->build_screen(kCaptionsScreenName, args, tree);
  if (problem.empty()) {
    try {
      host_->show(std::string{kCaptionsScreenName}, build_ui_tree(tree), false);
    } catch (const UiBuildError& error) {
      problem = error.what();
    }
  }
  if (!problem.empty() && on_warning_) {
    on_warning_("screen 'captions': " + problem);
  }
}

}  // namespace vnmaki
