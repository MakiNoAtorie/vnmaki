#include "engine/video/movie_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vnmaki {

namespace {

// A decode job makes this many pictures, and the next one starts when fewer are waiting.
constexpr std::size_t kBatchFrames = 4;
constexpr std::size_t kLowWater = 4;

}  // namespace

MoviePlayer::MoviePlayer(std::shared_ptr<const MovieData> movie, JobSystem& jobs, MovieAudio* audio,
                         bool loop)
    : movie_(std::move(movie)),
      jobs_(&jobs),
      audio_(audio),
      loop_(loop),
      decoder_(std::make_shared<VideoDecoder>(movie_)) {}

MoviePlayer::~MoviePlayer() { stop_audio(); }

void MoviePlayer::stop_audio() {
  if (token_ != 0 && audio_ != nullptr) {
    audio_->stop_movie_audio(token_);
  }
  token_ = 0;
}

void MoviePlayer::collect() {
  if (job_.valid() && job_.ready()) {
    try {
      std::vector<MovieFrame> batch = job_.get();
      if (!rewinding_) {
        for (MovieFrame& frame : batch) {
          queue_.push_back(std::move(frame));
        }
      }
      decoder_done_ = decoder_->at_end();
    } catch (const MovieError& error) {
      decoder_done_ = true;
      throw MovieError{"movie '" + movie_->path + "' " + error.what()};
    }
  }
  // No job runs now, so the decoder may be touched from this thread.
  if (rewinding_ && !job_.valid()) {
    decoder_->rewind();
    decoder_done_ = false;
    rewinding_ = false;
  }
}

void MoviePlayer::request() {
  if (job_.valid() || decoder_done_ || rewinding_ || queue_.size() >= kLowWater) {
    return;
  }
  job_ = jobs_->submit([decoder = decoder_] { return decoder->decode(kBatchFrames); });
}

void MoviePlayer::start() {
  current_ = std::move(queue_.front());
  queue_.pop_front();
  ready_ = true;
  changed_ = true;
  if (audio_ != nullptr && !movie_->audio.empty()) {
    token_ = audio_->start_movie_audio(movie_->audio, loop_);
  }
}

// Moves the clock: to the audio's position, or on by `seconds`.
void MoviePlayer::tick(double seconds) {
  std::optional<double> heard;
  if (token_ != 0) {
    heard = audio_->movie_audio_position(token_);
    if (!heard) {
      token_ = 0;  // The audio ended: the loop clock goes on from here.
    }
  }
  double next = heard ? *heard : time_ + seconds;
  bool wrapped = false;
  if (loop_) {
    next = std::fmod(next, movie_->duration);
    wrapped = next < time_;
  } else {
    next = std::max(next, time_);
  }
  time_ = next;
  if (wrapped) {
    queue_.clear();
    rewinding_ = true;
  }
}

void MoviePlayer::update(double seconds) {
  if (finished_) {
    return;
  }
  collect();
  if (!ready_) {
    if (!queue_.empty()) {
      start();
    } else if (decoder_done_ && !job_.valid()) {
      throw MovieError{"movie '" + movie_->path + "' has no pictures"};
    }
  } else {
    tick(seconds);
    collect();  // A wrap makes the decoder start again at once if its job has ended.
  }
  while (ready_ && !queue_.empty() && queue_.front().time <= time_) {
    current_ = std::move(queue_.front());
    queue_.pop_front();
    changed_ = true;
  }
  if (ready_ && !loop_ && time_ >= movie_->duration && queue_.empty() && decoder_done_ &&
      !job_.valid()) {
    finish();
    return;
  }
  request();
}

void MoviePlayer::finish() {
  finished_ = true;
  queue_.clear();
  stop_audio();
}

void MoviePlayer::prepare(IRenderer& renderer) {
  if (!changed_) {
    return;
  }
  const TextureId uploaded = renderer.create_texture(current_.image, TextureFilter::kLinear);
  if (texture_) {
    renderer.destroy_texture(*texture_);
  }
  texture_ = uploaded;
  changed_ = false;
}

std::optional<TextureId> MoviePlayer::release_texture() { return std::exchange(texture_, {}); }

}  // namespace vnmaki
