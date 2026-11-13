// Fuzzes the movie loader (engine/video/): libwebm's parser, the engine's work on the tracks
// (the Opus packets as an Ogg file, the durations) and the dav1d decoder. Movies ship with games
// and mods, so the file is untrusted input.
//
// Besides "does not crash" (everything is built with the same sanitizers): bad data is reported
// only as MovieError; a movie has a picture, a duration above 0 and times that are numbers from 0
// up; a decoded picture is opaque RGBA that is within the size limits; the audio is empty or an
// Ogg file that the audio decoder reads or rejects with AudioError; and rewinding the decoder
// gives the same first picture again.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "engine/audio/audio_decoder.hpp"
#include "engine/video/movie.hpp"
#include "engine/video/video_decoder.hpp"

namespace {

// Enough pictures to cross a keyframe and a few inter frames, few enough for thousands of runs.
constexpr std::size_t kMaxFrames = 8;
constexpr std::size_t kAudioBlockFrames = 1024;
constexpr std::string_view kOggMagic = "OggS";
constexpr std::size_t kBytesPerPixel = 4;

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

void check_audio(std::span<const std::byte> ogg) {
  require(ogg.size() >= kOggMagic.size());
  for (std::size_t i = 0; i < kOggMagic.size(); ++i) {
    require(static_cast<char>(ogg[i]) == kOggMagic[i]);
  }
  try {
    vnmaki::AudioDecoder decoder{std::vector<std::byte>{ogg.begin(), ogg.end()}};
    std::vector<float> block(kAudioBlockFrames * vnmaki::kAudioChannels);
    require(decoder.read(block) <= kAudioBlockFrames);
  } catch (const vnmaki::AudioError&) {
    return;  // An Opus track that is not valid is the audio decoder's to reject.
  }
}

void check_picture(const vnmaki::MovieFrame& frame) {
  const vnmaki::Size size = frame.image.size;
  require(size.width > 0 && size.width <= vnmaki::kMaxMovieDimension);
  require(size.height > 0 && size.height <= vnmaki::kMaxMovieDimension);
  require(static_cast<std::int64_t>(size.width) * size.height <= vnmaki::kMaxMoviePixels);
  require(frame.image.pixels.size() == static_cast<std::size_t>(size.width) *
                                           static_cast<std::size_t>(size.height) * kBytesPerPixel);
  for (std::size_t i = kBytesPerPixel - 1; i < frame.image.pixels.size(); i += kBytesPerPixel) {
    require(frame.image.pixels[i] == UINT8_MAX);
  }
  require(frame.time >= 0 && std::isfinite(frame.time));
}

// The pictures of up to kMaxFrames, or fewer if the movie ends or the AV1 data is bad.
std::vector<vnmaki::MovieFrame> decode_some(vnmaki::VideoDecoder& decoder) {
  std::vector<vnmaki::MovieFrame> frames;
  try {
    while (frames.size() < kMaxFrames && !decoder.at_end()) {
      for (vnmaki::MovieFrame& frame : decoder.decode(kMaxFrames - frames.size())) {
        frames.push_back(std::move(frame));
      }
    }
  } catch (const vnmaki::MovieError&) {
    return frames;  // Data that is not valid AV1: the pictures before it.
  }
  return frames;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::shared_ptr<const vnmaki::MovieData> movie;
  try {
    movie = std::make_shared<const vnmaki::MovieData>(
        vnmaki::open_movie(std::as_bytes(std::span{data, size})));
  } catch (const vnmaki::MovieError&) {
    return 0;  // Rejecting bad data is the expected outcome for most inputs.
  }
  require(!movie->video.empty());
  require(movie->size.width > 0 && movie->size.height > 0);
  require(movie->duration > 0 && std::isfinite(movie->duration));
  for (const vnmaki::MovieData::Packet& packet : movie->video) {
    require(packet.time >= 0 && std::isfinite(packet.time));
  }
  if (!movie->audio.empty()) {
    check_audio(movie->audio);
  }

  std::unique_ptr<vnmaki::VideoDecoder> decoder;
  try {
    decoder = std::make_unique<vnmaki::VideoDecoder>(movie);
  } catch (const vnmaki::MovieError&) {
    return 0;
  }
  const std::vector<vnmaki::MovieFrame> first = decode_some(*decoder);
  for (const vnmaki::MovieFrame& frame : first) {
    check_picture(frame);
  }
  decoder->rewind();
  const std::vector<vnmaki::MovieFrame> again = decode_some(*decoder);
  if (!first.empty() && !again.empty()) {
    require(first.front().image.pixels == again.front().image.pixels);
  }
  return 0;
}
