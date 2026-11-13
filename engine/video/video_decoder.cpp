#include "engine/video/video_decoder.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <dav1d/dav1d.h>

namespace vnmaki {

namespace {

constexpr std::size_t kBytesPerPixel = 4;
constexpr std::uint8_t kOpaque = UINT8_MAX;
constexpr int kBitsPerByte = 8;
constexpr std::size_t kConfigHeaderBytes = 4;  // The AV1 configuration record before its OBUs.
constexpr std::int64_t kNoPacket = -1;

// The picture's YCbCr (BT.601 or BT.709, limited or full range) to RGB.
constexpr float kMaxChannel = 255.0F;
constexpr float kLumaBlack = 16.0F;
constexpr float kChromaZero = 128.0F;
constexpr float kLimitedLumaRange = 219.0F;
constexpr float kLimitedChromaRange = 224.0F;
constexpr float kHalf = 0.5F;
constexpr float kBt601Red = 0.299F;
constexpr float kBt601Blue = 0.114F;
constexpr float kBt709Red = 0.2126F;
constexpr float kBt709Blue = 0.0722F;

struct Coefficients {
  float red = kBt709Red;
  float blue = kBt709Blue;
};

Coefficients coefficients_of(const Dav1dSequenceHeader* header) {
  if (header != nullptr && (header->mtrx == DAV1D_MC_BT470BG || header->mtrx == DAV1D_MC_BT601)) {
    return {.red = kBt601Red, .blue = kBt601Blue};
  }
  return {};
}

// A plane of the picture: its bytes, the stride, and the bytes in a sample.
struct Plane {
  std::span<const std::byte> bytes{};
  std::size_t stride = 0;
  std::size_t sample_bytes = 1;

  [[nodiscard]] unsigned at(std::size_t x, std::size_t y) const {
    const auto sample = bytes.subspan((y * stride) + (x * sample_bytes), sample_bytes);
    auto value = std::to_integer<unsigned>(sample.front());
    if (sample_bytes == 2) {
      value |= std::to_integer<unsigned>(sample.back()) << kBitsPerByte;
    }
    return value;
  }
};

Plane plane_of(const void* data, std::ptrdiff_t stride, int bits, int rows) {
  if (stride <= 0 || data == nullptr) {
    throw MovieError{"cannot be decoded: bad picture"};
  }
  const auto stride_bytes = static_cast<std::size_t>(stride);
  return Plane{
      .bytes = {static_cast<const std::byte*>(data), stride_bytes * static_cast<std::size_t>(rows)},
      .stride = stride_bytes,
      .sample_bytes = bits > kBitsPerByte ? std::size_t{2} : std::size_t{1}};
}

std::uint8_t channel(float value) {
  return static_cast<std::uint8_t>(std::clamp(value + kHalf, 0.0F, kMaxChannel));
}

Image convert(const Dav1dPicture& picture) {
  const int width = picture.p.w;
  const int height = picture.p.h;
  if (width <= 0 || height <= 0 || width > kMaxMovieDimension || height > kMaxMovieDimension ||
      static_cast<std::int64_t>(width) * height > kMaxMoviePixels) {
    throw MovieError{"cannot be decoded: bad picture size"};
  }
  if (picture.p.bpc < kBitsPerByte || picture.p.bpc > 2 * kBitsPerByte) {
    throw MovieError{"cannot be decoded: bad bit depth"};
  }
  const int shift = picture.p.bpc - kBitsPerByte;
  const bool gray = picture.p.layout == DAV1D_PIXEL_LAYOUT_I400;
  const bool half_width =
      picture.p.layout == DAV1D_PIXEL_LAYOUT_I420 || picture.p.layout == DAV1D_PIXEL_LAYOUT_I422;
  const bool half_height = picture.p.layout == DAV1D_PIXEL_LAYOUT_I420;
  const int chroma_rows = half_height ? (height + 1) / 2 : height;
  const Plane luma = plane_of(picture.data[0], picture.stride[0], picture.p.bpc, height);
  Plane blue_difference;
  Plane red_difference;
  if (!gray) {
    blue_difference = plane_of(picture.data[1], picture.stride[1], picture.p.bpc, chroma_rows);
    red_difference = plane_of(picture.data[2], picture.stride[1], picture.p.bpc, chroma_rows);
  }
  const bool full_range = picture.seq_hdr != nullptr && picture.seq_hdr->color_range != 0;
  const auto [kr, kb] = coefficients_of(picture.seq_hdr);
  const float kg = 1 - kr - kb;
  const float red_from_cr = 2 * (1 - kr);
  const float blue_from_cb = 2 * (1 - kb);
  const float green_from_cb = -(kb / kg) * blue_from_cb;
  const float green_from_cr = -(kr / kg) * red_from_cr;
  const float luma_scale = full_range ? 1.0F : kMaxChannel / kLimitedLumaRange;
  const float chroma_scale = full_range ? 1.0F : kMaxChannel / kLimitedChromaRange;
  const float luma_offset = full_range ? 0.0F : kLumaBlack;

  const auto columns = static_cast<std::size_t>(width);
  const auto rows = static_cast<std::size_t>(height);
  Image image{.size = Size{.width = width, .height = height},
              .pixels = std::vector<std::uint8_t>(columns * rows * kBytesPerPixel)};
  auto pixel = image.pixels.begin();
  for (std::size_t y = 0; y < rows; ++y) {
    for (std::size_t x = 0; x < columns; ++x) {
      const auto sample_y = static_cast<float>(luma.at(x, y) >> shift);
      const float level = (sample_y - luma_offset) * luma_scale;
      float cb = 0;
      float cr = 0;
      if (!gray) {
        const std::size_t cx = half_width ? x / 2 : x;
        const std::size_t cy = half_height ? y / 2 : y;
        cb = (static_cast<float>(blue_difference.at(cx, cy) >> shift) - kChromaZero) * chroma_scale;
        cr = (static_cast<float>(red_difference.at(cx, cy) >> shift) - kChromaZero) * chroma_scale;
      }
      *pixel++ = channel(level + (red_from_cr * cr));
      *pixel++ = channel(level + (green_from_cb * cb) + (green_from_cr * cr));
      *pixel++ = channel(level + (blue_from_cb * cb));
      *pixel++ = kOpaque;
    }
  }
  return image;
}

struct ContextCloser {
  void operator()(Dav1dContext* context) const { dav1d_close(&context); }
};

// Gives the picture back to dav1d when it goes away.
struct PictureGuard {
  PictureGuard() = default;
  PictureGuard(const PictureGuard&) = delete;
  PictureGuard& operator=(const PictureGuard&) = delete;
  PictureGuard(PictureGuard&&) = delete;
  PictureGuard& operator=(PictureGuard&&) = delete;
  ~PictureGuard() { dav1d_picture_unref(&picture); }

  Dav1dPicture picture{};
};

}  // namespace

struct VideoDecoder::Impl {
  explicit Impl(std::shared_ptr<const MovieData> data) : movie(std::move(data)) {
    Dav1dSettings settings{};
    dav1d_default_settings(&settings);
    settings.n_threads = 1;        // The movie decodes on one job; the pictures come in order.
    settings.max_frame_delay = 1;  // A picture comes out as soon as its packet is in.
    settings.frame_size_limit = static_cast<unsigned>(kMaxMoviePixels);
    settings.logger.callback = nullptr;
    Dav1dContext* opened = nullptr;
    if (dav1d_open(&opened, &settings) < 0) {
      throw MovieError{"cannot be decoded: the AV1 decoder does not start"};
    }
    context.reset(opened);
    feed_config();
  }

  // The sequence header of the track's configuration. Packets repeat it; a movie that leaves it
  // out of them needs it here. It gives no picture, and a bad one is not an error yet.
  void feed_config() {
    if (movie->av1_config.size() <= kConfigHeaderBytes) {
      return;
    }
    send(std::span{movie->av1_config}.subspan(kConfigHeaderBytes), kNoPacket, nullptr);
  }

  void send(std::span<const std::byte> bytes, std::int64_t index, std::vector<MovieFrame>* out) {
    if (bytes.empty()) {
      return;
    }
    Dav1dData data{};
    std::uint8_t* buffer = dav1d_data_create(&data, bytes.size());
    if (buffer == nullptr) {
      throw MovieError{"cannot be decoded: out of memory"};
    }
    std::ranges::transform(bytes, std::span{buffer, bytes.size()}.begin(),
                           [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
    data.m.offset = index;
    while (data.sz > 0) {
      const int sent = dav1d_send_data(context.get(), &data);
      if (sent < 0 && sent != DAV1D_ERR(EAGAIN)) {
        dav1d_data_unref(&data);
        if (out == nullptr) {
          return;
        }
        throw MovieError{"cannot be decoded: the video is not valid AV1"};
      }
      const std::size_t before = out != nullptr ? out->size() : 0;
      if (out != nullptr) {
        drain(*out);
      }
      if (sent == DAV1D_ERR(EAGAIN) && (out == nullptr || out->size() == before)) {
        dav1d_data_unref(&data);
        if (out == nullptr) {
          return;
        }
        throw MovieError{"cannot be decoded: the decoder does not go on"};
      }
    }
  }

  void drain(std::vector<MovieFrame>& out) {
    while (true) {
      PictureGuard guard;
      const int got = dav1d_get_picture(context.get(), &guard.picture);
      if (got == DAV1D_ERR(EAGAIN)) {
        return;
      }
      if (got < 0) {
        throw MovieError{"cannot be decoded: the video is not valid AV1"};
      }
      const std::int64_t index = guard.picture.m.offset;
      const bool known = index >= 0 && static_cast<std::size_t>(index) < movie->video.size();
      out.push_back(
          MovieFrame{.time = known ? movie->video.at(static_cast<std::size_t>(index)).time : 0,
                     .image = convert(guard.picture)});
    }
  }

  std::shared_ptr<const MovieData> movie;
  std::unique_ptr<Dav1dContext, ContextCloser> context;
  std::size_t next = 0;
  bool finished = false;
};

VideoDecoder::VideoDecoder(std::shared_ptr<const MovieData> movie)
    : impl_(std::make_unique<Impl>(std::move(movie))) {}

VideoDecoder::~VideoDecoder() = default;

std::vector<MovieFrame> VideoDecoder::decode(std::size_t max_frames) {
  std::vector<MovieFrame> frames;
  Impl& state = *impl_;
  while (frames.size() < max_frames && !state.finished) {
    if (state.next < state.movie->video.size()) {
      const std::size_t index = state.next++;
      state.send(state.movie->video.at(index).data, static_cast<std::int64_t>(index), &frames);
    } else {
      state.drain(frames);
      state.finished = true;
    }
  }
  return frames;
}

bool VideoDecoder::at_end() const { return impl_->finished; }

void VideoDecoder::rewind() {
  dav1d_flush(impl_->context.get());
  impl_->next = 0;
  impl_->finished = false;
  impl_->feed_config();
}

}  // namespace vnmaki
