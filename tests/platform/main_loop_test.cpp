// The main loop's scheduling: fixed-timestep updates, one draw per frame, events first. Time comes
// from a fake clock the tests advance by hand, so nothing here depends on real time.

#include "engine/platform/main_loop.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"

using namespace std::chrono_literals;
using vnmaki::Duration;

namespace {

/// A clock that only moves when told to. sleep_for() moves it and records the request.
class FakeClock final : public vnmaki::Clock {
 public:
  [[nodiscard]] Duration now() override { return now_; }
  void sleep_for(Duration duration) override {
    sleeps.push_back(duration);
    now_ += duration;
  }
  void advance(Duration duration) { now_ += duration; }

  std::vector<Duration> sleeps;

 private:
  Duration now_{1h};  // Not zero, so nothing relies on the clock starting at zero.
};

/// Records what the loop called, in order.
struct Recorder {
  std::vector<std::string> calls;
  std::vector<Duration> steps;
  std::vector<double> alphas;
  std::vector<vnmaki::InputEvent> events;

  [[nodiscard]] vnmaki::MainLoopHooks hooks() {
    return {
        .on_event =
            [this](const vnmaki::InputEvent& event) {
              calls.emplace_back("event");
              events.push_back(event);
            },
        .update =
            [this](Duration step) {
              calls.emplace_back("update");
              steps.push_back(step);
            },
        .draw =
            [this](double alpha) {
              calls.emplace_back("draw");
              alphas.push_back(alpha);
            },
    };
  }

  [[nodiscard]] std::size_t count(const std::string& call) const {
    std::size_t n = 0;
    for (const auto& c : calls) {
      n += c == call ? 1 : 0;
    }
    return n;
  }
};

constexpr Duration kStep = 10ms;

}  // namespace

TEST_CASE("main loop: runs one fixed update per step of elapsed time, then draws once") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};
  Recorder recorder;

  clock.advance(30ms);
  CHECK(loop.run_frame(recorder.hooks()));

  CHECK(recorder.calls == std::vector<std::string>{"update", "update", "update", "draw"});
  CHECK(recorder.steps == std::vector<Duration>{kStep, kStep, kStep});
}

TEST_CASE("main loop: carries leftover time into the next frame and draws with its fraction") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};
  Recorder recorder;

  clock.advance(25ms);  // 2 steps, 5 ms left over.
  loop.run_frame(recorder.hooks());
  CHECK(recorder.count("update") == 2);
  CHECK(recorder.alphas.back() == doctest::Approx(0.5));

  clock.advance(5ms);  // 5 + 5 = one more step, nothing left over.
  loop.run_frame(recorder.hooks());
  CHECK(recorder.count("update") == 3);
  CHECK(recorder.alphas.back() == doctest::Approx(0));
}

TEST_CASE("main loop: draws every frame even when no update is due") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};
  Recorder recorder;

  clock.advance(4ms);
  loop.run_frame(recorder.hooks());

  CHECK(recorder.calls == std::vector<std::string>{"draw"});
  CHECK(recorder.alphas.back() == doctest::Approx(0.4));
}

TEST_CASE("main loop: after a long stall, caps the updates and drops the backlog") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep, .max_updates_per_frame = 4}};
  Recorder recorder;

  clock.advance(1s + 3ms);  // A debugger pause: 100 steps due.
  loop.run_frame(recorder.hooks());
  CHECK(recorder.count("update") == 4);
  CHECK(recorder.alphas.back() == doctest::Approx(0.3));  // The sub-step remainder survives.

  clock.advance(10ms);  // Back to normal: the dropped steps do not come back.
  loop.run_frame(recorder.hooks());
  CHECK(recorder.count("update") == 5);
}

TEST_CASE("main loop: delivers pending events before the frame's updates and draw") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};
  Recorder recorder;
  const vnmaki::KeyEvent enter{.key = vnmaki::Key::kEnter, .pressed = true};

  platform.push_event(enter);
  platform.push_event(vnmaki::PointerMoveEvent{.position = {1, 2}});
  clock.advance(kStep);
  loop.run_frame(recorder.hooks());

  CHECK(recorder.calls == std::vector<std::string>{"event", "event", "update", "draw"});
  CHECK(recorder.events.front() == vnmaki::InputEvent{enter});
}

TEST_CASE("main loop: a quit request ends the loop after the event is delivered") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};
  Recorder recorder;

  platform.push_event(vnmaki::QuitRequestedEvent{});
  clock.advance(kStep);

  CHECK_FALSE(loop.run_frame(recorder.hooks()));
  CHECK(recorder.calls == std::vector<std::string>{"event"});
  CHECK(loop.quit_requested());
}

TEST_CASE("main loop: run() stops after the frame limit") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};
  Recorder recorder;

  constexpr std::uint64_t kFrames = 3;
  CHECK(loop.run(recorder.hooks(), kFrames) == kFrames);

  CHECK(recorder.count("draw") == kFrames);
}

TEST_CASE("main loop: run() stops when a hook requests quit") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};
  int draws = 0;
  constexpr int kQuitAfterDraws = 2;

  const auto frames = loop.run({.draw = [&](double /*alpha*/) {
    if (++draws == kQuitAfterDraws) {
      loop.request_quit();
    }
  }});

  CHECK(frames == kQuitAfterDraws);
}

TEST_CASE("main loop: a frame cap sleeps away the rest of a short frame") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep, .min_frame_time = 16ms}};

  // The frame's own work takes 6 ms of clock time.
  loop.run_frame({.draw = [&](double /*alpha*/) { clock.advance(6ms); }});
  CHECK(clock.sleeps == std::vector<Duration>{10ms});

  // A frame that already took longer than the cap does not sleep.
  loop.run_frame({.draw = [&](double /*alpha*/) { clock.advance(20ms); }});
  CHECK(clock.sleeps.size() == 1);
}

TEST_CASE("main loop: rejects a step or update cap that would never let time advance") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;

  CHECK_THROWS_AS(vnmaki::MainLoop(platform, clock, {.update_step = 0ms}), std::invalid_argument);
  CHECK_THROWS_AS(vnmaki::MainLoop(platform, clock, {.max_updates_per_frame = 0}),
                  std::invalid_argument);
}

TEST_CASE("main loop: without a frame cap it never sleeps") {
  vnmaki::HeadlessPlatform platform;
  FakeClock clock;
  vnmaki::MainLoop loop{platform, clock, {.update_step = kStep}};

  loop.run_frame({});

  CHECK(clock.sleeps.empty());
}
