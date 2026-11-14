#include "engine/video/movie_presenter.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/render/image.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

// The movie the story waits at, or null.
const MovieRequest* current_movie(const ScriptRuntime& runtime) {
  const BlockingRequest* request =
      runtime.state() == RunState::kWaiting ? runtime.request() : nullptr;
  return request != nullptr ? std::get_if<MovieRequest>(request) : nullptr;
}

}  // namespace

MoviePresenter::MoviePresenter(ScriptRuntime& runtime, const Vfs& vfs, JobSystem& jobs,
                               MovieAudio* audio)
    : runtime_(&runtime), vfs_(&vfs), jobs_(&jobs), audio_(audio) {}

MoviePresenter::~MoviePresenter() = default;

bool MoviePresenter::waiting() const { return current_movie(*runtime_) != nullptr; }

// Forgets the movie. Its texture goes at the next prepare(): only a renderer can destroy it.
void MoviePresenter::drop() {
  if (player_) {
    if (const std::optional<TextureId> texture = player_->release_texture()) {
      retired_.push_back(*texture);
    }
    player_.reset();
  }
  loading_ = {};
  path_.clear();
}

void MoviePresenter::end() {
  drop();
  runtime_->advance();
}

void MoviePresenter::update(double seconds) {
  const MovieRequest* request = current_movie(*runtime_);
  if (request == nullptr || request->path != path_) {
    drop();
  }
  if (request == nullptr) {
    return;
  }
  try {
    play(seconds, *request);
  } catch (const MovieError& error) {
    if (!warn_) {
      throw;
    }
    warn_(error.what());
    end();  // a movie that cannot play is skipped, not the game
  }
}

void MoviePresenter::play(double seconds, const MovieRequest& request) {
  if (!player_) {
    if (!loading_.valid()) {
      path_ = request.path;
      loading_ =
          jobs_->submit([vfs = vfs_, path = request.path] { return load_movie(*vfs, path); });
    }
    if (!loading_.ready()) {
      return;
    }
    player_ = std::make_unique<MoviePlayer>(loading_.get(), *jobs_, audio_, false);
  }
  player_->update(seconds);
  if (player_->finished()) {
    end();
  }
}

bool MoviePresenter::click() {
  const MovieRequest* request = current_movie(*runtime_);
  if (request == nullptr) {
    return false;
  }
  if (request->skippable) {
    end();
  }
  return true;
}

bool MoviePresenter::skip() {
  if (current_movie(*runtime_) == nullptr) {
    return false;
  }
  end();
  return true;
}

void MoviePresenter::prepare(IRenderer& renderer) {
  for (const TextureId texture : retired_) {
    renderer.destroy_texture(texture);
  }
  retired_.clear();
  if (!player_) {
    return;
  }
  if (!black_) {
    black_ = renderer.create_texture(
        Image{.size = Size{.width = 1, .height = 1}, .pixels = {0, 0, 0, UINT8_MAX}},
        TextureFilter::kNearest);
  }
  player_->prepare(renderer);
}

void MoviePresenter::draw(IRenderer& renderer, Size target) const {
  if (!player_ || !player_->texture() || !black_) {
    return;
  }
  const auto width = static_cast<float>(target.width);
  const auto height = static_cast<float>(target.height);
  const Size picture = player_->size();
  const float scale = std::min(width / static_cast<float>(picture.width),
                               height / static_cast<float>(picture.height));
  const float shown_width = static_cast<float>(picture.width) * scale;
  const float shown_height = static_cast<float>(picture.height) * scale;
  renderer.draw(quad_from_rect({.x = 0, .y = 0, .width = width, .height = height}),
                {.texture = *black_});
  renderer.draw(quad_from_rect({.x = (width - shown_width) / 2,
                                .y = (height - shown_height) / 2,
                                .width = shown_width,
                                .height = shown_height}),
                {.texture = *player_->texture()});
}

}  // namespace vnmaki
