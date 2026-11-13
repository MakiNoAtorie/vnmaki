#include "engine/platform/shell.hpp"

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>

#include <cstddef>
#include <string>
#endif

namespace vnmaki {

#ifdef _WIN32

namespace {

// One argument of a command line, quoted for CommandLineToArgvW: backslashes before a quote and
// before the closing quote are doubled.
std::wstring quote(const std::wstring& arg) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    result.append((c == L'"') ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    result += c;
  }
  result.append(backslashes * 2, L'\\');
  return result + L'"';
}

}  // namespace

std::filesystem::path executable_path() {
  std::wstring path(MAX_PATH, L'\0');
  while (true) {
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0) {
      return {};
    }
    if (size < path.size()) {
      path.resize(size);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

bool spawn_process(const std::filesystem::path& exe, std::span<const std::filesystem::path> args) {
  std::wstring command = quote(exe.native());
  for (const std::filesystem::path& arg : args) {
    command += L' ' + quote(arg.native());
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                     &startup, &process) == 0) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool open_folder(const std::filesystem::path& folder) {
  // ShellExecute returns a value above 32 when it works.
  const auto result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
}

#else

std::filesystem::path executable_path() { return {}; }

bool spawn_process(const std::filesystem::path& /*exe*/,
                   std::span<const std::filesystem::path> /*args*/) {
  return false;
}

bool open_folder(const std::filesystem::path& /*folder*/) { return false; }

#endif

}  // namespace vnmaki
