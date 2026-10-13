#pragma once

// Helpers for tests that render on a real GPU device, headless.

#include <memory>
#include <string>

#include "engine/render/image.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"

namespace vnmaki::test {

/// A headless SDL GPU renderer, or nullptr after printing "VNMAKI_SKIP: <reason>" when this
/// machine has no usable GPU device. ctest reports such a test as skipped (see
/// tests/render/CMakeLists.txt). With VNMAKI_REQUIRE_GPU=1 in the environment (CI runners that
/// have a software driver) a missing device fails the test instead.
[[nodiscard]] std::unique_ptr<SdlGpuRenderer> make_headless_renderer();

/// Reports that the GPU (or the window it needs) is unavailable: fails the test under
/// VNMAKI_REQUIRE_GPU=1, else prints the "VNMAKI_SKIP: <reason>" line.
void skip_without_gpu(const std::string& reason);

/// Compares `actual` with the golden image tools/generators/render_fixtures.py wrote as
/// golden/<name>.png: same size, and every channel of every pixel within `tolerance`. On a
/// mismatch, fails the current test with the first differing pixel and writes `actual` to
/// golden-actual/<name>.pam in the working directory for inspection.
void check_golden(const Image& actual, const std::string& name, int tolerance = 2);

/// Loads fixture images/<name> through the image decoder.
[[nodiscard]] Image load_fixture_image(const std::string& name);

}  // namespace vnmaki::test
