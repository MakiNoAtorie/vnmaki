#include "engine/tools/check.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "engine/audio/audio.hpp"
#include "engine/l10n/extract.hpp"
#include "engine/l10n/po.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/scene/scene.hpp"
#include "engine/text/markup.hpp"
#include "engine/tools/script_scan.hpp"
#include "engine/tools/story_graph.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kCsvHeader = "file,line,severity,code,message\n";
constexpr std::size_t kSayTextArgument = 1;  // say(who, text)

Finding make_finding(std::string_view file, std::uint32_t line, Severity severity,
                     std::string_view code, std::string message) {
  return Finding{.file = std::string{file},
                 .line = line,
                 .severity = severity,
                 .code = std::string{code},
                 .message = std::move(message)};
}

std::string in_quotes(std::string_view text) { return "'" + std::string{text} + "'"; }

// Digits and a ':' at the start of `text`: the number, and `text` moves past both.
std::optional<std::uint32_t> take_number(std::string_view& text) {
  std::uint32_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  const auto length = static_cast<std::size_t>(end - text.data());
  if (error != std::errc{} || length >= text.size() || text[length] != ':') {
    return std::nullopt;
  }
  text.remove_prefix(length + 1);
  return value;
}

// Lua and ProjectConfigError messages are "<file>:<line>: <reason>"; a config message can also
// have a column, and has no line when the problem is not at a place in the file.
void add_located_finding(std::vector<Finding>& findings, std::string_view file,
                         Severity severity, std::string_view code, std::string_view what) {
  std::uint32_t line = 0;
  const std::size_t colon = what.find(':');
  if (colon != std::string_view::npos) {
    what.remove_prefix(colon + 1);
    line = take_number(what).value_or(0);
    static_cast<void>(take_number(what));
    while (what.starts_with(' ')) {
      what.remove_prefix(1);
    }
  }
  findings.push_back(make_finding(file, line, severity, code, std::string{what}));
}

// `sol::state` only loads the script here: nothing runs.
void add_syntax_finding(std::vector<Finding>& findings, const ScriptSource& script) {
  sol::state lua;
  const sol::load_result loaded = lua.load(script.text, "=file", sol::load_mode::text);
  if (!loaded.valid()) {
    add_located_finding(findings, script.path, Severity::kError, "lua-syntax",
                        loaded.get<sol::error>().what());
  }
}

void add_text_finding(std::vector<Finding>& findings, const ScriptSource& script,
                      const ScanCall& call, const std::string& text) {
  try {
    static_cast<void>(parse_markup(text));
  } catch (const MarkupError& error) {
    findings.push_back(make_finding(script.path, call.line, Severity::kError, "bad-text-tag",
                                    std::string{error.what()} + " (byte " +
                                        std::to_string(error.offset()) + " of the text)"));
  }
}

// The names that `layered(name, ...)` and `animation(name, ...)` define: `show(name)` needs no
// image file for them.
std::set<std::string> defined_images(std::span<const ScriptSource> scripts) {
  std::set<std::string> names;
  for (const ScriptSource& script : scripts) {
    const std::vector<LuaToken> tokens = tokenize_lua(script.text);
    for (std::size_t i = 0; i + 2 < tokens.size(); ++i) {
      const bool definer = tokens[i].kind == LuaTokenKind::kName &&
                           (tokens[i].text == "layered" || tokens[i].text == "animation");
      const bool member = i > 0 && tokens[i - 1].kind == LuaTokenKind::kSymbol &&
                          (tokens[i - 1].text == "." || tokens[i - 1].text == ":");
      if (definer && !member && tokens[i + 1].text == "(" &&
          tokens[i + 2].kind == LuaTokenKind::kString) {
        names.insert(tokens[i + 2].text);
      }
    }
  }
  return names;
}

// ponytail: `show(tag, { image = ... })` and `movie = ...` are not checked (the value is inside a
// table that the scan does not read); add the table values to ScanCall when that matters.
bool show_names_a_file(const ScanCall& call, const std::set<std::string>& defined) {
  const auto is_option = [&call](std::string_view key) {
    return std::ranges::find(call.option_keys, key) != call.option_keys.end();
  };
  return !is_option("image") && !is_option("movie") && !is_option("displayable") &&
         !defined.contains(*call.arguments.front());
}

void add_asset_finding(std::vector<Finding>& findings, const Vfs& vfs, const ScriptSource& script,
                       const ScanCall& call, const std::set<std::string>& defined) {
  if (call.arguments.empty() || !call.arguments.front()) {
    return;
  }
  const std::string& name = *call.arguments.front();
  const auto missing = [&](std::string_view code, std::string_view kind) {
    findings.push_back(make_finding(script.path, call.line, Severity::kError, code,
                                    std::string{kind} + " " + in_quotes(name) + " not found"));
  };
  switch (call.kind) {
    case ScanCallKind::kScene:
      if (!resolve_image(vfs, name)) {
        missing("missing-image", "image");
      }
      break;
    case ScanCallKind::kShow:
      if (show_names_a_file(call, defined) && !resolve_image(vfs, name)) {
        missing("missing-image", "image");
      }
      break;
    case ScanCallKind::kMusic:
    case ScanCallKind::kSound:
    case ScanCallKind::kAmbient:
      if (!resolve_audio(vfs, name)) {
        missing("missing-audio", "audio");
      }
      break;
    case ScanCallKind::kVoice:
      if (!resolve_audio(vfs, std::string{kVoicePrefix} + name)) {
        missing("missing-audio", "voice");
      }
      break;
    case ScanCallKind::kMovie:
      if (!vfs.exists(name)) {
        missing("missing-movie", "movie");
      }
      break;
    default:
      break;
  }
}

void add_not_literal(std::vector<Finding>& findings, const ScriptSource& script,
                     std::uint32_t line, std::string_view what) {
  findings.push_back(make_finding(script.path, line, Severity::kWarning, "non-literal-string",
                                  std::string{what} + " is not a string literal, so it is not "
                                                      "extracted for translation"));
}

// `say` and `narrate`: the text must be a literal (it is also checked for tags); `menu`: the
// options must be literals in a table written in the call.
void add_string_findings(std::vector<Finding>& findings, const ScriptSource& script,
                         const ScanCall& call) {
  const bool say = call.kind == ScanCallKind::kSay;
  if (say || call.kind == ScanCallKind::kNarrate) {
    const std::size_t index = say ? kSayTextArgument : 0;
    if (call.arguments.size() <= index) {
      return;
    }
    if (call.arguments[index]) {
      add_text_finding(findings, script, call, *call.arguments[index]);
    } else {
      add_not_literal(findings, script, call.line,
                      say ? "the text of say" : "the text of narrate");
    }
  } else if (!call.table) {
    add_not_literal(findings, script, call.line, "the table of menu options");
  } else {
    for (const ScanElement& option : *call.table) {
      if (!option.text) {
        add_not_literal(findings, script, option.line, "a menu option");
      }
    }
  }
}

void add_call_findings(std::vector<Finding>& findings, const Vfs& vfs, const ScriptSource& script,
                       const std::set<std::string>& defined) {
  for (const ScanLabel& label : scan_labels(script.text)) {
    for (const ScanCall& call : label.calls) {
      if (call.kind == ScanCallKind::kSay || call.kind == ScanCallKind::kNarrate ||
          call.kind == ScanCallKind::kMenu) {
        add_string_findings(findings, script, call);
      } else {
        add_asset_finding(findings, vfs, script, call, defined);
      }
    }
  }
  for (const ScanScreen& screen : scan_screens(script.text)) {
    for (const ScanElement& value : screen.texts) {
      if (!value.text) {
        add_not_literal(findings, script, value.line,
                        "a text of screen " + in_quotes(screen.name.value_or("?")));
      }
    }
  }
}

// The translations that are in use (not fuzzy, not obsolete) must have tags that parse. A screen
// string has no tags: only the say box reads them.
void add_translation_findings(std::vector<Finding>& findings, const Vfs& vfs) {
  for (const VfsEntry& file : vfs.list(kTranslationsDir)) {
    if (!file.name.ends_with(".po") || file.is_directory) {
      continue;
    }
    const std::string path = std::string{kTranslationsDir} + "/" + file.name;
    const std::optional<std::vector<std::byte>> bytes = vfs.read(path);
    if (!bytes) {
      continue;
    }
    PoFile po;
    try {
      po = parse_po(std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    } catch (const PoError& error) {
      findings.push_back(
          make_finding(path, error.line(), Severity::kError, "po-syntax", error.what()));
      continue;
    }
    for (const PoEntry& entry : po.entries) {
      if (entry.obsolete || entry.fuzzy() || entry.str.empty() ||
          entry.context.starts_with("screen/")) {
        continue;
      }
      try {
        static_cast<void>(parse_markup(entry.str));
      } catch (const MarkupError& error) {
        findings.push_back(make_finding(
            path, entry.line, Severity::kError, "bad-translation-tag",
            "translation of " + in_quotes(entry.context) + ": " + error.what() + " (byte " +
                std::to_string(error.offset()) + " of the text)"));
      }
    }
  }
}

void add_graph_findings(std::vector<Finding>& findings, const StoryGraph& graph) {
  std::set<std::string_view> names;
  for (const GraphLabel& label : graph.labels) {
    names.insert(label.name);
    if (!label.reachable) {
      findings.push_back(make_finding(label.script, label.line, Severity::kWarning,
                                      "unreachable-label",
                                      "no path from 'start' reaches label " + in_quotes(label.name)));
    }
    if (label.dead_end) {
      findings.push_back(make_finding(label.script, label.line, Severity::kWarning, "dead-end",
                                      "label " + in_quotes(label.name) +
                                          " ends with no jump or ret (mark it { ending = true } "
                                          "if it ends the story)"));
    }
  }
  for (const GraphEdge& edge : graph.edges) {
    const std::string where =
        std::string{edge_kind_name(edge.kind)} + " from label " + in_quotes(edge.from);
    if (edge.dynamic) {
      findings.push_back(make_finding(edge.script, edge.line, Severity::kWarning, "dynamic-edge",
                                      where + " has a computed target"));
    } else if (!names.contains(edge.to)) {
      findings.push_back(make_finding(edge.script, edge.line, Severity::kError, "unknown-label",
                                      where + " goes to unknown label " + in_quotes(edge.to)));
    }
  }
}

}  // namespace

std::vector<Finding> check_project(const Vfs& vfs) {
  std::vector<Finding> findings;
  try {
    static_cast<void>(load_project_config(vfs));
  } catch (const ProjectConfigError& error) {
    add_located_finding(findings, kProjectConfigPath, Severity::kWarning, "project-config",
                        error.what());
  }
  const std::vector<ScriptSource> scripts = read_scripts(vfs);
  const std::set<std::string> defined = defined_images(scripts);
  for (const ScriptSource& script : scripts) {
    add_syntax_finding(findings, script);
    add_call_findings(findings, vfs, script, defined);
  }
  add_graph_findings(findings, build_story_graph(scripts));
  add_translation_findings(findings, vfs);
  std::ranges::stable_sort(findings, [](const Finding& a, const Finding& b) {
    return std::pair{std::string_view{a.file}, a.line} <
           std::pair{std::string_view{b.file}, b.line};
  });
  return findings;
}

bool has_errors(std::span<const Finding> findings) {
  return std::ranges::any_of(findings,
                             [](const Finding& f) { return f.severity == Severity::kError; });
}

std::string_view severity_name(Severity severity) {
  return severity == Severity::kError ? "error" : "warning";
}

std::string findings_to_csv(std::span<const Finding> findings) {
  std::string csv{kCsvHeader};
  for (const Finding& f : findings) {
    csv += csv_field(f.file) + "," + (f.line == 0 ? "" : std::to_string(f.line)) + "," +
           std::string{severity_name(f.severity)} + "," + csv_field(f.code) + "," +
           csv_field(f.message) + "\n";
  }
  return csv;
}

}  // namespace vnmaki
