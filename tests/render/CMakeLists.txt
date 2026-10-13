# Renderer tests render on a real GPU device without a window, so they get their own executable:
# only it needs a GPU, and on Linux it runs with SDL's offscreen video driver (no display), which
# reaches Vulkan through a software driver such as Mesa's lavapipe.
add_executable(vnmaki_render_tests ../unit/main.cpp gpu_test_support.cpp gpu_test_support.hpp
                                   renderer_test.cpp window_renderer_test.cpp)
target_link_libraries(vnmaki_render_tests PRIVATE vnmaki::engine vnmaki::options doctest::doctest)
target_compile_definitions(
  vnmaki_render_tests
  PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
          VNMAKI_GPU_TSAN_SUPPRESSIONS="${CMAKE_CURRENT_SOURCE_DIR}/gpu_driver.tsan.supp")
# A custom shader, built the way a game's would be.
target_sources(vnmaki_render_tests PRIVATE test_shaders.hpp)
vnmaki_add_shaders(
  vnmaki_render_tests
  NAMESPACE vnmaki::test_shaders
  SHADERS shaders/grayscale.frag.hlsl
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/shaders")

set(vnmaki_render_test_env "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND vnmaki_render_test_env "SDL_VIDEO_DRIVER=offscreen")
endif()
# A machine without any usable GPU device (not even a software one) skips the GPU tests: they
# print "VNMAKI_SKIP: <reason>" and ctest reports them as skipped. CI runners that have a software
# driver set VNMAKI_REQUIRE_GPU=1 so a broken driver setup fails instead of skipping.
doctest_discover_tests(vnmaki_render_tests TEST_PREFIX "unit." PROPERTIES ENVIRONMENT
                       "${vnmaki_render_test_env}" SKIP_REGULAR_EXPRESSION "VNMAKI_SKIP:")
