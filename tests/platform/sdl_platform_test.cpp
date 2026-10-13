// The SDL desktop platform with a real SDL window. On Linux, CTest runs this binary with SDL's
// offscreen video driver (no display needed); on Windows and macOS it uses the native one.

#include "engine/platform/sdl_platform.hpp"

#include <optional>
#include <vector>

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "tests/platform/printers.hpp"

using vnmaki::InputEvent;
using vnmaki::Size;

namespace {

std::vector<InputEvent> drain(vnmaki::Platform& platform) {
  std::vector<InputEvent> events;
  while (auto event = platform.poll_event()) {
    events.push_back(*event);
  }
  return events;
}

bool contains(const std::vector<InputEvent>& events, const InputEvent& wanted) {
  for (const auto& event : events) {
    if (event == wanted) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("sdl platform: opens a window of the requested size") {
  vnmaki::SdlPlatform platform{{.title = "vnmaki test", .size = {640, 360}}};

  REQUIRE(platform.sdl_window() != nullptr);
  const auto metrics = platform.window_metrics();
  CHECK(metrics.points == Size{640, 360});
  CHECK(metrics.display_scale > 0);

  // The pixel size is the point size times the window's pixel density (1 on a standard
  // display, 2 on a Retina one), as SDL itself reports it.
  const float density = SDL_GetWindowPixelDensity(platform.sdl_window());
  CHECK(static_cast<float>(metrics.pixels.width) == doctest::Approx(640 * density));
  CHECK(static_cast<float>(metrics.pixels.height) == doctest::Approx(360 * density));
}

TEST_CASE("sdl platform: the window is resizable and high-density aware") {
  vnmaki::SdlPlatform platform{{.title = "vnmaki test", .size = {640, 360}}};

  const SDL_WindowFlags flags = SDL_GetWindowFlags(platform.sdl_window());
  CHECK((flags & SDL_WINDOW_RESIZABLE) != 0);
  CHECK((flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0);
}

TEST_CASE("sdl platform: closing and reopening the window works (SDL shuts down cleanly)") {
  { const vnmaki::SdlPlatform first{{.title = "first", .size = {320, 200}}}; }
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);

  const vnmaki::SdlPlatform second{{.title = "second", .size = {320, 200}}};
  CHECK(second.window_metrics().points == Size{320, 200});
}

TEST_CASE("sdl platform: SDL events come out of poll_event as engine events") {
  vnmaki::SdlPlatform platform{{.title = "vnmaki test", .size = {640, 360}}};
  drain(platform);  // Window creation events.

  SDL_Event key{};
  key.key.type = SDL_EVENT_KEY_DOWN;
  key.key.key = SDLK_RETURN;
  key.key.down = true;
  REQUIRE(SDL_PushEvent(&key));
  SDL_Event quit{};
  quit.type = SDL_EVENT_QUIT;
  REQUIRE(SDL_PushEvent(&quit));

  const auto events = drain(platform);
  CHECK(contains(events, vnmaki::KeyEvent{.key = vnmaki::Key::kEnter, .pressed = true}));
  CHECK(contains(events, vnmaki::QuitRequestedEvent{}));
  CHECK(platform.poll_event() == std::nullopt);
}

TEST_CASE("sdl platform: resizing the window reports the new pixel size") {
  vnmaki::SdlPlatform platform{{.title = "vnmaki test", .size = {640, 360}}};
  drain(platform);

  REQUIRE(SDL_SetWindowSize(platform.sdl_window(), 800, 600));
  REQUIRE(SDL_SyncWindow(platform.sdl_window()));

  const auto metrics = platform.window_metrics();
  CHECK(metrics.points == Size{800, 600});
  CHECK(contains(drain(platform), vnmaki::WindowResizedEvent{.pixels = metrics.pixels}));
}

TEST_CASE("sdl platform: text input can be switched on and off") {
  vnmaki::SdlPlatform platform{{.title = "vnmaki test", .size = {640, 360}}};

  platform.set_text_input(true);
  CHECK(SDL_TextInputActive(platform.sdl_window()));
  platform.set_text_input(false);
  CHECK_FALSE(SDL_TextInputActive(platform.sdl_window()));
}

TEST_CASE("sdl platform: failing to start SDL video throws PlatformError with SDL's reason") {
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "no-such-driver", SDL_HINT_OVERRIDE);

  CHECK_THROWS_AS(vnmaki::SdlPlatform({.title = "never", .size = {320, 200}}),
                  vnmaki::PlatformError);

  SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
}
