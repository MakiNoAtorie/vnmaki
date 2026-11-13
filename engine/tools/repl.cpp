#include "engine/tools/repl.hpp"

#include <string>
#include <string_view>

namespace vnmaki {

void run_repl_line(ScriptRuntime& runtime, LogBuffer& log, std::string_view line) {
  log.add(LogLevel::kInfo, "> " + std::string{line});
  const EvalResult result = runtime.eval(line);
  if (!result.ok) {
    log.add(LogLevel::kError, result.text);
  } else if (!result.text.empty()) {
    log.add(LogLevel::kInfo, result.text);
  }
}

}  // namespace vnmaki
