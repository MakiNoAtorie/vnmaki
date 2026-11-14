#include "cli/build_command.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cli/run_command.hpp"
#include "engine/assets/importer.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/vfs/pak_mount.hpp"
#include "engine/vfs/pak_writer.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

namespace fs = std::filesystem;

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr std::string_view kRunnerName = "vnmaki.exe";
constexpr std::string_view kReleaseDirectory = "release";
constexpr std::string_view kInvalidNameChars = "<>:\"/\\|?*";

struct BuildArguments {
  fs::path project;
  fs::path out;
  std::optional<fs::path> runner;
};

std::optional<BuildArguments> parse_arguments(std::span<const std::string_view> args,
                                              std::ostream& err) {
  BuildArguments parsed;
  if (args.empty() || args.front() != "windows") {
    err << "vnmaki build: the platform must be 'windows' (the only one)\n";
    return std::nullopt;
  }
  for (std::size_t i = 1; i < args.size(); ++i) {
    const bool has_value = i + 1 < args.size();
    if (args[i] == "--out" && has_value && parsed.out.empty()) {
      parsed.out = utf8_path(args[++i]);
    } else if (args[i] == "--runner" && has_value && !parsed.runner) {
      parsed.runner = utf8_path(args[++i]);
    } else if (!args[i].starts_with("-") && parsed.project.empty()) {
      parsed.project = utf8_path(args[i]);
    } else {
      err << "vnmaki build: unexpected argument '" << args[i] << "'\n";
      return std::nullopt;
    }
  }
  if (parsed.project.empty() || parsed.out.empty()) {
    err << "vnmaki build: needs a project folder and --out\n";
    return std::nullopt;
  }
  return parsed;
}

// The release runner next to the running dev build: build/debug/cli -> build/release/cli.
fs::path default_runner() {
  const fs::path executable = executable_directory();  // SDL ends it with a separator
  const fs::path folder = executable.has_filename() ? executable : executable.parent_path();
  return folder.parent_path().parent_path() / std::string{kReleaseDirectory} / folder.filename() /
         std::string{kRunnerName};
}

// A title as a file name: Windows rejects some characters and a trailing dot or space.
std::string exe_name_of(std::string title) {
  for (char& c : title) {
    if (kInvalidNameChars.find(c) != std::string_view::npos ||
        std::iscntrl(static_cast<unsigned char>(c)) != 0) {
      c = '_';
    }
  }
  while (!title.empty() && (title.back() == '.' || title.back() == ' ')) {
    title.pop_back();
  }
  return (title.empty() ? std::string{"game"} : title) + ".exe";
}

// The virtual path of `out` inside the project, or empty when it is outside.
std::string out_inside(const fs::path& project, const fs::path& out) {
  std::error_code error;
  const fs::path relative =
      fs::weakly_canonical(out, error).lexically_relative(fs::weakly_canonical(project, error));
  if (error || relative.empty() || *relative.begin() == "..") {
    return {};
  }
  return to_generic_utf8(relative);
}

// What an earlier build leaves at the top of its folder: a pack must not take it in again.
bool is_build_output(std::string_view name) {
  std::string lower{name};
  std::ranges::transform(lower, lower.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.ends_with(".exe") || lower.ends_with(".dll") || lower == kGamePakName ||
         (lower.starts_with(kPatchPakPrefix) && lower.ends_with(kPakExtension));
}

// Every file of the project that the pack takes as it is, in order.
void collect_files(const Vfs& vfs, const std::string& directory, const std::string& skip,
                   std::vector<std::string>& files) {
  for (const VfsEntry& entry : vfs.list(directory)) {
    const std::string path = directory.empty() ? entry.name : directory + "/" + entry.name;
    if ((directory.empty() && (entry.name.starts_with(".") || is_build_output(entry.name))) ||
        path == skip) {
      continue;
    }
    if (entry.is_directory) {
      collect_files(vfs, path, skip, files);
    } else {
      files.push_back(path);
    }
  }
}

void copy_runner(const fs::path& runner, const fs::path& out, const std::string& exe_name) {
  fs::copy_file(runner, out / utf8_path(exe_name), fs::copy_options::overwrite_existing);
  for (const fs::directory_entry& entry : fs::directory_iterator{runner.parent_path()}) {
    std::string extension = to_utf8(entry.path().extension());
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".dll" && entry.is_regular_file()) {
      fs::copy_file(entry.path(), out / entry.path().filename(),
                    fs::copy_options::overwrite_existing);
    }
  }
}

}  // namespace

int build_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  const std::optional<BuildArguments> arguments = parse_arguments(args, err);
  if (!arguments) {
    err << "usage:" << kBuildUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }

  try {
    Vfs vfs;
    if (!vfs.mount(make_directory_mount(arguments->project))) {
      err << "vnmaki: " << to_utf8(arguments->project) << " is not a directory\n";
      return kExitError;
    }
    std::error_code same_error;
    if (fs::equivalent(arguments->out, arguments->project, same_error)) {
      err << "vnmaki: --out must not be the project folder itself\n";
      return kExitError;
    }
    const ProjectConfig config = load_project_config(vfs);
    const fs::path runner = arguments->runner ? *arguments->runner : default_runner();
    if (!fs::is_regular_file(runner)) {
      err << "vnmaki: no runner at " << to_utf8(runner)
          << "; build the release preset (build/release) or pass --runner <vnmaki.exe>\n";
      return kExitError;
    }

    const ImportReport report = Importer{vfs, arguments->project}.import_all();
    for (const std::string& problem : report.problems) {
      err << "vnmaki: " << problem << "\n";
    }
    if (!report.problems.empty()) {
      return kExitError;
    }

    // The pack takes the output of an importer in place of its source.
    std::set<std::string> replaced;
    for (const ImportedFile& file : report.files) {
      if (!file.path.empty()) {
        replaced.insert(file.source);
      }
    }
    std::vector<std::string> files;
    collect_files(vfs, "", out_inside(arguments->project, arguments->out), files);

    fs::create_directories(arguments->out);
    const fs::path pak_path = arguments->out / std::string{kGamePakName};
    const fs::path temporary = fs::path{pak_path}.concat(".tmp");
    std::size_t count = 0;
    {
      std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
      if (!stream) {
        throw PakError{"cannot write " + to_utf8(temporary)};
      }
      PakWriter pak{stream};
      for (const std::string& path : files) {
        if (replaced.contains(path)) {
          continue;
        }
        const std::optional<std::vector<std::byte>> data = vfs.read(path);
        if (!data) {
          throw PakError{"cannot read " + path};
        }
        pak.add(path, *data);
        ++count;
      }
      for (const ImportedFile& file : report.files) {
        if (!file.path.empty()) {
          pak.add(file.path, file.data);
          ++count;
        }
      }
      pak.finish();
    }
    fs::rename(temporary, pak_path);
    out << "packed " << count << " files into " << to_utf8(pak_path) << "\n";

    const std::string exe_name = exe_name_of(config.title);
    copy_runner(runner, arguments->out, exe_name);
    out << "runner " << to_utf8(runner) << " -> " << to_utf8(arguments->out / utf8_path(exe_name))
        << "\n";
    return 0;
  } catch (const ProjectConfigError& error) {
    err << "vnmaki: " << error.what() << "\n";
  } catch (const ImportSettingsError& error) {
    err << "vnmaki: " << error.what() << "\n";
  } catch (const PakError& error) {
    err << "vnmaki: " << error.what() << "\n";
  } catch (const PlatformError& error) {
    err << "vnmaki: " << error.what() << "\n";
  } catch (const std::runtime_error& error) {  // also std::filesystem::filesystem_error
    err << "vnmaki: " << error.what() << "\n";
  }
  return kExitError;
}

std::optional<int> build_or_packed_command(std::span<const std::string_view> args,
                                           std::ostream& out, std::ostream& err) {
  if (!args.empty() && args.front() == "build") {
    return build_command(args.subspan(1), out, err);
  }
  if (!args.empty() && !args.front().starts_with("-")) {
    return std::nullopt;
  }
  std::error_code error;
  fs::path folder;
  try {
    folder = executable_directory();
  } catch (const PlatformError&) {
    return std::nullopt;
  }
  if (!fs::is_regular_file(folder / std::string{kGamePakName}, error)) {
    return std::nullopt;
  }
  const std::string folder_text = to_utf8(folder);
  std::vector<std::string_view> run_args{folder_text};
  run_args.insert(run_args.end(), args.begin(), args.end());
  return run_command(run_args, out, err);
}

}  // namespace vnmaki::cli
