#pragma once

// The desktop Platform (Windows, Linux, macOS), built on SDL3. This header does not include SDL;
// it only names SDL's window type so the renderer can attach a GPU device to the window.

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"

struct SDL_Window;
struct SDL_Gamepad;

namespace vnmaki {

/// Thrown when the platform cannot start (no video driver, window creation failed). The message
/// includes the backend's reason.
class PlatformError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct WindowConfig {
  std::string title = "VNMaki";
  /// Initial size in points (the OS's window coordinates).
  Size size{.width = 1280, .height = 720};
};

/// Opens a resizable, high-density-aware window and turns SDL events (keyboard, mouse, gamepad,
/// window) into InputEvents. Gamepads are opened as they connect.
///
/// SDL is global, so only one SdlPlatform may exist at a time. Create and use it on the main
/// thread. Headless runs need no SdlPlatform: use HeadlessPlatform. On Linux without a display,
/// set SDL_VIDEO_DRIVER=offscreen to get a window that is never shown.
class SdlPlatform final : public Platform {
 public:
  /// Starts SDL's video and gamepad subsystems and opens the window. Throws PlatformError.
  explicit SdlPlatform(const WindowConfig& config);
  /// Closes the window and gamepads and shuts SDL down.
  ~SdlPlatform() override;

  SdlPlatform(const SdlPlatform&) = delete;
  SdlPlatform& operator=(const SdlPlatform&) = delete;
  SdlPlatform(SdlPlatform&&) = delete;
  SdlPlatform& operator=(SdlPlatform&&) = delete;

  [[nodiscard]] WindowMetrics window_metrics() const override;
  std::optional<InputEvent> poll_event() override;
  void set_text_input(bool enabled) override;

  /// The SDL window, for the SDL GPU renderer backend. Never null.
  [[nodiscard]] SDL_Window* sdl_window() const;

 private:
  struct WindowCloser {
    void operator()(SDL_Window* window) const;
  };
  struct GamepadCloser {
    void operator()(SDL_Gamepad* gamepad) const;
  };
  /// Shuts the SDL subsystems down; declared first so it is destroyed last.
  struct SdlSubsystems {
    SdlSubsystems();
    ~SdlSubsystems();
    SdlSubsystems(const SdlSubsystems&) = delete;
    SdlSubsystems& operator=(const SdlSubsystems&) = delete;
    SdlSubsystems(SdlSubsystems&&) = delete;
    SdlSubsystems& operator=(SdlSubsystems&&) = delete;
  };

  SdlSubsystems subsystems_;
  std::unique_ptr<SDL_Window, WindowCloser> window_;
  std::vector<std::unique_ptr<SDL_Gamepad, GamepadCloser>> gamepads_;
};

}  // namespace vnmaki
