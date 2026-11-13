#include "engine/assets/import_cache.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include "engine/core/utf8_path.hpp"

namespace vnmaki {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr unsigned kHexDigits = 16;
constexpr unsigned kBitsPerDigit = 4;
constexpr std::uint64_t kDigitMask = 0xF;

std::uint64_t hash_more(std::uint64_t state, std::span<const std::byte> bytes) {
  for (const std::byte byte : bytes) {
    state = (state ^ std::to_integer<std::uint64_t>(byte)) * kFnvPrime;
  }
  return state;
}

std::uint64_t hash_text(std::uint64_t state, std::string_view text) {
  for (const char c : text) {
    state = (state ^ static_cast<unsigned char>(c)) * kFnvPrime;
  }
  return state * kFnvPrime;  // the NUL byte that ends the field
}

}  // namespace

std::uint64_t hash_bytes(std::span<const std::byte> bytes) { return hash_more(kFnvOffset, bytes); }

std::string import_cache_key(std::span<const std::byte> source, std::string_view importer,
                             std::uint32_t version, std::string_view params) {
  std::uint64_t state = hash_more(kFnvOffset, source);
  state *= kFnvPrime;  // the NUL byte that ends the source
  state = hash_text(state, importer);
  state = hash_text(state, std::to_string(version));
  state = hash_text(state, params);
  constexpr std::string_view digits = "0123456789abcdef";
  std::string key{importer};
  key += '-';
  for (unsigned i = kHexDigits; i > 0; --i) {
    key += digits[(state >> ((i - 1) * kBitsPerDigit)) & kDigitMask];
  }
  return key;
}

std::optional<std::vector<std::byte>> ImportCache::find(const std::string& key) const {
  std::ifstream in{directory_ / key, std::ios::binary};
  if (!in) {
    return std::nullopt;
  }
  const std::vector<char> chars{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes;
  bytes.reserve(chars.size());
  for (const char c : chars) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

void ImportCache::store(const std::string& key, std::span<const std::byte> data) const {
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  std::filesystem::path temporary = directory_ / key;
  temporary += ".tmp";
  {
    std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out.flush()) {
      throw std::runtime_error{"cannot write " + to_utf8(temporary)};
    }
  }
  std::filesystem::rename(temporary, directory_ / key, error);
  if (error) {
    throw std::runtime_error{"cannot write " + to_utf8(directory_ / key) + ": " +
                             error.message()};
  }
}

}  // namespace vnmaki
