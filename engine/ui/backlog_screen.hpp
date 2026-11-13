#pragma once

// The backlog screen (docs/SPEC.md section 8): a read-only list of the lines the player has read,
// scrolled by keyboard, gamepad, wheel or pointer, with voice replay. It is an ordinary screen
// of the UiHost, so UiRenderer draws it with the others.
//
// The list is a scroll viewport at the granularity of whole lines: the screen holds the entries
// that fit the canvas, from `first_visible()` on, and a scroll moves the selected entry (the
// cursor) and so the window. UiRenderer has no clipping, so no entry is ever cut in half.
//
// While it is open, the screen takes every key, wheel, gamepad button and pointer event:
//   Up, Down, d-pad up/down, wheel   select the previous or next entry
//   Page Up, Page Down, shoulders    move one page
//   Home, End                        the oldest or the newest entry
//   Enter, Space, gamepad south      plays the voice of the selected entry
//   a click on an entry              plays its voice
//   Esc, L, gamepad east or north    close
// L and the gamepad's north button also open it (not while a modal screen is shown).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "engine/dialogue/backlog.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"

namespace vnmaki {

inline constexpr std::string_view kBacklogScreenName = "vnmaki:backlog";

class BacklogScreen {
 public:
  /// `host` and `backlog` must outlive this. `measure` sizes the text to decide how many entries
  /// fit; give UiRenderer::text_measure() in a window.
  BacklogScreen(UiHost& host, const Backlog& backlog, TextMeasure measure = estimate_text_size);

  /// Reads the entries and shows the screen with the newest one selected.
  void open();
  void close();
  /// Reads the entries again while the screen is open and the language (the backlog's translator
  /// revision) changed. The selected entry stays. Call it once a frame.
  void sync();
  [[nodiscard]] bool is_open() const { return open_; }

  /// Returns true if the event is used; the caller then does nothing else with it. `viewport`
  /// maps the pointer. Call it before the other screens and the story's inputs.
  [[nodiscard]] bool handle_event(const InputEvent& event, const Viewport& viewport);

  /// The index in the entries of the selected one, how many entries are shown and the index of
  /// the first one shown. Valid while open.
  [[nodiscard]] std::size_t selected() const { return selected_; }
  [[nodiscard]] std::size_t first_visible() const { return first_; }
  [[nodiscard]] std::size_t visible_count() const { return shown_; }
  [[nodiscard]] const std::vector<BacklogEntry>& entries() const { return entries_; }

 private:
  void load(bool keep_selection);
  void rebuild();
  void select(std::size_t index);
  void move(std::ptrdiff_t delta);
  void play(std::size_t index) const;

  UiHost* host_;
  const Backlog* backlog_;
  TextMeasure measure_;
  bool open_ = false;
  std::uint64_t revision_ = 0;  // the backlog revision that entries_ was read at
  std::vector<BacklogEntry> entries_{};
  std::vector<float> heights_{};  // the height of each entry as laid out, with its gap
  float available_ = 0;           // the height of the list area
  std::size_t selected_ = 0;
  std::size_t first_ = 0;
  std::size_t shown_ = 0;  // how many entries fit from first_
};

}  // namespace vnmaki
