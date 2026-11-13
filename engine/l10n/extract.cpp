#include "engine/l10n/extract.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/tools/script_scan.hpp"

namespace vnmaki {

namespace {

constexpr std::uint32_t kFnvOffset = 2166136261U;
constexpr std::uint32_t kFnvPrime = 16777619U;
constexpr std::size_t kHashDigits = 8;
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::uint32_t kNibbleMask = 0xF;
constexpr std::uint32_t kNibbleBits = 4;
constexpr std::size_t kSayTextArgument = 1;  // say(who, text)
constexpr std::string_view kHeaderText =
    "Content-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n";

using Key = std::pair<std::string, std::string>;  // context, source text

Key key_of(const PoEntry& entry) { return {entry.context, entry.id}; }

bool is_header(const PoEntry& entry) { return entry.context.empty() && entry.id.empty(); }

std::string scope_of(const std::string& context) { return context.substr(0, context.rfind('/')); }

std::uint32_t fnv1a(std::uint32_t hash, std::string_view bytes) {
  for (const char c : bytes) {
    hash = (hash ^ static_cast<unsigned char>(c)) * kFnvPrime;
  }
  return hash;
}

// A note is one line.
std::string one_line(std::string text) {
  std::ranges::replace_if(text, [](char c) { return c == '\n' || c == '\r'; }, ' ');
  return text;
}

struct Item {
  std::string scope{};
  std::string speaker{};
  std::string text{};
  std::uint32_t line = 0;
};

void add_label_items(const ScanLabel& label, std::vector<Item>& items) {
  const auto add = [&](const std::string& speaker, const ScanElement& value) {
    if (value.text && !value.text->empty()) {
      items.push_back({*label.name, speaker, *value.text, value.line});
    }
  };
  for (const ScanCall& call : label.calls) {
    if (call.kind == ScanCallKind::kSay && call.arguments.size() > kSayTextArgument) {
      add(call.speaker, {call.arguments[kSayTextArgument], call.line});
    } else if (call.kind == ScanCallKind::kNarrate && !call.arguments.empty()) {
      add({}, {call.arguments.front(), call.line});
    } else if (call.kind == ScanCallKind::kMenu && call.table) {
      for (const ScanElement& option : *call.table) {
        add({}, option);
      }
    }
  }
}

std::vector<Item> script_items(const ScriptSource& script) {
  std::vector<Item> items;
  for (const ScanLabel& label : scan_labels(script.text)) {
    if (label.name) {
      add_label_items(label, items);
    }
  }
  for (const ScanScreen& screen : scan_screens(script.text)) {
    for (const ScanElement& value : screen.texts) {
      if (screen.name && value.text && !value.text->empty()) {
        items.push_back({"screen/" + *screen.name, {}, *value.text, value.line});
      }
    }
  }
  std::ranges::stable_sort(items,
                           [](const Item& a, const Item& b) { return a.line < b.line; });
  return items;
}

}  // namespace

std::string translation_key(std::string_view scope, std::string_view speaker,
                            std::string_view text) {
  std::uint32_t hash = fnv1a(kFnvOffset, speaker);
  hash = fnv1a(hash, std::string_view{"\0", 1});
  hash = fnv1a(hash, text);
  std::string digits(kHashDigits, '0');
  for (std::size_t i = kHashDigits; i-- > 0;) {
    digits[i] = kHexDigits[hash & kNibbleMask];
    hash >>= kNibbleBits;
  }
  return std::string{scope} + "/" + digits;
}

std::string translation_path(std::string_view language) {
  return std::string{kTranslationsDir} + "/" + std::string{language} + ".po";
}

PoEntry po_header(std::string_view language) {
  PoEntry header;
  header.str = std::string{kHeaderText};
  if (!language.empty()) {
    header.str += "Language: " + std::string{language} + "\n";
  }
  return header;
}

PoFile extract_template(std::span<const ScriptSource> scripts) {
  PoFile file;
  file.entries.push_back(po_header({}));
  std::map<Key, std::size_t> index;
  for (const ScriptSource& script : scripts) {
    for (const Item& item : script_items(script)) {
      PoEntry entry;
      entry.context = translation_key(item.scope, item.speaker, item.text);
      entry.id = item.text;
      const std::string reference = script.path + ":" + std::to_string(item.line);
      const auto [found, added] = index.emplace(key_of(entry), file.entries.size());
      if (added) {
        if (!item.speaker.empty()) {
          entry.extracted.push_back("speaker: " + one_line(item.speaker));
        }
        entry.references.push_back(reference);
        file.entries.push_back(std::move(entry));
      } else {
        file.entries[found->second].references.push_back(reference);
      }
    }
  }
  return file;
}

PoFile update_translation(const PoFile& tmpl, const PoFile& old, std::string_view language) {
  // What `old` has for a key, an active entry before an obsolete one.
  std::map<Key, const PoEntry*> known;
  std::map<std::string, std::vector<const PoEntry*>> old_in_scope;  // active, in file order
  const PoEntry* old_header = nullptr;
  for (const PoEntry& entry : old.entries) {
    if (is_header(entry)) {
      old_header = old_header != nullptr ? old_header : &entry;
      continue;
    }
    const auto [slot, added] = known.emplace(key_of(entry), &entry);
    if (!added && slot->second->obsolete && !entry.obsolete) {
      slot->second = &entry;
    }
    if (!entry.obsolete) {
      old_in_scope[scope_of(entry.context)].push_back(&entry);
    }
  }
  std::set<Key> wanted;
  std::map<std::string, std::vector<const PoEntry*>> new_in_scope;
  for (const PoEntry& entry : tmpl.entries) {
    if (!is_header(entry)) {
      wanted.insert(key_of(entry));
      new_in_scope[scope_of(entry.context)].push_back(&entry);
    }
  }

  // An old entry that goes away, and the new key at its position that has no translation yet.
  std::map<Key, const PoEntry*> moved;  // new key -> old entry
  std::set<const PoEntry*> moved_from;
  for (const auto& [scope, fresh] : new_in_scope) {
    const auto previous = old_in_scope.find(scope);
    if (previous == old_in_scope.end()) {
      continue;
    }
    const std::size_t count = std::min(fresh.size(), previous->second.size());
    for (std::size_t i = 0; i < count; ++i) {
      const PoEntry& gone = *previous->second[i];
      if (!wanted.contains(key_of(gone)) && !known.contains(key_of(*fresh[i])) &&
          !gone.str.empty()) {
        moved.emplace(key_of(*fresh[i]), &gone);
        moved_from.insert(&gone);
      }
    }
  }

  PoFile out;
  out.entries.push_back(old_header != nullptr ? *old_header : po_header(language));
  for (const PoEntry& entry : tmpl.entries) {
    if (is_header(entry)) {
      continue;
    }
    PoEntry& added = out.entries.emplace_back(entry);
    const auto same = known.find(key_of(entry));
    const auto shifted = moved.find(key_of(entry));
    const PoEntry* source = same != known.end() ? same->second : nullptr;
    if (source == nullptr && shifted != moved.end()) {
      source = shifted->second;
    }
    if (source != nullptr) {
      added.comments = source->comments;
      added.flags = source->flags;
      added.str = source->str;
      added.set_fuzzy(source->fuzzy() || shifted != moved.end());
    }
  }
  for (const PoEntry& entry : old.entries) {
    if (!is_header(entry) && !wanted.contains(key_of(entry)) && !moved_from.contains(&entry) &&
        !entry.str.empty()) {
      out.entries.push_back(entry);
      out.entries.back().obsolete = true;
    }
  }
  return out;
}

}  // namespace vnmaki
