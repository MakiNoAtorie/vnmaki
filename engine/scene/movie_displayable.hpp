#pragma once

// A movie as a displayable (docs/SPEC.md sections 6 and 14): `show(tag, { movie = path, loop =
// true })`. It plays from time 0 and draws the picture of the moment like an image.

#include <memory>
#include <string>
#include <vector>

#include "engine/jobs/job_system.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/video/movie.hpp"
#include "engine/video/movie_audio.hpp"
#include "engine/video/movie_player.hpp"

namespace vnmaki {

class Vfs;

/// Loads its movie on a job when Scene::prepare() first reaches it, and starts the clock when the
/// first picture is decoded (see MoviePlayer). Its audio, if the scene has a MovieAudio, plays on
/// the movie channel. The movie file is a Vfs path, with its extension.
class MovieDisplayable final : public Displayable {
 public:
  /// `retired` collects the texture of a displayable that goes away, for Scene::prepare() to
  /// destroy: only the renderer can.
  MovieDisplayable(std::string path, bool loop, std::shared_ptr<std::vector<TextureId>> retired);
  MovieDisplayable(const MovieDisplayable&) = delete;
  MovieDisplayable& operator=(const MovieDisplayable&) = delete;
  MovieDisplayable(MovieDisplayable&&) = delete;
  MovieDisplayable& operator=(MovieDisplayable&&) = delete;
  ~MovieDisplayable() override;

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] bool loop() const { return loop_; }
  /// True while a movie that does not loop has not played to its end: what `wait` waits for.
  [[nodiscard]] bool playing() const { return !loop_ && !finished(); }
  /// Ends a movie that does not loop: the audio stops and the last picture stays.
  void finish();

  /// Loads the movie, makes the player and uploads the picture. Throws MovieError.
  void prepare(IRenderer& renderer, JobSystem& jobs, const Vfs& vfs, MovieAudio* audio);

  /// Moves the clock on. Throws MovieError if the video cannot be decoded.
  void update(double seconds) override;
  void draw(IRenderer& renderer) const override;
  /// The picture with the properties applied. Empty until the first picture is there.
  [[nodiscard]] RectF bounds() const override;

  /// The player, or null until the movie has loaded.
  [[nodiscard]] const MoviePlayer* player() const { return player_.get(); }

 private:
  [[nodiscard]] bool finished() const { return player_ ? player_->finished() : ended_; }

  std::string path_;
  bool loop_;
  std::shared_ptr<std::vector<TextureId>> retired_;
  JobHandle<std::shared_ptr<const MovieData>> loading_;
  std::unique_ptr<MoviePlayer> player_;
  bool ended_ = false;  // finish() came before the movie loaded
};

}  // namespace vnmaki
