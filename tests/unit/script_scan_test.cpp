// The Lua tokenizer and the story scan (engine/tools/script_scan.hpp). The expected tokens and
// calls are written out by hand from the source text of each case.

#include "engine/tools/script_scan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "fuzz/script_scan_properties.hpp"

namespace {

using vnmaki::LuaToken;
using vnmaki::LuaTokenKind;
using vnmaki::ScanCall;
using vnmaki::ScanCallKind;
using vnmaki::ScanLabel;

constexpr LuaTokenKind kName = LuaTokenKind::kName;
constexpr LuaTokenKind kNumber = LuaTokenKind::kNumber;
constexpr LuaTokenKind kString = LuaTokenKind::kString;
constexpr LuaTokenKind kSymbol = LuaTokenKind::kSymbol;

LuaToken token(LuaTokenKind kind, std::string text, std::uint32_t line = 1) {
  return {.kind = kind, .text = std::move(text), .line = line};
}

using Arguments = std::vector<std::optional<std::string>>;
using Elements = std::vector<vnmaki::ScanElement>;

ScanCall call(ScanCallKind kind, std::uint32_t line, std::uint32_t depth, Arguments arguments) {
  return {.kind = kind, .line = line, .depth = depth, .arguments = std::move(arguments)};
}

ScanCall menu_call(std::uint32_t line, Elements options) {
  ScanCall result = call(ScanCallKind::kMenu, line, 0, {std::nullopt});
  result.table = std::move(options);
  return result;
}

// The strings among the tokens.
std::vector<std::string> strings_of(std::string_view source) {
  std::vector<std::string> strings;
  for (const LuaToken& found : vnmaki::tokenize_lua(source)) {
    if (found.kind == kString) {
      strings.push_back(found.text);
    }
  }
  return strings;
}

}  // namespace

TEST_CASE("script_scan tokenizer: names, numbers, symbols and lines") {
  const std::vector<LuaToken> expected{
      token(kName, "local"),       token(kName, "x"),        token(kSymbol, "="),
      token(kNumber, "1.5e+3"),    token(kName, "jump", 2),  token(kSymbol, "(", 2),
      token(kString, "a", 2),      token(kSymbol, ")", 2),   token(kNumber, ".5", 3),
      token(kNumber, "0x1p-3", 3), token(kNumber, "0xe", 3), token(kSymbol, "+", 3),
      token(kNumber, "1", 3)};
  CHECK(vnmaki::tokenize_lua("local x = 1.5e+3\njump(\"a\")\n.5 0x1p-3 0xe+1") == expected);
  CHECK(vnmaki::tokenize_lua("").empty());
  CHECK(vnmaki::tokenize_lua(" \t\n\r\n -- nothing\n").empty());
}

TEST_CASE("script_scan tokenizer: short strings decode their escapes") {
  CHECK(strings_of(R"("a\nb")") == std::vector<std::string>{"a\nb"});
  CHECK(strings_of(R"('it\'s' "say \"hi\"" "back\\slash")") ==
        std::vector<std::string>{"it's", "say \"hi\"", "back\\slash"});
  CHECK(strings_of(R"("\65\066\x43\x4a")") == std::vector<std::string>{"ABCJ"});
  CHECK(strings_of("\"a\\z   \n   b\"") == std::vector<std::string>{"ab"});
  CHECK(strings_of("\"a\\\nb\"") == std::vector<std::string>{"a\nb"});
  CHECK(strings_of(R"("\u{48}")") == std::vector<std::string>{"\\u{48}"});
  CHECK(strings_of("\"\"") == std::vector<std::string>{""});
}

TEST_CASE("script_scan tokenizer: long strings keep their content and count their lines") {
  CHECK(strings_of("[[a]]") == std::vector<std::string>{"a"});
  CHECK(strings_of("[==[a]]b]=]c]==]") == std::vector<std::string>{"a]]b]=]c"});
  CHECK(strings_of("[[\nfirst\nsecond]]") == std::vector<std::string>{"first\nsecond"});
  CHECK(strings_of("[[a\r\nb]]") == std::vector<std::string>{"a\nb"});
  const std::vector<LuaToken> tokens = vnmaki::tokenize_lua("[[\n\n\n]] x");
  REQUIRE(tokens.size() == 2);
  CHECK(tokens[0].line == 1);
  CHECK(tokens[1].line == 4);
}

TEST_CASE("script_scan tokenizer: comments give no tokens and hide what they hold") {
  CHECK(vnmaki::tokenize_lua("-- jump(\"x\")\nx").size() == 1);
  CHECK(vnmaki::tokenize_lua("--[[ jump(\"x\")\n]] x").size() == 1);
  CHECK(vnmaki::tokenize_lua("--[==[ ]] still a comment ]==] x").size() == 1);
  CHECK(vnmaki::tokenize_lua("--[ not long\nx").size() == 1);
  // A string that holds "--" is a string, and a comment that holds a quote has no string.
  CHECK(strings_of("\"a -- b\" -- \"c\"") == std::vector<std::string>{"a -- b"});
  const std::vector<LuaToken> after = vnmaki::tokenize_lua("--[[\n\n]] x");
  REQUIRE(after.size() == 1);
  CHECK(after[0].line == 3);
}

TEST_CASE("script_scan tokenizer: line breaks are LF, CR, CRLF and LFCR") {
  const std::vector<LuaToken> tokens = vnmaki::tokenize_lua("a\nb\rc\r\nd\n\re");
  REQUIRE(tokens.size() == 5);
  CHECK(tokens[0].line == 1);
  CHECK(tokens[1].line == 2);
  CHECK(tokens[2].line == 3);
  CHECK(tokens[3].line == 4);
  CHECK(tokens[4].line == 5);
}

TEST_CASE("script_scan tokenizer: a script that is cut off still gives tokens") {
  // A short string ends at the line break; a long string and a comment at the end of the source.
  CHECK(vnmaki::tokenize_lua("x = \"open\ny").size() == 4);
  CHECK(strings_of("\"open\ny") == std::vector<std::string>{"open"});
  CHECK(strings_of("[[open") == std::vector<std::string>{"open"});
  CHECK(vnmaki::tokenize_lua("--[[ open").empty());
  CHECK(strings_of("\"a\\") == std::vector<std::string>{"a"});
  CHECK(strings_of("[==[a]=]") == std::vector<std::string>{"a]=]"});
  // A byte that no Lua token starts with is a symbol of its own.
  const std::vector<LuaToken> odd = vnmaki::tokenize_lua("a \xC3\xA9 b");
  CHECK(odd.size() == 4);
  CHECK(odd[1].kind == kSymbol);
}

TEST_CASE("script_scan: finds labels and the story calls in them") {
  const std::string source =
      "local eileen = 1\n"                       // 1
      "label(\"start\", function()\n"            // 2
      "  say(eileen, \"Hello\")\n"               // 3
      "  show \"eileen\"\n"                      // 4
      "  call(\"aside\")\n"                      // 5
      "  jump('next')\n"                         // 6
      "end)\n"                                   // 7
      "label(\"next\", function() ret() end,\n"  // 8
      "  { ending = true })\n";                  // 9
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 2);

  CHECK(labels[0].name == "start");
  CHECK(labels[0].line == 2);
  CHECK(labels[0].end_line == 7);
  CHECK_FALSE(labels[0].ending);
  ScanCall say = call(ScanCallKind::kSay, 3, 0, {std::nullopt, "Hello"});
  say.speaker = "eileen";
  const std::vector<ScanCall> expected{say, call(ScanCallKind::kShow, 4, 0, {"eileen"}),
                                       call(ScanCallKind::kCall, 5, 0, {"aside"}),
                                       call(ScanCallKind::kJump, 6, 0, {"next"})};
  CHECK(labels[0].calls == expected);

  CHECK(labels[1].name == "next");
  CHECK(labels[1].line == 8);
  CHECK(labels[1].end_line == 8);
  CHECK(labels[1].ending);
  CHECK(labels[1].calls == std::vector<ScanCall>{call(ScanCallKind::kRet, 8, 0, {})});
}

TEST_CASE("script_scan: a computed target or name is not a string literal") {
  const std::string source =
      "label(name, function()\n"            // 1
      "  jump(target)\n"                    // 2
      "end)\n"                              // 3
      "label(\"a\", function()\n"           // 4
      "  jump(\"x\" .. suffix)\n"           // 5
      "  call(table[\"k\"])\n"              // 6
      "  jump(\"f\" .. g(\"h\", \"i\"))\n"  // 7
      "  menu { \"one\", \"two\" }\n"       // 8
      "end)\n";                             // 9
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 2);
  CHECK_FALSE(labels[0].name.has_value());
  CHECK(labels[0].calls == std::vector<ScanCall>{call(ScanCallKind::kJump, 2, 0, {std::nullopt})});
  CHECK(labels[1].name == "a");
  const std::vector<ScanCall> expected{
      call(ScanCallKind::kJump, 5, 0, {std::nullopt}),
      call(ScanCallKind::kCall, 6, 0, {std::nullopt}),
      call(ScanCallKind::kJump, 7, 0, {std::nullopt}),
      menu_call(8, {{.text = "one", .line = 8}, {.text = "two", .line = 8}})};
  CHECK(labels[1].calls == expected);
}

TEST_CASE("script_scan: text in comments and strings is not a call") {
  const std::string source =
      "label(\"a\", function()\n"
      "  -- jump(\"commented\")\n"
      "  --[[ call(\"block\") ]]\n"
      "  local s = \"jump('in a string')\"\n"
      "  local t = [[ jump(\"in a long string\") ]]\n"
      "  jump(\"real\")\n"
      "end)\n";
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 1);
  CHECK(labels[0].calls == std::vector<ScanCall>{call(ScanCallKind::kJump, 6, 0, {"real"})});
}

TEST_CASE("script_scan: a method, a field or a definition is not a call") {
  const std::string source =
      "label(\"a\", function()\n"
      "  obj:jump(\"m\")\n"
      "  tbl.call(\"f\")\n"
      "  local function say() end\n"
      "  local jump_target = jump\n"
      "  show\n"
      "  jump(\"real\")\n"
      "end)\n";
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 1);
  CHECK(labels[0].calls == std::vector<ScanCall>{call(ScanCallKind::kJump, 7, 0, {"real"})});
}

TEST_CASE("script_scan: depth counts the blocks inside the label function") {
  const std::string source =
      "label(\"a\", function()\n"            // 1
      "  menu { \"x\", \"y\" }\n"            // 2
      "  if c == 1 then\n"                   // 3
      "    jump(\"b\")\n"                    // 4
      "  elseif c == 2 then\n"               // 5
      "    for i = 1, 2 do\n"                // 6
      "      call(\"c\")\n"                  // 7
      "    end\n"                            // 8
      "  else\n"                             // 9
      "    repeat\n"                         // 10
      "      jump(\"d\")\n"                  // 11
      "    until true\n"                     // 12
      "  end\n"                              // 13
      "  local f = function() ret() end\n"   // 14
      "  jump(\"e\")\n"                      // 15
      "end)\n"                               // 16
      "label(\"after\", function() end)\n";  // 17
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 2);
  const std::vector<ScanCall> expected{
      menu_call(2, {{.text = "x", .line = 2}, {.text = "y", .line = 2}}),
      call(ScanCallKind::kJump, 4, 1, {"b"}),
      call(ScanCallKind::kCall, 7, 2, {"c"}),
      call(ScanCallKind::kJump, 11, 2, {"d"}),
      call(ScanCallKind::kRet, 14, 1, {}),
      call(ScanCallKind::kJump, 15, 0, {"e"})};
  CHECK(labels[0].calls == expected);
  CHECK(labels[0].end_line == 16);
  CHECK(labels[1].name == "after");
  CHECK(labels[1].calls.empty());
}

TEST_CASE("script_scan: calls outside a label, and labels inside one, are left out") {
  const std::string source =
      "jump(\"top\")\n"
      "label(\"a\", function()\n"
      "  label(\"inner\", function() jump(\"x\") end)\n"
      "end)\n"
      "say(\"after\")\n"
      "label(\"b\", handler)\n";
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 2);
  CHECK(labels[0].name == "a");
  // The label that the function of `a` defines is not a label of the scan, but its calls are
  // in the function of `a`.
  CHECK(labels[0].calls == std::vector<ScanCall>{call(ScanCallKind::kJump, 3, 1, {"x"})});
  CHECK(labels[1].name == "b");
  CHECK(labels[1].calls.empty());
  CHECK(labels[1].end_line == 6);
}

TEST_CASE("script_scan: a label that is never closed ends at the last token") {
  const std::vector<ScanLabel> labels =
      vnmaki::scan_labels("label(\"a\", function()\n  jump(\"b\")\n");
  REQUIRE(labels.size() == 1);
  CHECK(labels[0].end_line == 2);
  CHECK(labels[0].calls == std::vector<ScanCall>{call(ScanCallKind::kJump, 2, 0, {"b"})});
}

TEST_CASE("script_scan: the scan of tokens is the scan of the source") {
  const std::string source = R"(label("a", function() jump("b") end, { ending = true }))";
  const std::vector<LuaToken> tokens = vnmaki::tokenize_lua(source);
  CHECK(vnmaki::scan_tokens(tokens) == vnmaki::scan_labels(source));
  CHECK(vnmaki::scan_tokens({}).empty());
}

namespace {

// Cases with the properties of fuzz/script_scan_properties.hpp: round trip through source, lines,
// and the shape of the labels.
using namespace std::string_view_literals;

constexpr std::array<std::string_view, 12> kPropertySamples{
    "",
    R"(label("a", function() jump("b") end))",
    "label('a', function()\r\n  say(\"x\\\n y\")\r\nend, { ending = true })",
    "[==[ ]] ]==] --[[ x ]] 'y' \"z\\065\\x41\\u{41}\\z   \n w\"",
    "x = 0x1p-3 + 1e+5 - .5 .. 3.. 7",
    R"(label("a", function() if x then menu {"a"} jump("b") end)",
    "label(label(\"x\", function() end), function() end)",
    "\xFF\xFE\x00\x01 [ [ ] ] [=[ ]=] -- \n"sv,
    "\"unterminated\nstring 'also\nand [[ long",
    "label(",
    "label(\"a\", function() jump(",
    "end end until until label(\"z\", function() end) end"};

constexpr std::array<std::string_view, 40> kFragments{"label(\"a\", ",
                                                      "function()",
                                                      " end",
                                                      " end)",
                                                      "jump(",
                                                      "call(",
                                                      "\"b\"",
                                                      "'c'",
                                                      ")",
                                                      "(",
                                                      "menu {",
                                                      "}",
                                                      " if x then ",
                                                      " else ",
                                                      "[[",
                                                      "]]",
                                                      "[==[",
                                                      "]==]",
                                                      "--",
                                                      "--[[",
                                                      "\n",
                                                      "\r\n",
                                                      "\\",
                                                      "\"",
                                                      ",",
                                                      "{ ending = true }",
                                                      "1e+",
                                                      "0x1p",
                                                      "\x80",
                                                      "ret()",
                                                      "a.b",
                                                      ".",
                                                      ":",
                                                      "x",
                                                      "=",
                                                      "7",
                                                      "-",
                                                      "+",
                                                      " ",
                                                      "show \"t\""};

constexpr std::uint64_t kSeed = 0x5CA7;
constexpr std::size_t kRandomCases = 3000;
constexpr std::size_t kMaxFragments = 40;

// A fixed generator (Knuth's MMIX constants), so a failure repeats on every platform; the
// standard distributions differ between libraries.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed) {}

  std::size_t below(std::size_t limit) {
    constexpr std::uint64_t kMultiplier = 6364136223846793005ULL;
    constexpr std::uint64_t kIncrement = 1442695040888963407ULL;
    constexpr unsigned kDiscardedLowBits = 33;  // the low bits of an LCG repeat quickly
    state_ = state_ * kMultiplier + kIncrement;
    return static_cast<std::size_t>((state_ >> kDiscardedLowBits) % limit);
  }

 private:
  std::uint64_t state_;
};

}  // namespace

TEST_CASE("script_scan: the properties of the fuzz target hold") {
  for (const std::string_view sample : kPropertySamples) {
    CHECK_NOTHROW(vnmaki::fuzz::check_script_scan_properties(sample));
  }
}

TEST_CASE("script_scan: the properties hold on random scripts") {
  Generator random{kSeed};
  for (std::size_t i = 0; i < kRandomCases; ++i) {
    std::string source;
    const std::size_t count = random.below(kMaxFragments);
    for (std::size_t j = 0; j < count; ++j) {
      source += kFragments.at(random.below(kFragments.size()));
    }
    vnmaki::fuzz::check_script_scan_properties(source);
  }
  CHECK(true);  // an abort in the loop fails the run
}

TEST_CASE("script_scan: asset calls and the keys of an option table") {
  const std::string source =
      "label('start', function()\n"                                               // 1
      "  scene('bg/a', { with = 'dissolve' })\n"                                  // 2
      "  show('x', { at = 'left', image = 'y', f = { a = 1 }, g = (b == 2) })\n"  // 3
      "  music 'm'\n"                                                             // 4
      "  sound('s') voice('v') movie('movies/m.webm') narrate('n')\n"             // 5
      "end)\n";
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 1);
  const std::vector<ScanCall>& calls = labels[0].calls;
  REQUIRE(calls.size() == 7);
  CHECK(calls[0] == ScanCall{.kind = ScanCallKind::kScene,
                             .line = 2,
                             .arguments = {"bg/a", std::nullopt},
                             .option_keys = {"with"}});
  CHECK(calls[1].kind == ScanCallKind::kShow);
  CHECK(calls[1].option_keys == std::vector<std::string>{"at", "image", "f", "g"});
  CHECK(calls[2] == call(ScanCallKind::kMusic, 4, 0, {"m"}));
  CHECK(calls[3] == call(ScanCallKind::kSound, 5, 0, {"s"}));
  CHECK(calls[4] == call(ScanCallKind::kVoice, 5, 0, {"v"}));
  CHECK(calls[5] == call(ScanCallKind::kMovie, 5, 0, {"movies/m.webm"}));
  CHECK(calls[6] == call(ScanCallKind::kNarrate, 5, 0, {"n"}));
}

TEST_CASE("script_scan: an ambient call is an asset call like music") {
  const std::string source =
      "label('start', function()\n"            // 1
      "  ambient('amb/rain', { fade = 2 })\n"  // 2
      "  ambient 'amb/wind'\n"                 // 3
      "  x.ambient('not/a/call')\n"            // 4
      "end)\n";
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 1);
  const std::vector<ScanCall>& calls = labels[0].calls;
  REQUIRE(calls.size() == 2);
  CHECK(calls[0] == ScanCall{.kind = ScanCallKind::kAmbient,
                             .line = 2,
                             .arguments = {"amb/rain", std::nullopt},
                             .option_keys = {"fade"}});
  CHECK(calls[1] == call(ScanCallKind::kAmbient, 3, 0, {"amb/wind"}));
}

TEST_CASE("script_scan: the speaker of say and the options of menu") {
  const std::string source =
      "label('start', function()\n"                               // 1
      "  say(eileen, 'Hi')\n"                                     // 2
      "  say(chars.bob, 'Yo') say('Zed', 'Hey') say(nil, 'x')\n"  // 3
      "  say(who(), 'y')\n"                                       // 4
      "  local c = menu { 'A', 'B', fixed = true,\n"              // 5
      "                   'C' .. x, [1] = 'k' }\n"                // 6
      "  menu({ 'D' })\n"                                         // 7
      "  menu(options)\n"                                         // 8
      "end)\n";
  const std::vector<ScanLabel> labels = vnmaki::scan_labels(source);
  REQUIRE(labels.size() == 1);
  const std::vector<ScanCall>& calls = labels[0].calls;
  REQUIRE(calls.size() == 8);
  CHECK(calls[0].speaker == "eileen");
  CHECK(calls[1].speaker == "chars.bob");
  CHECK(calls[2].speaker == "Zed");
  CHECK(calls[3].speaker.empty());
  CHECK(calls[4].speaker.empty());
  CHECK(calls[5].table == Elements{{.text = "A", .line = 5},
                                   {.text = "B", .line = 5},
                                   {.text = std::nullopt, .line = 6}});
  CHECK(calls[6].table == Elements{{.text = "D", .line = 7}});
  CHECK_FALSE(calls[7].table.has_value());
}

TEST_CASE("script_scan: the texts of screens") {
  const std::string source =
      "screen 'quick' {\n"                                                         // 1
      "  hbox { button { 'Save', on_click = f }, text { name, style = 's' } },\n"  // 2
      "  image { 'pic.png' }, x.text { 'no' }\n"                                   // 3
      "}\n"                                                                        // 4
      "screen('title', function(args)\n"                                           // 5
      "  local text = 1\n"                                                         // 6
      "  return vbox { text { 'Hello' }, text { args.title or '' } }\n"            // 7
      "end)\n"                                                                     // 8
      "screen(name, { button { 'X' } })\n"                                         // 9
      "label('l', function() say(nil, 'a') end)\n";
  const std::vector<vnmaki::ScanScreen> screens = vnmaki::scan_screens(source);
  REQUIRE(screens.size() == 3);
  CHECK(screens[0].name == "quick");
  CHECK(screens[0].line == 1);
  CHECK(screens[0].texts ==
        Elements{{.text = "Save", .line = 2}, {.text = std::nullopt, .line = 2}});
  CHECK(screens[1].name == "title");
  CHECK(screens[1].texts ==
        Elements{{.text = "Hello", .line = 7}, {.text = std::nullopt, .line = 7}});
  CHECK_FALSE(screens[2].name.has_value());
  CHECK(screens[2].texts == Elements{{.text = "X", .line = 9}});
}
