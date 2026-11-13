#include "engine/saves/saves.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "engine/core/version.hpp"
#include "engine/saves/save_file.hpp"
#include "engine/saves/thumbnail.hpp"

namespace vnmaki {

namespace {

constexpr std::size_t kMaxSlotNameLength = 32;
// A save file bigger than the largest payload can only be damaged: it compresses at least as
// well as it expands only for data that is not worth saving.
constexpr std::uintmax_t kMaxSaveFileBytes = kMaxSavePayloadBytes;
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8Continuation = 0x80;

bool valid_slot(std::string_view slot) {
  if (slot.empty() || slot.size() > kMaxSlotNameLength) {
    return false;
  }
  return std::ranges::all_of(slot, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

bool all_digits(std::string_view text) {
  return std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// Numbered slots in number order, then the others by name.
bool slot_before(const std::string& a, const std::string& b) {
  const bool a_number = all_digits(a);
  const bool b_number = all_digits(b);
  if (a_number != b_number) {
    return a_number;
  }
  if (a_number && a.size() != b.size()) {
    return a.size() < b.size();
  }
  return a < b;
}

// The first `limit` bytes of `text`, cut before a UTF-8 sequence rather than inside one.
std::string truncated(std::string text, std::size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  std::size_t cut = limit;
  while (cut > 0 &&
         (static_cast<unsigned char>(text.at(cut)) & kUtf8ContinuationMask) == kUtf8Continuation) {
    --cut;
  }
  text.resize(cut);
  return text;
}

std::string title_of(const ScriptRuntime& runtime) {
  const BlockingRequest* request = runtime.request();
  if (const auto* say = std::get_if<SayRequest>(request)) {
    return truncated(say->text, kMaxSaveTitleBytes);
  }
  if (const auto* narrate = std::get_if<NarrateRequest>(request)) {
    return truncated(narrate->text, kMaxSaveTitleBytes);
  }
  return truncated(runtime.position().label, kMaxSaveTitleBytes);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > kMaxSaveFileBytes) {
    throw SaveError{"save: the file cannot be read or is too large"};
  }
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    throw SaveError{"save: cannot open " + path.string()};
  }
  std::vector<std::byte> bytes;
  bytes.reserve(static_cast<std::size_t>(size));
  for (std::istreambuf_iterator<char> it{stream}, end; it != end; ++it) {
    bytes.push_back(static_cast<std::byte>(*it));
  }
  return bytes;
}

}  // namespace

Saves::Saves(ScriptRuntime& runtime, SavesOptions options)
    : runtime_(&runtime), options_(std::move(options)) {
  if (options_.engine_version.empty()) {
    options_.engine_version = std::string{engine_version_string()};
  }
}

std::filesystem::path Saves::path_of(std::string_view slot) const {
  return options_.directory / (std::string{slot} + std::string{kSaveExtension});
}

void Saves::warn(std::string_view message) const {
  if (options_.on_warning) {
    options_.on_warning(message);
  }
}

std::string Saves::capture(SaveData& data, bool full) const {
  const LabelSnapshot* snapshot = runtime_->label_snapshot();
  if (runtime_->state() != RunState::kWaiting || snapshot == nullptr) {
    return "nothing to save: the story is not waiting at a line or menu";
  }
  const ScriptPosition position = runtime_->position();
  data.engine_version = options_.engine_version;
  data.game_version = options_.game_version;
  data.label = position.label;
  data.step = position.step;
  data.source_hash = runtime_->label_source_hash(position.label).value_or(0);
  data.snapshot = *snapshot;
  const std::span<const InputResult> inputs = runtime_->input_log();
  data.inputs.assign(inputs.begin(), inputs.end());
  data.persistent = options_.persistent;
  data.title = title_of(*runtime_);
  if (full) {
    data.thumbnail = stub_thumbnail();
    if (options_.thumbnail) {
      try {
        data.thumbnail = options_.thumbnail();
      } catch (const std::exception& error) {
        warn(std::string{"thumbnail: "} + error.what());
      }
    }
    data.time = options_.clock ? options_.clock()
                               : std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
  }
  return {};
}

std::string Saves::write_file(std::string_view slot, const SaveData& data) const {
  const std::filesystem::path path = path_of(slot);
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  try {
    std::filesystem::create_directories(options_.directory);
    const std::vector<std::byte> bytes = encode_save(data);
    {
      std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
      stream.flush();
      if (!stream) {
        return "cannot write " + temporary.string();
      }
    }
    // A save is written beside its slot and then renamed over it, so a crash leaves the old one.
    std::filesystem::rename(temporary, path);
  } catch (const std::exception& error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return std::string{"cannot write slot '"} + std::string{slot} + "': " + error.what();
  }
  return {};
}

std::string Saves::save(std::string_view slot) {
  if (!valid_slot(slot)) {
    return "bad slot name '" + std::string{slot} + "'";
  }
  SaveData data;
  if (std::string problem = capture(data, true); !problem.empty()) {
    return problem;
  }
  return write_file(slot, data);
}

std::string Saves::load(std::string_view slot) {
  if (!valid_slot(slot)) {
    return "bad slot name '" + std::string{slot} + "'";
  }
  const std::string where = "slot '" + std::string{slot} + "'";
  SaveData data;
  try {
    if (!std::filesystem::exists(path_of(slot))) {
      return where + " is empty";
    }
    data = decode_save(read_file(path_of(slot)));
  } catch (const std::exception& error) {
    return where + ": " + error.what();
  }
  if (!runtime_->has_label(data.label)) {
    return where + ": the label '" + data.label + "' no longer exists";
  }
  if (std::string problem = runtime_->run_on_load(data.snapshot, data.game_version);
      !problem.empty()) {
    return where + ": " + problem;
  }

  // Where the story is now, to go back to it if the replay fails.
  SaveData before;
  const bool can_go_back = capture(before, false).empty();

  const bool source_changed = runtime_->label_source_hash(data.label) != data.source_hash;
  runtime_->begin_load();
  if (source_changed) {
    warn(where + ": label '" + data.label +
         "' changed since the game was saved; starting it from the beginning");
    runtime_->replay(data.snapshot, {}, 0);
  } else {
    runtime_->replay(data.snapshot, data.inputs, data.step);
  }
  if (runtime_->state() == RunState::kFailed) {
    std::string problem =
        where + ": the story could not follow the save (" + runtime_->error() + ")";
    bool went_back = false;
    if (can_go_back) {
      runtime_->replay(before.snapshot, before.inputs, before.step);
      went_back = runtime_->state() != RunState::kFailed;
      if (!went_back) {
        problem += "; and it could not go back (" + runtime_->error() + ")";
      }
    }
    runtime_->end_load(went_back);
    return problem;
  }
  runtime_->end_load(false);
  last_position_ = runtime_->position();
  return {};
}

std::string Saves::remove(std::string_view slot) {
  if (!valid_slot(slot)) {
    return "bad slot name '" + std::string{slot} + "'";
  }
  std::error_code error;
  std::filesystem::remove(path_of(slot), error);
  return error ? "cannot delete slot '" + std::string{slot} + "': " + error.message()
               : std::string{};
}

bool Saves::exists(std::string_view slot) const {
  std::error_code ignored;
  return valid_slot(slot) && std::filesystem::exists(path_of(slot), ignored);
}

std::vector<SlotInfo> Saves::list() const {
  std::vector<SlotInfo> slots;
  std::error_code error;
  std::filesystem::directory_iterator entries{options_.directory, error};
  if (error) {
    return slots;
  }
  for (const std::filesystem::directory_entry& entry : entries) {
    const std::filesystem::path& path = entry.path();
    if (path.extension() != kSaveExtension || !valid_slot(path.stem().string())) {
      continue;
    }
    SlotInfo info{.slot = path.stem().string()};
    try {
      SaveData data = decode_save(read_file(path));
      info.title = std::move(data.title);
      info.label = std::move(data.label);
      info.game_version = std::move(data.game_version);
      info.time = data.time;
      info.thumbnail = std::move(data.thumbnail);
    } catch (const std::exception& problem) {
      info.error = problem.what();
    }
    slots.push_back(std::move(info));
  }
  std::ranges::sort(
      slots, [](const SlotInfo& a, const SlotInfo& b) { return slot_before(a.slot, b.slot); });
  return slots;
}

void Saves::poll() {
  if (runtime_->state() != RunState::kWaiting) {
    return;
  }
  for (const SaveRequest& request : runtime_->take_save_requests()) {
    const std::string problem = request.load ? load(request.slot) : save(request.slot);
    if (!problem.empty()) {
      warn(std::string{request.load ? "load" : "save"} + ": " + problem);
    } else if (request.load) {
      break;  // the rest came from the story that the load replaced
    }
  }
  auto_save_if_due();
}

void Saves::auto_save_if_due() {
  if (options_.auto_save.empty() || runtime_->state() != RunState::kWaiting) {
    return;
  }
  const ScriptPosition position = runtime_->position();
  if (last_position_ == position) {
    return;
  }
  last_position_ = position;
  const bool at_menu = std::holds_alternative<MenuRequest>(*runtime_->request());
  const auto due = [&](AutoSaveEvent event) {
    return std::ranges::find(options_.auto_save, event) != options_.auto_save.end();
  };
  if ((due(AutoSaveEvent::kLabel) && position.step == 1) ||
      (due(AutoSaveEvent::kMenu) && at_menu)) {
    if (std::string problem = auto_save(); !problem.empty()) {
      warn("auto save: " + problem);
    }
  }
}

}  // namespace vnmaki
