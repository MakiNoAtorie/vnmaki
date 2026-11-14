#pragma once

#if defined(_MSC_VER)
#include <cstdlib>

#include <crtdbg.h>
#endif

namespace vnmaki {

// MSVC debug builds open a modal dialog on abort() and asserts, which blocks unattended runs.
inline void disable_crt_dialogs() {
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  for ([[maybe_unused]] const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
}

}  // namespace vnmaki
