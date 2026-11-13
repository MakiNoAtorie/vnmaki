#pragma once

// AV1 decoding (docs/SPEC.md section 14): a movie's video packets to RGBA pictures, with dav1d.

#include <cstddef>
#include <memory>
#include <vector>

#include "engine/render/image.hpp"
#include "engine/video/movie.hpp"

namespace vnmaki {

/// One decoded picture.
struct MovieFrame {
  double time = 0;  ///< Seconds from the start of the movie.
  /// Opaque RGBA8 (so premultiplied alpha is the same as straight alpha), at most
  /// kMaxMoviePixels pixels.
  Image image{};
};

/// Decodes the video of a movie a few pictures at a time, in order. Not thread-safe: one thread
/// at a time, but it may change threads between calls (a job decodes while the main thread
/// waits for the result).
class VideoDecoder {
 public:
  /// Throws MovieError if the decoder cannot start.
  explicit VideoDecoder(std::shared_ptr<const MovieData> movie);
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;
  VideoDecoder(VideoDecoder&&) = delete;
  VideoDecoder& operator=(VideoDecoder&&) = delete;
  ~VideoDecoder();

  /// Decodes up to `max_frames` more pictures. Throws MovieError ("cannot be decoded") for data
  /// that is not valid AV1; the decoder is then unusable until rewind().
  [[nodiscard]] std::vector<MovieFrame> decode(std::size_t max_frames);
  /// True once every picture of the movie has been given out.
  [[nodiscard]] bool at_end() const;
  /// Goes back to the first picture.
  void rewind();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
