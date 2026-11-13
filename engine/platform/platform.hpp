#pragma once

#include <deque>
#include <optional>

#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"

namespace vnmaki {

/// The window size used when nothing else is asked for, in points.
inline constexpr Size kDefaultWindowSize{.width = 1280, .height = 720};

/// The engine's view of the OS: one window and the events it receives. The desktop backend is
/// SdlPlatform (sdl_platform.hpp); HeadlessPlatform stands in for tests and CI. Other targets
/// implement this interface.
///
/// A Platform is used from the main thread only.
class Platform {
 public:
  Platform() = default;
  Platform(const Platform&) = delete;
  Platform& operator=(const Platform&) = delete;
  Platform(Platform&&) = delete;
  Platform& operator=(Platform&&) = delete;
  /// Closes the window.
  virtual ~Platform() = default;

  /// The window's current size in points and pixels.
  [[nodiscard]] virtual WindowMetrics window_metrics() const = 0;

  /// The next pending event, or nullopt once the queue is empty. Never blocks.
  virtual std::optional<InputEvent> poll_event() = 0;

  /// Turns TextInputEvents on or off (and, on some systems, the on-screen keyboard or input
  /// method). Off by default; turn it on only while a text field has focus.
  virtual void set_text_input(bool enabled) = 0;

  /// Switches the window between fullscreen (on the desktop it is on) and windowed.
  virtual void set_fullscreen(bool fullscreen) = 0;

  /// Makes the whole window translucent: 1 is opaque, 0 invisible. A system that cannot do it
  /// ignores the call.
  virtual void set_window_opacity(float opacity) = 0;
};

/// A Platform with no window: nothing is drawn and the OS is never touched. Tests and CI inject
/// the events a window would have produced.
class HeadlessPlatform final : public Platform {
 public:
  /// `window_size` is the simulated window's size; it has as many pixels as points.
  explicit HeadlessPlatform(Size window_size = kDefaultWindowSize);

  [[nodiscard]] WindowMetrics window_metrics() const override;
  std::optional<InputEvent> poll_event() override;
  void set_text_input(bool enabled) override;
  void set_fullscreen(bool fullscreen) override { fullscreen_ = fullscreen; }
  void set_window_opacity(float opacity) override { opacity_ = opacity; }

  /// What the last set_fullscreen() and set_window_opacity() asked for (the defaults before).
  [[nodiscard]] bool fullscreen() const { return fullscreen_; }
  [[nodiscard]] float window_opacity() const { return opacity_; }

  /// Queues an event for poll_event(). A WindowResizedEvent also resizes the simulated window.
  void push_event(InputEvent event);

  [[nodiscard]] bool text_input_enabled() const;

 private:
  WindowMetrics metrics_;
  std::deque<InputEvent> events_;
  bool text_input_ = false;
  bool fullscreen_ = false;
  float opacity_ = 1;
};

}  // namespace vnmaki
