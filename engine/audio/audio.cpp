#include "engine/audio/audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <miniaudio.h>

#include "engine/audio/audio_decoder.hpp"
#include "engine/core/byte_stream.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

// The audio-name convention: audio name N is the first of these files that exists. A name may
// hold "/": "bgm/theme" is "audio/bgm/theme.opus".
constexpr std::string_view kAudioDirectory = "audio/";
constexpr std::array<std::string_view, 4> kAudioExtensions{".opus", ".ogg", ".flac", ".wav"};
constexpr std::string_view kVoicePrefix = "voice/";

constexpr std::array kDefaultChannels{
    kMusicChannel, kSoundChannel, kVoiceChannel, kAmbientChannel, kUiChannel,
};

// The mixer works in blocks of this many frames (10 ms): ducking moves once a block, and a track
// that ended leaves at the end of its block.
constexpr std::size_t kBlockFrames = 480;
constexpr auto kFramesPerSecond = static_cast<double>(kAudioSampleRate);

// The longest time one update() mixes without a device, in seconds.
constexpr double kMaxUpdateSeconds = 60.0;

std::string audio_path(std::string_view name, std::string_view extension) {
  std::string path{kAudioDirectory};
  path += name;
  path += extension;
  return path;
}

std::string quoted_audio(std::string_view name) {
  std::string message = "audio '";
  message += name;
  message += "'";
  return message;
}

std::string missing_audio(std::string_view name) {
  std::string message = quoted_audio(name);
  message += " not found (no ";
  message += audio_path(name, kAudioExtensions.front());
  message += ", .ogg, .flac or .wav)";
  return message;
}

std::string bad_seconds(std::string_view option) {
  std::string message = "option '";
  message += option;
  message += "' must be a number from 0 to 86400";
  return message;
}

// False for NaN too.
bool seconds_in_range(double seconds) { return seconds >= 0 && seconds <= kMaxAudioSeconds; }

std::uint64_t frames_of(double seconds) {
  return static_cast<std::uint64_t>(std::llround(seconds * kFramesPerSecond));
}

// The music a channel is set to loop: the part of the audio state that snapshots keep.
struct MusicSpec {
  std::string name{};
  std::optional<std::uint64_t> loop_start{};  ///< Frames; from the call, not the file.
  std::optional<std::uint64_t> loop_end{};

  friend bool operator==(const MusicSpec&, const MusicSpec&) = default;
};

struct Track {
  std::string name;
  std::optional<MusicSpec> music;  ///< What the track was started as, for a music track.
  AudioDecoder decoder;
  AudioLoopPoints loop_points{};
  bool loop = false;
  std::uint64_t position = 0;
  double gain = 1;
  double gain_step = 0;  ///< Added to `gain` after every frame.
  bool fading_out = false;
  bool ended = false;
};

struct Channel {
  double volume = 1;
  double duck = 1;
  std::optional<MusicSpec> music{};
  std::vector<Track> tracks{};
};

bool plays(const Channel& channel, const MusicSpec& music) {
  return std::any_of(channel.tracks.begin(), channel.tracks.end(), [&music](const Track& track) {
    return !track.fading_out && track.music == music;
  });
}

// Fades out the tracks `which` picks over `fade` seconds; with no fade they stop at once.
void fade_out(Channel& channel, double fade, const std::function<bool(const Track&)>& which) {
  if (fade <= 0) {
    std::erase_if(channel.tracks, which);
    return;
  }
  for (Track& track : channel.tracks) {
    if (which(track) && !track.fading_out) {
      track.fading_out = true;
      track.gain_step = -1 / (fade * kFramesPerSecond);
    }
  }
}

bool is_music(const Track& track) { return track.loop; }

// --- Serialized form -------------------------------------------------------------------------
// See engine/core/byte_stream.hpp. A u32 format version, a u32 count, then for each channel
// that is not at its defaults, in byte order of the names: the name, the volume (f64), and a u8
// that is 1 when the channel has music. Music is its name, then for the loop start and the loop
// end a u8 that is 1 when the call gave one, followed by the frame (u64).

constexpr std::uint32_t kFormatVersion = 1;
constexpr std::string_view kStateName = "audio state";

struct ChannelSettings {
  double volume = 1;
  std::optional<MusicSpec> music{};
};

bool read_flag(ByteReader& reader) {
  const std::uint8_t flag = reader.u8();
  if (flag > 1) {
    throw std::invalid_argument{"audio state: a flag is neither 0 nor 1"};
  }
  return flag == 1;
}

void write_flag(ByteWriter& writer, bool flag) {
  writer.u8(flag ? std::uint8_t{1} : std::uint8_t{0});
}

void write_frame(ByteWriter& writer, const std::optional<std::uint64_t>& frame) {
  write_flag(writer, frame.has_value());
  if (frame) {
    writer.u64(*frame);
  }
}

std::optional<std::uint64_t> read_frame(ByteReader& reader) {
  if (!read_flag(reader)) {
    return std::nullopt;
  }
  return reader.u64();
}

std::map<std::string, ChannelSettings, std::less<>> read_state(std::span<const std::byte> state) {
  std::map<std::string, ChannelSettings, std::less<>> channels;
  // A snapshot taken without audio has no bytes: every channel is at its defaults.
  if (state.empty()) {
    return channels;
  }
  ByteReader reader{state, kStateName};
  if (reader.u32() != kFormatVersion) {
    throw std::invalid_argument{"audio state: unknown format version"};
  }
  const std::uint32_t count = reader.u32();
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string name = reader.string();
    ChannelSettings settings;
    settings.volume = reader.f64();
    if (name.empty() || !(settings.volume >= 0 && settings.volume <= 1)) {
      throw std::invalid_argument{"audio state: a channel has no name or a volume out of range"};
    }
    if (read_flag(reader)) {
      MusicSpec music;
      music.name = reader.string();
      music.loop_start = read_frame(reader);
      music.loop_end = read_frame(reader);
      settings.music = std::move(music);
    }
    if (!channels.emplace(std::move(name), std::move(settings)).second) {
      throw std::invalid_argument{"audio state: a channel is there twice"};
    }
  }
  if (!reader.at_end()) {
    throw std::invalid_argument{"audio state: unexpected data after the last channel"};
  }
  return channels;
}

}  // namespace

struct Audio::Impl {
  Impl(const Vfs& files, AudioOptions audio_options) : vfs(&files), options(audio_options) {
    for (const std::string_view name : kDefaultChannels) {
      channels.try_emplace(std::string{name});
    }
    block.resize(kBlockFrames * kAudioChannels);
  }
  Impl(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() { close_device(); }

  void open_device();
  void close_device();

  /// The channel `name`, made if it is new. Call with `mutex` held.
  Channel& channel(std::string_view name) {
    const auto found = channels.find(name);
    return found != channels.end() ? found->second
                                   : channels.try_emplace(std::string{name}).first->second;
  }

  /// Opens audio name `name` as a track at its start: a music track when `music` is not null.
  /// Returns nothing, and why in `problem`, when it cannot.
  [[nodiscard]] std::optional<Track> make_track(std::string_view name, const MusicSpec* music,
                                                std::string& problem) const;
  std::string set_music(std::string_view channel_name, MusicSpec music, double fade);
  void reconcile();

  /// Adds the next out.size() / kAudioChannels frames of every channel to `out`. Call with
  /// `mutex` held.
  void mix(std::span<float> out);
  void mix_block(std::span<float> out);
  void mix_track(Track& track, double level, std::span<float> out);

  const Vfs* vfs;
  AudioOptions options;

  // Guards `channels` and `block`: the device thread mixes while the engine's thread changes
  // what plays. Files are opened before it is taken.
  // ponytail: the device thread decodes under this lock, so a command waits for at most one
  // device period. Hand tracks over through a queue if that ever shows up as a hitch.
  mutable std::mutex mutex;
  std::map<std::string, Channel, std::less<>> channels;
  std::vector<float> block;  ///< One block of one track, before it is mixed in.

  // Used by the engine's thread only.
  bool pending = false;  ///< restore() changed the music; the tracks follow at update().
  double carry = 0;      ///< The part of a frame update() has not mixed yet.
  std::vector<float> discard;

#if VNMAKI_AUDIO_DEVICE
  static void on_device(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
    auto* impl = static_cast<Impl*>(device->pUserData);
    const std::span<float> out{static_cast<float*>(output), frames * kAudioChannels};
    const std::scoped_lock lock{impl->mutex};
    impl->mix(out);  // miniaudio hands over a silent buffer.
  }

  ma_context context{};
  ma_device device{};
  bool has_context = false;
  bool has_device = false;
#endif
};

#if VNMAKI_AUDIO_DEVICE

void Audio::Impl::open_device() {
  if (options.output == AudioOutput::kManual) {
    return;
  }
  const std::array<ma_backend, 1> null_backend{ma_backend_null};
  const bool null_device = options.output == AudioOutput::kNullDevice;
  if (ma_context_init(null_device ? null_backend.data() : nullptr, null_device ? 1 : 0, nullptr,
                      &context) != MA_SUCCESS) {
    throw AudioError{"cannot start the audio system"};
  }
  has_context = true;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = static_cast<ma_uint32>(kAudioChannels);
  config.sampleRate = kAudioSampleRate;
  config.dataCallback = &Impl::on_device;
  config.pUserData = this;
  if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
    close_device();
    throw AudioError{"cannot open the audio device"};
  }
  has_device = true;
  if (ma_device_start(&device) != MA_SUCCESS) {
    close_device();
    throw AudioError{"cannot start the audio device"};
  }
}

void Audio::Impl::close_device() {
  if (has_device) {
    ma_device_uninit(&device);  // Stops the device thread.
    has_device = false;
  }
  if (has_context) {
    ma_context_uninit(&context);
    has_context = false;
  }
}

bool Audio::device_supported() noexcept { return true; }

#else

void Audio::Impl::open_device() {
  if (options.output != AudioOutput::kManual) {
    throw AudioError{"this build has no audio device (VNMAKI_ENABLE_THREADS is off)"};
  }
}

void Audio::Impl::close_device() {}

bool Audio::device_supported() noexcept { return false; }

#endif

std::optional<Track> Audio::Impl::make_track(std::string_view name, const MusicSpec* music,
                                             std::string& problem) const {
  for (const std::string_view extension : kAudioExtensions) {
    const std::string path = audio_path(name, extension);
    if (!vfs->exists(path)) {
      continue;
    }
    std::optional<std::vector<std::byte>> bytes = vfs->read(path);
    if (!bytes) {
      problem = quoted_audio(name) + ": cannot read " + path;
      return std::nullopt;
    }
    try {
      AudioDecoder decoder{std::move(*bytes)};
      AudioLoopPoints points;
      if (music != nullptr) {
        const AudioLoopPoints tagged = decoder.loop_points().value_or(AudioLoopPoints{});
        points.start = music->loop_start.value_or(tagged.start);
        points.end = music->loop_end.value_or(tagged.end);
        if (points.end != 0 && points.end <= points.start) {
          problem = "option 'loop_end' must be after 'loop_start'";
          return std::nullopt;
        }
      }
      return Track{.name = std::string{name},
                   .music = music != nullptr ? std::optional{*music} : std::nullopt,
                   .decoder = std::move(decoder),
                   .loop_points = points,
                   .loop = music != nullptr};
    } catch (const AudioError& error) {
      problem = quoted_audio(name) + ": " + path + " " + error.what();
      return std::nullopt;
    }
  }
  problem = missing_audio(name);
  return std::nullopt;
}

std::string Audio::Impl::set_music(std::string_view channel_name, MusicSpec music, double fade) {
  {
    const std::scoped_lock lock{mutex};
    Channel& target = channel(channel_name);
    if (plays(target, music)) {
      target.music = std::move(music);
      return {};
    }
  }
  if (pending) {
    // A replay is running: only the state changes. reconcile() starts the track.
    const bool exists = std::any_of(kAudioExtensions.begin(), kAudioExtensions.end(),
                                    [this, &music](std::string_view extension) {
                                      return vfs->exists(audio_path(music.name, extension));
                                    });
    if (!exists) {
      return missing_audio(music.name);
    }
    const std::scoped_lock lock{mutex};
    channel(channel_name).music = std::move(music);
    return {};
  }
  std::string problem;
  std::optional<Track> track = make_track(music.name, &music, problem);
  if (!track) {
    return problem;
  }
  if (fade > 0) {
    track->gain = 0;
    track->gain_step = 1 / (fade * kFramesPerSecond);
  }
  const std::scoped_lock lock{mutex};
  Channel& target = channel(channel_name);
  fade_out(target, fade, is_music);
  target.music = std::move(music);
  target.tracks.push_back(std::move(*track));
  return {};
}

// Makes the music tracks match each channel's music, with no fade: the state was restored, and
// the player should hear the step as it was.
void Audio::Impl::reconcile() {
  pending = false;
  std::vector<std::pair<std::string, std::optional<MusicSpec>>> wanted;
  {
    const std::scoped_lock lock{mutex};
    wanted.reserve(channels.size());
    for (const auto& [name, target] : channels) {
      wanted.emplace_back(name, target.music);
    }
  }
  std::string problems;
  for (const auto& [name, music] : wanted) {
    std::optional<Track> track;
    bool keep = false;
    {
      const std::scoped_lock lock{mutex};
      keep = music.has_value() && plays(channel(name), *music);
    }
    if (music.has_value() && !keep) {
      std::string problem;
      track = make_track(music->name, &*music, problem);
      if (!track) {
        problems += problems.empty() ? "" : "; ";
        problems += problem;
      }
    }
    const std::scoped_lock lock{mutex};
    Channel& target = channel(name);
    std::erase_if(target.tracks, [keep, &music](const Track& old) {
      return is_music(old) && !(keep && !old.fading_out && old.music == music);
    });
    if (track) {
      target.tracks.push_back(std::move(*track));
    }
  }
  if (!problems.empty()) {
    throw AudioError{problems};
  }
}

void Audio::Impl::mix(std::span<float> out) {
  while (!out.empty()) {
    const std::size_t samples = std::min(out.size(), kBlockFrames * kAudioChannels);
    mix_block(out.first(samples));
    out = out.subspan(samples);
  }
}

void Audio::Impl::mix_block(std::span<float> out) {
  const auto voice = channels.find(kVoiceChannel);
  const bool voice_plays = voice != channels.end() && !voice->second.tracks.empty();
  const std::size_t frames = out.size() / kAudioChannels;
  const double seconds = static_cast<double>(frames) / kFramesPerSecond;
  for (auto& [name, target] : channels) {
    if (name == kMusicChannel) {
      const double goal = voice_plays ? options.duck_volume : 1;
      const double step =
          options.duck_fade > 0 ? (1 - options.duck_volume) * seconds / options.duck_fade : 1;
      target.duck = target.duck < goal ? std::min(target.duck + step, goal)
                                       : std::max(target.duck - step, goal);
    }
    const double level = target.volume * target.duck;
    for (Track& track : target.tracks) {
      mix_track(track, level, out);
    }
    std::erase_if(target.tracks, [](const Track& track) { return track.ended; });
  }
}

void Audio::Impl::mix_track(Track& track, double level, std::span<float> out) {
  const std::size_t total = out.size() / kAudioChannels;
  std::size_t mixed = 0;
  bool restarted = false;  // The track went back to its loop start and has given nothing since.
  while (mixed < total && !track.ended) {
    std::size_t wanted = total - mixed;
    const bool before_loop_end = track.loop && track.loop_points.end > track.position;
    if (before_loop_end) {
      wanted = static_cast<std::size_t>(
          std::min<std::uint64_t>(wanted, track.loop_points.end - track.position));
    }
    const std::span<float> decoded = std::span{block}.first(wanted * kAudioChannels);
    const std::size_t got = track.decoder.read(decoded);
    if (got > 0) {
      restarted = false;
    }
    const std::span<float> to = out.subspan(mixed * kAudioChannels);
    for (std::size_t frame = 0; frame < got; ++frame) {
      const auto gain = static_cast<float>(track.gain * level);
      for (std::size_t side = 0; side < kAudioChannels; ++side) {
        const std::size_t sample = (frame * kAudioChannels) + side;
        to[sample] += decoded[sample] * gain;
      }
      track.gain = std::clamp(track.gain + track.gain_step, 0.0, 1.0);
    }
    track.position += got;
    mixed += got;
    if (track.fading_out && track.gain <= 0) {
      track.ended = true;
      break;
    }
    const bool at_end =
        got < wanted || (before_loop_end && track.position >= track.loop_points.end);
    if (!at_end) {
      continue;
    }
    // A loop that gives no frames (its start is past the end of the file) would never finish.
    const bool empty_loop = restarted && got == 0;
    if (!track.loop || empty_loop || !track.decoder.seek(track.loop_points.start)) {
      track.ended = true;
      break;
    }
    track.position = track.loop_points.start;
    restarted = true;
  }
}

Audio::Audio(const Vfs& vfs, AudioOptions options) : impl_(std::make_unique<Impl>(vfs, options)) {
  impl_->open_device();
}

Audio::~Audio() = default;

std::string Audio::music(const MusicRequest& request) {
  if (request.channel.empty()) {
    return "the channel name is empty";
  }
  if (!seconds_in_range(request.fade)) {
    return bad_seconds("fade");
  }
  MusicSpec music;
  music.name = request.name;
  if (request.loop_start) {
    if (!seconds_in_range(*request.loop_start)) {
      return bad_seconds("loop_start");
    }
    music.loop_start = frames_of(*request.loop_start);
  }
  if (request.loop_end) {
    if (!seconds_in_range(*request.loop_end)) {
      return bad_seconds("loop_end");
    }
    music.loop_end = frames_of(*request.loop_end);
  }
  return impl_->set_music(request.channel, std::move(music), request.fade);
}

std::string Audio::sound(const SoundRequest& request) {
  if (request.channel.empty()) {
    return "the channel name is empty";
  }
  std::string problem;
  std::optional<Track> track = impl_->make_track(request.name, nullptr, problem);
  if (!track) {
    return problem;
  }
  const std::scoped_lock lock{impl_->mutex};
  impl_->channel(request.channel).tracks.push_back(std::move(*track));
  return {};
}

std::string Audio::voice(std::string_view name) {
  std::string file{kVoicePrefix};
  file += name;
  std::string problem;
  std::optional<Track> track = impl_->make_track(file, nullptr, problem);
  if (!track) {
    return problem;
  }
  const std::scoped_lock lock{impl_->mutex};
  Channel& voices = impl_->channel(kVoiceChannel);
  voices.tracks.clear();
  voices.tracks.push_back(std::move(*track));
  return {};
}

std::string Audio::stop(std::string_view channel_name, double fade) {
  if (channel_name.empty()) {
    return "the channel name is empty";
  }
  if (!seconds_in_range(fade)) {
    return bad_seconds("fade");
  }
  const std::scoped_lock lock{impl_->mutex};
  Channel& target = impl_->channel(channel_name);
  target.music.reset();
  // After restore() the music tracks wait for update(), which may find them wanted again.
  const bool keep_music = impl_->pending;
  fade_out(target, fade,
           [keep_music](const Track& track) { return !(keep_music && is_music(track)); });
  return {};
}

std::string Audio::volume(std::string_view channel_name, double level) {
  if (channel_name.empty()) {
    return "the channel name is empty";
  }
  if (!(level >= 0 && level <= 1)) {
    return "volume must be a number from 0 to 1";
  }
  const std::scoped_lock lock{impl_->mutex};
  impl_->channel(channel_name).volume = level;
  return {};
}

std::vector<std::byte> Audio::capture() const {
  const std::scoped_lock lock{impl_->mutex};
  const auto changed = [](const Channel& entry) {
    return entry.volume < 1 || entry.music.has_value();
  };
  std::uint32_t count = 0;
  for (const auto& [name, entry] : impl_->channels) {
    if (changed(entry)) {
      ++count;
    }
  }
  ByteWriter writer;
  writer.u32(kFormatVersion);
  writer.u32(count);
  for (const auto& [name, entry] : impl_->channels) {
    if (!changed(entry)) {
      continue;
    }
    writer.string(name);
    writer.f64(entry.volume);
    write_flag(writer, entry.music.has_value());
    if (entry.music) {
      writer.string(entry.music->name);
      write_frame(writer, entry.music->loop_start);
      write_frame(writer, entry.music->loop_end);
    }
  }
  return writer.take();
}

void Audio::restore(std::span<const std::byte> state) {
  std::map<std::string, ChannelSettings, std::less<>> settings = read_state(state);
  const std::scoped_lock lock{impl_->mutex};
  for (auto& [name, entry] : impl_->channels) {
    entry.volume = 1;
    entry.music.reset();
    // Sounds and voices are not state: where the story goes back to, they had not played.
    std::erase_if(entry.tracks, [](const Track& track) { return !is_music(track); });
  }
  for (auto& [name, restored] : settings) {
    Channel& entry = impl_->channel(name);
    entry.volume = restored.volume;
    entry.music = std::move(restored.music);
  }
  impl_->pending = true;
}

void Audio::update(double seconds) {
  if (impl_->pending) {
    impl_->reconcile();
  }
  if (impl_->options.output != AudioOutput::kManual || !(seconds > 0)) {
    return;
  }
  const double frames = (std::min(seconds, kMaxUpdateSeconds) * kFramesPerSecond) + impl_->carry;
  const double whole = std::round(frames);
  impl_->carry = frames - whole;
  auto left = static_cast<std::size_t>(std::max(whole, 0.0));
  impl_->discard.resize(kBlockFrames * kAudioChannels);
  const std::scoped_lock lock{impl_->mutex};
  while (left > 0) {
    const std::size_t count = std::min(left, kBlockFrames);
    const std::span<float> out = std::span{impl_->discard}.first(count * kAudioChannels);
    std::fill(out.begin(), out.end(), 0.0F);
    impl_->mix(out);
    left -= count;
  }
}

std::vector<std::string> Audio::channels() const {
  const std::scoped_lock lock{impl_->mutex};
  std::vector<std::string> names;
  names.reserve(impl_->channels.size());
  for (const auto& [name, entry] : impl_->channels) {
    names.push_back(name);
  }
  return names;
}

AudioChannelState Audio::channel(std::string_view name) const {
  const std::scoped_lock lock{impl_->mutex};
  AudioChannelState state;
  const auto found = impl_->channels.find(name);
  if (found == impl_->channels.end()) {
    return state;
  }
  const Channel& entry = found->second;
  state.volume = entry.volume;
  state.duck = entry.duck;
  state.music = entry.music ? entry.music->name : std::string{};
  state.tracks.reserve(entry.tracks.size());
  for (const Track& track : entry.tracks) {
    state.tracks.push_back(AudioTrackState{.name = track.name,
                                           .loop = track.loop,
                                           .loop_points = track.loop_points,
                                           .position = track.position,
                                           .gain = track.gain,
                                           .fading_out = track.fading_out});
  }
  return state;
}

bool Audio::voice_playing() const {
  const std::scoped_lock lock{impl_->mutex};
  const auto voice = impl_->channels.find(kVoiceChannel);
  return voice != impl_->channels.end() && !voice->second.tracks.empty();
}

}  // namespace vnmaki
