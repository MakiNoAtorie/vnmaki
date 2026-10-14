#pragma once

// The clips tools/generators/audio_fixtures.py writes to fixtures/audio/files.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace vnmaki::test {

/// The bytes of clip `name` ("tone.wav"), as memory_vfs takes them.
inline std::string audio_fixture(const std::string& name) {
  const std::filesystem::path path =
      std::filesystem::path{VNMAKI_FIXTURES_DIR} / "audio" / "files" / name;
  std::ifstream file{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"missing audio fixture " + name};
  }
  return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

inline std::vector<std::byte> audio_fixture_bytes(const std::string& name) {
  const std::string text = audio_fixture(name);
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

}  // namespace vnmaki::test
