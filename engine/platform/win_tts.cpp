// Windows self-voicing: SAPI 5 through COM (docs/SPEC.md section 17). The COM apartment is the
// one of the thread that makes the backend, which must also destroy it.

#include <windows.h>

#include <sapi.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "engine/platform/tts.hpp"

namespace vnmaki {

namespace {

constexpr unsigned kMaxVolume = 100;

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                       nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
  return wide;
}

class SapiTts final : public TtsBackend {
 public:
  explicit SapiTts(unsigned volume_percent) {
    // S_FALSE means the thread had COM already; the call still counts as one use.
    com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                reinterpret_cast<void**>(&voice_)))) {
      voice_ = nullptr;
      return;
    }
    // A machine without an installed voice has no default voice token.
    ISpObjectToken* token = nullptr;
    if (FAILED(voice_->GetVoice(&token)) || token == nullptr) {
      voice_->Release();
      voice_ = nullptr;
      return;
    }
    token->Release();
    voice_->SetVolume(static_cast<USHORT>(volume_percent > kMaxVolume ? kMaxVolume
                                                                      : volume_percent));
  }

  ~SapiTts() override {
    if (voice_ != nullptr) {
      stop();
      voice_->Release();
    }
    if (com_) {
      CoUninitialize();
    }
  }

  SapiTts(const SapiTts&) = delete;
  SapiTts& operator=(const SapiTts&) = delete;
  SapiTts(SapiTts&&) = delete;
  SapiTts& operator=(SapiTts&&) = delete;

  void speak(std::string_view utf8) override {
    if (voice_ != nullptr) {
      // Asynchronous; the purge ends the speech in progress.
      voice_->Speak(widen(utf8).c_str(), sapi_speak_flags(), nullptr);
    }
  }

  void stop() override {
    if (voice_ != nullptr) {
      voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    }
  }

  [[nodiscard]] bool wait_until_done(std::chrono::milliseconds timeout) override {
    return voice_ == nullptr || voice_->WaitUntilDone(static_cast<ULONG>(timeout.count())) == S_OK;
  }

  [[nodiscard]] bool available() const override { return voice_ != nullptr; }

 private:
  ISpVoice* voice_ = nullptr;
  bool com_ = false;
};

}  // namespace

// SPF_IS_NOT_XML: without it SAPI reads text that starts with `<` as markup and drops or garbles it.
unsigned long sapi_speak_flags() { return SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML; }

std::unique_ptr<TtsBackend> make_system_tts(unsigned volume_percent) {
  return std::make_unique<SapiTts>(volume_percent);
}

}  // namespace vnmaki
