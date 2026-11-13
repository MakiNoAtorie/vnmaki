// The system TTS backend (docs/SPEC.md section 17). On Windows it speaks one short word through
// SAPI at 1% volume; a machine without a SAPI voice prints VNMAKI_SKIP and ctest skips the test.

#include <chrono>
#include <iostream>
#include <memory>

#include <doctest/doctest.h>

#include "engine/platform/tts.hpp"

namespace {

constexpr unsigned kQuiet = 1;
constexpr std::chrono::seconds kTimeout{20};

}  // namespace

TEST_CASE("tts: the null backend says it cannot be heard") {
  vnmaki::NullTts tts;
  tts.speak("nothing");
  tts.stop();
  CHECK_FALSE(tts.available());
  CHECK(tts.wait_until_done(std::chrono::milliseconds{0}));
}

TEST_CASE("tts: the system backend speaks one short line and finishes") {
  const std::unique_ptr<vnmaki::TtsBackend> tts = vnmaki::make_system_tts(kQuiet);
  REQUIRE(tts != nullptr);
  if (!tts->available()) {
    std::cout << "VNMAKI_SKIP: no TTS voice is installed on this machine\n";
    return;
  }
  tts->speak("Hi");
  CHECK(tts->wait_until_done(kTimeout));

  // New speech stops the old: a long text that is replaced and then stopped ends at once.
  tts->speak("This sentence is long enough that it must be stopped before its end.");
  tts->speak("Ok");
  tts->stop();
  CHECK(tts->wait_until_done(kTimeout));
}
