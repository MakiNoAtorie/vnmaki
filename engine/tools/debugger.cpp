#include "engine/tools/debugger.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <lua.hpp>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "engine/runtime/script_runtime.hpp"
#include "engine/tools/inspect.hpp"

namespace vnmaki {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

// The one thread of the story; DAP needs an id for it.
constexpr int kThreadId = 1;
// The most fields of a table that `variables` lists.
constexpr std::size_t kMaxTableFields = 200;
// The longest string value shown in full.
constexpr std::size_t kMaxStringValue = 200;
// A double with an integral value up to this is shown as an integer.
constexpr double kIntegerLimit = 1e15;

std::string lower(std::string_view text) {
  std::string out{text};
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string quote_text(std::string_view text) {
  std::string out = "\"";
  out += text.substr(0, kMaxStringValue);
  if (text.size() > kMaxStringValue) {
    out += "...";
  }
  return out + "\"";
}

std::string number_text(double value) {
  if (std::isfinite(value) && value == std::trunc(value) && std::fabs(value) < kIntegerLimit) {
    return std::format("{}", static_cast<std::int64_t>(value));
  }
  return std::format("{}", value);
}

std::string type_name(sol::type type) {
  switch (type) {
    case sol::type::lua_nil:
    case sol::type::none:
      return "nil";
    case sol::type::boolean:
      return "boolean";
    case sol::type::number:
      return "number";
    case sol::type::string:
      return "string";
    case sol::type::table:
      return "table";
    case sol::type::function:
      return "function";
    case sol::type::thread:
      return "thread";
    default:
      return "userdata";
  }
}

template <typename Object>
std::string describe(const Object& value) {
  switch (value.get_type()) {
    case sol::type::lua_nil:
    case sol::type::none:
      return "nil";
    case sol::type::boolean:
      return value.template as<bool>() ? "true" : "false";
    case sol::type::number:
      return number_text(value.template as<double>());
    case sol::type::string:
      return quote_text(value.template as<std::string>());
    default:
      return type_name(value.get_type());
  }
}

Json variable(std::string name, std::string value, int reference = 0, std::string type = {}) {
  Json out = {
      {"name", std::move(name)}, {"value", std::move(value)}, {"variablesReference", reference}};
  if (!type.empty()) {
    out["type"] = std::move(type);
  }
  return out;
}

bool is_identifier(std::string_view text) {
  if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front())) != 0) {
    return false;
  }
  return std::ranges::all_of(text,
                             [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
}

int stack_depth(lua_State* thread) {
  lua_Debug ar;
  int depth = 0;
  while (lua_getstack(thread, depth, &ar) != 0) {
    ++depth;
  }
  return depth;
}

// One Lua frame of a script file (the engine's own Lua and C frames are left out).
struct ScriptFrame {
  int level = 0;
  std::string name{};
  std::string path{};  // the VFS path of the script
  int line = 0;
};

std::vector<ScriptFrame> script_frames(lua_State* thread, std::string_view label) {
  std::vector<ScriptFrame> frames;
  lua_Debug ar;
  for (int level = 0; lua_getstack(thread, level, &ar) != 0; ++level) {
    lua_getinfo(thread, "nSl", &ar);
    if (ar.source == nullptr || ar.source[0] != '@') {
      continue;
    }
    frames.push_back({.level = level,
                      .name = ar.name != nullptr ? ar.name : "?",
                      .path = ar.source + 1,
                      .line = ar.currentline});
  }
  if (!frames.empty()) {
    frames.back().name = std::string{label};  // the label function, called by the engine
  }
  return frames;
}

// The locals of the frame at `level`, in order, without Lua's temporaries.
std::vector<std::pair<std::string, sol::main_object>> frame_locals(lua_State* thread, int level) {
  std::vector<std::pair<std::string, sol::main_object>> locals;
  lua_Debug ar;
  if (lua_getstack(thread, level, &ar) == 0 || lua_checkstack(thread, 4) == 0) {
    return locals;
  }
  for (int index = 1;; ++index) {
    const char* name = lua_getlocal(thread, &ar, index);
    if (name == nullptr) {
      break;
    }
    sol::main_object value = sol::stack::pop<sol::main_object>(thread);
    if (name[0] != '(') {
      locals.emplace_back(name, std::move(value));
    }
  }
  return locals;
}

}  // namespace

class Debugger::Impl {
 public:
  Impl(ScriptRuntime& runtime, DapChannel& channel, fs::path project_dir,
       std::function<bool()> pump)
      : runtime_(&runtime),
        channel_(&channel),
        project_dir_(fs::absolute(std::move(project_dir))),
        pump_(std::move(pump)) {
    if (active_ != nullptr) {
      throw std::logic_error("Debugger: there is one already");
    }
    active_ = this;
    runtime_->set_thread_hook(&Impl::install_hook);
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    runtime_->set_thread_hook({});
    active_ = nullptr;  // hooks that are left in old threads do nothing now
  }

  bool poll() {
    if (mode_ == Mode::kStopped) {
      return !session_over_;  // the pump of a stop draws a frame, which calls this: stop() reads
    }
    if (session_over_) {
      return false;
    }
    if (pending_goto_) {
      const PendingGoto go = std::move(*pending_goto_);
      pending_goto_.reset();
      perform_goto(go);
    }
    report_end();  // before the next request, so the client sees the end first
    handle_messages();
    return !session_over_;
  }

  [[nodiscard]] bool configured() const { return configured_; }
  [[nodiscard]] bool holding() const { return mode_ == Mode::kHolding; }

 private:
  enum class Mode : std::uint8_t {
    kRunning,
    kStopped,  // waiting inside the line hook
    kHolding,  // the story waits at a yield point after a goto
  };
  enum class Step : std::uint8_t { kNone, kIn, kOver, kOut, kPause };
  struct PendingGoto {
    Json request{};
    std::string label{};
    std::uint32_t step = 1;
  };
  // What a `variablesReference` stands for.
  struct Reference {
    enum class Kind : std::uint8_t { kLocals, kStore, kTable };
    Kind kind = Kind::kLocals;
    int level = 0;            // kLocals: the Lua stack level
    std::size_t row = 0;      // kStore: index into store_rows_, or kRoot
    sol::main_table table{};  // kTable
  };
  static constexpr std::size_t kRoot = static_cast<std::size_t>(-1);

  // ---- the Lua hook ----

  // Called for every thread the runtime makes. The line hook goes next to the hook that is there
  // (the instruction limit's count hook), and passes count events on to it.
  static void install_hook(lua_State* thread) {
    const lua_Hook current = lua_gethook(thread);
    if (current == &Impl::line_hook) {
      return;
    }
    if (current != nullptr) {
      previous_hook_ = current;
    }
    lua_sethook(thread, &Impl::line_hook, lua_gethookmask(thread) | LUA_MASKLINE,
                lua_gethookcount(thread));
  }

  static void line_hook(lua_State* thread, lua_Debug* ar) {
    if (ar->event != LUA_HOOKLINE) {
      if (previous_hook_ != nullptr) {
        previous_hook_(thread, ar);
      }
      return;
    }
    if (active_ == nullptr) {
      return;
    }
    active_->on_line(thread, ar);
    if (active_ != nullptr && active_->abort_) {
      active_->abort_ = false;
      luaL_error(thread, "stopped by the debugger to go to a label");  // no C++ objects live here
    }
  }

  void on_line(lua_State* thread, lua_Debug* ar) {
    if (session_over_ || mode_ == Mode::kStopped || runtime_->replaying() ||
        thread != runtime_->label_thread()) {
      return;
    }
    lua_getinfo(thread, "Sl", ar);
    if (ar->source == nullptr || ar->source[0] != '@') {
      return;  // the engine's own Lua is "=vnmaki"
    }
    std::string_view reason;
    if (has_breakpoint(ar->source + 1, ar->currentline)) {
      reason = "breakpoint";
    } else {
      reason = step_reason(thread);
    }
    if (!reason.empty()) {
      stop(reason);
    }
  }

  [[nodiscard]] std::string_view step_reason(lua_State* thread) const {
    switch (step_) {
      case Step::kNone:
        return {};
      case Step::kIn:
        return "step";
      case Step::kPause:
        return "pause";
      case Step::kOver:
        return thread != step_thread_ || stack_depth(thread) <= step_depth_ ? "step"
                                                                            : std::string_view{};
      case Step::kOut:
        return thread != step_thread_ || stack_depth(thread) < step_depth_ ? "step"
                                                                           : std::string_view{};
    }
    return {};
  }

  [[nodiscard]] bool has_breakpoint(std::string_view path, int line) const {
    const auto found = breakpoints_.find(lower(path));
    return found != breakpoints_.end() && found->second.contains(line);
  }

  // Waits here until the client resumes the story (or the session ends).
  void stop(std::string_view reason) {
    mode_ = Mode::kStopped;
    step_ = Step::kNone;
    clear_references();
    event("stopped", {{"reason", reason}, {"threadId", kThreadId}, {"allThreadsStopped", true}});
    while (mode_ == Mode::kStopped) {
      handle_messages();
      if (mode_ == Mode::kStopped && pump_ && !pump_()) {
        end_session();  // the window was closed
      }
    }
    clear_references();
  }

  // ---- messages ----

  // Handles requests until none is left or one resumes the story: whatever follows it is for
  // the next stop, so the answer to it does not depend on timing.
  void handle_messages() {
    resumed_ = false;
    std::string body;
    while (!session_over_ && !resumed_) {
      const DapChannel::Status status = channel_->receive(body);
      if (status == DapChannel::Status::kNothing) {
        return;
      }
      if (status == DapChannel::Status::kClosed) {
        end_session();
        return;
      }
      dispatch(body);
    }
  }

  void end_session() {
    session_over_ = true;
    resumed_ = true;
    mode_ = Mode::kRunning;
    step_ = Step::kNone;
    breakpoints_.clear();
  }

  void send(const Json& message) {
    channel_->send(message.dump(-1, ' ', false, Json::error_handler_t::replace));
  }

  void respond(const Json& request, Json body = Json::object()) {
    send({{"seq", next_seq_++},
          {"type", "response"},
          {"request_seq", request.value("seq", 0)},
          {"success", true},
          {"command", request.value("command", "")},
          {"body", std::move(body)}});
  }

  void respond_error(const Json& request, std::string_view message) {
    send({{"seq", next_seq_++},
          {"type", "response"},
          {"request_seq", request.value("seq", 0)},
          {"success", false},
          {"command", request.value("command", "")},
          {"message", message}});
  }

  void event(std::string_view name, Json body = nullptr) {
    Json message = {{"seq", next_seq_++}, {"type", "event"}, {"event", name}};
    if (!body.is_null()) {
      message["body"] = std::move(body);
    }
    send(message);
  }

  void dispatch(const std::string& text) {
    const Json request = Json::parse(text, nullptr, false);
    if (!request.is_object() || request.value("type", "") != "request") {
      return;
    }
    const std::string command = request.value("command", "");
    try {
      handle(command, request, request.value("arguments", Json::object()));
    } catch (const Json::exception& error) {
      respond_error(request, std::string{"bad arguments for '"} + command + "': " + error.what());
    }
  }

  void handle(const std::string& command, const Json& request, const Json& args) {
    if (command == "initialize") {
      respond(request,
              {{"supportsConfigurationDoneRequest", true}, {"supportsEvaluateForHovers", true}});
      event("initialized");
    } else if (command == "launch" || command == "attach" || command == "setExceptionBreakpoints") {
      respond(request);
    } else if (command == "setBreakpoints") {
      set_breakpoints(request, args);
    } else if (command == "configurationDone") {
      configured_ = true;
      respond(request);
      resumed_ = true;  // the host starts the story now; later requests are for its first stop
    } else if (command == "threads") {
      respond(request, {{"threads", Json::array({{{"id", kThreadId}, {"name", "story"}}})}});
    } else if (command == "stackTrace") {
      stack_trace(request);
    } else if (command == "scopes") {
      scopes(request, args);
    } else if (command == "variables") {
      variables(request, args);
    } else if (command == "evaluate") {
      evaluate(request, args);
    } else if (command == "continue") {
      resume(request, Step::kNone, {{"allThreadsContinued", true}});
    } else if (command == "next") {
      resume(request, Step::kOver);
    } else if (command == "stepIn") {
      resume(request, Step::kIn);
    } else if (command == "stepOut") {
      resume(request, Step::kOut);
    } else if (command == "pause") {
      respond(request);
      if (mode_ == Mode::kRunning) {
        step_ = Step::kPause;
      }
    } else if (command == "disconnect" || command == "terminate") {
      respond(request);
      end_session();
    } else if (command == "vnmaki/goto") {
      request_goto(request, args);
    } else {
      respond_error(request, "unsupported request '" + command + "'");
    }
  }

  // ---- requests ----

  // The client's path of a script as the VFS path, lower case (paths are not case-sensitive on
  // Windows, and so the compare is not either).
  [[nodiscard]] std::string vfs_path(std::string_view client_path) const {
    const fs::path path{client_path};
    if (path.is_absolute()) {
      std::error_code error;
      const fs::path relative =
          fs::weakly_canonical(path, error).lexically_relative(fs::weakly_canonical(project_dir_));
      if (!relative.empty() && *relative.begin() != "..") {
        return lower(relative.generic_string());
      }
    }
    return lower(path.generic_string());
  }

  void set_breakpoints(const Json& request, const Json& args) {
    const std::string path = args.at("source").value("path", "");
    std::set<int> lines;
    Json reply = Json::array();
    for (const Json& breakpoint : args.value("breakpoints", Json::array())) {
      const int line = breakpoint.at("line").get<int>();
      lines.insert(line);
      // ponytail: a line with no code is "verified" too and never hits; ask Lua for the lines
      // with code (lua_getinfo "L") when that matters.
      reply.push_back({{"verified", true}, {"line", line}});
    }
    breakpoints_[vfs_path(path)] = std::move(lines);
    respond(request, {{"breakpoints", std::move(reply)}});
  }

  [[nodiscard]] bool require_stopped(const Json& request) {
    if (mode_ == Mode::kRunning) {
      respond_error(request, runtime_->state() == RunState::kFinished ? "the story has ended"
                                                                      : "the story is running");
      return false;
    }
    if (runtime_->label_thread() == nullptr) {
      respond_error(request, "no label is running");
      return false;
    }
    return true;
  }

  void stack_trace(const Json& request) {
    if (!require_stopped(request)) {
      return;
    }
    frames_ = script_frames(runtime_->label_thread(), runtime_->position().label);
    Json list = Json::array();
    int id = 0;
    for (const ScriptFrame& frame : frames_) {
      const fs::path file = project_dir_ / frame.path;
      list.push_back(
          {{"id", ++id},
           {"name", frame.name},
           {"line", frame.line},
           {"column", 1},
           {"source",
            {{"name", file.filename().generic_string()}, {"path", file.generic_string()}}}});
    }
    respond(request, {{"stackFrames", std::move(list)}, {"totalFrames", frames_.size()}});
  }

  int new_reference(Reference reference) {
    references_.push_back(std::move(reference));
    return static_cast<int>(references_.size());
  }

  void scopes(const Json& request, const Json& args) {
    if (!require_stopped(request)) {
      return;
    }
    const int frame_id = args.at("frameId").get<int>();
    if (frame_id < 1 || static_cast<std::size_t>(frame_id) > frames_.size()) {
      respond_error(request, "no such frame");
      return;
    }
    const int locals =
        new_reference({.kind = Reference::Kind::kLocals,
                       .level = frames_[static_cast<std::size_t>(frame_id) - 1].level});
    const int store = new_reference({.kind = Reference::Kind::kStore, .row = kRoot});
    respond(request,
            {{"scopes",
              Json::array(
                  {{{"name", "Locals"}, {"variablesReference", locals}, {"expensive", false}},
                   {{"name", "store"}, {"variablesReference", store}, {"expensive", false}}})}});
  }

  void variables(const Json& request, const Json& args) {
    if (!require_stopped(request)) {
      return;
    }
    const int id = args.at("variablesReference").get<int>();
    if (id < 1 || static_cast<std::size_t>(id) > references_.size()) {
      respond_error(request, "no such variablesReference");
      return;
    }
    const Reference reference = references_[static_cast<std::size_t>(id) - 1];
    Json list = Json::array();
    switch (reference.kind) {
      case Reference::Kind::kLocals:
        for (auto& [name, value] : frame_locals(runtime_->label_thread(), reference.level)) {
          list.push_back(value_variable(name, value));
        }
        break;
      case Reference::Kind::kStore:
        list = store_variables(reference.row);
        break;
      case Reference::Kind::kTable:
        list = table_variables(reference.table);
        break;
    }
    respond(request, {{"variables", std::move(list)}});
  }

  // A variable for a Lua value; a table gets a reference that `variables` expands.
  template <typename Object>
  Json value_variable(const std::string& name, const Object& value) {
    int reference = 0;
    if (value.get_type() == sol::type::table) {
      reference = new_reference({.kind = Reference::Kind::kTable, .table = sol::main_table{value}});
    }
    return variable(name, describe(value), reference, type_name(value.get_type()));
  }

  Json table_variables(const sol::main_table& table) {
    std::vector<std::pair<std::string, sol::object>> fields;
    table.for_each([&](const sol::object& key, const sol::object& value) {
      if (fields.size() >= kMaxTableFields) {
        return;
      }
      const std::string key_text =
          key.get_type() == sol::type::string ? key.as<std::string>() : describe(key);
      fields.emplace_back(key_text, value);
    });
    std::ranges::sort(fields, {}, &std::pair<std::string, sol::object>::first);
    Json list = Json::array();
    for (const auto& [name, value] : fields) {
      list.push_back(value_variable(name, value));
    }
    return list;
  }

  // The fields of store row `row` (kRoot: of `store` itself), from the rows the Inspectors show.
  Json store_variables(std::size_t row) {
    if (!store_rows_) {
      try {
        store_rows_ = inspect_store(*runtime_);
      } catch (const std::exception& error) {
        return Json::array({variable("error", error.what())});
      }
    }
    const std::vector<StoreRow>& rows = *store_rows_;
    const int parent_depth = row == kRoot ? -1 : rows[row].depth;
    Json list = Json::array();
    for (std::size_t i = row == kRoot ? 0 : row + 1;
         i < rows.size() && rows[i].depth > parent_depth; ++i) {
      if (rows[i].depth != parent_depth + 1) {
        continue;
      }
      const bool expanded = rows[i].is_table && rows[i].value.empty();
      const int reference =
          expanded ? new_reference({.kind = Reference::Kind::kStore, .row = i}) : 0;
      list.push_back(variable(rows[i].key, expanded ? "table" : rows[i].value, reference));
    }
    return list;
  }

  // A watch. A local of the frame answers by its name; any other expression runs in the script
  // sandbox like a REPL line (`store` and `persistent` are visible, locals are not).
  void evaluate(const Json& request, const Json& args) {
    if (!require_stopped(request)) {
      return;
    }
    const std::string expression = args.at("expression").get<std::string>();
    const int frame_id = args.value("frameId", 0);
    if (is_identifier(expression) && frame_id >= 1 &&
        static_cast<std::size_t>(frame_id) <= frames_.size()) {
      for (const auto& [name, value] : frame_locals(
               runtime_->label_thread(), frames_[static_cast<std::size_t>(frame_id) - 1].level)) {
        if (name == expression) {
          respond(request, {{"result", describe(value)}, {"variablesReference", 0}});
          return;
        }
      }
    }
    const EvalResult result = runtime_->eval(expression);
    if (!result.ok) {
      respond_error(request, result.text);
      return;
    }
    respond(request, {{"result", result.text}, {"variablesReference", 0}});
  }

  void resume(const Json& request, Step step, Json body = Json::object()) {
    respond(request, std::move(body));
    lua_State* thread = runtime_->label_thread();
    step_ = step;
    step_thread_ = thread;
    step_depth_ = thread != nullptr ? stack_depth(thread) : 0;
    clear_references();
    if (mode_ != Mode::kRunning) {
      mode_ = Mode::kRunning;
      resumed_ = true;
    }
  }

  // ---- goto ----

  void request_goto(const Json& request, const Json& args) {
    const std::string label = args.at("label").get<std::string>();
    const auto step = static_cast<std::uint32_t>(args.value("step", 1));
    if (!runtime_->has_label(label)) {
      respond_error(request, "no label '" + label + "'");
      return;
    }
    if (runtime_->state() == RunState::kIdle && runtime_->label_thread() == nullptr) {
      respond_error(request, "the story has not started");
      return;
    }
    PendingGoto go{.request = request, .label = label, .step = step};
    if (mode_ == Mode::kStopped) {
      // The story is in the middle of a run, so it cannot be replayed from here: stop the run
      // with an error, and poll() goes on once the run has unwound.
      pending_goto_ = std::move(go);
      abort_ = true;
      mode_ = Mode::kRunning;
      resumed_ = true;
      return;
    }
    perform_goto(go);
  }

  void perform_goto(const PendingGoto& go) {
    const std::string problem = jump_to_label(*runtime_, go.label, go.step);
    mode_ = Mode::kHolding;
    step_ = Step::kNone;
    clear_references();
    if (problem.empty()) {
      const ScriptPosition position = runtime_->position();
      respond(go.request, {{"label", position.label}, {"step", position.step}});
    } else {
      respond_error(go.request, problem);
    }
    if (runtime_->state() == RunState::kWaiting) {
      event("stopped", {{"reason", "goto"}, {"threadId", kThreadId}, {"allThreadsStopped", true}});
    }
  }

  // ---- the end ----

  void report_end() {
    const RunState state = runtime_->state();
    if (terminated_ || !configured_ ||
        (state != RunState::kFinished && state != RunState::kFailed)) {
      return;
    }
    terminated_ = true;
    if (state == RunState::kFailed) {
      event("output", {{"category", "stderr"}, {"output", runtime_->error() + "\n"}});
    }
    event("exited", {{"exitCode", state == RunState::kFailed ? 1 : 0}});
    event("terminated");
  }

  void clear_references() {
    references_.clear();
    store_rows_.reset();
  }

  inline static Impl* active_ = nullptr;
  inline static lua_Hook previous_hook_ = nullptr;

  ScriptRuntime* runtime_;
  DapChannel* channel_;
  fs::path project_dir_;
  std::function<bool()> pump_;
  int next_seq_ = 1;
  Mode mode_ = Mode::kRunning;
  Step step_ = Step::kNone;
  lua_State* step_thread_ = nullptr;
  int step_depth_ = 0;
  bool configured_ = false;
  bool session_over_ = false;
  bool terminated_ = false;
  bool resumed_ = false;
  bool abort_ = false;
  std::optional<PendingGoto> pending_goto_;
  std::map<std::string, std::set<int>> breakpoints_;
  std::vector<ScriptFrame> frames_;
  std::vector<Reference> references_;
  std::optional<std::vector<StoreRow>> store_rows_;
};

Debugger::Debugger(ScriptRuntime& runtime, DapChannel& channel, std::filesystem::path project_dir,
                   std::function<bool()> pump)
    : impl_(std::make_unique<Impl>(runtime, channel, std::move(project_dir), std::move(pump))) {}

Debugger::~Debugger() = default;

bool Debugger::poll() { return impl_->poll(); }
bool Debugger::configured() const { return impl_->configured(); }
bool Debugger::holding() const { return impl_->holding(); }

}  // namespace vnmaki
