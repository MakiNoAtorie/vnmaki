#pragma once

// The script debugger (docs/SPEC.md section 18): a Debug Adapter Protocol server for the Lua
// scripts of the story. A client such as VS Code sends requests (breakpoints, stepping, stack,
// locals, `store`, watches, and the custom `vnmaki/goto`) through a DapChannel.
//
// Breakpoints and steps use a Lua line hook on the thread of the running label. This is the
// only module that uses `lua_sethook`, `lua_getstack`, `lua_getinfo` and `lua_getlocal`. The
// story never stops while the runtime replays (a load, a rollback, a reload, a goto).
//
// While the story is stopped the debugger waits inside the hook. It calls `pump` again and again
// (the window draws and handles OS events there, but the game gets no input) and reads requests.

#include <filesystem>
#include <functional>
#include <memory>

#include "engine/tools/dap.hpp"

namespace vnmaki {

class ScriptRuntime;

class Debugger {
 public:
  /// `pump` runs once per pass of the wait while the story is stopped, and returns false when the
  /// user closed the window (the debugger then ends the session). It may be empty. `project_dir`
  /// is where the client's source paths are relative to. Only one Debugger can exist.
  Debugger(ScriptRuntime& runtime, DapChannel& channel, std::filesystem::path project_dir,
           std::function<bool()> pump);
  Debugger(const Debugger&) = delete;
  Debugger& operator=(const Debugger&) = delete;
  Debugger(Debugger&&) = delete;
  Debugger& operator=(Debugger&&) = delete;
  ~Debugger();

  /// Handles the requests that have arrived, then a goto that a stop left open, and sends
  /// `terminated` when the story is over. Call it between frames, never from a script. Returns
  /// false when the session is over (disconnect, or the client closed the channel).
  bool poll();

  /// True after the client sent `configurationDone`: its breakpoints are set, so the host starts
  /// the story now.
  [[nodiscard]] bool configured() const;

  /// True after a goto, until the client continues or steps. A host that moves the story by
  /// itself (`vnmaki debug --headless`) waits while this is true.
  [[nodiscard]] bool holding() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
