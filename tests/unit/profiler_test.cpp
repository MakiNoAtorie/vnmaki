// The profiler with a fake clock: exact phase times, the CSV text, and the load timing.

#include "engine/tools/profiler.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

#include <doctest/doctest.h>

#include "engine/core/load_timing.hpp"
#include "engine/platform/main_loop.hpp"

using namespace std::chrono_literals;
using vnmaki::Duration;

namespace {

class FakeClock final : public vnmaki::Clock {
 public:
  [[nodiscard]] Duration now() override { return now_; }
  void sleep_for(Duration duration) override { now_ += duration; }
  void advance(Duration duration) { now_ += duration; }

 private:
  Duration now_{1h};
};

constexpr Duration kEvents = 1ms;
constexpr Duration kUpdate = 2ms;
constexpr Duration kPrepare = 3ms;
constexpr Duration kRest = 4ms;  // the rest of the draw hook
constexpr Duration kSlowLoad = 5ms;
constexpr std::size_t kThreeFrames = 3;

// One frame the way story_window runs it: prepare is a part of the draw hook.
void run_frame(vnmaki::Profiler& profiler, FakeClock& clock) {
  profiler.begin_prepare();
  clock.advance(kPrepare);
  profiler.end_prepare();
  profiler.set_gpu({.passes = 2, .quads = 5, .draw_calls = 3},
                   {.texture_bytes = 64, .target_bytes = 128, .textures = 1, .targets = 1});
  profiler.end_frame(
      {.events = kEvents, .update = kUpdate, .draw = kPrepare + kRest, .updates = 2});
}

}  // namespace

TEST_CASE("profiler: a frame splits the draw hook into prepare and draw") {
  FakeClock clock;
  vnmaki::Profiler profiler{clock};
  run_frame(profiler, clock);

  REQUIRE(profiler.frames().size() == 1);
  const vnmaki::FrameSample& frame = profiler.frames().front();
  CHECK(frame.events == kEvents);
  CHECK(frame.update == kUpdate);
  CHECK(frame.prepare == kPrepare);
  CHECK(frame.draw == kRest);
  CHECK(frame.total() == kEvents + kUpdate + kPrepare + kRest);
  CHECK(frame.gpu.draw_calls == 3);
  CHECK(frame.memory.target_bytes == 128);
}

TEST_CASE("profiler: keeps the last frames; a disabled one keeps none") {
  FakeClock clock;
  vnmaki::Profiler ring{clock, {.max_frames = kThreeFrames}};
  constexpr int kFrames = 5;
  for (int i = 0; i < kFrames; ++i) {
    run_frame(ring, clock);
  }
  CHECK(ring.frames().size() == kThreeFrames);
  CHECK(ring.frames().front().frame == kFrames - kThreeFrames);
  CHECK(ring.frames().back().frame == kFrames - 1);

  vnmaki::Profiler everything{clock, {.enabled = true, .max_frames = 0}};
  for (int i = 0; i < kFrames; ++i) {
    run_frame(everything, clock);
  }
  CHECK(everything.frames().size() == kFrames);

  vnmaki::Profiler off{clock, {.enabled = false}};
  run_frame(off, clock);
  CHECK(off.frames().empty());
}

TEST_CASE("profiler: the frames CSV has a header and one row for each frame") {
  FakeClock clock;
  vnmaki::Profiler profiler{clock};
  run_frame(profiler, clock);
  run_frame(profiler, clock);
  std::ostringstream out;
  profiler.write_frames_csv(out);
  CHECK(out.str() ==
        "frame,events_us,update_us,prepare_us,draw_us,total_us,updates,passes,quads,draw_calls,"
        "texture_bytes,target_bytes,textures,targets\n"
        "0,1000,2000,3000,4000,10000,2,2,5,3,64,128,1,1\n"
        "1,1000,2000,3000,4000,10000,2,2,5,3,64,128,1,1\n");
}

TEST_CASE("profiler: records the loads of LoadTimer, slowest first, and stops with its sink") {
  FakeClock clock;
  {
    vnmaki::Profiler profiler{clock};
    { const vnmaki::LoadTimer timer{"images/a.png"}; }
    {
      const vnmaki::LoadTimer timer{"fonts/b,c.ttf"};
      std::this_thread::sleep_for(kSlowLoad);
    }
    REQUIRE(profiler.loads().size() == 2);
    const auto slowest = profiler.slowest_loads(1);
    REQUIRE(slowest.size() == 1);
    CHECK(slowest.front().path == "fonts/b,c.ttf");
    CHECK(slowest.front().time >= kSlowLoad);

    std::ostringstream out;
    profiler.write_loads_csv(out);
    CHECK(out.str().starts_with("path,time_us\nimages/a.png,"));
    CHECK(out.str().find("\"fonts/b,c.ttf\",") != std::string::npos);
  }
  // The profiler is gone and so is its sink: a timer does nothing.
  const vnmaki::LoadTimer timer{"after.png"};
}

TEST_CASE("profiler: the loads file is next to the frames file") {
  CHECK(vnmaki::loads_csv_path("out/run.csv") == std::filesystem::path{"out/run_loads.csv"});
  CHECK(vnmaki::loads_csv_path("p") == std::filesystem::path{"p_loads"});
}
