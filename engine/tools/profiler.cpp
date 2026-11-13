#include "engine/tools/profiler.hpp"

#include <algorithm>
#include <fstream>
#include <string_view>

#include "engine/core/load_timing.hpp"
#include "engine/core/utf8_path.hpp"

namespace vnmaki {

namespace {

std::int64_t micros(Duration time) {
  return std::chrono::duration_cast<std::chrono::microseconds>(time).count();
}

// A CSV field: quoted when it holds a comma, a quote or a line break.
std::string csv_field(std::string_view text) {
  if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string{text};
  }
  std::string quoted{"\""};
  for (const char c : text) {
    quoted += c;
    if (c == '"') {
      quoted += '"';
    }
  }
  quoted += '"';
  return quoted;
}

template <typename Writer>
std::string write_file(const std::filesystem::path& path, const Writer& writer) {
  std::ofstream file{path, std::ios::binary};
  if (!file) {
    return "cannot write " + to_utf8(path);
  }
  writer(file);
  file.flush();
  return file ? std::string{} : "cannot write " + to_utf8(path);
}

}  // namespace

Profiler::Profiler(Clock& clock, ProfilerConfig config) : clock_(&clock), config_(config) {
  if (config_.enabled) {
    set_load_sink([this](std::string_view path, std::chrono::nanoseconds time) {
      const std::scoped_lock lock{loads_mutex_};
      if (loads_.size() < kMaxLoads) {
        loads_.push_back({.path = std::string{path}, .time = time});
      }
    });
  }
}

Profiler::~Profiler() {
  if (config_.enabled) {
    set_load_sink({});
  }
}

void Profiler::begin_prepare() {
  if (config_.enabled) {
    prepare_start_ = clock_->now();
    prepare_ = Duration::zero();
  }
}

void Profiler::end_prepare() {
  if (config_.enabled) {
    prepare_ = clock_->now() - prepare_start_;
  }
}

void Profiler::set_gpu(const FrameStats& gpu, const TextureMemory& memory) {
  gpu_ = gpu;
  memory_ = memory;
}

void Profiler::end_frame(const FrameTiming& timing) {
  if (!config_.enabled) {
    return;
  }
  const Duration prepare = std::min(prepare_, timing.draw);
  frames_.push_back({.frame = next_frame_++,
                     .events = timing.events,
                     .update = timing.update,
                     .prepare = prepare,
                     .draw = timing.draw - prepare,
                     .updates = timing.updates,
                     .gpu = gpu_,
                     .memory = memory_});
  if (config_.max_frames != 0 && frames_.size() > config_.max_frames) {
    frames_.pop_front();
  }
  // A frame whose draw hook ended early (the story ended) sets neither mark nor GPU numbers.
  prepare_ = Duration::zero();
  gpu_ = {};
}

std::vector<LoadSample> Profiler::loads() const {
  const std::scoped_lock lock{loads_mutex_};
  return loads_;
}

std::vector<LoadSample> Profiler::slowest_loads(std::size_t count) const {
  std::vector<LoadSample> sorted = loads();
  const std::size_t kept = std::min(count, sorted.size());
  std::ranges::partial_sort(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(kept),
                            [](const LoadSample& a, const LoadSample& b) {
                              return a.time > b.time;
                            });
  sorted.resize(kept);
  return sorted;
}

void Profiler::write_frames_csv(std::ostream& out) const {
  out << "frame,events_us,update_us,prepare_us,draw_us,total_us,updates,passes,quads,draw_calls,"
         "texture_bytes,target_bytes,textures,targets\n";
  for (const FrameSample& f : frames_) {
    out << f.frame << ',' << micros(f.events) << ',' << micros(f.update) << ','
        << micros(f.prepare) << ',' << micros(f.draw) << ',' << micros(f.total()) << ','
        << f.updates << ',' << f.gpu.passes << ',' << f.gpu.quads << ',' << f.gpu.draw_calls << ','
        << f.memory.texture_bytes << ',' << f.memory.target_bytes << ',' << f.memory.textures
        << ',' << f.memory.targets << '\n';
  }
}

void Profiler::write_loads_csv(std::ostream& out) const {
  out << "path,time_us\n";
  for (const LoadSample& load : loads()) {
    out << csv_field(load.path) << ',' << micros(load.time) << '\n';
  }
}

std::string Profiler::write_csv_files(const std::filesystem::path& frames_csv) const {
  std::string problem = write_file(frames_csv, [this](std::ostream& out) { write_frames_csv(out); });
  if (problem.empty()) {
    problem = write_file(loads_csv_path(frames_csv),
                         [this](std::ostream& out) { write_loads_csv(out); });
  }
  return problem;
}

std::filesystem::path loads_csv_path(const std::filesystem::path& frames_csv) {
  std::filesystem::path loads = frames_csv;
  std::filesystem::path name = frames_csv.stem();
  name += "_loads";
  name += frames_csv.extension();
  loads.replace_filename(name);
  return loads;
}

}  // namespace vnmaki
