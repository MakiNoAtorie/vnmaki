#include "cli/window_command.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

#include "cli/image_view.hpp"
#include "engine/platform/main_loop.hpp"
#include "engine/platform/sdl_platform.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitFailure = 1;
constexpr int kExitUsageError = 2;

/// Paces the loop until a renderer with vsync does: about 60 frames a second.
constexpr Duration kFrameTime = std::chrono::duration_cast<Duration>(std::chrono::seconds{1}) / 60;

std::optional<std::uint64_t> parse_frame_count(std::string_view text) {
  std::uint64_t frames = 0;
  const char* const end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, frames);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return frames;
}

}  // namespace

int window_command(std::span<const std::string_view> args, std::ostream& err) {
  std::optional<std::uint64_t> max_frames;
  std::optional<std::filesystem::path> image_file;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--frames" && i + 1 < args.size()) {
      const std::string_view count = args[++i];
      max_frames = parse_frame_count(count);
      if (!max_frames) {
        err << "vnmaki window: --frames needs a whole number, not '" << count << "'\n";
        return kExitUsageError;
      }
    } else if (args[i] == "--image" && i + 1 < args.size()) {
      image_file = std::filesystem::path{args[++i]};
    } else {
      err << "vnmaki window: unknown option '" << args[i]
          << "'\nusage: vnmaki window [--frames N] [--image FILE]\n";
      return kExitUsageError;
    }
  }

  try {
    SdlPlatform platform{WindowConfig{}};
    if (image_file) {
      return run_image_view(platform, *image_file, max_frames, err);
    }
    SteadyClock clock;
    MainLoop loop{platform, clock, {.min_frame_time = kFrameTime}};
    loop.run({}, max_frames);
  } catch (const PlatformError& error) {
    err << "vnmaki window: " << error.what() << '\n';
    return kExitFailure;
  }
  return 0;
}

}  // namespace vnmaki::cli
