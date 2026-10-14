// The tag syntax is in docs/TEXT_TAGS.md.

#include "engine/text/markup.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/render/renderer.hpp"
#include "fuzz/markup_properties.hpp"

namespace {

using vnmaki::parse_markup;
using vnmaki::StyledText;
using vnmaki::TextStyle;

/// The style of the run that holds byte `offset` of the plain text.
TextStyle style_at(const StyledText& styled, std::size_t offset) {
  for (const vnmaki::StyledRun& run : styled.runs) {
    if (offset >= run.begin && offset < run.end) {
      return run.style;
    }
  }
  FAIL("no run holds byte " << offset);
  return {};
}

struct BadMarkup {
  std::string_view markup;
  std::size_t offset = 0;
  std::string_view reason;
};

}  // namespace

TEST_CASE("markup: text without tags is one run in the default style") {
  const StyledText styled = parse_markup("Hello.");
  CHECK(styled.text == "Hello.");
  REQUIRE(styled.runs.size() == 1);
  CHECK(styled.runs[0] == vnmaki::StyledRun{.begin = 0, .end = 6, .style = {}});
  CHECK(styled.pauses.empty());
  CHECK(parse_markup("").runs.empty());
}

TEST_CASE("markup: color styles the text between its tags") {
  const StyledText styled = parse_markup("I {color=#f00}really{/color} mean it.");
  CHECK(styled.text == "I really mean it.");
  REQUIRE(styled.runs.size() == 3);
  CHECK(styled.runs[0].style == TextStyle{});
  CHECK(styled.runs[1].begin == 2);
  CHECK(styled.runs[1].end == 8);
  CHECK(styled.runs[1].style.color == vnmaki::rgba8(255, 0, 0));
  CHECK(styled.runs[2].style == TextStyle{});
}

TEST_CASE("markup: a color is #rgb, #rgba, #rrggbb or #rrggbbaa") {
  CHECK(style_at(parse_markup("{color=#1a2B3c}x{/color}"), 0).color == vnmaki::rgba8(26, 43, 60));
  CHECK(style_at(parse_markup("{color=#1a2b3c80}x{/color}"), 0).color ==
        vnmaki::rgba8(26, 43, 60, 128));
  CHECK(style_at(parse_markup("{color=#0f08}x{/color}"), 0).color == vnmaki::rgba8(0, 255, 0, 136));
}

TEST_CASE("markup: size and font set the size in pixels and the font family") {
  const StyledText styled = parse_markup("a{size=30}b{font=serif}c{/font}{/size}");
  CHECK(styled.text == "abc");
  CHECK(style_at(styled, 0).size == 0);
  CHECK(style_at(styled, 1).size == 30);
  CHECK(style_at(styled, 1).font.empty());
  CHECK(style_at(styled, 2).size == 30);
  CHECK(style_at(styled, 2).font == "serif");
}

TEST_CASE("markup: b and i nest") {
  const StyledText styled = parse_markup("{b}a{i}b{/i}{/b}{i}c{/i}");
  CHECK(styled.text == "abc");
  REQUIRE(styled.runs.size() == 3);
  CHECK(style_at(styled, 0).bold);
  CHECK_FALSE(style_at(styled, 0).italic);
  CHECK(style_at(styled, 1).bold);
  CHECK(style_at(styled, 1).italic);
  CHECK_FALSE(style_at(styled, 2).bold);
  CHECK(style_at(styled, 2).italic);
}

TEST_CASE("markup: speed sets the reveal speed and pause marks a wait") {
  constexpr double kHalfSecond = 0.5;
  constexpr double kSlow = 2.5;
  const StyledText styled = parse_markup("ab{pause=0.5}c{speed=2.5}d{/speed}{pause=2}");
  CHECK(styled.text == "abcd");
  CHECK(style_at(styled, 2).speed == 0);
  CHECK(style_at(styled, 3).speed == kSlow);
  REQUIRE(styled.pauses.size() == 2);
  CHECK(styled.pauses[0] == vnmaki::TextPause{.offset = 2, .seconds = kHalfSecond});
  CHECK(styled.pauses[1] == vnmaki::TextPause{.offset = 4, .seconds = 2});
}

TEST_CASE("markup: ruby gives its base text the furigana") {
  const StyledText styled =
      parse_markup("{ruby=かんじ}漢字{/ruby}だ{ruby=a}x{/ruby}{ruby=a}y{/ruby}");
  CHECK(styled.text == "漢字だxy");
  // Each ruby tag is its own run, also beside one with the same furigana.
  REQUIRE(styled.runs.size() == 4);
  CHECK(styled.runs[0].end == 6);
  CHECK(styled.runs[0].style.ruby == "かんじ");
  CHECK(styled.runs[1].style.ruby.empty());
  CHECK(styled.runs[2].style.ruby == "a");
  CHECK(styled.runs[3].style.ruby == "a");
}

TEST_CASE("markup: link gives its text an id") {
  const StyledText styled = parse_markup("Go to the {link=shop}shop{/link}.");
  CHECK(styled.text == "Go to the shop.");
  CHECK(style_at(styled, 9).link.empty());
  CHECK(style_at(styled, 10).link == "shop");
  CHECK(style_at(styled, 14).link.empty());
}

TEST_CASE("markup: {{ is a brace, and } alone is plain text") {
  const StyledText styled = parse_markup("a {{b} c } d");
  CHECK(styled.text == "a {b} c } d");
  REQUIRE(styled.runs.size() == 1);
  CHECK(vnmaki::escape_markup("{b} {{") == "{{b} {{{{");
  CHECK(parse_markup(vnmaki::escape_markup("{b} {{ }")).text == "{b} {{ }");
}

TEST_CASE("markup: a bad tag is an error at the byte where the tag starts") {
  const std::vector<BadMarkup> cases{
      {.markup = "ab{colr=#fff}c", .offset = 2, .reason = "unknown tag 'colr'"},
      {.markup = "ab{b", .offset = 2, .reason = "'}'"},
      {.markup = "a{b{i}", .offset = 1, .reason = "'}'"},
      {.markup = "a{b}c", .offset = 1, .reason = "{b} is not closed"},
      {.markup = "a{/b}", .offset = 1, .reason = "{/b} closes no tag"},
      {.markup = "{b}a{i}b{/b}", .offset = 8, .reason = "{/b} does not match {i}"},
      {.markup = "{b=1}a{/b}", .offset = 0, .reason = "takes no value"},
      {.markup = "{color}a{/color}", .offset = 0, .reason = "color"},
      {.markup = "{color=red}a{/color}", .offset = 0, .reason = "color"},
      {.markup = "{color=#12345}a{/color}", .offset = 0, .reason = "color"},
      {.markup = "{size=0}a{/size}", .offset = 0, .reason = "size"},
      {.markup = "{size=-3}a{/size}", .offset = 0, .reason = "size"},
      {.markup = "{size=2000}a{/size}", .offset = 0, .reason = "size"},
      {.markup = "{font=}a{/font}", .offset = 0, .reason = "font"},
      {.markup = "{speed=fast}a{/speed}", .offset = 0, .reason = "speed"},
      {.markup = "{speed=0}a{/speed}", .offset = 0, .reason = "speed"},
      {.markup = "a{pause}", .offset = 1, .reason = "pause"},
      {.markup = "a{pause=1e3}", .offset = 1, .reason = "pause"},
      {.markup = "{ruby=}a{/ruby}", .offset = 0, .reason = "ruby"},
      {.markup = "{ruby=a}b{ruby=c}d{/ruby}{/ruby}", .offset = 9, .reason = "nested"},
      {.markup = "{link=a}b{link=c}d{/link}{/link}", .offset = 9, .reason = "nested"},
      {.markup = "{}", .offset = 0, .reason = "unknown tag ''"},
  };
  for (const BadMarkup& bad : cases) {
    CAPTURE(bad.markup);
    try {
      (void)parse_markup(bad.markup);
      FAIL("no error");
    } catch (const vnmaki::MarkupError& error) {
      CHECK(error.offset() == bad.offset);
      CHECK(std::string_view{error.what()}.find(bad.reason) != std::string_view::npos);
    }
  }
}

TEST_CASE("markup: the fuzz target's properties hold for texts made of tag pieces") {
  // The same check fuzz/markup_fuzzer.cpp runs, on a fixed set of strings: every sequence of
  // up to four of these pieces.
  const std::vector<std::string_view> pieces{
      "a",        "{",       "}", "{{", "{b}",          "{/b}",     "{pause=1}",
      "{ruby=x}", "{/ruby}", "=", "/",  "{color=#f00}", "{/color}", "\xE3\x81\x82"};
  constexpr std::size_t kLength = 4;
  std::size_t total = 1;
  for (std::size_t i = 0; i < kLength; ++i) {
    total *= pieces.size() + 1;
  }
  for (std::size_t code = 0; code < total; ++code) {
    std::string markup;
    for (std::size_t rest = code; rest > 0; rest /= pieces.size() + 1) {
      const std::size_t pick = rest % (pieces.size() + 1);
      if (pick < pieces.size()) {
        markup += pieces[pick];
      }
    }
    vnmaki::fuzz::check_markup_properties(markup);
  }
}
