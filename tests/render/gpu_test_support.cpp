#include "tests/render/gpu_test_support.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/render/renderer.hpp"

#if defined(__SANITIZE_ADDRESS__)
#define VNMAKI_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VNMAKI_ASAN 1
#endif
#endif

#if defined(VNMAKI_ASAN) && !defined(_MSC_VER)
// LeakSanitizer reports false leaks in GPU processes: the driver (e.g. Mesa's lavapipe and its
// LLVM) is unloaded when the device is destroyed, so memory its globals still point to looks
// leaked; with dlclose() disabled the reports vanish. Leak checking stays on for every test
// executable that does not load a GPU driver (tests/cli/CMakeLists.txt turns it off for the CLI
// tests that do).
extern "C" const char* __asan_default_options() { return "detect_leaks=0"; }  // NOLINT
#endif

#if defined(__SANITIZE_THREAD__)
#define VNMAKI_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VNMAKI_TSAN 1
#endif
#endif

#if defined(VNMAKI_TSAN)
// Races inside the GPU driver's own threads are not the engine's
// (tests/render/gpu_driver.tsan.supp).
extern "C" const char* __tsan_default_options() {  // NOLINT
  return "suppressions=" VNMAKI_GPU_TSAN_SUPPRESSIONS;
}
#endif

namespace vnmaki::test {

namespace {

const std::filesystem::path kFixtures = std::filesystem::path{VNMAKI_FIXTURES_DIR} / "render";

Image load_png(const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::binary};
  REQUIRE_MESSAGE(file, "missing fixture " << path.string());
  const std::vector<char> chars{std::istreambuf_iterator<char>{file},
                                std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes;
  bytes.reserve(chars.size());
  for (const char c : chars) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return decode_image(bytes);
}

/// Writes a PAM (portable arbitrary map) file: a tiny header and the raw RGBA bytes.
void write_pam(const std::filesystem::path& path, const Image& image) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file{path, std::ios::binary};
  file << "P7\nWIDTH " << image.size.width << "\nHEIGHT " << image.size.height
       << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
  for (const std::uint8_t byte : image.pixels) {
    file.put(static_cast<char>(byte));
  }
}

}  // namespace

void skip_without_gpu(const std::string& reason) {
  const char* required = std::getenv("VNMAKI_REQUIRE_GPU");  // NOLINT(concurrency-mt-unsafe)
  if (required != nullptr && std::string{required} == "1") {
    FAIL("no GPU although VNMAKI_REQUIRE_GPU=1: " << reason);
  }
  std::cout << "VNMAKI_SKIP: no usable GPU device on this machine: " << reason << '\n';
}

std::unique_ptr<SdlGpuRenderer> make_headless_renderer() {
  try {
    return SdlGpuRenderer::create_headless(RendererConfig{.debug = false, .vsync = false});
  } catch (const RenderError& error) {
    skip_without_gpu(error.what());
    return nullptr;
  }
}

void check_golden(const Image& actual, const std::string& name, int tolerance) {
  const Image expected = load_png(kFixtures / "golden" / (name + ".png"));
  REQUIRE(actual.size == expected.size);
  REQUIRE(actual.pixels.size() == expected.pixels.size());

  constexpr std::size_t kChannels = 4;
  std::size_t mismatches = 0;
  std::size_t first_mismatch = 0;
  for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
    if (std::abs(int{actual.pixels[i]} - int{expected.pixels[i]}) > tolerance) {
      if (mismatches == 0) {
        first_mismatch = i - (i % kChannels);
      }
      ++mismatches;
    }
  }
  if (mismatches == 0) {
    return;
  }
  const std::filesystem::path dump = std::filesystem::path{"golden-actual"} / (name + ".pam");
  write_pam(dump, actual);
  const auto pixel = first_mismatch / kChannels;
  const auto width = static_cast<std::size_t>(expected.size.width);
  const auto channels = [&](const Image& image) {
    std::string text;
    for (std::size_t c = 0; c < kChannels; ++c) {
      text += (c == 0 ? "" : ",") + std::to_string(image.pixels[first_mismatch + c]);
    }
    return text;
  };
  FAIL("golden image '" << name << "': " << mismatches << " channel values differ by more than "
                        << tolerance << "; first at pixel (" << pixel % width << ", "
                        << pixel / width << "): actual " << channels(actual) << ", expected "
                        << channels(expected) << ". Actual image written to "
                        << std::filesystem::absolute(dump).string());
}

Image load_fixture_image(const std::string& name) { return load_png(kFixtures / "images" / name); }

}  // namespace vnmaki::test
