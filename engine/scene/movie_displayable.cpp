#include "engine/scene/movie_displayable.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

MovieDisplayable::MovieDisplayable(std::string path, bool loop,
                                   std::shared_ptr<std::vector<TextureId>> retired)
    : path_(std::move(path)), loop_(loop), retired_(std::move(retired)) {}

MovieDisplayable::~MovieDisplayable() {
  if (!player_) {
    return;
  }
  try {
    if (const std::optional<TextureId> texture = player_->release_texture()) {
      retired_->push_back(*texture);
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Out of memory: the texture stays on the GPU until the renderer goes.
  }
}

void MovieDisplayable::finish() {
  ended_ = true;
  if (player_) {
    player_->finish();
  }
}

void MovieDisplayable::prepare(IRenderer& renderer, JobSystem& jobs, const Vfs& vfs,
                               MovieAudio* audio) {
  if (!player_) {
    if (ended_) {
      return;
    }
    if (!loading_.valid()) {
      loading_ = jobs.submit([&vfs, path = path_] { return load_movie(vfs, path); });
    }
    if (!loading_.ready()) {
      return;
    }
    player_ = std::make_unique<MoviePlayer>(loading_.get(), jobs, audio, loop_);
  }
  player_->prepare(renderer);
}

void MovieDisplayable::update(double seconds) {
  if (player_) {
    player_->update(seconds);
  }
}

void MovieDisplayable::draw(IRenderer& renderer) const {
  if (!player_ || !player_->texture()) {
    return;
  }
  Color tint = properties().tint;
  tint.a *= properties().alpha;
  renderer.draw(Quad{.corners = place_corners(properties(), player_->size()),
                     .uv = properties().crop,
                     .tint = tint},
                draw_state(*player_->texture()));
}

RectF MovieDisplayable::bounds() const {
  return bounding_box(
      place_corners(properties(), player_ && player_->ready() ? player_->size() : Size{}));
}

}  // namespace vnmaki
