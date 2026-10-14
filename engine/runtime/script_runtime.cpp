#include "engine/runtime/script_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <sol/sol.hpp>

#include "engine/text/markup.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

// The script-facing API, in Lua so argument errors and "unknown label" errors
// point at the caller's script and line (error level 2). It runs once per VM
// with the engine primitives as arguments; they never become globals, so
// scripts reach the engine only through the functions defined here.
constexpr std::string_view kPrelude = R"lua(
local engine, dev_mode, random_seed = ...

local debug_getinfo, debug_sethook = debug.getinfo, debug.sethook
local error, type, tostring, select, rawset = error, type, tostring, select, rawset
local rawget = rawget

-- All game state. The engine keeps this table, snapshots it at label entry
-- and fills it again on replay; the RNG state is its __rng field.
local store = { __rng = random_seed }
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
  local info = debug_getinfo(fn, "S")
  local problem = engine.define_label(name, fn, where(2), info.source, info.linedefined,
                                      info.lastlinedefined)
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

-- Raises a bad text tag at the caller of the API function that called this.
local function check_text(text)
  local problem = engine.check_text(text)
  if problem then
    error(problem, 3)
  end
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
  check_text(text)
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
  check_text(text)
  engine.narrate(text)
end

-- The function the engine calls with a link's id when the player activates a
-- {link=id} part of the text on screen. It runs outside the labels: it cannot
-- block, and the replay does not run it again, so it must not change `store`.
local link_handler
function on_link(handler)
  if type(handler) ~= "function" then
    bad_argument("on_link", 1, "function", handler)
  end
  link_handler = handler
end

-- Visuals. The engine primitives return why a call cannot be done, or nil.
-- Option keys these functions do not know are left for later features.
local function option(fname, options, key, expected)
  local value = options[key]
  if value ~= nil and type(value) ~= expected then
    error(format("bad option '%s' to '%s' (%s expected, got %s)", key, fname, expected,
                 type(value)), 3)
  end
  return value
end

local function options_argument(fname, options)
  if options == nil then
    return {}
  end
  if type(options) ~= "table" then
    bad_argument(fname, 2, "table or nil", options, 1)
  end
  return options
end

function scene(image, options)
  if type(image) ~= "string" then
    bad_argument("scene", 1, "string", image)
  end
  options_argument("scene", options)
  require_label("scene")
  local problem = engine.scene(image)
  if problem then
    error(problem, 2)
  end
  engine.scene_command("scene " .. image)
end

function show(tag, options)
  if type(tag) ~= "string" then
    bad_argument("show", 1, "string", tag)
  end
  options = options_argument("show", options)
  local image = option("show", options, "image", "string")
  local at = option("show", options, "at", "string")
  local x = option("show", options, "x", "number")
  local y = option("show", options, "y", "number")
  local layer = option("show", options, "layer", "string")
  require_label("show")
  local problem = engine.show(tag, image or tag, at, x, y, layer)
  if problem then
    error(problem, 2)
  end
  local command = "show " .. tag
  if image then
    command = command .. " image " .. image
  end
  if at then
    command = command .. " at " .. at
  end
  if x then
    command = command .. format(" x %.14g", x)
  end
  if y then
    command = command .. format(" y %.14g", y)
  end
  if layer then
    command = command .. " layer " .. layer
  end
  engine.scene_command(command)
end

function hide(tag)
  if type(tag) ~= "string" then
    bad_argument("hide", 1, "string", tag)
  end
  require_label("hide")
  engine.hide(tag)
  engine.scene_command("hide " .. tag)
end

-- The minimal menu: a list of option texts. Returns the number of the
-- chosen option, from 1.
function menu(options)
  if type(options) ~= "table" then
    bad_argument("menu", 1, "table", options)
  end
  local texts = {}
  for i = 1, #options do
    local option = options[i]
    if type(option) ~= "string" then
      error(format("bad option #%d to 'menu' (string expected, got %s)", i, type(option)), 2)
    end
    texts[i] = option
  end
  if #texts == 0 then
    error("menu() needs at least one option", 2)
  end
  require_label("menu")
  return engine.menu(texts)
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

-- The deterministic engine RNG. Its state lives in store.__rng, so snapshots
-- and replay cover it. It follows Lua's math.random argument rules.
local tointeger, engine_random_integer, engine_random_float = math.tointeger,
    engine.random_integer, engine.random_float
local function random_integer(low, high)
  local value, state = engine_random_integer(rawget(store, "__rng"), low, high)
  rawset(store, "__rng", state)
  return value
end
local function random_float()
  local value, state = engine_random_float(rawget(store, "__rng"))
  rawset(store, "__rng", state)
  return value
end
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
rand = math.random

-- The restricted standard library.
local os_clock, os_date, os_difftime, os_time = os.clock, os.date, os.difftime, os.time
os = { clock = os_clock, date = os_date, difftime = os_difftime, time = os_time }
debug = nil
dofile = nil
loadfile = nil
string.dump = nil

-- Globals the engine owns: scripts read them but cannot replace them.
local reserved = { store = store }
do
  local warned = {}
  setmetatable(_G, {
    __index = reserved,
    __newindex = function(globals, key, value)
      if reserved[key] ~= nil then
        error(format("'%s' is owned by the engine and cannot be replaced (change its fields)",
                     tostring(key)), 2)
      end
      if dev_mode and not warned[key] then
        warned[key] = true
        engine.warn(format("%s: new global '%s' (keep game state in store, or use a local)",
                           where(2), tostring(key)))
      end
      rawset(globals, key, value)
    end,
    __metatable = false,
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

local limits = { store = store }
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
function limits.link(id)
  if link_handler == nil then
    return false
  end
  link_handler(id)
  return true
end
return limits
)lua";

// How many labels may be called inside each other (frames on the call stack).
constexpr std::size_t kMaxCallDepth = 1000;

// The instruction-limit hook runs every kHookPeriod instructions.
constexpr std::uint32_t kHookPeriod = 1000;

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

// FNV-1a over the lines first_line..last_line (from 1) of `text`, without
// carriage returns, so the hash does not depend on the platform's line ends
// or on where the lines are in the file.
// ponytail: covers only the label function's own text, not helpers it calls.
// Hash the whole script if edits to helpers must invalidate saves too.
std::uint64_t hash_lines(std::string_view text, std::int64_t first_line, std::int64_t last_line) {
  constexpr std::uint64_t kOffsetBasis = 0xCBF29CE484222325ULL;
  constexpr std::uint64_t kPrime = 0x100000001B3ULL;
  std::uint64_t hash = kOffsetBasis;
  std::int64_t line = 1;
  for (const char c : text) {
    if (line >= first_line && line <= last_line && c != '\r') {
      hash = (hash ^ static_cast<std::uint64_t>(static_cast<unsigned char>(c))) * kPrime;
    }
    if (c == '\n') {
      ++line;
    }
  }
  return hash;
}

// Reads the boolean, number or string at `index` into `value`; false for any
// other type.
bool read_scalar(lua_State* lua, int index, StoreValue& value) {
  switch (lua_type(lua, index)) {
    case LUA_TBOOLEAN:
      value = lua_toboolean(lua, index) != 0;
      return true;
    case LUA_TNUMBER:
      if (lua_isinteger(lua, index) != 0) {
        value = static_cast<std::int64_t>(lua_tointegerx(lua, index, nullptr));
      } else {
        value = static_cast<double>(lua_tonumberx(lua, index, nullptr));
      }
      return true;
    case LUA_TSTRING: {
      std::size_t size = 0;
      const char* text = lua_tolstring(lua, index, &size);
      value = std::string{text, size};
      return true;
    }
    default:
      return false;
  }
}

// Whether restore can build `snapshot` without a Lua error: it has the store
// table, every table reference exists and every key is a valid table key.
bool is_valid(const StoreSnapshot& snapshot) {
  if (snapshot.tables.empty()) {
    return false;
  }
  for (const StoreTable& table : snapshot.tables) {
    for (const auto& [key, value] : table.entries) {
      if (std::holds_alternative<StoreTableRef>(key)) {
        return false;
      }
      if (const auto* number = std::get_if<double>(&key);
          number != nullptr && std::isnan(*number)) {
        return false;
      }
      if (const auto* ref = std::get_if<StoreTableRef>(&value);
          ref != nullptr && ref->index >= snapshot.tables.size()) {
        return false;
      }
    }
  }
  return true;
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
  void choose(std::size_t index);
  bool activate_link(std::string_view id);
  void replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
              std::uint32_t step);

  [[nodiscard]] bool replaying() const { return replaying_; }
  [[nodiscard]] const LabelSnapshot* label_snapshot() const {
    return frames_.empty() ? nullptr : &frames_.back().snapshot;
  }
  [[nodiscard]] std::span<const InputResult> input_log() const {
    if (frames_.empty()) {
      return {};
    }
    return frames_.back().inputs;
  }
  [[nodiscard]] StoreSnapshot capture_store() const;
  [[nodiscard]] std::optional<std::uint64_t> label_source_hash(std::string_view name) const {
    const auto found = labels_.find(name);
    return found == labels_.end() ? std::nullopt : std::optional{found->second.source_hash};
  }

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
    std::uint64_t source_hash = 0;
  };

  // One running label: its coroutine, how many yield points it reached, the
  // snapshot taken when it was entered and the answers given since.
  struct Frame {
    std::string label;
    sol::thread thread;
    sol::coroutine coroutine;
    std::uint32_t step = 0;
    LabelSnapshot snapshot{};
    std::vector<InputResult> inputs{};
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
  [[nodiscard]] bool enter(const std::string& name);
  void run(std::optional<std::int64_t> resume_value = std::nullopt);
  [[nodiscard]] bool replay_answer(std::optional<std::int64_t>& resume_value);
  void record(const InputResult& input);
  void restore_store(const StoreSnapshot& snapshot);
  void fail(std::string message);
  void reset_instruction_budget();
  [[nodiscard]] std::uint64_t random_state(const sol::stack_object& state) const;

  // What replay() still has to do while replaying_ is set.
  struct Replay {
    std::vector<InputResult> inputs;
    std::size_t next_input = 0;
    std::size_t depth = 0;  // frames_.size() with the replayed label on top
    std::uint32_t step = 0;
  };

  const Vfs* vfs_;
  ScriptRuntimeOptions options_;
  ScriptObserver* observer_;
  Allocator allocator_;
  // Declared after the allocator and before every Lua reference: the VM is
  // destroyed after the references and before the allocator state.
  sol::state lua_;
  sol::table engine_;
  sol::table store_;
  sol::protected_function limits_configure_;
  sol::protected_function limits_watch_;
  sol::protected_function limits_reset_;
  sol::protected_function link_;

  std::map<std::string, Label, std::less<>> labels_;
  // Script path -> source text, for the label source hashes; kept only while
  // scripts load.
  std::map<std::string, std::string, std::less<>> sources_;
  std::vector<Frame> frames_;  // back() is the running label; the rest called it
  std::optional<std::variant<BlockingRequest, Transfer>> pending_;
  bool started_ = false;
  RunState state_ = RunState::kIdle;
  std::uint32_t hook_period_ = 0;
  BlockingRequest request_;
  std::string error_;
  bool replaying_ = false;
  Replay replay_;
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
  sol::protected_function_result limits =
      prelude_function(engine_, options_.dev_mode, static_cast<std::int64_t>(options_.random_seed));
  if (!limits.valid()) {
    throw std::logic_error("vnmaki prelude: " + error_text(limits));
  }
  const sol::table limits_table = limits;
  store_ = limits_table["store"];
  limits_configure_ = limits_table["configure"];
  limits_watch_ = limits_table["watch"];
  limits_reset_ = limits_table["reset"];
  link_ = limits_table["link"];
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
                                   const sol::stack_object& function, const std::string& location,
                                   const std::string& source, std::int64_t first_line,
                                   std::int64_t last_line) -> sol::object {
    const MemoryLimitScope unlimited{allocator_, false};
    std::string problem;
    if (started_) {
      problem = "label() can only be called while scripts load";
    } else if (const auto existing = labels_.find(name); existing != labels_.end()) {
      problem = "label '" + name + "' is already defined at " + existing->second.location;
    } else {
      // A function from a loaded script hashes its own lines; any other
      // (from load(), or a C function) hashes the whole chunk it came from.
      const auto script = source.starts_with('@')
                              ? sources_.find(std::string_view{source}.substr(1))
                              : sources_.end();
      const std::uint64_t source_hash =
          script != sources_.end()
              ? hash_lines(script->second, first_line, last_line)
              : hash_lines(source, 1, std::numeric_limits<std::int64_t>::max());
      labels_.emplace(name, Label{.function = function.as<sol::main_function>(),
                                  .location = location,
                                  .source_hash = source_hash});
      return sol::lua_nil;
    }
    return sol::make_object(state, problem);
  };
  // The prelude raises the problem at the script line of the say or narrate call.
  engine_["check_text"] = [this](sol::this_state state, const std::string& text) -> sol::object {
    std::string problem;
    try {
      (void)parse_markup(text);
    } catch (const MarkupError& error) {
      problem = "bad text tag at byte " + std::to_string(error.offset()) + ": " + error.what();
    }
    if (problem.empty()) {
      return sol::lua_nil;
    }
    const MemoryLimitScope unlimited{allocator_, false};
    return sol::make_object(state, problem);
  };
  engine_["say"] = sol::yielding(
      [this](sol::this_state state, const std::string& speaker, const std::string& text) {
        yield_at(state, SayRequest{.speaker = speaker, .text = text});
      });
  engine_["narrate"] = sol::yielding([this](sol::this_state state, const std::string& text) {
    yield_at(state, NarrateRequest{.text = text});
  });
  // The prelude checked that `options` is a list of strings. A stack_table
  // makes no registry reference, so nothing here can raise a Lua error.
  engine_["menu"] = sol::yielding([this](sol::this_state state, const sol::stack_table& options) {
    MenuRequest request;
    const std::size_t count = options.size();
    request.options.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
      request.options.push_back(options.get<std::string>(i));
    }
    yield_at(state, std::move(request));
  });
  // The scene changes during a replay too; only the report is skipped.
  const auto problem_or_nil = [this](sol::this_state state,
                                     const std::string& problem) -> sol::object {
    if (problem.empty()) {
      return sol::lua_nil;
    }
    const MemoryLimitScope unlimited{allocator_, false};
    return sol::make_object(state, problem);
  };
  engine_["scene"] = [this, problem_or_nil](sol::this_state state, const std::string& image) {
    return problem_or_nil(state,
                          options_.scene != nullptr ? options_.scene->scene(image) : std::string{});
  };
  engine_["show"] = [this, problem_or_nil](
                        sol::this_state state, const std::string& tag, const std::string& image,
                        const sol::optional<std::string>& at, sol::optional<double> x,
                        sol::optional<double> y, const sol::optional<std::string>& layer) {
    if (options_.scene == nullptr) {
      return problem_or_nil(state, {});
    }
    ShowRequest request{.tag = tag, .image = image};
    if (at) {
      request.at = *at;
    }
    if (x) {
      request.x = *x;
    }
    if (y) {
      request.y = *y;
    }
    if (layer) {
      request.layer = *layer;
    }
    return problem_or_nil(state, options_.scene->show(request));
  };
  engine_["hide"] = [this](const std::string& tag) {
    if (options_.scene != nullptr) {
      options_.scene->hide(tag);
    }
  };
  engine_["scene_command"] = [this](const std::string& command) {
    if (!replaying_) {
      observer_->on_scene_command(command);
    }
  };
  engine_["transfer"] =
      sol::yielding([this](sol::this_state state, const std::string& kind,
                           const std::string& label) { transfer(state, kind, label); });
  engine_["print"] = [this](const std::string& text) {
    if (!replaying_) {
      observer_->on_print(text);
    }
  };
  engine_["warn"] = [this](const std::string& message) {
    if (!replaying_) {
      observer_->on_warning(message);
    }
  };
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
  // The RNG primitives take the state (store.__rng) and return the value and
  // the next state, which the prelude writes back to the store.
  engine_["random_integer"] = [this](const sol::stack_object& state, std::int64_t low,
                                     std::int64_t high) {
    std::uint64_t rng = random_state(state);
    // Unbiased: reject the top partial interval of the 64-bit output.
    const auto span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
    std::uint64_t value = splitmix64(rng);
    if (span != std::numeric_limits<std::uint64_t>::max()) {
      const std::uint64_t count = span + 1;
      const std::uint64_t threshold = (0 - count) % count;
      while (value < threshold) {
        value = splitmix64(rng);
      }
      value %= count;
    }
    return std::tuple{static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + value),
                      static_cast<std::int64_t>(rng)};
  };
  engine_["random_float"] = [this](const sol::stack_object& state) {
    constexpr int kMantissaBits = 53;
    constexpr int kDroppedBits = std::numeric_limits<std::uint64_t>::digits - kMantissaBits;
    constexpr double kScale = 1.0 / static_cast<double>(std::uint64_t{1} << kMantissaBits);
    std::uint64_t rng = random_state(state);
    const auto value = static_cast<double>(splitmix64(rng) >> kDroppedBits) * kScale;
    return std::tuple{value, static_cast<std::int64_t>(rng)};
  };
}

// The RNG state a script holds in store.__rng. A script that removed or
// broke the field starts again from the seed.
std::uint64_t ScriptRuntime::Impl::random_state(const sol::stack_object& state) const {
  if (lua_isinteger(state.lua_state(), state.stack_index()) != 0) {
    return static_cast<std::uint64_t>(
        lua_tointegerx(state.lua_state(), state.stack_index(), nullptr));
  }
  return options_.random_seed;
}

// Copies the store breadth-first. Sorting each table's fields by key before
// numbering the tables they hold makes the result independent of Lua's
// iteration order.
StoreSnapshot ScriptRuntime::Impl::capture_store() const {
  struct Field {
    StoreValue key;
    StoreValue value;
    std::optional<sol::table> table;  // set when the value is a table
  };
  lua_State* lua = lua_.lua_state();
  const int top = lua_gettop(lua);
  StoreSnapshot snapshot;
  std::vector<sol::table> tables{store_};
  std::map<const void*, std::uint32_t> indices{{store_.pointer(), 0}};
  for (std::size_t current = 0; current < tables.size(); ++current) {
    std::vector<Field> fields;
    tables[current].push(lua);
    lua_pushnil(lua);
    while (lua_next(lua, -2) != 0) {
      Field field;
      const bool key_ok = read_scalar(lua, -2, field.key);
      const int value_type = lua_type(lua, -1);
      if (key_ok && value_type == LUA_TTABLE) {
        field.table = sol::table{lua, -1};
      } else if (!key_ok || !read_scalar(lua, -1, field.value)) {
        std::string problem = "store holds a ";
        problem += lua_typename(lua, key_ok ? value_type : lua_type(lua, -2));
        problem += key_ok ? " value" : " key";
        if (const auto* name = std::get_if<std::string>(&field.key); key_ok && name != nullptr) {
          problem += " (field '" + *name + "')";
        }
        lua_settop(lua, top);
        throw ScriptError(problem + "; it keeps only booleans, numbers, strings and tables");
      }
      fields.push_back(std::move(field));
      lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
    std::sort(fields.begin(), fields.end(),
              [](const Field& a, const Field& b) { return a.key < b.key; });
    StoreTable table;
    table.entries.reserve(fields.size());
    for (Field& field : fields) {
      if (field.table) {
        const auto [found, is_new] =
            indices.try_emplace(field.table->pointer(), static_cast<std::uint32_t>(tables.size()));
        if (is_new) {
          tables.push_back(std::move(*field.table));
        }
        field.value = StoreTableRef{.index = found->second};
      }
      table.entries.emplace_back(std::move(field.key), std::move(field.value));
    }
    snapshot.tables.push_back(std::move(table));
  }
  return snapshot;
}

// Makes the store equal to `snapshot` (which is_valid). The store table
// itself is kept, because scripts hold references to it; the tables inside
// it are new.
void ScriptRuntime::Impl::restore_store(const StoreSnapshot& snapshot) {
  lua_State* lua = lua_.lua_state();
  std::vector<sol::table> tables;
  tables.reserve(snapshot.tables.size());
  tables.push_back(store_);
  for (std::size_t i = 1; i < snapshot.tables.size(); ++i) {
    tables.push_back(lua_.create_table());
  }

  store_.push(lua);
  lua_pushnil(lua);
  while (lua_next(lua, -2) != 0) {
    // Setting a field to nil during a traversal is allowed.
    lua_pop(lua, 1);
    lua_pushvalue(lua, -1);
    lua_pushnil(lua);
    lua_rawset(lua, -4);
  }
  lua_pop(lua, 1);

  const auto push = [lua, &tables](const StoreValue& value) {
    if (const auto* boolean = std::get_if<bool>(&value)) {
      lua_pushboolean(lua, *boolean ? 1 : 0);
    } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      lua_pushinteger(lua, *integer);
    } else if (const auto* number = std::get_if<double>(&value)) {
      lua_pushnumber(lua, *number);
    } else if (const auto* text = std::get_if<std::string>(&value)) {
      lua_pushlstring(lua, text->data(), text->size());
    } else {
      tables[std::get<StoreTableRef>(value).index].push(lua);
    }
  };
  for (std::size_t i = 0; i < snapshot.tables.size(); ++i) {
    tables[i].push(lua);
    for (const auto& [key, value] : snapshot.tables[i].entries) {
      push(key);
      push(value);
      lua_rawset(lua, -3);
    }
    lua_pop(lua, 1);
  }
}

bool ScriptRuntime::Impl::in_label(sol::this_state state) const {
  return !frames_.empty() && state.lua_state() == frames_.back().thread.thread_state();
}

// The one engine-side path every blocking call (say, narrate, menu; later
// pause) goes through. run() sees the pending request when the coroutine
// yields, counts the step and hands the request to the engine, or answers it
// from the input log during a replay.
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
  const std::string& source =
      sources_.insert_or_assign(std::string{path}, to_string(*bytes)).first->second;
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
  sources_.clear();
  const std::string label{name};
  if (!has_label(label)) {
    fail("unknown label '" + label + "'");
    return;
  }
  if (enter(label)) {
    run();
  }
}

// Snapshots the state and puts label `name` on top of the stack. Returns
// false, with the runtime failed, if the store cannot be snapshotted.
bool ScriptRuntime::Impl::enter(const std::string& name) {
  LabelSnapshot snapshot;
  snapshot.label = name;
  try {
    snapshot.store = capture_store();
  } catch (const ScriptError& error) {
    fail("entering label '" + name + "': " + error.what());
    return false;
  }
  if (options_.scene_state != nullptr) {
    snapshot.scene = options_.scene_state->capture();
  }
  frames_.push_back(make_frame(name));
  frames_.back().snapshot = std::move(snapshot);
  if (!replaying_) {
    observer_->on_label_entered(name);
  }
  return true;
}

void ScriptRuntime::Impl::advance() {
  if (state_ != RunState::kWaiting || std::holds_alternative<MenuRequest>(request_)) {
    return;
  }
  run();
}

void ScriptRuntime::Impl::choose(std::size_t index) {
  const auto* menu = std::get_if<MenuRequest>(&request_);
  if (state_ != RunState::kWaiting || menu == nullptr) {
    return;
  }
  if (index >= menu->options.size()) {
    throw std::out_of_range("ScriptRuntime::choose: the menu has no option " +
                            std::to_string(index));
  }
  record(MenuChoice{.index = index});
  run(static_cast<std::int64_t>(index) + 1);
}

// Every label on the stack logs the answer: a caller's log covers the labels
// it called, so the caller can be replayed past the call.
void ScriptRuntime::Impl::record(const InputResult& input) {
  for (Frame& frame : frames_) {
    frame.inputs.push_back(input);
  }
}

void ScriptRuntime::Impl::replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
                                 std::uint32_t step) {
  // Copies first: the arguments may belong to the frame replaced below.
  LabelSnapshot entry = snapshot;
  Replay replay{.inputs = {inputs.begin(), inputs.end()}, .step = step};
  started_ = true;
  sources_.clear();
  error_.clear();
  if (!has_label(entry.label)) {
    fail("replay: unknown label '" + entry.label + "'");
    return;
  }
  if (!is_valid(entry.store)) {
    fail("replay: the store snapshot of label '" + entry.label + "' is malformed");
    return;
  }
  if (!frames_.empty()) {
    frames_.pop_back();
  }
  if (options_.scene_state != nullptr) {
    try {
      options_.scene_state->restore(entry.scene);
    } catch (const std::invalid_argument& error) {
      fail("replay: the scene snapshot of label '" + entry.label + "' is malformed (" +
           error.what() + ")");
      return;
    }
  }
  replaying_ = step > 0;
  restore_store(entry.store);
  frames_.push_back(make_frame(entry.label));
  frames_.back().snapshot = std::move(entry);
  replay.depth = frames_.size();
  replay_ = std::move(replay);
  if (!replaying_) {
    observer_->on_label_entered(frames_.back().label);
  }
  run();
}

// Answers the request a replay just reached from the input log:
// `resume_value` becomes what the blocking call returns. Returns false, with
// the runtime failed, if the log has no answer for it.
bool ScriptRuntime::Impl::replay_answer(std::optional<std::int64_t>& resume_value) {
  const auto* menu = std::get_if<MenuRequest>(&request_);
  if (menu == nullptr) {
    return true;  // say and narrate take no answer
  }
  const std::string where =
      "replay: label '" + frames_.back().label + "', step " + std::to_string(frames_.back().step);
  if (replay_.next_input >= replay_.inputs.size()) {
    fail(where + ": the input log has no choice for this menu");
    return false;
  }
  const InputResult input = replay_.inputs[replay_.next_input++];
  const std::size_t index = std::get<MenuChoice>(input).index;
  if (index >= menu->options.size()) {
    fail(where + ": the logged choice is not an option of this menu");
    return false;
  }
  record(input);
  resume_value = static_cast<std::int64_t>(index) + 1;
  return true;
}

// Resumes the running label until it reaches a yield point, the story ends
// or a script fails, following jumps, calls and returns on the way.
//
// `resume_value` is what the blocking call the label waits in returns (the
// chosen option of a menu).
void ScriptRuntime::Impl::run(std::optional<std::int64_t> resume_value) {
  // One budget for the whole run, so labels that jump or call each other
  // forever without yielding hit the limit too.
  reset_instruction_budget();
  while (true) {
    state_ = RunState::kIdle;
    pending_.reset();
    bool yielded = false;
    {
      const MemoryLimitScope limited{allocator_, true};
      sol::coroutine& coroutine = frames_.back().coroutine;
      const sol::protected_function_result result =
          resume_value ? coroutine(*resume_value) : coroutine();
      resume_value.reset();
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
      if (replaying_ && (frames_.size() != replay_.depth || frame.step != replay_.step)) {
        // Not the target yet: answer from the log and go on, with a fresh
        // budget as in live play.
        if (!replay_answer(resume_value)) {
          return;
        }
        reset_instruction_budget();
        continue;
      }
      replaying_ = false;
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
    if (replaying_ && kind != TransferKind::kCall && frames_.size() == replay_.depth) {
      fail("replay: label '" + frames_.back().label + "' ended before step " +
           std::to_string(replay_.step));
      return;
    }
    if (kind == TransferKind::kJump) {
      frames_.pop_back();
      if (!enter(target)) {
        return;
      }
    } else if (kind == TransferKind::kCall) {
      if (frames_.size() >= kMaxCallDepth) {
        fail("label '" + frames_.back().label + "': more than " + std::to_string(kMaxCallDepth) +
             " nested calls (a call without a return?)");
        return;
      }
      if (!enter(target)) {
        return;
      }
    } else {
      frames_.pop_back();
      if (frames_.empty()) {
        state_ = RunState::kFinished;
        return;
      }
      if (!replaying_) {
        observer_->on_label_resumed(frames_.back().label);
      }
    }
  }
}

bool ScriptRuntime::Impl::activate_link(std::string_view id) {
  if (state_ != RunState::kWaiting) {
    return false;
  }
  reset_instruction_budget();
  const MemoryLimitScope limited{allocator_, true};
  const sol::protected_function_result result = link_(std::string{id});
  if (!result.valid()) {
    fail(error_text(result));
    return false;
  }
  return result.get<bool>();
}

void ScriptRuntime::Impl::fail(std::string message) {
  state_ = RunState::kFailed;
  replaying_ = false;
  error_ = std::move(message);
}

void ScriptRuntime::Impl::reset_instruction_budget() {
  if (hook_period_ != 0) {
    limits_reset_(options_.instruction_limit / hook_period_);
  }
}

ScriptRuntime::ScriptRuntime(const Vfs& vfs, ScriptRuntimeOptions options)
    : impl_(std::make_unique<Impl>(vfs, options)) {}

ScriptRuntime::~ScriptRuntime() = default;

void ScriptRuntime::load_script(std::string_view path) { impl_->load_script(path); }
void ScriptRuntime::load_scripts() { impl_->load_scripts(); }
bool ScriptRuntime::has_label(std::string_view name) const { return impl_->has_label(name); }
void ScriptRuntime::start(std::string_view name) { impl_->start(name); }
void ScriptRuntime::advance() { impl_->advance(); }
void ScriptRuntime::choose(std::size_t index) { impl_->choose(index); }
bool ScriptRuntime::activate_link(std::string_view id) { return impl_->activate_link(id); }
void ScriptRuntime::replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
                           std::uint32_t step) {
  impl_->replay(snapshot, inputs, step);
}
bool ScriptRuntime::replaying() const { return impl_->replaying(); }
const LabelSnapshot* ScriptRuntime::label_snapshot() const { return impl_->label_snapshot(); }
std::span<const InputResult> ScriptRuntime::input_log() const { return impl_->input_log(); }
StoreSnapshot ScriptRuntime::store_snapshot() const { return impl_->capture_store(); }
std::optional<std::uint64_t> ScriptRuntime::label_source_hash(std::string_view name) const {
  return impl_->label_source_hash(name);
}
RunState ScriptRuntime::state() const { return impl_->state(); }
const BlockingRequest* ScriptRuntime::request() const { return impl_->request(); }
ScriptPosition ScriptRuntime::position() const { return impl_->position(); }
const std::string& ScriptRuntime::error() const { return impl_->error(); }

}  // namespace vnmaki
