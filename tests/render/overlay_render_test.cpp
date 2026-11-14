// The dev overlay on a real GPU device, headless: it draws one frame over a render target, F12
// shows and hides it, and a line typed into its console runs in the script sandbox.

#include <cstddef>
#include <memory>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/tools/overlay.hpp"
#include "engine/tools/screen_editor.hpp"
#include "engine/tools/story_graph.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::DevOverlay;
using vnmaki::HeadlessPlatform;
using vnmaki::Image;
using vnmaki::InputEvent;
using vnmaki::Key;
using vnmaki::KeyEvent;
using vnmaki::LogBuffer;
using vnmaki::LogLevel;
using vnmaki::PointerButtonEvent;
using vnmaki::PointerMoveEvent;
using vnmaki::ScriptRuntime;
using vnmaki::SdlGpuRenderer;
using vnmaki::Size;
using vnmaki::TextInputEvent;
using vnmaki::TextureId;
using vnmaki::TraceRecorder;
using vnmaki::Vfs;

constexpr Size kScreen{.width = 1280, .height = 720};
constexpr float kInsideConsole = 100;
constexpr float kOutsideConsole = 1000;
constexpr float kInsideGraph = 800;
// A probe node of the screen editor test: below the editor window, away from the other windows.
constexpr float kProbeX = 700;
constexpr float kProbeY = 560;
constexpr float kProbeSize = 100;
constexpr float kInsideGraphY = 100;
constexpr int kSettleFrames = 3;
// The tab bar of the inspectors: its y, and the x range and step of a sweep that clicks each tab.
constexpr float kTabBarY = 421;
constexpr float kTabsFromX = 26;
constexpr float kTabsToX = 450;
constexpr float kTabStepX = 15;
constexpr std::size_t kBytesPerPixel = 4;

KeyEvent key(Key which, bool pressed) { return {.key = which, .pressed = pressed}; }

// A headless renderer with a screen-sized target, and the parts the overlay needs.
struct Fixture {
  std::unique_ptr<SdlGpuRenderer> renderer = vnmaki::test::make_headless_renderer();
  HeadlessPlatform platform{kScreen};
  Vfs vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua", "store.gold = 5\nlabel('start', function() end)"}});
  LogBuffer log;
  ScriptRuntime runtime{vfs, {.observer = &log}};
  TextureId target{};

  Fixture() {
    runtime.load_scripts();
    if (renderer) {
      target = renderer->create_render_target(kScreen, vnmaki::TextureFilter::kLinear);
    }
  }

  // One frame: a black pass, then the overlay over it. Returns how many pixels are not black.
  std::size_t frame(DevOverlay& overlay) {
    renderer->begin_frame();
    renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
    overlay.draw(kScreen);
    static_cast<void>(renderer->end_frame());
    const Image image = renderer->read_pixels(target);
    std::size_t lit = 0;
    for (std::size_t i = 0; i + kBytesPerPixel <= image.pixels.size(); i += kBytesPerPixel) {
      if (image.pixels[i] != 0 || image.pixels[i + 1] != 0 || image.pixels[i + 2] != 0) {
        ++lit;
      }
    }
    return lit;
  }

  void settle(DevOverlay& overlay) {
    for (int i = 0; i < kSettleFrames; ++i) {
      static_cast<void>(frame(overlay));
    }
  }
};

}  // namespace

TEST_CASE("overlay: a visible overlay draws one frame, a hidden one nothing") {
  Fixture fixture;
  if (!fixture.renderer) {
    return;
  }
  DevOverlay overlay{*fixture.renderer, fixture.platform, fixture.runtime, fixture.log};
  fixture.log.add(LogLevel::kWarning, "something to read");
  CHECK_FALSE(overlay.visible());
  CHECK(fixture.frame(overlay) == 0);
  overlay.set_visible(true);
  CHECK(fixture.frame(overlay) > 0);
  overlay.set_visible(false);
  CHECK(fixture.frame(overlay) == 0);
}

TEST_CASE("overlay: F12 toggles it and never reaches the game") {
  Fixture fixture;
  if (!fixture.renderer) {
    return;
  }
  DevOverlay overlay{*fixture.renderer, fixture.platform, fixture.runtime, fixture.log};
  CHECK_FALSE(overlay.handle_event(key(Key::kSpace, true)));  // hidden: the game gets it
  CHECK(overlay.handle_event(key(Key::kF12, true)));
  CHECK(overlay.visible());
  CHECK(overlay.handle_event(key(Key::kF12, false)));
  CHECK(overlay.visible());
  CHECK(overlay.handle_event(key(Key::kF12, true)));
  CHECK_FALSE(overlay.visible());
}

TEST_CASE("overlay: the pointer is taken over a window and passes outside") {
  Fixture fixture;
  if (!fixture.renderer) {
    return;
  }
  DevOverlay overlay{*fixture.renderer, fixture.platform, fixture.runtime, fixture.log};
  overlay.set_visible(true);
  // ImGui learns where the pointer is, and which window is under it, by drawing frames. So a
  // second move, after the frames, shows what the first one did.
  const auto taken_at = [&](float x) {
    const InputEvent event = PointerMoveEvent{.position = {.x = x, .y = x}};
    static_cast<void>(overlay.handle_event(event));
    fixture.settle(overlay);
    return overlay.handle_event(event);
  };
  CHECK(taken_at(kInsideConsole));
  CHECK_FALSE(taken_at(kOutsideConsole));
}

TEST_CASE("overlay: a line typed into the console runs in the sandbox") {
  Fixture fixture;
  if (!fixture.renderer) {
    return;
  }
  DevOverlay overlay{*fixture.renderer, fixture.platform, fixture.runtime, fixture.log};
  overlay.set_visible(true);
  fixture.settle(overlay);
  CHECK(fixture.platform.text_input_enabled());  // the console field has the keyboard
  CHECK(overlay.handle_event(TextInputEvent{.text = "store.gold + 1"}));
  CHECK(overlay.handle_event(key(Key::kEnter, true)));
  fixture.settle(overlay);
  static_cast<void>(overlay.handle_event(key(Key::kEnter, false)));
  fixture.settle(overlay);
  CHECK(fixture.platform.text_input_enabled());  // the field keeps the keyboard for the next line
  const auto& entries = fixture.log.entries();
  REQUIRE(entries.size() >= 2);
  CHECK(entries.at(entries.size() - 2).text == "> store.gold + 1");
  CHECK(entries.back().text == "6");
  overlay.set_visible(false);
  CHECK_FALSE(fixture.platform.text_input_enabled());
}

TEST_CASE("overlay: every inspector tab draws with a scene and audio") {
  Fixture fixture;
  if (!fixture.renderer) {
    return;
  }
  vnmaki::Scene scene{fixture.vfs, kScreen};
  vnmaki::Audio audio{fixture.vfs};
  DevOverlay overlay{*fixture.renderer, fixture.platform, fixture.runtime, fixture.log};
  overlay.set_inspect_targets(&scene, &audio);
  overlay.set_visible(true);
  REQUIRE(fixture.runtime.eval("store.bag = { a = { b = 1 }, c = 2 }").ok);
  fixture.settle(overlay);
  // Click along the tab bar, so that each tab is drawn for a frame (a bad Push or Pop in a tab
  // asserts in ImGui, and the test fails).
  for (float x = kTabsFromX; x < kTabsToX; x += kTabStepX) {
    const vnmaki::PointF at{.x = x, .y = kTabBarY};
    static_cast<void>(overlay.handle_event(PointerMoveEvent{.position = at}));
    fixture.settle(overlay);
    static_cast<void>(overlay.handle_event(
        PointerButtonEvent{.button = vnmaki::MouseButton::kLeft, .pressed = true, .position = at}));
    fixture.settle(overlay);
    static_cast<void>(overlay.handle_event(PointerButtonEvent{
        .button = vnmaki::MouseButton::kLeft, .pressed = false, .position = at}));
    fixture.settle(overlay);
  }
  CHECK(fixture.frame(overlay) > 0);
}

TEST_CASE("overlay: the story graph window draws and follows the trace") {
  Fixture fixture;
  if (!fixture.renderer) {
    return;
  }
  DevOverlay overlay{*fixture.renderer, fixture.platform, fixture.runtime, fixture.log};
  overlay.set_visible(true);
  fixture.settle(overlay);
  const std::size_t console_only = fixture.frame(overlay);

  TraceRecorder trace;
  overlay.set_story(fixture.vfs, trace);
  fixture.settle(overlay);
  const std::size_t with_graph = fixture.frame(overlay);
  CHECK(with_graph > console_only);

  // The window takes the pointer where the console does not reach.
  const InputEvent inside = PointerMoveEvent{.position = {.x = kInsideGraph, .y = kInsideGraphY}};
  static_cast<void>(overlay.handle_event(inside));
  fixture.settle(overlay);
  CHECK(overlay.handle_event(inside));

  // A label entered at run time changes the revision, so the window builds its rows again.
  trace.on_label_entered("start");
  trace.on_label_entered("start");
  fixture.settle(overlay);
  CHECK(fixture.frame(overlay) > 0);
}

TEST_CASE(
    "overlay: with Pick on, a click on a node selects it, outlines it and is not the game's") {
  Fixture fixture;
  if (!fixture.renderer) {
    return;
  }
  vnmaki::UiHost host{kScreen};
  vnmaki::UiNode probe = vnmaki::make_node(vnmaki::NodeKind::kBox);
  probe.style.width = kProbeSize;
  probe.style.height = kProbeSize;
  probe.style.position = vnmaki::PointF{.x = kProbeX, .y = kProbeY};
  host.show("probe", std::move(probe), false);
  vnmaki::ScreenEditor editor{fixture.vfs, {}, host};
  DevOverlay overlay{*fixture.renderer, fixture.platform, fixture.runtime, fixture.log};
  overlay.set_screen_editor(
      &editor, [] { return vnmaki::compute_viewport(kScreen, kScreen, vnmaki::ScaleMode::kFit); });
  overlay.set_visible(true);
  const vnmaki::PointF inside{.x = kProbeX + (kProbeSize / 2), .y = kProbeY + (kProbeSize / 2)};
  const InputEvent move = PointerMoveEvent{.position = inside};
  static_cast<void>(overlay.handle_event(move));
  fixture.settle(overlay);
  const InputEvent press = PointerButtonEvent{.pressed = true, .position = inside};

  CHECK_FALSE(overlay.handle_event(press));  // Pick is off: the game gets the click
  CHECK_FALSE(editor.has_selection());

  overlay.set_pick_mode(true);
  CHECK(overlay.handle_event(press));
  REQUIRE(editor.has_selection());
  CHECK(editor.screen_name() == "probe");
  const std::size_t outlined = fixture.frame(overlay);
  CHECK(outlined > 0);  // the window and the outline draw with a selection

  const vnmaki::PointF outside{.x = kProbeX - kProbeSize, .y = kProbeY};  // no node, no window
  CHECK(overlay.handle_event(PointerButtonEvent{.pressed = true, .position = outside}));
  CHECK_FALSE(editor.has_selection());
}
