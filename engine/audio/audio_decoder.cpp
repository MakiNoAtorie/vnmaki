#include "engine/audio/audio_decoder.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <miniaudio.h>
#include <opusfile.h>
#include <vorbis/vorbisfile.h>

namespace vnmaki {

namespace {

// How many frames of the file are decoded ahead of the converter.
constexpr std::size_t kInputFrames = 1024;

// A tag value above this is not a sample position (and could overflow the conversion below).
constexpr std::uint64_t kMaxTagSample = std::uint64_t{1} << 40U;

bool supported_format(std::int64_t channels, std::int64_t rate) {
  return channels >= 1 && channels <= AudioDecoder::kMaxSourceChannels &&
         rate >= AudioDecoder::kMinSourceRate && rate <= AudioDecoder::kMaxSourceRate;
}

std::optional<std::uint64_t> tag_sample(const char* value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  const std::string_view text{value};
  std::uint64_t sample = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), sample);
  if (error != std::errc{} || end != text.data() + text.size() || sample > kMaxTagSample) {
    return std::nullopt;
  }
  return sample;
}

// The RPG Maker tag convention, which most tools write: LOOPSTART with LOOPLENGTH or LOOPEND, in
// samples at the file's rate. LOOPSTART alone loops to the end of the file.
std::optional<AudioLoopPoints> tag_loop_points(const char* start, const char* length,
                                               const char* end, std::uint32_t rate) {
  const std::optional<std::uint64_t> first = tag_sample(start);
  if (!first) {
    return std::nullopt;
  }
  std::uint64_t last = 0;
  if (const std::optional<std::uint64_t> count = tag_sample(length)) {
    last = *first + *count;
  } else if (const std::optional<std::uint64_t> stop = tag_sample(end)) {
    last = *stop;
  }
  const AudioLoopPoints points{.start = *first * kAudioSampleRate / rate,
                               .end = last * kAudioSampleRate / rate};
  // Checked in output frames: two samples of a 192 kHz file can be the same frame.
  if (last != 0 && points.end <= points.start) {
    return std::nullopt;
  }
  return points;
}

// One decoder library reading one file, in the file's own channel count and sample rate.
class Source {
 public:
  Source() = default;
  Source(const Source&) = delete;
  Source(Source&&) = delete;
  Source& operator=(const Source&) = delete;
  Source& operator=(Source&&) = delete;
  virtual ~Source() = default;

  /// Decodes up to out.size() / channels() frames; 0 means the end.
  virtual std::size_t read(std::span<float> out) = 0;
  virtual bool seek(std::uint64_t frame) = 0;
  [[nodiscard]] virtual std::uint32_t channels() const = 0;
  [[nodiscard]] virtual std::uint32_t rate() const = 0;
  [[nodiscard]] virtual std::optional<AudioLoopPoints> loop_points() const { return std::nullopt; }
};

// --- Opus (opusfile) --------------------------------------------------------------------------

class OpusSource final : public Source {
 public:
  /// Null when `data` is not an Opus file.
  static std::unique_ptr<Source> open(std::span<const std::byte> data) {
    int error = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    OggOpusFile* file = op_open_memory(bytes, data.size(), &error);
    if (file == nullptr) {
      return nullptr;
    }
    return std::make_unique<OpusSource>(file);
  }

  // opusfile mixes every channel layout down to stereo, so a chained file whose links differ
  // still gives one format.
  std::size_t read(std::span<float> out) override {
    const auto size = static_cast<int>(std::min<std::size_t>(out.size(), kMaxRead));
    for (;;) {
      const int frames = op_read_float_stereo(file_.get(), out.data(), size);
      if (frames != OP_HOLE) {  // A hole is a gap in the data; the next read continues after it.
        return frames > 0 ? static_cast<std::size_t>(frames) : 0;
      }
    }
  }

  bool seek(std::uint64_t frame) override {
    return op_pcm_seek(file_.get(), static_cast<ogg_int64_t>(frame)) == 0;
  }

  explicit OpusSource(OggOpusFile* file) : file_(file) {}

  [[nodiscard]] std::uint32_t channels() const override { return kAudioChannels; }
  // Opus always decodes at 48 kHz.
  [[nodiscard]] std::uint32_t rate() const override { return kAudioSampleRate; }

  [[nodiscard]] std::optional<AudioLoopPoints> loop_points() const override {
    const OpusTags* tags = op_tags(file_.get(), 0);
    if (tags == nullptr) {
      return std::nullopt;
    }
    return tag_loop_points(opus_tags_query(tags, "LOOPSTART", 0),
                           opus_tags_query(tags, "LOOPLENGTH", 0),
                           opus_tags_query(tags, "LOOPEND", 0), kAudioSampleRate);
  }

 private:
  static constexpr std::size_t kMaxRead = 1U << 20U;

  struct Close {
    void operator()(OggOpusFile* file) const { op_free(file); }
  };

  std::unique_ptr<OggOpusFile, Close> file_;
};

// --- Vorbis (vorbisfile) ----------------------------------------------------------------------

// The file in memory, as the C stream vorbisfile reads.
struct MemoryStream {
  std::span<const std::byte> data{};
  std::size_t position = 0;
};

std::size_t stream_read(void* out, std::size_t size, std::size_t count, void* user) {
  auto* stream = static_cast<MemoryStream*>(user);
  if (size == 0) {
    return 0;
  }
  const std::size_t items = std::min(count, (stream->data.size() - stream->position) / size);
  if (items > 0) {
    std::memcpy(out, stream->data.subspan(stream->position).data(), items * size);
    stream->position += items * size;
  }
  return items;
}

int stream_seek(void* user, ogg_int64_t offset, int whence) {
  auto* stream = static_cast<MemoryStream*>(user);
  const auto size = static_cast<ogg_int64_t>(stream->data.size());
  ogg_int64_t base = 0;
  if (whence == SEEK_CUR) {
    base = static_cast<ogg_int64_t>(stream->position);
  } else if (whence == SEEK_END) {
    base = size;
  } else if (whence != SEEK_SET) {
    return -1;
  }
  if (offset < -base || offset > size - base) {
    return -1;
  }
  stream->position = static_cast<std::size_t>(base + offset);
  return 0;
}

// What vorbisfile's tell callback returns (long).
using StreamOffset = decltype(std::declval<ov_callbacks>().tell_func(nullptr));

StreamOffset stream_tell(void* user) {
  const auto* stream = static_cast<const MemoryStream*>(user);
  return static_cast<StreamOffset>(stream->position);
}

class VorbisSource final : public Source {
 public:
  /// Null when `data` is not a Vorbis file. Throws AudioError for a Vorbis file in a format the
  /// engine does not play.
  static std::unique_ptr<Source> open(std::span<const std::byte> data) {
    auto source = std::make_unique<VorbisSource>(data);
    const ov_callbacks callbacks{.read_func = stream_read,
                                 .seek_func = stream_seek,
                                 .close_func = nullptr,
                                 .tell_func = stream_tell};
    if (ov_open_callbacks(&source->stream_, &source->file_, nullptr, 0, callbacks) != 0) {
      return nullptr;
    }
    source->open_ = true;
    const vorbis_info* info = ov_info(&source->file_, 0);
    if (info == nullptr || !supported_format(info->channels, info->rate)) {
      throw AudioError{"has an unsupported channel count or sample rate"};
    }
    source->channels_ = static_cast<std::uint32_t>(info->channels);
    source->rate_ = static_cast<std::uint32_t>(info->rate);
    return source;
  }

  explicit VorbisSource(std::span<const std::byte> data) : stream_{.data = data} {}
  VorbisSource(const VorbisSource&) = delete;
  VorbisSource(VorbisSource&&) = delete;
  VorbisSource& operator=(const VorbisSource&) = delete;
  VorbisSource& operator=(VorbisSource&&) = delete;
  ~VorbisSource() override {
    if (open_) {
      ov_clear(&file_);
    }
  }

  std::size_t read(std::span<float> out) override {
    const auto wanted = static_cast<int>(std::min<std::size_t>(out.size() / channels_, kMaxRead));
    for (;;) {
      float** pcm = nullptr;
      int link = 0;
      const auto frames = static_cast<std::int64_t>(ov_read_float(&file_, &pcm, wanted, &link));
      if (frames == OV_HOLE) {  // A gap in the data; the next read continues after it.
        continue;
      }
      // ponytail: a chained file plays its first link only, because a later link may have
      // another channel count or rate. Rebuild the converter per link if a game needs chains.
      if (frames <= 0 || link != 0 || pcm == nullptr) {
        return 0;
      }
      const auto count = static_cast<std::size_t>(frames);
      const std::span<float*> planes{pcm, channels_};
      for (std::size_t channel = 0; channel < channels_; ++channel) {
        const std::span<const float> plane{planes[channel], count};
        for (std::size_t frame = 0; frame < count; ++frame) {
          out[(frame * channels_) + channel] = plane[frame];
        }
      }
      return count;
    }
  }

  bool seek(std::uint64_t frame) override {
    return ov_pcm_seek(&file_, static_cast<ogg_int64_t>(frame)) == 0;
  }

  [[nodiscard]] std::uint32_t channels() const override { return channels_; }
  [[nodiscard]] std::uint32_t rate() const override { return rate_; }

  [[nodiscard]] std::optional<AudioLoopPoints> loop_points() const override {
    vorbis_comment* tags = ov_comment(&file_, 0);
    if (tags == nullptr) {
      return std::nullopt;
    }
    return tag_loop_points(vorbis_comment_query(tags, "LOOPSTART", 0),
                           vorbis_comment_query(tags, "LOOPLENGTH", 0),
                           vorbis_comment_query(tags, "LOOPEND", 0), rate_);
  }

 private:
  static constexpr std::size_t kMaxRead = 1U << 20U;

  MemoryStream stream_;
  // vorbisfile's query functions take a non-const file and change nothing.
  mutable OggVorbis_File file_{};
  bool open_ = false;
  std::uint32_t channels_ = 0;
  std::uint32_t rate_ = 0;
};

// --- FLAC and WAV (miniaudio's dr_flac and dr_wav) --------------------------------------------

class MiniaudioSource final : public Source {
 public:
  /// Null when `data` is not a FLAC or WAV file. Throws AudioError for one in a format the
  /// engine does not play.
  static std::unique_ptr<Source> open(std::span<const std::byte> data) {
    auto source = std::make_unique<MiniaudioSource>();
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_memory(data.data(), data.size(), &config, &source->decoder_) !=
        MA_SUCCESS) {
      return nullptr;
    }
    source->open_ = true;
    if (!supported_format(source->decoder_.outputChannels, source->decoder_.outputSampleRate)) {
      throw AudioError{"has an unsupported channel count or sample rate"};
    }
    return source;
  }

  MiniaudioSource() = default;
  MiniaudioSource(const MiniaudioSource&) = delete;
  MiniaudioSource(MiniaudioSource&&) = delete;
  MiniaudioSource& operator=(const MiniaudioSource&) = delete;
  MiniaudioSource& operator=(MiniaudioSource&&) = delete;
  ~MiniaudioSource() override {
    if (open_) {
      ma_decoder_uninit(&decoder_);
    }
  }

  std::size_t read(std::span<float> out) override {
    ma_uint64 frames = 0;
    ma_decoder_read_pcm_frames(&decoder_, out.data(), out.size() / channels(), &frames);
    return static_cast<std::size_t>(frames);
  }

  bool seek(std::uint64_t frame) override {
    return ma_decoder_seek_to_pcm_frame(&decoder_, frame) == MA_SUCCESS;
  }

  [[nodiscard]] std::uint32_t channels() const override { return decoder_.outputChannels; }
  [[nodiscard]] std::uint32_t rate() const override { return decoder_.outputSampleRate; }

 private:
  ma_decoder decoder_{};
  bool open_ = false;
};

std::unique_ptr<Source> open_source(std::span<const std::byte> data) {
  if (data.empty()) {
    return nullptr;
  }
  if (std::unique_ptr<Source> opus = OpusSource::open(data)) {
    return opus;
  }
  if (std::unique_ptr<Source> vorbis = VorbisSource::open(data)) {
    return vorbis;
  }
  return MiniaudioSource::open(data);
}

// Decoders can give anything for damaged data. Keep what reaches the mixer finite and in range.
void limit(std::span<float> samples) {
  for (float& sample : samples) {
    sample = std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F;
  }
}

}  // namespace

struct AudioDecoder::Impl {
  explicit Impl(std::vector<std::byte> bytes) : encoded(std::move(bytes)) {}
  Impl(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    if (converting) {
      ma_data_converter_uninit(&converter, nullptr);
    }
  }

  // ponytail: the whole encoded file stays in memory, because the Vfs has no streams. Only the
  // decoding is streamed. Read through a Vfs stream when the Vfs gets one.
  std::vector<std::byte> encoded;
  std::unique_ptr<Source> source;  // Reads `encoded`, so it is destroyed first.
  ma_data_converter converter{};
  bool converting = false;
  std::vector<float> input;  // Decoded frames the converter has not taken yet.
  std::size_t input_frames = 0;
  std::size_t input_used = 0;
  bool ended = false;
};

AudioDecoder::AudioDecoder(std::vector<std::byte> encoded)
    : impl_(std::make_unique<Impl>(std::move(encoded))) {
  impl_->source = open_source(impl_->encoded);
  if (impl_->source == nullptr) {
    throw AudioError{"is not an Opus, Vorbis, FLAC or WAV file"};
  }
  const ma_data_converter_config config = ma_data_converter_config_init(
      ma_format_f32, ma_format_f32, impl_->source->channels(),
      static_cast<ma_uint32>(kAudioChannels), impl_->source->rate(), kAudioSampleRate);
  if (ma_data_converter_init(&config, nullptr, &impl_->converter) != MA_SUCCESS) {
    throw AudioError{"has channels or a sample rate the engine cannot convert"};
  }
  impl_->converting = true;
  impl_->input.resize(kInputFrames * impl_->source->channels());
}

AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&&) noexcept = default;
AudioDecoder::~AudioDecoder() = default;

std::size_t AudioDecoder::read(std::span<float> out) {
  Impl& impl = *impl_;
  const std::size_t channels = impl.source->channels();
  const std::size_t wanted = out.size() / kAudioChannels;
  std::size_t done = 0;
  while (done < wanted) {
    if (impl.input_used == impl.input_frames && !impl.ended) {
      impl.input_frames = impl.source->read(impl.input);
      impl.input_used = 0;
      impl.ended = impl.input_frames == 0;
      limit(std::span{impl.input}.first(impl.input_frames * channels));
    }
    ma_uint64 taken = impl.input_frames - impl.input_used;
    ma_uint64 made = wanted - done;
    const std::span<const float> from = std::span{impl.input}.subspan(impl.input_used * channels);
    const std::span<float> to = out.subspan(done * kAudioChannels);
    if (ma_data_converter_process_pcm_frames(&impl.converter, from.data(), &taken, to.data(),
                                             &made) != MA_SUCCESS) {
      impl.ended = true;
      break;
    }
    impl.input_used += static_cast<std::size_t>(taken);
    done += static_cast<std::size_t>(made);
    if (taken == 0 && made == 0) {
      impl.ended = true;  // The file ended and the converter has nothing left.
      break;
    }
  }
  limit(out.first(done * kAudioChannels));
  return done;
}

bool AudioDecoder::seek(std::uint64_t frame) {
  Impl& impl = *impl_;
  impl.input_frames = 0;
  impl.input_used = 0;
  ma_data_converter_reset(&impl.converter);
  const std::uint64_t rate = impl.source->rate();
  const bool can_seek =
      frame <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / rate;
  impl.ended = !can_seek || !impl.source->seek(frame * rate / kAudioSampleRate);
  return !impl.ended;
}

std::optional<AudioLoopPoints> AudioDecoder::loop_points() const {
  return impl_->source->loop_points();
}

}  // namespace vnmaki
