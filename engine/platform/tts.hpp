#pragma once

// Text to speech for self-voicing (docs/SPEC.md section 17). The engine speaks through a
// TtsBackend: SAPI on Windows (win_tts.cpp), a null backend elsewhere. A backend belongs to the
// thread that made it (COM on Windows): call it from that thread only.

#include <chrono>
#include <memory>
#include <string_view>

namespace vnmaki {

class TtsBackend {
 public:
  TtsBackend() = default;
  TtsBackend(const TtsBackend&) = delete;
  TtsBackend& operator=(const TtsBackend&) = delete;
  TtsBackend(TtsBackend&&) = delete;
  TtsBackend& operator=(TtsBackend&&) = delete;
  virtual ~TtsBackend() = default;

  /// Starts to speak `utf8` and returns at once. It stops the speech in progress first.
  virtual void speak(std::string_view utf8) = 0;
  /// Stops the speech in progress.
  virtual void stop() = 0;
  /// Waits until the speech ends, at most `timeout`. Returns whether it ended.
  [[nodiscard]] virtual bool wait_until_done(std::chrono::milliseconds timeout) = 0;
  /// False if nothing can be heard: the null backend, or no voice installed.
  [[nodiscard]] virtual bool available() const = 0;
};

/// Speaks nothing.
class NullTts final : public TtsBackend {
 public:
  void speak(std::string_view /*utf8*/) override {}
  void stop() override {}
  [[nodiscard]] bool wait_until_done(std::chrono::milliseconds /*timeout*/) override {
    return true;
  }
  [[nodiscard]] bool available() const override { return false; }
};

/// The system's TTS (SAPI on Windows) at `volume_percent` (1 to 100), or a NullTts. Never null,
/// never throws; a machine without a voice gives a backend with available() false.
[[nodiscard]] std::unique_ptr<TtsBackend> make_system_tts(unsigned volume_percent = 100);

}  // namespace vnmaki
