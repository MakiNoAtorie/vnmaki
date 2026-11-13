#include "engine/ui/playback_indicator.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "engine/render/renderer.hpp"

namespace vnmaki {

namespace {

constexpr int kMarginDivisor = 64;    // gap to the canvas edge: width / 64
constexpr int kWidthDivisor = 8;      // label width: width / 8
constexpr int kHeightDivisor = 16;    // label height: height / 16
constexpr int kTextSizeDivisor = 32;  // text size: height / 32
constexpr std::uint8_t kBackgroundRed = 16;
constexpr std::uint8_t kBackgroundGreen = 16;
constexpr std::uint8_t kBackgroundBlue = 32;
constexpr std::uint8_t kBackgroundAlpha = 204;

}  // namespace

std::string playback_indicator_text(SkipMode skip, bool automatic) {
  const std::string skip_text = skip == SkipMode::kAll ? "SKIP ALL" : "SKIP";
  if (skip == SkipMode::kOff) {
    return automatic ? "AUTO" : "";
  }
  return automatic ? "AUTO " + skip_text : skip_text;
}

UiNode make_playback_screen(Size canvas, std::string_view text) {
  const int margin = canvas.width / kMarginDivisor;
  const int width = canvas.width / kWidthDivisor;

  UiNode label = make_node(NodeKind::kText);
  label.text = std::string{text};
  label.label = label.text;
  label.style.text_size = static_cast<float>(canvas.height / kTextSizeDivisor);

  UiNode panel = make_node(NodeKind::kBox);
  panel.style.position = PointF{.x = static_cast<float>(canvas.width - margin - width),
                                .y = static_cast<float>(margin)};
  panel.style.width = static_cast<float>(width);
  panel.style.height = static_cast<float>(canvas.height / kHeightDivisor);
  panel.style.align = Align::kCenter;
  panel.style.justify = Justify::kCenter;
  panel.style.background =
      rgba8(kBackgroundRed, kBackgroundGreen, kBackgroundBlue, kBackgroundAlpha);
  panel.children.push_back(std::move(label));

  UiNode root = make_node(NodeKind::kBox);
  root.children.push_back(std::move(panel));
  return root;
}

void PlaybackIndicator::sync(SkipMode skip, bool automatic) {
  std::string text = playback_indicator_text(skip, automatic);
  if (text == shown_) {
    return;
  }
  if (text.empty()) {
    host_->hide(kPlaybackScreenName);
  } else {
    host_->show(std::string{kPlaybackScreenName}, make_playback_screen(host_->canvas(), text),
                false);
  }
  shown_ = std::move(text);
}

}  // namespace vnmaki
