#pragma once

// Movie playback (docs/SPEC.md section 14): decodes a movie ahead on a job thread, follows a
// clock, and gives the picture of the moment as a texture.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/video/movie.hpp"
#include "engine/video/movie_audio.hpp"
#include "engine/video/video_decoder.hpp"

namespace vnmaki {

/// Plays one movie from time 0. Call update() and prepare() from one thread (the main thread);
/// only the decoding runs on a job.
///
/// The clock does not start until the first picture is decoded. Then, if the movie has audio and
/// there is a MovieAudio, the audio starts and the picture follows its position (the audio
/// clock). Without audio, or once the audio has ended, the picture follows the seconds that
/// update() gets (the loop clock). With `loop`, the movie starts again when the clock reaches its
/// end (or the audio starts again), and it never finishes.
class MoviePlayer {
 public:
  /// `jobs` and `audio` (may be null) must outlive the player.
  MoviePlayer(std::shared_ptr<const MovieData> movie, JobSystem& jobs, MovieAudio* audio,
              bool loop);
  MoviePlayer(const MoviePlayer&) = delete;
  MoviePlayer& operator=(const MoviePlayer&) = delete;
  MoviePlayer(MoviePlayer&&) = delete;
  MoviePlayer& operator=(MoviePlayer&&) = delete;
  ~MoviePlayer();

  /// Moves the clock on by `seconds` and takes the pictures the decoder has made. Throws
  /// MovieError ("movie '<path>' ...") if the video cannot be decoded.
  void update(double seconds);
  /// Uploads the picture of the moment if it changed, and destroys the texture it replaces.
  void prepare(IRenderer& renderer);
  /// The texture to destroy when the player is not drawn any more (the renderer may be gone when
  /// the player is). Empty after that.
  [[nodiscard]] std::optional<TextureId> release_texture();

  /// Ends the movie now: the audio stops and the picture stays.
  void finish();

  /// True once the first picture has been decoded.
  [[nodiscard]] bool ready() const { return ready_; }
  /// True once a movie that does not loop has played to its end (or finish() was called).
  [[nodiscard]] bool finished() const { return finished_; }
  /// Seconds into the movie.
  [[nodiscard]] double time() const { return time_; }
  /// The picture of the moment, or null before the first one.
  [[nodiscard]] const Image* picture() const { return ready_ ? &current_.image : nullptr; }
  /// The uploaded picture of the moment; empty before prepare() has uploaded one.
  [[nodiscard]] std::optional<TextureId> texture() const { return texture_; }
  /// The picture's size: the file's until the first picture is there.
  [[nodiscard]] Size size() const { return ready_ ? current_.image.size : movie_->size; }

 private:
  void collect();
  void request();
  void start();
  void tick(double seconds);
  void stop_audio();

  std::shared_ptr<const MovieData> movie_;
  JobSystem* jobs_;
  MovieAudio* audio_;
  bool loop_;
  std::shared_ptr<VideoDecoder> decoder_;
  JobHandle<std::vector<MovieFrame>> job_;
  std::deque<MovieFrame> queue_;
  MovieFrame current_;
  bool decoder_done_ = false;
  bool rewinding_ = false;
  bool ready_ = false;
  bool finished_ = false;
  bool changed_ = false;
  double time_ = 0;
  std::uint64_t token_ = 0;
  std::optional<TextureId> texture_;
};

}  // namespace vnmaki
