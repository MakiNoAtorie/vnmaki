#include "engine/ui/backlog_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"

namespace vnmaki {

namespace {

constexpr int kMarginDivisor = 24;       // canvas edge to the list: width / 24
constexpr int kTitleSizeDivisor = 18;    // the "Backlog" heading: height / 18
constexpr int kHeadingSizeDivisor = 32;  // a speaker line: height / 32
constexpr int kBodySizeDivisor = 26;     // the text of an entry: height / 26
constexpr int kEntryGapDivisor = 90;     // space between entries: height / 90
constexpr int kEntryPaddingDivisor = 120;

constexpr std::uint8_t kShadeRed = 8;
constexpr std::uint8_t kShadeGreen = 8;
constexpr std::uint8_t kShadeBlue = 20;
constexpr std::uint8_t kShadeAlpha = 235;
constexpr std::uint8_t kSelectedRed = 60;
constexpr std::uint8_t kSelectedGreen = 70;
constexpr std::uint8_t kSelectedBlue = 120;
constexpr std::uint8_t kHoverRed = 40;
constexpr std::uint8_t kHoverGreen = 46;
constexpr std::uint8_t kHoverBlue = 84;
constexpr std::uint8_t kSpeakerRed = 200;
constexpr std::uint8_t kSpeakerGreen = 210;
constexpr std::uint8_t kSpeakerBlue = 255;

const Color kShade = rgba8(kShadeRed, kShadeGreen, kShadeBlue, kShadeAlpha);
const Color kSelectedFill = rgba8(kSelectedRed, kSelectedGreen, kSelectedBlue);
const Color kHoverFill = rgba8(kHoverRed, kHoverGreen, kHoverBlue);
const Color kSpeakerColor = rgba8(kSpeakerRed, kSpeakerGreen, kSpeakerBlue);

struct Metrics {
  float margin = 0;
  float title_size = 0;
  float heading_size = 0;
  float body_size = 0;
  float gap = 0;
  float padding = 0;
};

Metrics metrics_for(Size canvas) {
  const auto height = static_cast<float>(canvas.height);
  return {.margin = static_cast<float>(canvas.width / kMarginDivisor),
          .title_size = height / kTitleSizeDivisor,
          .heading_size = height / kHeadingSizeDivisor,
          .body_size = height / kBodySizeDivisor,
          .gap = height / kEntryGapDivisor,
          .padding = height / kEntryPaddingDivisor};
}

UiNode text_node(std::string text, float size, Color color, bool markup) {
  UiNode node = make_node(NodeKind::kText);
  node.label = text;
  node.text = std::move(text);
  node.markup = markup;
  node.style.text_size = size;
  node.style.color = color;
  return node;
}

// One entry: the speaker line (with a voice mark) over the text. A line with a voice is a button
// that plays it; its `returns` is the entry's index.
UiNode entry_node(const BacklogEntry& entry, std::size_t index, bool selected,
                  const Metrics& metrics) {
  UiNode node = make_node(entry.voice.empty() ? NodeKind::kBox : NodeKind::kButton);
  node.style.padding = metrics.padding;
  if (!entry.voice.empty()) {
    node.returns = static_cast<std::int64_t>(index);
    node.style.focus_background = kHoverFill;
  }
  if (selected) {
    node.style.background = kSelectedFill;
    node.style.focus_background = kSelectedFill;
  }
  std::string heading = entry.speaker;
  if (!entry.voice.empty()) {
    heading += heading.empty() ? "[voice]" : "  [voice]";
  }
  if (!heading.empty()) {
    node.children.push_back(
        text_node(std::move(heading), metrics.heading_size, kSpeakerColor, false));
  }
  node.children.push_back(text_node(entry.text, metrics.body_size, kWhite, true));
  node.label = entry.speaker.empty() ? entry.text : entry.speaker + ": " + entry.text;
  return node;
}

// The summed height of entries [from, to).
// ponytail: a sum over the range; prefix sums if lists of 10000 entries scroll slowly.
float span_height(const std::vector<float>& heights, std::size_t from, std::size_t to) {
  float total = 0;
  for (std::size_t i = from; i < to; ++i) {
    total += heights[i];
  }
  return total;
}

}  // namespace

BacklogScreen::BacklogScreen(UiHost& host, const Backlog& backlog, TextMeasure measure)
    : host_(&host), backlog_(&backlog), measure_(std::move(measure)) {}

void BacklogScreen::open() {
  entries_ = backlog_->entries();
  const Size canvas = host_->canvas();
  const Metrics metrics = metrics_for(canvas);
  const float wrap = static_cast<float>(canvas.width) - (2 * (metrics.margin + metrics.padding));
  heights_.clear();
  heights_.reserve(entries_.size());
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    float height = (2 * metrics.padding) + metrics.gap;
    for (const UiNode& child : entry_node(entries_[i], i, false, metrics).children) {
      height += measure_(child, wrap).height;
    }
    heights_.push_back(height);
  }
  const UiNode title = text_node("Backlog", metrics.title_size, kWhite, false);
  available_ = static_cast<float>(canvas.height) - (2 * metrics.margin) -
               measure_(title, wrap).height - metrics.gap;
  selected_ = entries_.empty() ? 0 : entries_.size() - 1;
  first_ = 0;
  open_ = true;
  rebuild();
}

void BacklogScreen::close() {
  if (open_) {
    host_->hide(kBacklogScreenName);
    open_ = false;
    entries_.clear();
  }
}

// Fits the window around the selected entry, then builds the screen from it.
void BacklogScreen::rebuild() {
  first_ = std::min(first_, selected_);
  while (first_ < selected_ && span_height(heights_, first_, selected_ + 1) > available_) {
    ++first_;
  }
  shown_ = entries_.empty() ? 0 : 1;
  while (first_ + shown_ < entries_.size() &&
         span_height(heights_, first_, first_ + shown_ + 1) <= available_) {
    ++shown_;
  }

  const Metrics metrics = metrics_for(host_->canvas());
  UiNode list = make_node(NodeKind::kVbox);
  list.style.gap = metrics.gap;
  list.style.grow = 1;
  for (std::size_t i = first_; i < first_ + shown_; ++i) {
    list.children.push_back(entry_node(entries_[i], i, i == selected_, metrics));
  }
  UiNode root = make_node(NodeKind::kVbox);
  root.style.background = kShade;
  root.style.padding = metrics.margin;
  root.style.gap = metrics.gap;
  root.children.push_back(text_node("Backlog", metrics.title_size, kWhite, false));
  root.children.push_back(std::move(list));
  host_->show(std::string{kBacklogScreenName}, std::move(root), false);
}

void BacklogScreen::select(std::size_t index) {
  selected_ = index;
  rebuild();
}

void BacklogScreen::move(std::ptrdiff_t delta) {
  if (entries_.empty()) {
    return;
  }
  const auto last = static_cast<std::ptrdiff_t>(entries_.size()) - 1;
  const std::ptrdiff_t target =
      std::clamp(static_cast<std::ptrdiff_t>(selected_) + delta, std::ptrdiff_t{0}, last);
  select(static_cast<std::size_t>(target));
}

void BacklogScreen::play(std::size_t index) const {
  if (index < entries_.size()) {
    // A voice file that is gone has nowhere to report to; the line stays readable.
    static_cast<void>(backlog_->play_voice(entries_[index]));
  }
}

bool BacklogScreen::handle_event(const InputEvent& event, const Viewport& viewport) {
  if (!open_) {
    const auto* key = std::get_if<KeyEvent>(&event);
    const auto* pad = std::get_if<GamepadButtonEvent>(&event);
    const bool wants_open =
        (key != nullptr && key->pressed && !key->repeat && key->key == Key::kL) ||
        (pad != nullptr && pad->pressed && pad->button == GamepadButton::kNorth);
    if (!wants_open || host_->has_modal()) {
      return false;
    }
    open();
    return true;
  }
  const auto page = static_cast<std::ptrdiff_t>(std::max<std::size_t>(shown_, 1));
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (!key->pressed) {
      return true;
    }
    switch (key->key) {
      case Key::kUp:
        move(-1);
        break;
      case Key::kDown:
        move(1);
        break;
      case Key::kPageUp:
        move(-page);
        break;
      case Key::kPageDown:
        move(page);
        break;
      case Key::kHome:
        select(0);
        break;
      case Key::kEnd:
        select(entries_.empty() ? 0 : entries_.size() - 1);
        break;
      case Key::kEnter:
      case Key::kKeypadEnter:
      case Key::kSpace:
        if (!key->repeat) {
          play(selected_);
        }
        break;
      case Key::kEscape:
      case Key::kL:
        if (!key->repeat) {
          close();
        }
        break;
      default:
        break;
    }
    return true;
  }
  if (const auto* wheel = std::get_if<WheelEvent>(&event)) {
    if (wheel->dy != 0) {
      move(wheel->dy > 0 ? -1 : 1);
    }
    return true;
  }
  if (const auto* pad = std::get_if<GamepadButtonEvent>(&event)) {
    if (!pad->pressed) {
      return true;
    }
    switch (pad->button) {
      case GamepadButton::kDpadUp:
        move(-1);
        break;
      case GamepadButton::kDpadDown:
        move(1);
        break;
      case GamepadButton::kLeftShoulder:
        move(-page);
        break;
      case GamepadButton::kRightShoulder:
        move(page);
        break;
      case GamepadButton::kSouth:
        play(selected_);
        break;
      case GamepadButton::kEast:
      case GamepadButton::kNorth:
        close();
        break;
      default:
        break;
    }
    return true;
  }
  if (std::holds_alternative<PointerMoveEvent>(event) ||
      std::holds_alternative<PointerButtonEvent>(event)) {
    const UiEventResult result = host_->handle_event(event, viewport);
    if (result.activation && result.activation->screen == kBacklogScreenName) {
      if (const auto* index = std::get_if<std::int64_t>(&result.activation->value)) {
        play(static_cast<std::size_t>(*index));
      }
    }
    return true;
  }
  return false;
}

}  // namespace vnmaki
