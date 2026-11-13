#include "engine/tools/hot_reload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/render/renderer.hpp"

namespace vnmaki {

namespace {

constexpr std::uint8_t kBannerRed = 176;
constexpr std::uint8_t kBannerGreen = 24;
constexpr std::uint8_t kBannerBlue = 24;
constexpr std::uint8_t kBannerAlpha = 240;
constexpr int kBannerHeightDivisor = 16;  // banner height: canvas height / 16
constexpr int kTextSizeDivisor = 36;      // text size: canvas height / 36
constexpr float kBannerPadding = 12;

constexpr std::string_view kScriptsKey = "scripts";
constexpr std::string_view kShaderDirectory = "shaders/";
constexpr std::string_view kShaderExtension = ".frag.hlsl";
constexpr std::array<std::string_view, 4> kImageExtensions{".png", ".jpg", ".jpeg", ".webp"};

bool has_suffix(std::string_view text, std::string_view suffix) { return text.ends_with(suffix); }

bool is_image(std::string_view path) {
  for (const std::string_view extension : kImageExtensions) {
    if (has_suffix(path, extension)) {
      return true;
    }
  }
  return false;
}

std::string_view first_line(std::string_view text) { return text.substr(0, text.find('\n')); }

}  // namespace

UiNode make_hot_reload_banner(Size canvas, std::string_view text) {
  UiNode label = make_node(NodeKind::kText);
  label.text = std::string{text};
  label.label = label.text;
  label.style.text_size = static_cast<float>(canvas.height / kTextSizeDivisor);

  UiNode panel = make_node(NodeKind::kBox);
  panel.style.position = PointF{};
  panel.style.width = static_cast<float>(canvas.width);
  panel.style.height = static_cast<float>(canvas.height / kBannerHeightDivisor);
  panel.style.padding = kBannerPadding;
  panel.style.align = Align::kStart;
  panel.style.justify = Justify::kCenter;
  panel.style.background = rgba8(kBannerRed, kBannerGreen, kBannerBlue, kBannerAlpha);
  panel.children.push_back(std::move(label));

  UiNode root = make_node(NodeKind::kBox);
  root.children.push_back(std::move(panel));
  return root;
}

void HotReload::report(const std::string& key, const std::string& problem) {
  if (problem.empty()) {
    errors_.erase(key);
    return;
  }
  errors_.insert_or_assign(key, problem);
  log_->add(LogLevel::kError, "hot reload: " + problem);
}

void HotReload::sync_banner() {
  banner_.clear();
  if (!errors_.empty()) {
    banner_ = "Hot reload failed: " + std::string{first_line(errors_.begin()->second)};
    if (errors_.size() > 1) {
      banner_ += " (and " + std::to_string(errors_.size() - 1) + " more, see the log)";
    }
  }
  if (banner_ == shown_) {
    return;
  }
  if (banner_.empty()) {
    static_cast<void>(ui_->hide(kHotReloadScreenName));
  } else {
    ui_->show(std::string{kHotReloadScreenName}, make_hot_reload_banner(ui_->canvas(), banner_),
              false);
  }
  shown_ = banner_;
}

void HotReload::apply(std::span<const std::string> paths) {
  bool scripts = false;
  for (const std::string& path : paths) {
    if (has_suffix(path, ".lua")) {
      scripts = true;
    } else if (path.starts_with(kShaderDirectory) && has_suffix(path, kShaderExtension)) {
      const std::string name = path.substr(
          kShaderDirectory.size(), path.size() - kShaderDirectory.size() - kShaderExtension.size());
      report(path, scene_->recompile_shader(name));
    } else if (is_image(path)) {
      report(path, scene_->reload_image(path, *jobs_));
    }
  }
  // The scripts go last: the replay then draws the scene with the new images and shaders.
  if (scripts) {
    const std::string problem = runtime_->reload_scripts();
    report(std::string{kScriptsKey},
           problem.empty() ? problem : problem + " (the old scripts stay)");
    if (problem.empty()) {
      log_->add(LogLevel::kInfo, "hot reload: scripts reloaded");
      if (on_scripts_reloaded_) {
        on_scripts_reloaded_();
      }
    }
  }
  sync_banner();
}

}  // namespace vnmaki
