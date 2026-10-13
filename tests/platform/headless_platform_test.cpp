// Headless mode: a Platform with no window, for tests and CI. Events are injected.

#include <optional>

#include <doctest/doctest.h>

#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"
#include "tests/platform/printers.hpp"

using vnmaki::InputEvent;
using vnmaki::Size;

TEST_CASE("headless platform: reports its simulated window size as both points and pixels") {
  const vnmaki::HeadlessPlatform platform{Size{800, 600}};

  CHECK(platform.window_metrics() ==
        vnmaki::WindowMetrics{.points = {800, 600}, .pixels = {800, 600}});
}

TEST_CASE("headless platform: delivers injected events in order, then reports an empty queue") {
  vnmaki::HeadlessPlatform platform;
  const vnmaki::KeyEvent space_down{.key = vnmaki::Key::kSpace, .pressed = true};
  const vnmaki::PointerMoveEvent move{.position = {10, 20}};

  platform.push_event(space_down);
  platform.push_event(move);
  platform.push_event(vnmaki::QuitRequestedEvent{});

  CHECK(platform.poll_event() == std::optional<InputEvent>{space_down});
  CHECK(platform.poll_event() == std::optional<InputEvent>{move});
  CHECK(platform.poll_event() == std::optional<InputEvent>{vnmaki::QuitRequestedEvent{}});
  CHECK(platform.poll_event() == std::nullopt);
}

TEST_CASE("headless platform: an injected resize changes the window size like a real window") {
  vnmaki::HeadlessPlatform platform{Size{1280, 720}};

  platform.push_event(vnmaki::WindowResizedEvent{.pixels = {1920, 1080}});

  CHECK(platform.window_metrics().pixels == Size{1920, 1080});
  CHECK(platform.poll_event() ==
        std::optional<InputEvent>{vnmaki::WindowResizedEvent{.pixels = {1920, 1080}}});
}

TEST_CASE("headless platform: remembers whether text input is on") {
  vnmaki::HeadlessPlatform platform;
  CHECK_FALSE(platform.text_input_enabled());

  platform.set_text_input(true);
  CHECK(platform.text_input_enabled());

  platform.set_text_input(false);
  CHECK_FALSE(platform.text_input_enabled());
}
