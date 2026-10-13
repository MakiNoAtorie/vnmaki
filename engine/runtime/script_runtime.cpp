#include "engine/runtime/script_runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sol/sol.hpp>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

// The script-facing API, in Lua so argument errors and "unknown label" errors
// point at the caller's script and line (error level 2). It runs once per VM
// with the engine primitives as arguments; they never become globals, so
// scripts reach the engine only through the functions defined here.
constexpr std::string_view kPrelude = R"lua(
local engine, dev_mode = ...

local debug_getinfo, debug_sethook = debug.getinfo, debug.sethook
local error, type, tostring, select, rawset = error, type, tostring, select, rawset
local setmetatable, format, concat = setmetatable, string.format, table.concat

-- "script:line" of the code `level` calls up from the function calling this
-- (the same counting as error()'s level).
local function where(level)
  local info = debug_getinfo(level + 1, "Sl")
  if info == nil then
    return "?"
  end
  return info.short_src .. ":" .. tostring(info.currentline)
end

-- Raises "bad argument" at the caller of the API function that called this
-- (`depth` more levels up when called through a helper).
local function bad_argument(fname, index, expected, value, depth)
  error(format("bad argument #%d to '%s' (%s expected, got %s)", index, fname, expected,
               type(value)), 3 + (depth or 0))
end

-- Raises at the caller of the API function unless a label is running on
-- this coroutine (so not from top-level code or a coroutine the script made).
local function require_label(fname, depth)
  if not engine.in_label() then
    error(fname .. "() can only be called from a running label", 3 + (depth or 0))
  end
end

local function require_known_label(fname, name)
  if type(name) ~= "string" then
    bad_argument(fname, 1, "string", name, 1)
  end
  require_label(fname, 1)
  if not engine.has_label(name) then
    error(format("unknown label '%s'", name), 3)
  end
end

-- The instruction limit: a count hook on every thread that runs script code
-- spends one tick per hook_period instructions.
local hook_period, ticks_left = 0, 0

-- "script:line" of the innermost game-script code `level` calls up from the
-- function calling this, skipping engine code; nil if there is none.
local function script_location(level)
  while true do
    local info = debug_getinfo(level + 1, "Sl")
    if info == nil then
      return nil
    end
    if info.what ~= "C" and info.source ~= "=vnmaki" then
      return info.short_src .. ":" .. tostring(info.currentline)
    end
    level = level + 1
  end
end

local function spend_tick()
  ticks_left = ticks_left - 1
  if ticks_left < 0 then
    local location = script_location(3)
    local prefix = location and (location .. ": ") or ""
    error(prefix .. "script ran too long without yielding (instruction limit reached)", 0)
  end
end

local function on_count()
  spend_tick()
end

-- A label runs on a fresh coroutine, whose hook count starts over, so moving
-- between labels costs a tick too: labels that jump to each other forever
-- without yielding still run out.
local function transfer(kind, name)
  if hook_period > 0 then
    spend_tick()
  end
  engine.transfer(kind, name)
end

function label(name, fn)
  if type(name) ~= "string" then
    bad_argument("label", 1, "string", name)
  end
  if type(fn) ~= "function" then
    bad_argument("label", 2, "function", fn)
  end
  local problem = engine.define_label(name, fn, where(2))
  if problem ~= nil then
    error(problem, 2)
  end
end

function jump(name)
  require_known_label("jump", name)
  transfer("jump", name)
end

function call(name)
  require_known_label("call", name)
  transfer("call", name)
end

function ret()
  require_label("ret")
  transfer("ret", "")
end

function say(who, text)
  local speaker
  if type(who) == "string" then
    speaker = who
  elseif type(who) == "table" and type(who.name) == "string" then
    speaker = who.name
  elseif who ~= nil then
    bad_argument("say", 1, "string, character or nil", who)
  end
  if type(text) ~= "string" then
    bad_argument("say", 2, "string", text)
  end
  require_label("say")
  if speaker == nil then
    engine.narrate(text)
  else
    engine.say(speaker, text)
  end
end

function narrate(text)
  if type(text) ~= "string" then
    bad_argument("narrate", 1, "string", text)
  end
  require_label("narrate")
  engine.narrate(text)
end

function print(...)
  local parts = {}
  for i = 1, select("#", ...) do
    parts[i] = tostring((select(i, ...)))
  end
  engine.print(concat(parts, "\t"))
end

-- Source text only: binary chunks can crash the VM.
local raw_load = load
function load(chunk, chunkname, _, ...)
  if select("#", ...) > 0 then
    return raw_load(chunk, chunkname, "t", (...))
  end
  return raw_load(chunk, chunkname, "t")
end

vfs = {}
function vfs.read(path)
  if type(path) ~= "string" then
    bad_argument("vfs.read", 1, "string", path)
  end
  return engine.vfs_read(path)
end
function vfs.exists(path)
  if type(path) ~= "string" then
    bad_argument("vfs.exists", 1, "string", path)
  end
  return engine.vfs_exists(path)
end
function vfs.list(directory)
  if type(directory) ~= "string" then
    bad_argument("vfs.list", 1, "string", directory)
  end
  return engine.vfs_list(directory)
end

-- A stub until the state ticket replaces it with the deterministic RNG kept
-- in the store. It follows Lua's math.random argument rules.
local tointeger, random_integer, random_float = math.tointeger, engine.random_integer,
    engine.random_float
local function integer_argument(value, index)
  local n = tointeger(value)
  if n == nil then
    if type(value) == "number" then
      error(format("bad argument #%d to 'random' (number has no integer representation)",
                   index), 3)
    end
    bad_argument("random", index, "number", value)
  end
  return n
end
function math.random(...)
  local count = select("#", ...)
  if count == 0 then
    return random_float()
  end
  local low, high
  if count == 1 then
    low, high = 1, integer_argument((...), 1)
    if high == 0 then
      return random_integer(math.mininteger, math.maxinteger)
    end
  elseif count == 2 then
    low, high = integer_argument((...), 1), integer_argument((select(2, ...)), 2)
  else
    error("wrong number of arguments to 'random'", 2)
  end
  if low > high then
    error(format("bad argument #%d to 'random' (interval is empty)", count), 2)
  end
  return random_integer(low, high)
end
math.randomseed = nil

-- The restricted standard library.
local os_clock, os_date, os_difftime, os_time = os.clock, os.date, os.difftime, os.time
os = { clock = os_clock, date = os_date, difftime = os_difftime, time = os_time }
debug = nil
dofile = nil
loadfile = nil
string.dump = nil

store = {}

if dev_mode then
  local warned = {}
  setmetatable(_G, {
    __newindex = function(globals, key, value)
      if not warned[key] then
        warned[key] = true
        engine.warn(format("%s: new global '%s' (keep game state in store, or use a local)",
                           where(2), tostring(key)))
      end
      rawset(globals, key, value)
    end,
  })
end

local function watch(thread)
  if hook_period > 0 then
    debug_sethook(thread, on_count, "", hook_period)
  end
end

-- Coroutines a script creates are watched too.
local co_create, co_resume, co_close = coroutine.create, coroutine.resume, coroutine.close
local unpack = table.unpack
function coroutine.create(fn)
  local thread = co_create(fn)
  watch(thread)
  return thread
end
local function wrap_results(thread, ok, ...)
  if ok then
    return ...
  end
  co_close(thread)
  error((...), 0)
end
function coroutine.wrap(fn)
  local thread = coroutine.create(fn)
  return function(...)
    return wrap_results(thread, co_resume(thread, ...))
  end
end

local limits = {}
function limits.configure(period)
  hook_period = period
  watch(coroutine.running())
end
function limits.watch(thread)
  watch(thread)
end
function limits.reset(ticks)
  ticks_left = ticks
end
return limits
)lua";

// How many labels may be called inside each other (frames on the call stack).
constexpr std::size_t kMaxCallDepth = 1000;

// The instruction-limit hook runs every kHookPeriod instructions.
constexpr std::uint32_t kHookPeriod = 1000;

// Seed of the math.random stub (ticket 11 replaces the stub).
constexpr std::uint64_t kRandomStubSeed = 0x5EED0F7E570B5EULL;

// splitmix64 (Steele, Lea, Flood 2014): a small generator with a fixed,
// platform-independent output sequence.
std::uint64_t splitmix64(std::uint64_t& state) {
  constexpr std::uint64_t kIncrement = 0x9E3779B97F4A7C15ULL;
  constexpr std::uint64_t kMul1 = 0xBF58476D1CE4E5B9ULL;
  constexpr std::uint64_t kMul2 = 0x94D049BB133111EBULL;
  constexpr int kShift1 = 30;
  constexpr int kShift2 = 27;
  constexpr int kShift3 = 31;
  state += kIncrement;
  std::uint64_t z = state;
  z = (z ^ (z >> kShift1)) * kMul1;
  z = (z ^ (z >> kShift2)) * kMul2;
  return z ^ (z >> kShift3);
}

ScriptObserver& null_observer() {
  static ScriptObserver observer;
  return observer;
}

std::string error_text(const sol::protected_function_result& result) {
  const sol::type type = result.get_type();
  if (type == sol::type::string) {
    return result.get<sol::error>().what();
  }
  return "(error object is a " + sol::type_name(result.lua_state(), type) + " value)";
}

std::string to_string(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte b : bytes) {
    text.push_back(static_cast<char>(b));
  }
  return text;
}

}  // namespace

class ScriptRuntime::Impl {
 public:
  Impl(const Vfs& vfs, const ScriptRuntimeOptions& options);
  Impl(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() = default;

  void load_script(std::string_view path);
  void load_scripts();
  [[nodiscard]] bool has_label(std::string_view name) const { return labels_.contains(name); }
  void start(std::string_view name);
  void advance();

  [[nodiscard]] RunState state() const { return state_; }
  [[nodiscard]] const BlockingRequest* request() const {
    return state_ == RunState::kWaiting ? &request_ : nullptr;
  }
  [[nodiscard]] ScriptPosition position() const {
    if (frames_.empty()) {
      return {};
    }
    return {.label = frames_.back().label, .step = frames_.back().step};
  }
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  // Tracks the VM's memory. The limit is enforced only while script code
  // runs (enforce): engine-side work, including the engine primitives that
  // scripts call, never fails for it, so a Lua memory error can only be raised
  // where Lua code (not C++ with live objects) is on top of the stack.
  struct Allocator {
    std::size_t used = 0;
    std::size_t limit = 0;
    bool enforce = false;
  };

  // Sets whether the memory limit applies, until the end of the scope.
  class MemoryLimitScope {
   public:
    MemoryLimitScope(Allocator& allocator, bool enforce)
        : allocator_(&allocator), previous_(allocator.enforce) {
      allocator.enforce = enforce;
    }
    MemoryLimitScope(const MemoryLimitScope&) = delete;
    MemoryLimitScope(MemoryLimitScope&&) = delete;
    MemoryLimitScope& operator=(const MemoryLimitScope&) = delete;
    MemoryLimitScope& operator=(MemoryLimitScope&&) = delete;
    ~MemoryLimitScope() { allocator_->enforce = previous_; }

   private:
    Allocator* allocator_;
    bool previous_;
  };

  struct Label {
    sol::main_function function;
    std::string location;  // "script:line" of its label() call
  };

  // One running label: its coroutine and how many yield points it reached.
  struct Frame {
    std::string label;
    sol::thread thread;
    sol::coroutine coroutine;
    std::uint32_t step = 0;
  };

  enum class TransferKind : std::uint8_t { kJump, kCall, kReturn };
  struct Transfer {
    TransferKind kind;
    std::string label;
  };

  static void* allocate(void* user_data, void* block, std::size_t old_size,
                        std::size_t new_size) noexcept;

  void bind_engine();
  [[nodiscard]] bool in_label(sol::this_state state) const;
  void yield_at(sol::this_state state, BlockingRequest request);
  void transfer(sol::this_state state, std::string_view kind, std::string label);
  [[nodiscard]] Frame make_frame(const std::string& name);
  void enter(const std::string& name);
  void run();
  void fail(std::string message);
  void reset_instruction_budget();
  [[nodiscard]] std::uint64_t random_u64();

  const Vfs* vfs_;
  ScriptRuntimeOptions options_;
  ScriptObserver* observer_;
  Allocator allocator_;
  // Declared after the allocator and before every Lua reference: the VM is
  // destroyed after the references and before the allocator state.
  sol::state lua_;
  sol::table engine_;
  sol::protected_function limits_configure_;
  sol::protected_function limits_watch_;
  sol::protected_function limits_reset_;

  std::map<std::string, Label, std::less<>> labels_;
  std::vector<Frame> frames_;  // back() is the running label; the rest called it
  std::optional<std::variant<BlockingRequest, Transfer>> pending_;
  bool started_ = false;
  RunState state_ = RunState::kIdle;
  std::uint32_t hook_period_ = 0;
  BlockingRequest request_;
  std::string error_;
  std::uint64_t random_state_ = kRandomStubSeed;
};

void* ScriptRuntime::Impl::allocate(void* user_data, void* block, std::size_t old_size,
                                    std::size_t new_size) noexcept {
  auto* allocator = static_cast<Allocator*>(user_data);
  // When block is null, old_size encodes the kind of object, not a size.
  const std::size_t freed = block == nullptr ? 0 : old_size;
  if (new_size == 0) {
    // The lua_Alloc contract is realloc/free semantics; Lua owns the block.
    std::free(block);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    allocator->used -= freed;
    return nullptr;
  }
  if (allocator->enforce && allocator->limit != 0 && new_size > freed &&
      allocator->used - freed + new_size > allocator->limit) {
    return nullptr;  // Lua raises "not enough memory"
  }
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
  void* grown = std::realloc(block, new_size);
  if (grown != nullptr) {
    allocator->used = allocator->used - freed + new_size;
  }
  return grown;
}

ScriptRuntime::Impl::Impl(const Vfs& vfs, const ScriptRuntimeOptions& options)
    : vfs_(&vfs),
      options_(options),
      observer_(options.observer != nullptr ? options.observer : &null_observer()),
      allocator_{.used = 0, .limit = options.memory_limit_bytes},
      lua_(sol::default_at_panic, &Impl::allocate, &allocator_) {
  lua_.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::string, sol::lib::table,
                      sol::lib::math, sol::lib::utf8, sol::lib::os, sol::lib::debug);
  // sol2 keeps a default message handler (it appends a traceback) in a
  // global that scripts could replace. Errors stay plain "script:line: text".
  sol::protected_function::set_default_handler(sol::object{lua_.lua_state(), sol::lua_nil});
  bind_engine();
  sol::load_result prelude = lua_.load(kPrelude, "=vnmaki", sol::load_mode::text);
  if (!prelude.valid()) {
    const sol::error error = prelude;
    throw std::logic_error(std::string{"vnmaki prelude: "} + error.what());
  }
  sol::protected_function prelude_function = prelude;
  sol::protected_function_result limits = prelude_function(engine_, options_.dev_mode);
  if (!limits.valid()) {
    throw std::logic_error("vnmaki prelude: " + error_text(limits));
  }
  const sol::table limits_table = limits;
  limits_configure_ = limits_table["configure"];
  limits_watch_ = limits_table["watch"];
  limits_reset_ = limits_table["reset"];
  if (options_.instruction_limit != 0) {
    hook_period_ = std::min(options_.instruction_limit, kHookPeriod);
    limits_configure_(hook_period_);
  }
}

void ScriptRuntime::Impl::bind_engine() {
  engine_ = lua_.create_table();
  engine_["in_label"] = [this](sol::this_state state) { return in_label(state); };
  engine_["has_label"] = [this](const std::string& name) { return has_label(name); };
  // Primitives that create Lua values do so inside an unlimited scope and
  // return them as references, so the push back to Lua cannot run out of
  // memory either.
  engine_["define_label"] = [this](sol::this_state state, const std::string& name,
                                   const sol::stack_object& function,
                                   const std::string& location) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    std::string problem;
    if (started_) {
      problem = "label() can only be called while scripts load";
    } else if (const auto existing = labels_.find(name); existing != labels_.end()) {
      problem = "label '" + name + "' is already defined at " + existing->second.location;
    } else {
      labels_.emplace(name,
                      Label{.function = function.as<sol::main_function>(), .location = location});
      return sol::lua_nil;
    }
    return sol::make_object(state, problem);
  };
  engine_["say"] = sol::yielding(
      [this](sol::this_state state, const std::string& speaker, const std::string& text) {
        yield_at(state, SayRequest{.speaker = speaker, .text = text});
      });
  engine_["narrate"] = sol::yielding([this](sol::this_state state, const std::string& text) {
    yield_at(state, NarrateRequest{.text = text});
  });
  engine_["transfer"] =
      sol::yielding([this](sol::this_state state, const std::string& kind,
                           const std::string& label) { transfer(state, kind, label); });
  engine_["print"] = [this](const std::string& text) { observer_->on_print(text); };
  engine_["warn"] = [this](const std::string& message) { observer_->on_warning(message); };
  engine_["vfs_read"] = [this](sol::this_state state, const std::string& path) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    const std::optional<std::vector<std::byte>> bytes = vfs_->read(path);
    if (!bytes) {
      return sol::lua_nil;
    }
    return sol::make_object(state, to_string(*bytes));
  };
  engine_["vfs_exists"] = [this](const std::string& path) { return vfs_->exists(path); };
  engine_["vfs_list"] = [this](sol::this_state state, const std::string& directory) {
    const MemoryLimitScope unlimited{allocator_, false};
    sol::state_view lua{state};
    sol::table entries = lua.create_table();
    int index = 1;
    for (const VfsEntry& entry : vfs_->list(directory)) {
      entries[index++] =
          lua.create_table_with("name", entry.name, "is_directory", entry.is_directory);
    }
    return entries;
  };
  engine_["random_integer"] = [this](std::int64_t low, std::int64_t high) {
    // Unbiased: reject the top partial interval of the 64-bit output.
    const std::uint64_t span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
    std::uint64_t value = random_u64();
    if (span != std::numeric_limits<std::uint64_t>::max()) {
      const std::uint64_t count = span + 1;
      const std::uint64_t threshold = (0 - count) % count;
      while (value < threshold) {
        value = random_u64();
      }
      value %= count;
    }
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + value);
  };
  engine_["random_float"] = [this] {
    constexpr int kMantissaBits = 53;
    constexpr int kDroppedBits = std::numeric_limits<std::uint64_t>::digits - kMantissaBits;
    constexpr double kScale = 1.0 / static_cast<double>(std::uint64_t{1} << kMantissaBits);
    return static_cast<double>(random_u64() >> kDroppedBits) * kScale;
  };
}

bool ScriptRuntime::Impl::in_label(sol::this_state state) const {
  return !frames_.empty() && state.lua_state() == frames_.back().thread.thread_state();
}

// The one engine-side path every blocking call (say, narrate; later menu and
// pause) goes through. run() sees the pending request when the coroutine
// yields, counts the step and hands the request to the engine. Step counting
// and replay (ticket 11) hook in here and in run().
void ScriptRuntime::Impl::yield_at(sol::this_state state, BlockingRequest request) {
  if (!in_label(state)) {
    throw std::runtime_error("blocking calls can only be made from a running label");
  }
  pending_ = std::move(request);
}

void ScriptRuntime::Impl::transfer(sol::this_state state, std::string_view kind,
                                   std::string label) {
  if (!in_label(state)) {
    throw std::runtime_error("jump, call and ret can only be used from a running label");
  }
  TransferKind transfer_kind = TransferKind::kReturn;
  if (kind == "jump") {
    transfer_kind = TransferKind::kJump;
  } else if (kind == "call") {
    transfer_kind = TransferKind::kCall;
  }
  pending_ = Transfer{.kind = transfer_kind, .label = std::move(label)};
}

void ScriptRuntime::Impl::load_script(std::string_view path) {
  if (started_) {
    throw ScriptError("cannot load '" + std::string{path} + "': the story already started");
  }
  const std::optional<std::vector<std::byte>> bytes = vfs_->read(path);
  if (!bytes) {
    throw ScriptError("cannot read script '" + std::string{path} + "'");
  }
  const std::string source = to_string(*bytes);
  reset_instruction_budget();
  const MemoryLimitScope limited{allocator_, true};
  sol::load_result chunk = lua_.load(source, "@" + std::string{path}, sol::load_mode::text);
  if (!chunk.valid()) {
    const sol::error error = chunk;
    throw ScriptError(error.what());
  }
  sol::protected_function chunk_function = chunk;
  const sol::protected_function_result result = chunk_function();
  if (!result.valid()) {
    throw ScriptError(error_text(result));
  }
}

void ScriptRuntime::Impl::load_scripts() {
  std::vector<std::string> paths;
  std::vector<std::string> directories{"scripts"};
  while (!directories.empty()) {
    const std::string directory = std::move(directories.back());
    directories.pop_back();
    for (const VfsEntry& entry : vfs_->list(directory)) {
      std::string path = directory + "/" + entry.name;
      if (entry.is_directory) {
        directories.push_back(std::move(path));
      } else if (path.ends_with(".lua")) {
        paths.push_back(std::move(path));
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const std::string& path : paths) {
    load_script(path);
  }
}

ScriptRuntime::Impl::Frame ScriptRuntime::Impl::make_frame(const std::string& name) {
  const Label& label = labels_.find(name)->second;
  sol::thread thread = sol::thread::create(lua_.lua_state());
  limits_watch_(thread);
  sol::coroutine coroutine{thread.thread_state(), label.function};
  return Frame{.label = name, .thread = std::move(thread), .coroutine = std::move(coroutine)};
}

void ScriptRuntime::Impl::start(std::string_view name) {
  if (started_) {
    throw std::logic_error("ScriptRuntime::start: the story already started");
  }
  started_ = true;
  const std::string label{name};
  if (!has_label(label)) {
    fail("unknown label '" + label + "'");
    return;
  }
  enter(label);
  run();
}

void ScriptRuntime::Impl::enter(const std::string& name) {
  frames_.push_back(make_frame(name));
  observer_->on_label_entered(name);
}

void ScriptRuntime::Impl::advance() {
  if (state_ != RunState::kWaiting) {
    return;
  }
  run();
}

// Resumes the running label until it reaches a yield point, the story ends
// or a script fails, following jumps, calls and returns on the way.
void ScriptRuntime::Impl::run() {
  // One budget for the whole run, so labels that jump or call each other
  // forever without yielding hit the limit too.
  reset_instruction_budget();
  while (true) {
    state_ = RunState::kIdle;
    pending_.reset();
    bool yielded = false;
    {
      const MemoryLimitScope limited{allocator_, true};
      const sol::protected_function_result result = frames_.back().coroutine();
      if (!result.valid()) {
        fail(error_text(result));
        return;
      }
      yielded = result.status() == sol::call_status::yielded;
    }
    if (yielded && !pending_) {
      fail("label '" + frames_.back().label +
           "' yielded outside an engine call (coroutine.yield in a label?)");
      return;
    }
    if (yielded && std::holds_alternative<BlockingRequest>(*pending_)) {
      Frame& frame = frames_.back();
      ++frame.step;
      request_ = std::get<BlockingRequest>(std::move(*pending_));
      state_ = RunState::kWaiting;
      observer_->on_blocked(request_, ScriptPosition{.label = frame.label, .step = frame.step});
      return;
    }
    TransferKind kind = TransferKind::kReturn;
    std::string target;
    if (yielded) {
      auto& transfer = std::get<Transfer>(*pending_);
      kind = transfer.kind;
      target = std::move(transfer.label);
    }
    if (kind == TransferKind::kJump) {
      frames_.pop_back();
      enter(target);
    } else if (kind == TransferKind::kCall) {
      if (frames_.size() >= kMaxCallDepth) {
        fail("label '" + frames_.back().label + "': more than " + std::to_string(kMaxCallDepth) +
             " nested calls (a call without a return?)");
        return;
      }
      enter(target);
    } else {
      frames_.pop_back();
      if (frames_.empty()) {
        state_ = RunState::kFinished;
        return;
      }
      observer_->on_label_resumed(frames_.back().label);
    }
  }
}

void ScriptRuntime::Impl::fail(std::string message) {
  state_ = RunState::kFailed;
  error_ = std::move(message);
}

void ScriptRuntime::Impl::reset_instruction_budget() {
  if (hook_period_ != 0) {
    limits_reset_(options_.instruction_limit / hook_period_);
  }
}

std::uint64_t ScriptRuntime::Impl::random_u64() { return splitmix64(random_state_); }

ScriptRuntime::ScriptRuntime(const Vfs& vfs, ScriptRuntimeOptions options)
    : impl_(std::make_unique<Impl>(vfs, options)) {}

ScriptRuntime::~ScriptRuntime() = default;

void ScriptRuntime::load_script(std::string_view path) { impl_->load_script(path); }
void ScriptRuntime::load_scripts() { impl_->load_scripts(); }
bool ScriptRuntime::has_label(std::string_view name) const { return impl_->has_label(name); }
void ScriptRuntime::start(std::string_view name) { impl_->start(name); }
void ScriptRuntime::advance() { impl_->advance(); }
RunState ScriptRuntime::state() const { return impl_->state(); }
const BlockingRequest* ScriptRuntime::request() const { return impl_->request(); }
ScriptPosition ScriptRuntime::position() const { return impl_->position(); }
const std::string& ScriptRuntime::error() const { return impl_->error(); }

}  // namespace vnmaki
