#include "engine/saves/persistent_store.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/saves/save_file.hpp"

namespace vnmaki {

namespace {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > kMaxSavePayloadBytes) {
    throw SaveError{"persistent: the file cannot be read or is too large"};
  }
  std::ifstream stream{path, std::ios::binary};
  std::vector<std::byte> bytes;
  bytes.reserve(static_cast<std::size_t>(size));
  for (std::istreambuf_iterator<char> it{stream}, end; it != end; ++it) {
    bytes.push_back(static_cast<std::byte>(*it));
  }
  return bytes;
}

}  // namespace

PersistentStore::PersistentStore(ScriptRuntime& runtime, std::filesystem::path directory,
                                 std::function<void(std::string_view)> on_warning)
    : runtime_(&runtime),
      file_(std::move(directory) / kPersistentFileName),
      on_warning_(std::move(on_warning)) {}

PersistentStore::~PersistentStore() {
  flush();  // flush() reports its problems and does not throw
}

void PersistentStore::warn(std::string_view message) const {
  if (on_warning_) {
    on_warning_(message);
  }
}

void PersistentStore::load() {
  try {
    std::error_code ignored;
    if (std::filesystem::exists(file_, ignored)) {
      PersistentData data = decode_persistent(read_file(file_));
      if (std::string problem = runtime_->restore_persistent(data.store); !problem.empty()) {
        throw SaveError{problem};
      }
      seen_.insert(data.seen.begin(), data.seen.end());
    }
    written_ = runtime_->persistent_snapshot();
  } catch (const std::exception& error) {
    // Keep the unreadable file (a newer game's data, or a repairable one): flush() must not
    // replace it with defaults.
    std::filesystem::path bad = file_;
    bad += ".bad";
    std::error_code moved;
    std::filesystem::rename(file_, bad, moved);
    written_ = runtime_->persistent_snapshot();
    warn("persistent: " + std::string{error.what()} + "; starting with no saved data" +
         (moved ? "" : " (the old file is kept as " + bad.filename().string() + ")"));
  }
}

void PersistentStore::flush() {
  try {
    StoreSnapshot now = runtime_->persistent_snapshot();
    if (now == written_ && !seen_dirty_) {
      return;
    }
    PersistentData data{.store = now, .seen = {seen_.begin(), seen_.end()}};
    const std::vector<std::byte> bytes = encode_persistent(data);
    std::filesystem::path temporary = file_;
    temporary += ".tmp";
    std::filesystem::create_directories(file_.parent_path());
    {
      std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
      stream.flush();
      if (!stream) {
        throw std::runtime_error{"cannot write " + temporary.string()};
      }
    }
    // Written beside the file and renamed over it, so a crash leaves the old one.
    std::filesystem::rename(temporary, file_);
    written_ = std::move(now);
    seen_dirty_ = false;
    warned_ = false;
  } catch (const std::exception& error) {
    if (!warned_) {  // a failing disk would otherwise warn on every frame
      warned_ = true;
      warn("persistent: " + std::string{error.what()});
    }
  }
}

SeenStep PersistentStore::key_of(const ScriptPosition& position) const {
  return SeenStep{.label = position.label,
                  .step = position.step,
                  .hash = runtime_->label_source_hash(position.label).value_or(0)};
}

bool PersistentStore::seen(const ScriptPosition& position) const {
  return seen_.contains(key_of(position));
}

void PersistentStore::mark_seen(const ScriptPosition& position) {
  if (seen_.insert(key_of(position)).second) {
    seen_dirty_ = true;
  }
}

}  // namespace vnmaki
