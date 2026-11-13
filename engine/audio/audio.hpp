#pragma once

// Audio (docs/SPEC.md sections 7, 8 and 14): named channels that mix music, sounds and voices.
// The script calls `music`, `sound`, `voice`, `stop` and `volume` change it, and snapshots
// capture which music each channel plays.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/audio_decoder.hpp"
#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

class Vfs;

/// The channels every game has. A script makes another by naming it.
inline constexpr std::string_view kMusicChannel = "music";
inline constexpr std::string_view kSoundChannel = "sound";
inline constexpr std::string_view kVoiceChannel = "voice";
inline constexpr std::string_view kAmbientChannel = "ambient";
inline constexpr std::string_view kUiChannel = "ui";

/// The longest fade, and the latest loop point, a script may give, in seconds.
inline constexpr double kMaxAudioSeconds = 86'400.0;

inline constexpr double kDefaultDuckVolume = 0.4;
inline constexpr double kDefaultDuckFade = 0.25;

/// Where the mix goes.
enum class AudioOutput : std::uint8_t {
  kManual,      ///< Nowhere. Audio::update(seconds) mixes that much: tests and headless runs.
  kNullDevice,  ///< miniaudio's null device: a device thread mixes in real time, to nowhere.
  kDevice,      ///< The system's default playback device.
};

struct AudioOptions {
  AudioOutput output = AudioOutput::kManual;
  /// The music channel's gain while a voice plays, from 0 to 1 (1 turns ducking off).
  double duck_volume = kDefaultDuckVolume;
  /// Seconds the music takes to duck, and to come back.
  double duck_fade = kDefaultDuckFade;
};

/// One file playing on a channel.
struct AudioTrackState {
  std::string name{};  ///< As the script named it; a voice has the "voice/" prefix.
  bool loop = false;
  AudioLoopPoints loop_points{};
  std::uint64_t position = 0;  ///< The next frame of the file to play, at kAudioSampleRate.
  double gain = 1;             ///< The fade: 0 is silent, 1 is full.
  bool fading_out = false;     ///< Its gain falls to 0, and then it stops.
};

struct AudioChannelState {
  double volume = 1;
  double duck = 1;      ///< The gain ducking gives now: 1, or less while a voice plays.
  std::string music{};  ///< The music the channel is set to loop; empty for none.
  /// What is playing, oldest first. During a crossfade the old and the new music are both here.
  std::vector<AudioTrackState> tracks{};
};

/// The mixer.
///
/// Audio name `N` is the VFS file `audio/N.opus`, else `audio/N.ogg`, `audio/N.flac` or
/// `audio/N.wav`. `voice(N)` plays audio name `voice/N`.
///
/// `music` loops a file on a channel (default "music"), cross-fading from the music the channel
/// played; music that is already playing there carries on. `sound` plays a file once over
/// whatever else plays. `voice` plays a file once on the voice channel, in place of the voice
/// before it. While a voice plays, the music channel ducks. `stop` ends everything on a channel
/// and `volume` sets a channel's volume, from 0 to 1.
///
/// The snapshot state is each channel's volume and music. restore() sets it and stops sounds and
/// voices, but leaves the music tracks alone until the next update(): a replay restores a
/// snapshot and then runs the script calls again, and music that comes out the same never
/// stops. The runtime does not pass `sound` and `voice` on while it replays.
///
/// Call the commands, update() and restore() from one thread. With a device, mixing runs on the
/// device's thread; the state queries may be called from any thread.
class Audio final : public SceneState, public AudioCommands {
 public:
  /// `vfs` must outlive this. Throws AudioError when the output device cannot be opened, and
  /// for any output but kManual in a build without threads (see device_supported()).
  explicit Audio(const Vfs& vfs, AudioOptions options = {});
  Audio(const Audio&) = delete;
  Audio(Audio&&) = delete;
  Audio& operator=(const Audio&) = delete;
  Audio& operator=(Audio&&) = delete;
  ~Audio() override;

  /// False when the engine was built without threads (VNMAKI_ENABLE_THREADS=OFF).
  [[nodiscard]] static bool device_supported() noexcept;

  [[nodiscard]] std::string music(const MusicRequest& request) override;
  [[nodiscard]] std::string sound(const SoundRequest& request) override;
  /// Also how a backlog entry plays its voice again: `name` is what the script passed.
  [[nodiscard]] std::string voice(std::string_view name) override;
  [[nodiscard]] std::string stop(std::string_view channel_name, double fade) override;
  [[nodiscard]] std::string volume(std::string_view channel_name, double level) override;

  [[nodiscard]] std::vector<std::byte> capture() const override;
  void restore(std::span<const std::byte> state) override;

  /// Call once a frame. Starts and stops music tracks to match a restored state; throws
  /// AudioError, after doing the rest, if a file of that state cannot be played. With
  /// AudioOutput::kManual it then mixes `seconds` of audio.
  void update(double seconds);

  /// Every channel, in byte order of the names.
  [[nodiscard]] std::vector<std::string> channels() const;
  /// The state of channel `name`; a channel nothing has named yet is at its defaults.
  [[nodiscard]] AudioChannelState channel(std::string_view name) const;
  /// True while a voice plays: auto-advance waits for this to end.
  [[nodiscard]] bool voice_playing() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
