#pragma once

#include <string_view>

#include "engine/runtime/script_runtime.hpp"
#include "engine/tools/log_buffer.hpp"

namespace vnmaki {

/// Runs one line of the console's REPL in the script sandbox (ScriptRuntime::eval) and logs
/// "> line", then the values it returned (info) or the error (error). `print` goes to the log
/// through the runtime's observer.
///
/// A change to `store` stays until the next rollback or load, which restore the snapshot of a
/// label entry. `persistent` and `prefs` keep it.
void run_repl_line(ScriptRuntime& runtime, LogBuffer& log, std::string_view line);

}  // namespace vnmaki
