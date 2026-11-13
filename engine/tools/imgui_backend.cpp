#include "engine/tools/imgui_backend.hpp"

#include <algorithm>
#include <variant>

#include <SDL3/SDL_gpu.h>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>

#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"

namespace vnmaki {

namespace {

// ImGui asserts on a delta time of 0 or less.
constexpr float kMinDeltaTime = 1.0F / 1000.0F;

// The ImGui key of an engine key, or ImGuiKey_None for one it has no use for.
ImGuiKey to_imgui_key(Key key) {
  const auto in_run = [key](Key first, Key last, ImGuiKey start) -> ImGuiKey {
    if (key < first || key > last) {
      return ImGuiKey_None;
    }
    return static_cast<ImGuiKey>(static_cast<int>(start) +
                                 (static_cast<int>(key) - static_cast<int>(first)));
  };
  for (const ImGuiKey run :
       {in_run(Key::kA, Key::kZ, ImGuiKey_A), in_run(Key::kDigit0, Key::kDigit9, ImGuiKey_0),
        in_run(Key::kF1, Key::kF12, ImGuiKey_F1)}) {
    if (run != ImGuiKey_None) {
      return run;
    }
  }
  switch (key) {
    case Key::kEscape:
      return ImGuiKey_Escape;
    case Key::kEnter:
      return ImGuiKey_Enter;
    case Key::kKeypadEnter:
      return ImGuiKey_KeypadEnter;
    case Key::kSpace:
      return ImGuiKey_Space;
    case Key::kTab:
      return ImGuiKey_Tab;
    case Key::kBackspace:
      return ImGuiKey_Backspace;
    case Key::kDelete:
      return ImGuiKey_Delete;
    case Key::kInsert:
      return ImGuiKey_Insert;
    case Key::kHome:
      return ImGuiKey_Home;
    case Key::kEnd:
      return ImGuiKey_End;
    case Key::kPageUp:
      return ImGuiKey_PageUp;
    case Key::kPageDown:
      return ImGuiKey_PageDown;
    case Key::kLeft:
      return ImGuiKey_LeftArrow;
    case Key::kRight:
      return ImGuiKey_RightArrow;
    case Key::kUp:
      return ImGuiKey_UpArrow;
    case Key::kDown:
      return ImGuiKey_DownArrow;
    case Key::kLeftShift:
      return ImGuiKey_LeftShift;
    case Key::kRightShift:
      return ImGuiKey_RightShift;
    case Key::kLeftCtrl:
      return ImGuiKey_LeftCtrl;
    case Key::kRightCtrl:
      return ImGuiKey_RightCtrl;
    case Key::kLeftAlt:
      return ImGuiKey_LeftAlt;
    case Key::kRightAlt:
      return ImGuiKey_RightAlt;
    case Key::kLeftGui:
      return ImGuiKey_LeftSuper;
    case Key::kRightGui:
      return ImGuiKey_RightSuper;
    default:
      return ImGuiKey_None;
  }
}

// ImGui's button index: left 0, right 1, middle 2, then the side buttons.
int to_imgui_button(MouseButton button) {
  switch (button) {
    case MouseButton::kLeft:
      return ImGuiMouseButton_Left;
    case MouseButton::kRight:
      return ImGuiMouseButton_Right;
    case MouseButton::kMiddle:
      return ImGuiMouseButton_Middle;
    case MouseButton::kX1:
      return 3;
    case MouseButton::kX2:
      return 4;
  }
  return ImGuiMouseButton_Left;
}

}  // namespace

ImguiBackend::ImguiBackend(SdlGpuRenderer& renderer) : renderer_(&renderer) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // a game folder gets no .ini file
  io.LogFilename = nullptr;
  ImGui::StyleColorsDark();
  const GpuInfo gpu = renderer.gpu_info();
  ImGui_ImplSDLGPU3_InitInfo info;
  info.Device = gpu.device;
  info.ColorTargetFormat = static_cast<SDL_GPUTextureFormat>(gpu.screen_format);
  if (!ImGui_ImplSDLGPU3_Init(&info)) {
    ImGui::DestroyContext();
    throw RenderError{"cannot start the ImGui renderer backend"};
  }
  renderer.set_overlay([this](const GpuOverlayTarget& target) { draw(target); });
}

ImguiBackend::~ImguiBackend() {
  renderer_->set_overlay({});
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();
}

bool ImguiBackend::handle_event(const InputEvent& event) {
  ImGuiIO& io = ImGui::GetIO();
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    const ImGuiKey imgui_key = to_imgui_key(key->key);
    if (imgui_key != ImGuiKey_None) {
      io.AddKeyEvent(imgui_key, key->pressed);
    }
    return io.WantCaptureKeyboard;
  }
  if (const auto* text = std::get_if<TextInputEvent>(&event)) {
    io.AddInputCharactersUTF8(text->text.c_str());
    return io.WantTextInput;
  }
  if (const auto* move = std::get_if<PointerMoveEvent>(&event)) {
    io.AddMousePosEvent(move->position.x, move->position.y);
    return io.WantCaptureMouse;
  }
  if (const auto* button = std::get_if<PointerButtonEvent>(&event)) {
    io.AddMousePosEvent(button->position.x, button->position.y);
    io.AddMouseButtonEvent(to_imgui_button(button->button), button->pressed);
    return io.WantCaptureMouse;
  }
  if (const auto* wheel = std::get_if<WheelEvent>(&event)) {
    io.AddMouseWheelEvent(-wheel->dx, wheel->dy);  // ImGui: a negative x scrolls right
    return io.WantCaptureMouse;
  }
  return false;
}

void ImguiBackend::begin_frame(Size pixels, double seconds) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2{static_cast<float>(pixels.width), static_cast<float>(pixels.height)};
  io.DeltaTime = std::max(static_cast<float>(seconds), kMinDeltaTime);
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui::NewFrame();
}

void ImguiBackend::end_frame() {
  ImGui::Render();
  ready_ = true;
}

void ImguiBackend::draw(const GpuOverlayTarget& target) {
  if (!ready_) {
    return;
  }
  ready_ = false;
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr || data->TotalVtxCount == 0) {
    return;
  }
  ImGui_ImplSDLGPU3_PrepareDrawData(data, target.commands);
  SDL_GPUColorTargetInfo color{};
  color.texture = target.texture;
  color.load_op = SDL_GPU_LOADOP_LOAD;
  color.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(target.commands, &color, 1, nullptr);
  if (pass == nullptr) {
    throw RenderError{"cannot begin the overlay render pass"};
  }
  ImGui_ImplSDLGPU3_RenderDrawData(data, target.commands, pass);
  SDL_EndGPURenderPass(pass);
}

}  // namespace vnmaki
