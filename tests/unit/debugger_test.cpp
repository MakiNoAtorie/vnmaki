// The DAP transport and the script debugger (engine/tools/dap.hpp, debugger.hpp).

#include "engine/tools/debugger.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "engine/runtime/script_runtime.hpp"
#include "engine/tools/dap.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using Json = nlohmann::json;
using vnmaki::DapChannel;
using vnmaki::Debugger;
using vnmaki::RunState;

constexpr std::size_t kSplitAt = 7;
constexpr int kFirstSeq = 100;

// Replays a fixed list of requests. An empty entry stands for "nothing yet" once. After the last
// one there is nothing yet, until end_input() closes it.
class ScriptedChannel final : public DapChannel {
 public:
  void push(std::string_view command, Json arguments = Json::object()) {
    const Json message = {{"seq", next_seq_++},
                          {"type", "request"},
                          {"command", command},
                          {"arguments", std::move(arguments)}};
    incoming_.push_back(message.dump());
  }
  void push_gap() { incoming_.emplace_back(); }
  void end_input() { closed_ = true; }

  Status receive(std::string& body) override {
    if (incoming_.empty()) {
      return closed_ ? Status::kClosed : Status::kNothing;
    }
    body = std::move(incoming_.front());
    incoming_.pop_front();
    return body.empty() ? Status::kNothing : Status::kMessage;
  }
  void send(std::string_view json) override { sent.push_back(Json::parse(json)); }

  // The response to the n-th request pushed (1 is the first).
  [[nodiscard]] Json response(int n) const {
    for (const Json& message : sent) {
      if (message.value("type", "") == "response" &&
          message.value("request_seq", 0) == kFirstSeq + n - 1) {
        return message;
      }
    }
    return Json{};
  }
  [[nodiscard]] std::size_t count_events(std::string_view event) const {
    std::size_t count = 0;
    for (const Json& message : sent) {
      count += message.value("event", "") == event ? 1 : 0;
    }
    return count;
  }
  [[nodiscard]] std::string reason_of_stop(std::size_t n) const {
    for (const Json& message : sent) {
      if (message.value("event", "") == "stopped" && n-- == 0) {
        return message["body"]["reason"];
      }
    }
    return {};
  }

  std::vector<Json> sent;

 private:
  std::deque<std::string> incoming_;
  int next_seq_ = kFirstSeq;
  bool closed_ = false;
};

// Lines: 1 label, 2 local, 3 store, 4 narrate a, 5 store, 6 narrate b, 7 end; then `warm`.
const char* const kScript =
    "label('start', function()\n"
    "  local mood = 'calm'\n"
    "  store.affection = 3\n"
    "  narrate('a')\n"
    "  store.affection = store.affection + 1\n"
    "  narrate('b')\n"
    "end)\n"
    "label('warm', function()\n"
    "  narrate('w1')\n"
    "  narrate('w2')\n"
    "end)\n";

struct Session {
  vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kScript}});
  vnmaki::ScriptRuntime runtime{vfs};
  ScriptedChannel channel;
  std::vector<std::uint32_t> steps_while_stopped;
  bool keep_window = true;
  std::unique_ptr<Debugger> debugger;

  Session() {
    runtime.load_scripts();
    debugger = std::make_unique<Debugger>(runtime, channel, "project", [this] {
      steps_while_stopped.push_back(runtime.position().step);
      return keep_window;
    });
  }
  // The client's setup: the breakpoints, then configurationDone.
  void configure(int line) {
    channel.push("initialize");
    channel.push("setBreakpoints",
                 {{"source", {{"path", "scripts/main.lua"}}}, {"breakpoints", {{{"line", line}}}}});
    channel.push("configurationDone");
    REQUIRE(debugger->poll());
    REQUIRE(debugger->configured());
  }
};

}  // namespace

TEST_CASE("dap: a message is framed with its length") {
  CHECK(vnmaki::frame_dap_message("{}") == "Content-Length: 2\r\n\r\n{}");
}

TEST_CASE("dap: the framer joins pieces and splits messages") {
  const std::string stream =
      vnmaki::frame_dap_message("{\"a\":1}") + vnmaki::frame_dap_message("{\"b\":22}");
  vnmaki::DapFramer framer;
  framer.feed(std::string_view{stream}.substr(0, kSplitAt));
  CHECK_FALSE(framer.next().has_value());
  framer.feed(std::string_view{stream}.substr(kSplitAt));
  CHECK(framer.next() == "{\"a\":1}");
  CHECK(framer.next() == "{\"b\":22}");
  CHECK_FALSE(framer.next().has_value());
}

TEST_CASE("dap: a header with no length is skipped, and the name is not case sensitive") {
  vnmaki::DapFramer framer;
  framer.feed("Foo: bar\r\n\r\ncontent-length:  2\r\n\r\n{}");
  CHECK(framer.next() == "{}");
}

TEST_CASE("debugger: a breakpoint stops the story, and the stop shows locals, store and watches") {
  Session s;
  s.configure(5);
  s.runtime.start("start");  // up to narrate('a')
  REQUIRE(s.runtime.state() == RunState::kWaiting);
  s.channel.push("threads");
  s.channel.push("stackTrace", {{"threadId", 1}});
  s.channel.push("scopes", {{"frameId", 1}});
  s.channel.push("variables", {{"variablesReference", 1}});
  s.channel.push("variables", {{"variablesReference", 2}});
  s.channel.push("evaluate", {{"expression", "store.affection"}, {"frameId", 1}});
  s.channel.push("evaluate", {{"expression", "mood"}, {"frameId", 1}});
  s.channel.push("continue");
  s.runtime.advance();  // line 5 is next: the breakpoint
  CHECK(s.runtime.state() == RunState::kWaiting);
  CHECK(s.runtime.position().step == 2);

  CHECK(s.channel.reason_of_stop(0) == "breakpoint");
  const Json frames = s.channel.response(5)["body"]["stackFrames"];
  REQUIRE(frames.size() == 1);
  CHECK(frames[0]["name"] == "start");
  CHECK(frames[0]["line"] == 5);
  CHECK(frames[0]["source"]["name"] == "main.lua");
  CHECK(s.channel.response(6)["body"]["scopes"].size() == 2);
  const Json locals = s.channel.response(7)["body"]["variables"];
  REQUIRE(locals.size() == 1);
  CHECK(locals[0]["name"] == "mood");
  CHECK(locals[0]["value"] == "\"calm\"");
  bool found_affection = false;
  const Json store_fields = s.channel.response(8)["body"]["variables"];
  for (const Json& field : store_fields) {
    if (field["name"] == "affection") {
      found_affection = true;
      CHECK(field["value"] == "3");
    }
  }
  CHECK(found_affection);
  CHECK(s.channel.response(9)["body"]["result"] == "3");
  CHECK(s.channel.response(10)["body"]["result"] == "\"calm\"");
}

TEST_CASE("debugger: the step count does not change while the story is stopped") {
  Session s;
  s.configure(5);
  s.runtime.start("start");
  const std::uint32_t step = s.runtime.position().step;
  s.channel.push_gap();
  s.channel.push_gap();
  s.channel.push("evaluate", {{"expression", "store.affection = 9"}});
  s.channel.push_gap();
  s.channel.push("continue");
  s.runtime.advance();
  REQUIRE_FALSE(s.steps_while_stopped.empty());
  for (const std::uint32_t seen : s.steps_while_stopped) {
    CHECK(seen == step);  // the label has not reached a yield point since
  }
  CHECK(s.runtime.position().step == step + 1);
}

TEST_CASE("debugger: next stops at the next line, stepIn and stepOut too") {
  Session s;
  s.configure(2);
  s.channel.push("stackTrace", {{"threadId", 1}});
  s.channel.push("next");
  s.channel.push("stackTrace", {{"threadId", 1}});
  s.channel.push("stepIn");
  s.channel.push("stackTrace", {{"threadId", 1}});
  s.channel.push("stepOut");  // the label function is the outermost frame: runs on to a yield
  s.runtime.start("start");
  CHECK(s.channel.reason_of_stop(0) == "breakpoint");
  CHECK(s.channel.reason_of_stop(1) == "step");
  CHECK(s.channel.reason_of_stop(2) == "step");
  const Json first = s.channel.response(4);
  const Json second = s.channel.response(6);
  const Json third = s.channel.response(8);
  CHECK(first["body"]["stackFrames"][0]["line"] == 2);
  CHECK(second["body"]["stackFrames"][0]["line"] == 3);
  CHECK(third["body"]["stackFrames"][0]["line"] == 4);
  CHECK(s.runtime.state() == RunState::kWaiting);
}

TEST_CASE("debugger: pause stops at the next line that runs") {
  Session s;
  s.configure(99);
  s.channel.push("pause");
  REQUIRE(s.debugger->poll());
  s.channel.push("stackTrace", {{"threadId", 1}});
  s.channel.push("continue");
  s.runtime.start("start");
  CHECK(s.channel.reason_of_stop(0) == "pause");
  CHECK(s.channel.response(5)["body"]["stackFrames"][0]["line"] == 2);
  CHECK(s.runtime.state() == RunState::kWaiting);
}

TEST_CASE("debugger: replay does not stop at a breakpoint") {
  Session s;
  s.configure(3);
  s.channel.push("continue");
  s.runtime.start("start");  // stops at line 3, then continues
  REQUIRE(s.channel.count_events("stopped") == 1);
  s.runtime.advance();
  REQUIRE(s.runtime.position().step == 2);
  CHECK(s.runtime.roll_back());  // replays line 3 again, silently
  CHECK(s.runtime.position().step == 1);
  CHECK(s.channel.count_events("stopped") == 1);
  s.runtime.advance();  // the lines after narrate('a') have no breakpoint
  CHECK(s.channel.count_events("stopped") == 1);
}

TEST_CASE("debugger: breakpoints work in the new VM after a reload") {
  Session s;
  s.configure(5);
  s.runtime.start("start");
  CHECK(s.runtime.reload_scripts().empty());  // replays to the same yield point, with no stop
  REQUIRE(s.channel.count_events("stopped") == 0);
  s.channel.push("continue");
  s.runtime.advance();
  CHECK(s.channel.reason_of_stop(0) == "breakpoint");
  CHECK(s.runtime.position().step == 2);
}

TEST_CASE("debugger: vnmaki/goto from a stop goes to the label through replay") {
  Session s;
  s.configure(5);
  s.runtime.start("start");
  s.channel.push("vnmaki/goto", {{"label", "warm"}, {"step", 2}});
  s.channel.push("stackTrace", {{"threadId", 1}});
  s.runtime.advance();                            // stops at line 5, then the goto ends the run
  CHECK(s.runtime.state() == RunState::kFailed);  // the run was cut short, goto repairs it
  CHECK(s.debugger->poll());
  CHECK(s.runtime.state() == RunState::kWaiting);
  CHECK(s.runtime.position().label == "warm");
  CHECK(s.runtime.position().step == 2);
  CHECK(s.debugger->holding());
  const Json answer = s.channel.response(4);
  CHECK(answer["success"] == true);
  CHECK(answer["body"]["label"] == "warm");
  CHECK(answer["body"]["step"] == 2);
  CHECK(s.channel.reason_of_stop(1) == "goto");
  CHECK(s.channel.response(5)["body"]["stackFrames"][0]["name"] == "warm");
}

TEST_CASE("debugger: vnmaki/goto at a yield point, and a goto that cannot be reached") {
  Session s;
  s.configure(99);
  s.runtime.start("start");
  s.channel.push("vnmaki/goto", {{"label", "warm"}, {"step", 3}});  // warm has two steps
  s.channel.push("vnmaki/goto", {{"label", "nowhere"}});
  s.channel.push("vnmaki/goto", {{"label", "warm"}, {"step", 1}});
  CHECK(s.debugger->poll());
  CHECK(s.channel.response(4)["success"] == false);
  CHECK(s.channel.response(5)["success"] == false);
  CHECK(s.channel.response(6)["success"] == true);
  CHECK(s.runtime.position().label == "warm");
  CHECK(s.runtime.position().step == 1);
}

TEST_CASE("debugger: a goto before the story starts is refused") {
  Session s;
  s.channel.push("vnmaki/goto", {{"label", "warm"}});
  CHECK(s.debugger->poll());
  CHECK(s.channel.response(1)["success"] == false);
  CHECK(s.runtime.state() == RunState::kIdle);
}

TEST_CASE("debugger: closing the window or the channel ends the session and the story goes on") {
  SUBCASE("window") {
    Session s;
    s.configure(5);
    s.runtime.start("start");
    s.keep_window = false;
    s.channel.push_gap();
    s.runtime.advance();
    CHECK(s.runtime.state() == RunState::kWaiting);  // it ran on to the next yield
    CHECK_FALSE(s.debugger->poll());
    s.runtime.advance();
    CHECK(s.runtime.state() == RunState::kFinished);
  }
  SUBCASE("channel closed") {
    Session s;
    s.configure(5);
    s.runtime.start("start");
    s.channel.end_input();
    s.runtime.advance();  // stops at line 5; the input ends, so the stop ends
    CHECK(s.runtime.state() == RunState::kWaiting);
    CHECK_FALSE(s.debugger->poll());
  }
}

TEST_CASE("debugger: the end of the story sends exited and terminated once") {
  Session s;
  s.configure(99);
  s.runtime.start("start");
  s.runtime.advance();
  s.runtime.advance();
  REQUIRE(s.runtime.state() == RunState::kFinished);
  CHECK(s.debugger->poll());
  CHECK(s.debugger->poll());
  CHECK(s.channel.count_events("exited") == 1);
  CHECK(s.channel.count_events("terminated") == 1);
}

TEST_CASE("debugger: a request it does not know, or with bad arguments, gets an error") {
  Session s;
  s.channel.push("frobnicate");
  s.channel.push("setBreakpoints", {{"source", 5}});
  s.channel.push("stackTrace");
  CHECK(s.debugger->poll());
  CHECK(s.channel.response(1)["success"] == false);
  CHECK(s.channel.response(2)["success"] == false);
  CHECK(s.channel.response(3)["success"] == false);  // the story is not stopped
}
