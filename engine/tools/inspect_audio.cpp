#include "engine/tools/inspect.hpp"

#include <string>
#include <utility>
#include <vector>

#include "engine/audio/audio_decoder.hpp"

namespace vnmaki {

std::vector<AudioRow> inspect_audio(const Audio& audio) {
  std::vector<AudioRow> rows;
  for (const std::string& name : audio.channels()) {
    const AudioChannelState channel = audio.channel(name);
    if (channel.tracks.empty()) {
      rows.push_back({.channel = name, .volume = channel.volume});
    }
    for (const AudioTrackState& track : channel.tracks) {
      rows.push_back({.channel = name,
                      .file = track.name,
                      .volume = channel.volume,
                      .position = static_cast<double>(track.position) / kAudioSampleRate,
                      .loop = track.loop});
    }
  }
  return rows;
}

}  // namespace vnmaki
