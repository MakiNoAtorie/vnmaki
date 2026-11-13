#pragma once

// The movie presenter (docs/SPEC.md section 14): plays the script's `movie(path)` calls. The
// story waits at a MovieRequest; this class loads the movie, plays it over everything, and moves
// the story on when it has played, or when the player or skip mode ends it. A replay never
// reaches it (the runtime passes movie steps by itself).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/video/movie.hpp"
#include "engine/video/movie_audio.hpp"
#include "engine/video/movie_player.hpp"

namespace vnmaki {

struct MovieRequest;
class ScriptRuntime;
class Vfs;

/// All of `runtime`, `vfs`, `jobs` and `audio` (may be null) must outlive it. Call everything
/// from the main thread.
class MoviePresenter {
 public:
  MoviePresenter(ScriptRuntime& runtime, const Vfs& vfs, JobSystem& jobs,
                 MovieAudio* audio = nullptr);
  MoviePresenter(const MoviePresenter&) = delete;
  MoviePresenter& operator=(const MoviePresenter&) = delete;
  MoviePresenter(MoviePresenter&&) = delete;
  MoviePresenter& operator=(MoviePresenter&&) = delete;
  ~MoviePresenter();

  /// True while the story waits at a `movie`.
  [[nodiscard]] bool waiting() const;

  /// Call once per update. Starts the movie when the story waits at one (loading it on a job),
  /// moves it on, and advances the story when it has played. A story that is not at a movie (a
  /// load, a rollback) drops the one that plays. Throws MovieError if the movie cannot be played,
  /// unless a warning handler is set: then it reports the error, skips the movie and advances.
  void update(double seconds);
  void set_warning_handler(std::function<void(std::string_view)> handler) {
    warn_ = std::move(handler);
  }

  /// The player clicked. If the story waits at a movie, returns true (the input is used) and ends
  /// the movie when it is skippable.
  bool click();
  /// Skip mode is on. If the story waits at a movie, ends it and returns true.
  bool skip();

  /// Uploads the picture. Call before drawing, with the renderer that draws.
  void prepare(IRenderer& renderer);
  /// Draws black and the picture, as large as fits, centered in `target` (the size of the pass).
  /// It draws nothing when no movie plays; call it after the scene.
  void draw(IRenderer& renderer, Size target) const;

 private:
  void end();
  void drop();
  void play(double seconds, const MovieRequest& request);

  ScriptRuntime* runtime_;
  const Vfs* vfs_;
  JobSystem* jobs_;
  MovieAudio* audio_;
  std::function<void(std::string_view)> warn_;
  JobHandle<std::shared_ptr<const MovieData>> loading_;
  std::unique_ptr<MoviePlayer> player_;
  std::string path_;  // the movie that loads or plays
  std::vector<TextureId> retired_;
  std::optional<TextureId> black_;
};

}  // namespace vnmaki
