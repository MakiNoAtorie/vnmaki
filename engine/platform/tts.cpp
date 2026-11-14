// The system TTS where there is none: a null backend. Windows has win_tts.cpp.

#include "engine/platform/tts.hpp"

#include <memory>

namespace vnmaki {

std::unique_ptr<TtsBackend> make_system_tts(unsigned /*volume_percent*/) {
  return std::make_unique<NullTts>();
}

}  // namespace vnmaki
