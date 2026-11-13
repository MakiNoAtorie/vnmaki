// The Phase 7 demo (docs/PLAN.md task 53), part 3: the say box of the template in vertical Japanese
// on a real GPU device. The test does what the story window does for `--lang ja`: it reads
// project.toml (`[vertical] ja = true`), takes the sample translation of translations/ja.po for a
// narrated line, runs the default `say` screen with that language's styles, and draws it with the
// template font. tools/generators/template_vertical_fixtures.py drew the golden image; its header
// says what the golden proves.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/say_box.hpp"
#include "engine/l10n/translator.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};  // resolution of the template
constexpr std::uint8_t kBackdrop = 26;
constexpr std::size_t kGlyphs = 15;  // "おわり。あなたの物語を書こう。"
constexpr const char* kNarration = "The end. Open scripts/main.lua to write your own story.";
constexpr const char* kJapanese = "おわり。あなたの物語を書こう。";

}  // namespace

TEST_CASE("template: the say box in vertical Japanese matches the golden") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(
      vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_TEMPLATES_DIR} / "default")));
  const vnmaki::ProjectConfig config = vnmaki::load_project_config(vfs);
  REQUIRE(config.languages == std::vector<std::string>{"en", "ja"});
  vnmaki::Translator translator{vfs, config};
  REQUIRE(translator.set_language("ja").empty());
  REQUIRE(translator.vertical());

  vnmaki::Scene scene{vfs, kVirtualSize};  // the template's top-level code makes sprites
  vnmaki::ScriptRuntime runtime{vfs,
                                {.scene = &scene,
                                 .canvas_width = kVirtualSize.width,
                                 .canvas_height = kVirtualSize.height}};
  // As the story window does it, before the scripts load.
  REQUIRE(runtime.set_language_styles(translator.style_file(), translator.vertical()).empty());
  runtime.load_scripts();

  // The scope of the narrated line in scripts/main.lua is its label.
  const std::string text = translator.translate("warm", "", kNarration);
  REQUIRE(text == kJapanese);
  const vnmaki::DialogueLine line{.speaker = {}, .color = std::nullopt, .text = text};

  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/default.ttf");
  vnmaki::SayBoxView view{*renderer, font, vnmaki::SayBoxStyle::for_resolution(kVirtualSize)};
  view.set_screen_source(
      [&](const vnmaki::DialogueLine& shown) -> std::optional<vnmaki::UiNode> {
        const vnmaki::ScreenNodeDef args{.kind = "args", .props = {{"speaker", shown.speaker},
                                                                   {"text", shown.text}}};
        vnmaki::ScreenNodeDef tree;
        REQUIRE(runtime.build_screen("say", args, tree).empty());
        return vnmaki::build_ui_tree(tree);
      },
      kVirtualSize);
  REQUIRE(view.glyph_count(line) == kGlyphs);

  const vnmaki::TextureId target =
      renderer->create_render_target(kVirtualSize, vnmaki::TextureFilter::kNearest);
  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(kBackdrop, kBackdrop, kBackdrop)});
  view.draw(line, kGlyphs, 1);
  const vnmaki::FrameStats stats = renderer->end_frame();

  CHECK(stats.draw_calls == 2);  // the panel and the columns (a narration has no name)
  vnmaki::test::check_golden_in("template_vertical", renderer->read_pixels(target), "say_vertical");
}
