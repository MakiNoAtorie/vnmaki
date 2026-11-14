// The launcher window (dev tools only): ImGui over the SDL GPU renderer, as the overlay does.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "cli/launcher.hpp"
#include "cli/new_command.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/platform/main_loop.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"
#include "engine/tools/imgui_backend.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitFailure = 1;
constexpr std::size_t kFolderBytes = 512;
constexpr Size kWindowSize{.width = 900, .height = 640};
constexpr Duration kFrameTime = std::chrono::duration_cast<Duration>(std::chrono::seconds{1}) / 60;

struct Button {
  const char* label;
  LauncherAction action;
};

constexpr std::array<Button, 6> kButtons{{{"New", LauncherAction::kNew},
                                          {"Run", LauncherAction::kRun},
                                          {"Check", LauncherAction::kCheck},
                                          {"Extract strings", LauncherAction::kExtractStrings},
                                          {"Build windows", LauncherAction::kBuildWindows},
                                          {"Open folder", LauncherAction::kOpenFolder}}};

// A language add-on of templates/addons/ and whether New applies it.
struct AddonChoice {
  std::string name;
  bool selected = false;
};

// One frame of the launcher. `folder` is the text field.
void draw_launcher(Launcher& launcher, std::array<char, kFolderBytes>& folder,
                   std::vector<AddonChoice>& addons, std::size_t& shown_log_size) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin("Launcher", nullptr, kFlags);

  ImGui::TextUnformatted("Project folder");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##folder", folder.data(), folder.size());
  for (std::size_t i = 0; i < kButtons.size(); ++i) {
    if (i > 0) {
      ImGui::SameLine();
    }
    if (ImGui::Button(kButtons[i].label)) {
      std::vector<std::string> languages;
      for (const AddonChoice& addon : addons) {
        if (addon.selected && kButtons[i].action == LauncherAction::kNew) {
          languages.push_back(addon.name);
        }
      }
      launcher.run(kButtons[i].action, folder.data(), languages);
    }
  }
  if (!addons.empty()) {
    ImGui::TextUnformatted("New with the language:");
    for (AddonChoice& addon : addons) {
      ImGui::SameLine();
      ImGui::Checkbox(addon.name.c_str(), &addon.selected);
    }
  }

  ImGui::SeparatorText("Recent projects");
  ImGui::BeginChild("##recent", ImVec2{0, ImGui::GetTextLineHeightWithSpacing() * 5}, true);
  for (const auto& project : launcher.recent().list()) {
    const std::string text = to_utf8(project);
    if (ImGui::Selectable(text.c_str(), text == folder.data())) {
      folder.fill('\0');
      text.copy(folder.data(), folder.size() - 1);
    }
  }
  ImGui::EndChild();

  ImGui::SeparatorText("Log");
  if (ImGui::Button("Clear log")) {
    launcher.clear_log();
  }
  ImGui::BeginChild("##log", ImVec2{0, 0}, true, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::TextUnformatted(launcher.log().c_str());
  if (launcher.log().size() != shown_log_size) {
    shown_log_size = launcher.log().size();
    ImGui::SetScrollHereY(1);  // the newest line
  }
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace

int run_launcher_window(Launcher& launcher, std::optional<std::uint64_t> max_frames,
                        std::ostream& err) {
  try {
    SdlPlatform platform{WindowConfig{.title = "VNMaki Launcher", .size = kWindowSize}};
    const std::unique_ptr<SdlGpuRenderer> renderer = SdlGpuRenderer::create_for_window(platform);
    ImguiBackend backend{*renderer};  // dies before the renderer
    SteadyClock clock;
    MainLoop loop{platform, clock, {.min_frame_time = kFrameTime}};
    std::array<char, kFolderBytes> folder{};
    std::size_t shown_log_size = 0;
    std::vector<AddonChoice> addons;
    for (std::string& name : available_addons()) {
      addons.push_back({.name = std::move(name)});
    }
    bool text_input = false;
    auto last_frame = std::chrono::steady_clock::now();

    MainLoopHooks hooks;
    hooks.on_event = [&](const InputEvent& event) {
      static_cast<void>(backend.handle_event(event));
    };
    hooks.draw = [&](double /*alpha*/) {
      const auto now = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(now - last_frame).count();
      last_frame = now;
      renderer->begin_frame();
      renderer->begin_pass({.target = kScreen, .clear = rgba8(24, 24, 28)});
      backend.begin_frame(platform.window_metrics().pixels, seconds);
      draw_launcher(launcher, folder, addons, shown_log_size);
      const bool wants_text = ImGui::GetIO().WantTextInput;
      if (wants_text != text_input) {
        platform.set_text_input(wants_text);
        text_input = wants_text;
      }
      backend.end_frame();
      static_cast<void>(renderer->end_frame());
    };
    loop.run(hooks, max_frames);
  } catch (const PlatformError& error) {
    err << "vnmaki launcher: " << error.what() << '\n';
    return kExitFailure;
  } catch (const RenderError& error) {
    err << "vnmaki launcher: " << error.what() << '\n';
    return kExitFailure;
  }
  return 0;
}

}  // namespace vnmaki::cli
