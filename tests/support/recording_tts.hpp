#pragma once

// A TtsBackend that speaks nothing and keeps what it is told, so a test can check the exact
// strings of self-voicing (docs/SPEC.md section 17) with no voice installed.

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/tts.hpp"

namespace vnmaki::test {

/// A speak stops the speech before it, as a real backend does; `stops` counts the stop() calls.
class RecordingTts final : public TtsBackend {
 public:
  void speak(std::string_view utf8) override { spoken.emplace_back(utf8); }
  void stop() override { ++stops; }
  [[nodiscard]] bool wait_until_done(std::chrono::milliseconds /*timeout*/) override {
    return true;
  }
  [[nodiscard]] bool available() const override { return true; }

  std::vector<std::string> spoken;
  std::size_t stops = 0;
};

}  // namespace vnmaki::test
