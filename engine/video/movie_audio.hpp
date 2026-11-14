#pragma once

// The audio side of a movie (docs/SPEC.md section 14). A movie's Opus track plays on the `movie`
// channel and the picture follows its clock. Audio (engine/audio/audio.hpp) implements this, so
// the video module needs no audio headers.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vnmaki {

class MovieAudio {
 public:
  MovieAudio() = default;
  MovieAudio(const MovieAudio&) = default;
  MovieAudio(MovieAudio&&) = default;
  MovieAudio& operator=(const MovieAudio&) = default;
  MovieAudio& operator=(MovieAudio&&) = default;
  virtual ~MovieAudio() = default;

  /// Plays an Ogg Opus file on the movie channel from its start, in place of the movie that plays
  /// there. Returns a token that names this playback, or 0 when the file cannot be played.
  [[nodiscard]] virtual std::uint64_t start_movie_audio(std::vector<std::byte> ogg_opus,
                                                        bool loop) = 0;
  /// Seconds of the file that `token` has played (a loop starts again at 0). Empty once the file
  /// has ended or another movie took the channel.
  [[nodiscard]] virtual std::optional<double> movie_audio_position(std::uint64_t token) const = 0;
  /// Stops `token`, if it still plays.
  virtual void stop_movie_audio(std::uint64_t token) = 0;
};

}  // namespace vnmaki
