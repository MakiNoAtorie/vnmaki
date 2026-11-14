// String extraction and the update of `.po` files (engine/l10n/extract.hpp). The expected keys
// come from FNV-1a values worked out in Python, not from the code under test.

#include "engine/l10n/extract.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "engine/l10n/po.hpp"
#include "engine/tools/story_graph.hpp"

namespace {

using vnmaki::PoEntry;
using vnmaki::PoFile;
using vnmaki::ScriptSource;

PoFile extract(const std::string& script) {
  const std::vector<ScriptSource> scripts{{"scripts/main.lua", script}};
  return vnmaki::extract_template(scripts);
}

// The entries of the file without its header.
std::vector<PoEntry> messages(const PoFile& file) {
  return {file.entries.begin() + 1, file.entries.end()};
}

PoEntry message(std::string context, std::string id, std::string str) {
  PoEntry result;
  result.context = std::move(context);
  result.id = std::move(id);
  result.str = std::move(str);
  return result;
}

const PoEntry* find(const PoFile& file, std::string_view id) {
  for (const PoEntry& entry : file.entries) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("extract: a key is the scope and the FNV-1a hash of the speaker and the text") {
  CHECK(vnmaki::translation_key("start", "", "") == "start/050c5d1f");
  CHECK(vnmaki::translation_key("start", "eileen", "You came back.") == "start/861e044d");
  CHECK(vnmaki::translation_key("screen/title", "", "Hello") == "screen/title/18fbf6c5");
}

TEST_CASE("extract: the template lists say, narrate, menu and screen texts") {
  const PoFile file = extract(
      "screen 'title' { button { 'Start' }, text { args.name } }\n"     // 1
      "label('start', function()\n"                                     // 2
      "  say(eileen, 'You came back.')\n"                               // 3
      "  say(nil, 'Hello')\n"                                           // 4
      "  narrate 'Night.'\n"                                            // 5
      "  local c = menu { 'Yes', 'No', fixed = true, 'Maybe' .. x }\n"  // 6
      "  say(eileen, 'You came back.')\n"                               // 7
      "  say(eileen, text)\n"                                           // 8
      "  say(eileen, '')\n"                                             // 9
      "end)\n");
  REQUIRE(file.entries.size() == 7);
  const PoEntry& header = file.entries[0];
  CHECK(header.context.empty());
  CHECK(header.id.empty());
  CHECK(header.str == "Content-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n");

  const std::vector<PoEntry> entries = messages(file);
  CHECK(entries[0].context == "screen/title/" + vnmaki::translation_key("", "", "Start").substr(1));
  CHECK(entries[0].id == "Start");
  CHECK(entries[0].references == std::vector<std::string>{"scripts/main.lua:1"});
  CHECK(entries[1].context == "start/861e044d");
  CHECK(entries[1].id == "You came back.");
  CHECK(entries[1].extracted == std::vector<std::string>{"speaker: eileen"});
  CHECK(entries[1].references ==
        std::vector<std::string>{"scripts/main.lua:3", "scripts/main.lua:7"});
  CHECK(entries[2].id == "Hello");
  CHECK(entries[2].extracted.empty());
  CHECK(entries[3].id == "Night.");
  CHECK(entries[4].id == "Yes");
  CHECK(entries[5].id == "No");
  for (const PoEntry& entry : entries) {
    CHECK(entry.str.empty());
  }
}

TEST_CASE("extract: a speaker or a label makes another key") {
  const PoFile file = extract(
      "label('a', function() say('X', 'Hi') say('Y', 'Hi') end)\n"
      "label('b', function() say('X', 'Hi') end)\n");
  const std::vector<PoEntry> entries = messages(file);
  REQUIRE(entries.size() == 3);
  CHECK(entries[0].context != entries[1].context);
  CHECK(entries[0].context.starts_with("a/"));
  CHECK(entries[2].context.starts_with("b/"));
  CHECK(entries[0].context.substr(2) == entries[2].context.substr(2));
}

TEST_CASE("extract: scripts come in the order given, and a note is one line") {
  const std::vector<ScriptSource> scripts{
      {"scripts/a.lua", "label('l', function() say('A\\nB', 'x') end)"},
      {"scripts/b.lua", "label('m', function() narrate('y') end)"}};
  const PoFile file = vnmaki::extract_template(scripts);
  REQUIRE(file.entries.size() == 3);
  CHECK(file.entries[1].extracted == std::vector<std::string>{"speaker: A B"});
  CHECK(file.entries[2].references == std::vector<std::string>{"scripts/b.lua:1"});
}

TEST_CASE("extract: a new language has the template's entries and a header") {
  const PoFile tmpl = extract("label('start', function() say(nil, 'Hi') end)");
  const PoFile file = vnmaki::update_translation(tmpl, {}, "ja");
  REQUIRE(file.entries.size() == 2);
  CHECK(file.entries[0].str.ends_with("Language: ja\n"));
  CHECK(file.entries[1] == tmpl.entries[1]);
}

TEST_CASE("extract: an update keeps translations, notes and flags, and the new references") {
  const PoFile before = extract("label('start', function()\n  say(nil, 'Hi')\nend)");
  PoFile old = vnmaki::update_translation(before, {}, "ja");
  old.entries[1].str = "Konnichiwa";
  old.entries[1].comments = {"keep me"};
  old.entries[1].flags = {"fuzzy", "no-wrap"};
  old.entries[0].str += "Plural-Forms: nplurals=1;\n";
  const PoFile after = extract("label('start', function()\n\n\n  say(nil, 'Hi')\nend)");
  const PoFile updated = vnmaki::update_translation(after, old, "ja");
  REQUIRE(updated.entries.size() == 2);
  CHECK(updated.entries[0] == old.entries[0]);
  CHECK(updated.entries[1].str == "Konnichiwa");
  CHECK(updated.entries[1].comments == std::vector<std::string>{"keep me"});
  CHECK(updated.entries[1].flags == std::vector<std::string>{"fuzzy", "no-wrap"});
  CHECK(updated.entries[1].references == std::vector<std::string>{"scripts/main.lua:4"});
}

TEST_CASE("extract: a changed text at the same position moves its translation as fuzzy") {
  const PoFile before = extract(
      "label('start', function()\n  say(nil, 'One')\n  say(nil, 'Two')\n  say(nil, 'Three')\nend)");
  PoFile old = vnmaki::update_translation(before, {}, "ja");
  old.entries[1].str = "Ichi";
  old.entries[2].str = "Ni";
  old.entries[3].str = "San";
  const PoFile after = extract(
      "label('start', function()\n  say(nil, 'One')\n  say(nil, 'Twos')\n"
      "  say(nil, 'Three')\nend)");
  const PoFile updated = vnmaki::update_translation(after, old, "ja");
  REQUIRE(updated.entries.size() == 4);  // header, three entries, no obsolete one
  CHECK(updated.entries[1].str == "Ichi");
  CHECK_FALSE(updated.entries[1].fuzzy());
  CHECK(updated.entries[2].id == "Twos");
  CHECK(updated.entries[2].str == "Ni");
  CHECK(updated.entries[2].fuzzy());
  CHECK(updated.entries[2].context == vnmaki::translation_key("start", "", "Twos"));
  CHECK(updated.entries[3].str == "San");
  CHECK_FALSE(updated.entries[3].fuzzy());
}

TEST_CASE("extract: a removed text becomes obsolete, and comes back when it is added again") {
  const PoFile two = extract("label('a', function() say(nil, 'One') say(nil, 'Two') end)");
  PoFile old = vnmaki::update_translation(two, {}, "ja");
  old.entries[1].str = "Ichi";
  old.entries[2].str = "Ni";
  // 'Two' goes away. The position 1 of the label has no new key, so nothing is fuzzy.
  const PoFile one = extract("label('a', function() say(nil, 'One') end)");
  const PoFile shorter = vnmaki::update_translation(one, old, "ja");
  REQUIRE(shorter.entries.size() == 3);
  CHECK(shorter.entries[1].str == "Ichi");
  CHECK(shorter.entries[2].id == "Two");
  CHECK(shorter.entries[2].str == "Ni");
  CHECK(shorter.entries[2].obsolete);
  // A new text in another label does not take the translation of 'Two'.
  const PoFile other = extract(
      "label('a', function() say(nil, 'One') end)\nlabel('b', function() say(nil, 'New') end)");
  const PoFile moved = vnmaki::update_translation(other, old, "ja");
  REQUIRE(moved.entries.size() == 4);
  CHECK(moved.entries[2].id == "New");
  CHECK(moved.entries[2].str.empty());
  CHECK(moved.entries[3].obsolete);
  // 'Two' again: the translation returns, not fuzzy, and the obsolete entry is gone.
  const PoFile back = vnmaki::update_translation(two, shorter, "ja");
  CHECK(back.entries == old.entries);
}

TEST_CASE("extract: an entry without a translation is dropped, not made obsolete") {
  const PoFile two = extract("label('a', function() say(nil, 'One') say(nil, 'Two') end)");
  const PoFile old = vnmaki::update_translation(two, {}, "ja");
  const PoFile one = extract("label('a', function() say(nil, 'One') end)");
  CHECK(vnmaki::update_translation(one, old, "ja").entries.size() == 2);
}

TEST_CASE("extract: several changed texts of one label each move to their own position") {
  const PoFile before = extract("label('a', function() say(nil, 'One') say(nil, 'Two') end)");
  PoFile old = vnmaki::update_translation(before, {}, "ja");
  old.entries[1].str = "Ichi";
  old.entries[2].str = "Ni";
  // Both texts change: both move, each to its own position.
  const PoFile after = extract("label('a', function() say(nil, 'Uno') say(nil, 'Dos') end)");
  const PoFile updated = vnmaki::update_translation(after, old, "ja");
  REQUIRE(updated.entries.size() == 3);
  CHECK(find(updated, "Uno")->str == "Ichi");
  CHECK(find(updated, "Dos")->str == "Ni");
  CHECK(find(updated, "Uno")->fuzzy());
  CHECK(find(updated, "Dos")->fuzzy());
}

TEST_CASE("extract: updating twice gives the same file") {
  const PoFile before = extract("label('a', function() say(nil, 'One') say(nil, 'Two') end)");
  PoFile old = vnmaki::update_translation(before, {}, "ja");
  old.entries[1].str = "Ichi";
  old.entries[2].str = "Ni";
  const PoFile after = extract("label('a', function() say(nil, 'Uno') narrate('Three') end)");
  const PoFile once = vnmaki::update_translation(after, old, "ja");
  const PoFile twice = vnmaki::update_translation(after, once, "ja");
  CHECK(twice == once);
  CHECK(vnmaki::parse_po(vnmaki::write_po(once)) == once);
}
