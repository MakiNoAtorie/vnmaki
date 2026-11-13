#pragma once

// Movie files (docs/SPEC.md section 14): AV1 video and Opus audio in WebM. A movie ships with a
// game or a mod, so the file is untrusted input.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/geometry.hpp"

namespace vnmaki {

class Vfs;

/// The file is not a movie the engine can play. what() completes a sentence that starts with the
/// movie's path, e.g. "is not a WebM file".
class MovieError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// The largest picture, and the most pictures and bytes of audio, a movie may have.
inline constexpr int kMaxMovieDimension = 8192;
inline constexpr std::int64_t kMaxMoviePixels = std::int64_t{1} << 25;
inline constexpr std::size_t kMaxMoviePackets = std::size_t{1} << 20;

/// A parsed movie: the encoded video packets (decoded later, by VideoDecoder) and the audio as an
/// Ogg Opus file for AudioDecoder. It has no state, so jobs share it.
struct MovieData {
  struct Packet {
    std::vector<std::byte> data{};
    double time = 0;  ///< Seconds from the start of the movie.
  };

  std::string path{};  ///< The Vfs path, set by load_movie(); for messages.
  Size size{};         ///< The picture's size in pixels, as the file declares it.
  /// Seconds one pass takes: the time of the last video packet plus one frame.
  double duration = 0;
  /// The AV1 configuration OBUs of the track (the sequence header), if the file has them.
  std::vector<std::byte> av1_config{};
  std::vector<Packet> video{};  ///< In decode order; at least one.
  /// An Ogg Opus file made from the audio track; empty when the movie has no audio.
  std::vector<std::byte> audio{};
};

/// Parses a WebM file with one AV1 video track and at most one Opus audio track. Throws
/// MovieError for anything else.
[[nodiscard]] MovieData open_movie(std::span<const std::byte> webm);

/// Reads and parses the movie at `path`. Throws MovieError; the message is "movie '<path>' ..."
/// (open_movie()'s message after the path).
[[nodiscard]] std::shared_ptr<const MovieData> load_movie(const Vfs& vfs, std::string_view path);

/// The text of what load_movie() would throw for `path`, or an empty string for a movie that
/// loads: what `show` and `movie` raise at the script line.
[[nodiscard]] std::string movie_problem(const Vfs& vfs, std::string_view path);

}  // namespace vnmaki
