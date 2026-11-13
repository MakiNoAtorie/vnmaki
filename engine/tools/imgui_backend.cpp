#include "engine/tools/imgui_backend.hpp"

#include <algorithm>

#include <SDL3/SDL_gpu.h>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"

namespace vnmaki {

namespace {

// ImGui asserts on a delta time of 0 or less.
constexpr float kMinDeltaTime = 1.0F / 1000.0F;

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
