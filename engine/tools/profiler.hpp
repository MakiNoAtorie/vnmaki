#pragma once

// The profiler (docs/SPEC.md section 18): the time of each phase of every frame, the GPU work and
// memory, and the asset loads. It needs no ImGui, so `vnmaki run --profile` works in every build;
// the overlay panel that shows it is dev-only (overlay.hpp).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

#include "engine/platform/main_loop.hpp"
#include "engine/render/renderer.hpp"

namespace vnmaki {

/// One frame. `draw` is the draw hook without `prepare`.
struct FrameSample {
  std::uint64_t frame = 0;
  Duration events{0};
  Duration update{0};
  Duration prepare{0};
  Duration draw{0};
  int updates = 0;
  FrameStats gpu{};
  TextureMemory memory{};

  [[nodiscard]] Duration total() const { return events + update + prepare + draw; }
};

/// One asset load: the VFS path and the time to read and decode it.
struct LoadSample {
  std::string path;
  Duration time{0};
};

struct ProfilerConfig {
  /// A disabled profiler records nothing and sets no load sink.
  bool enabled = true;
  /// How many of the last frames it keeps; zero keeps all of them (for the CSV file).
  std::size_t max_frames = 300;
};

/// Records frames and loads. The frame calls are for the main thread; the loads come from job
/// threads and are safe. Only one enabled profiler can exist, because there is one load sink
/// (engine/core/load_timing.hpp).
class Profiler {
 public:
  /// The length of the frame graph.
  static constexpr std::size_t kGraphFrames = 300;
  /// At most this many loads are kept; later ones are dropped.
  static constexpr std::size_t kMaxLoads = 10'000;

  explicit Profiler(Clock& clock, ProfilerConfig config = {});
  ~Profiler();
  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;
  Profiler(Profiler&&) = delete;
  Profiler& operator=(Profiler&&) = delete;

  [[nodiscard]] bool enabled() const { return config_.enabled; }

  /// Marks the start and the end of the prepare phase inside the draw hook (the uploads and the
  /// layout before the first pass).
  void begin_prepare();
  void end_prepare();
  /// The renderer's numbers for the frame that ends now.
  void set_gpu(const FrameStats& gpu, const TextureMemory& memory);
  /// Closes the frame: adds a sample from the loop's timing and the marks.
  void end_frame(const FrameTiming& timing);

  /// The last frames, oldest first.
  [[nodiscard]] const std::deque<FrameSample>& frames() const { return frames_; }
  /// All loads so far, in the order they ended.
  [[nodiscard]] std::vector<LoadSample> loads() const;
  /// The `count` slowest loads, slowest first.
  [[nodiscard]] std::vector<LoadSample> slowest_loads(std::size_t count) const;

  /// The header `frame,events_us,update_us,prepare_us,draw_us,total_us,updates,passes,quads,
  /// draw_calls,texture_bytes,target_bytes,textures,targets`, then a row for each kept frame.
  void write_frames_csv(std::ostream& out) const;
  /// The header `path,time_us`, then a row for each load.
  void write_loads_csv(std::ostream& out) const;
  /// Writes `frames_csv` and, next to it, the loads file (see loads_csv_path). Returns the reason
  /// when a file cannot be written, else an empty string.
  [[nodiscard]] std::string write_csv_files(const std::filesystem::path& frames_csv) const;

 private:
  Clock* clock_;
  ProfilerConfig config_;
  std::deque<FrameSample> frames_;
  std::uint64_t next_frame_ = 0;
  Duration prepare_start_{0};
  Duration prepare_{0};
  FrameStats gpu_{};
  TextureMemory memory_{};
  mutable std::mutex loads_mutex_;
  std::vector<LoadSample> loads_;  // guarded by loads_mutex_
};

/// `out.csv` -> `out_loads.csv`.
[[nodiscard]] std::filesystem::path loads_csv_path(const std::filesystem::path& frames_csv);

}  // namespace vnmaki
