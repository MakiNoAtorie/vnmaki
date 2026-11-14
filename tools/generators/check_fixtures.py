#!/usr/bin/env python3
"""Generate the projects and the expected output of the `vnmaki check` CLI tests.

Usage: check_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.

Each case is a small project (<case>/) and the CSV that `vnmaki check` must print (<case>.csv).
There is one case for each finding code, and `mixed`, which has findings of several codes in two
scripts (CSV quoting, order by file and line). The expected rows
are written here, with the line of each finding found by searching the script for a marker text,
so the expectation does not come from the engine's tokenizer. The messages are the ones that
engine/tools/check.cpp, the markup parser, Lua and toml++ give.
"""

import shutil
import sys
from pathlib import Path

HEADER = "file,line,severity,code,message\n"
MAIN = "scripts/main.lua"
OK_TITLE = 'title = "Check fixture"\n'
DUMMY_ASSET = b"not a real file: `vnmaki check` only looks that it exists\n"


def csv_field(text):
    if any(c in text for c in ',"\r\n'):
        return '"' + text.replace('"', '""') + '"'
    return text


class Case:
    def __init__(self, name, scripts, project=OK_TITLE, assets=(), expected=(), files=None):
        self.name = name
        self.scripts = scripts  # {path: text}
        self.files = files or {}  # {path: text} of other files, e.g. translations/ja.po
        self.project = project
        self.assets = assets  # paths of files that exist
        self.expected = list(expected)  # (file, marker or line, severity, code, message)

    def line_of(self, path, marker):
        if isinstance(marker, int):
            return marker
        for number, line in enumerate({**self.files, **self.scripts}[path].split("\n"), 1):
            if marker in line:
                return number
        raise SystemExit(f"{self.name}: no line of {path} has {marker!r}")

    def csv(self):
        located = [(path, 0 if marker == 0 else self.line_of(path, marker), severity, code, message)
                   for path, marker, severity, code, message in self.expected]
        located.sort(key=lambda row: row[:2])  # stable: by file, then line
        rows = [HEADER]
        for path, line, severity, code, message in located:
            rows.append(",".join([csv_field(path), str(line) if line else "", severity, code,
                                  csv_field(message)]) + "\n")
        return "".join(rows)

    def write(self, out):
        root = out / self.name
        (root / "scripts").mkdir(parents=True)
        (root / "project.toml").write_bytes(self.project.encode("utf-8"))
        for path, text in self.scripts.items():
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(text.encode("utf-8"))
        for path, text in self.files.items():
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(text.encode("utf-8"))
        for path in self.assets:
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(DUMMY_ASSET)
        (out / f"{self.name}.csv").write_bytes(self.csv().encode("utf-8"))


def script(*lines):
    return "\n".join(lines) + "\n"


def start(*body):
    """A script whose one label `start` runs `body` and ends the story on purpose."""
    return script('label("start", function()', *(f"  {line}" for line in body),
                  "end, { ending = true })")


def cases():
    yield Case("lua_syntax", {MAIN: start("local x = = 1")},
               expected=[(MAIN, "local x", "error", "lua-syntax",
                          "unexpected symbol near '='")])

    yield Case("unknown_label", {MAIN: script(
        'label("start", function()', '  jump("nowhere")', "end)")},
               expected=[(MAIN, "nowhere", "error", "unknown-label",
                          "jump from label 'start' goes to unknown label 'nowhere'")])

    yield Case("bad_text_tag", {MAIN: start(
        'say("Eileen", "Fine {b}bold{/b} and {{literal}} text.")',
        'say("Eileen", "Hi {wobble}there")',
        'narrate("{b}never closed")')},
               expected=[(MAIN, "wobble", "error", "bad-text-tag",
                          "unknown tag 'wobble' (byte 3 of the text)"),
                         (MAIN, "never closed", "error", "bad-text-tag",
                          "{b} is not closed (byte 0 of the text)")])

    yield Case("missing_image", {MAIN: script(
        'layered("hero", { base = "hero/base" })',
        'animation("blink", { frames = { "hero/base" }, fps = 4 })',
        start('scene("bg/ok")',
              'scene("bg/nowhere")',
              'show("ghost")',
              'show("hero")',
              'show("blink", { at = "left" })',
              'show("ghost2", { image = "bg/ok" })',
              'show("ghost3", { at = "right" })'))},
               assets=["images/bg/ok.png"],
               expected=[(MAIN, "bg/nowhere", "error", "missing-image",
                          "image 'bg/nowhere' not found"),
                         (MAIN, '"ghost")', "error", "missing-image", "image 'ghost' not found"),
                         (MAIN, "ghost3", "error", "missing-image", "image 'ghost3' not found")])

    yield Case("missing_audio", {MAIN: start('music("bgm/ok")',
                                             'music("bgm/nowhere", { fade = 1 })',
                                             'sound("sfx/nowhere")',
                                             'sound "sfx/ok"',
                                             'ambient("amb/ok")',
                                             'ambient("amb/nowhere", { fade = 1 })',
                                             'voice("eileen/nowhere")',
                                             'voice("eileen/ok")')},
               assets=["audio/bgm/ok.ogg", "audio/sfx/ok.wav", "audio/amb/ok.flac",
                       "audio/voice/eileen/ok.opus"],
               expected=[(MAIN, "bgm/nowhere", "error", "missing-audio",
                          "audio 'bgm/nowhere' not found"),
                         (MAIN, "sfx/nowhere", "error", "missing-audio",
                          "audio 'sfx/nowhere' not found"),
                         (MAIN, "amb/nowhere", "error", "missing-audio",
                          "audio 'amb/nowhere' not found"),
                         (MAIN, "eileen/nowhere", "error", "missing-audio",
                          "voice 'eileen/nowhere' not found")])

    yield Case("missing_movie", {MAIN: start('movie("movies/ok.webm")',
                                             'movie("movies/nowhere.webm", { skippable = true })')},
               assets=["movies/ok.webm"],
               expected=[(MAIN, "nowhere", "error", "missing-movie",
                          "movie 'movies/nowhere.webm' not found")])

    ja_po = script(
        'msgid ""', 'msgstr ""', '"Language: ja\\n"', "",
        'msgctxt "start/00000000"', 'msgid "Hi"', 'msgstr "{b}x{/b}"', "",
        '#, fuzzy', 'msgctxt "start/22222222"', 'msgid "Fuzzy"', 'msgstr "{b}unfinished"', "",
        'msgctxt "screen/title/33333333"', 'msgid "Screen"', 'msgstr "{{no tags here"', "",
        'msgctxt "start/11111111"', 'msgid "Bye"', 'msgstr "Sayonara {wobble}"')
    yield Case("bad_translation_tag", {MAIN: start('say("Eileen", "Hi")')},
               files={"translations/ja.po": ja_po},
               expected=[("translations/ja.po", 'msgid "Bye"', "error", "bad-translation-tag",
                          "translation of 'start/11111111': unknown tag 'wobble' "
                          "(byte 9 of the text)")])

    yield Case("po_syntax", {MAIN: start('say("Eileen", "Hi")')},
               files={"translations/ja.po": 'msgid "a"\nmsgstr "b\n'},
               expected=[("translations/ja.po", 2, "error", "po-syntax",
                          "string is not closed")])

    yield Case("unreachable_label", {MAIN: script(
        start('say("Eileen", "Hello.")'),
        'label("orphan", function() narrate("Nobody comes here.") end, { ending = true })')},
               expected=[(MAIN, '"orphan"', "warning", "unreachable-label",
                          "no path from 'start' reaches label 'orphan'")])

    yield Case("dead_end", {MAIN: script(
        'label("start", function() jump("stuck") end)',
        'label("stuck", function()', '  narrate("The story just stops.")', "end)")},
               expected=[(MAIN, 'label("stuck"', "warning", "dead-end",
                          "label 'stuck' ends with no jump or ret (mark it { ending = true } "
                          "if it ends the story)")])

    yield Case("dynamic_edge", {MAIN: script(
        'label("start", function()', "  jump(store.next)", "end)")},
               expected=[(MAIN, "store.next", "warning", "dynamic-edge",
                          "jump from label 'start' has a computed target")])

    not_literal = " is not a string literal, so it is not extracted for translation"
    yield Case("non_literal_string", {MAIN: script(
        'screen("hud", function() return text { store.mood } end)',
        start('say("Eileen", store.line)',
              'narrate("Fine " .. store.name)',
              'menu { "Yes", store.no }',
              'menu(store.options)',
              'say("Eileen", "Fine.")'))},
               expected=[(MAIN, "store.mood", "warning", "non-literal-string",
                          "a text of screen 'hud'" + not_literal),
                         (MAIN, "store.line", "warning", "non-literal-string",
                          "the text of say" + not_literal),
                         (MAIN, "store.name", "warning", "non-literal-string",
                          "the text of narrate" + not_literal),
                         (MAIN, "store.no }", "warning", "non-literal-string",
                          "a menu option" + not_literal),
                         (MAIN, "store.options", "warning", "non-literal-string",
                          "the table of menu options" + not_literal)])

    yield Case("project_config", {MAIN: start('say("Eileen", "Hello.")')},
               project='version = "1.0.0"\n',
               expected=[("project.toml", 0, "warning", "project-config",
                          "missing required key 'title'")])

    yield Case("project_config_line", {MAIN: start('say("Eileen", "Hello.")')},
               project=OK_TITLE + "rollback_depth = -1\n",
               expected=[("project.toml", 2, "warning", "project-config",
                          "'rollback_depth' must be a whole number from 0 to 10000")])

    other = "scripts/chapter/two.lua"
    yield Case("mixed", {
        MAIN: script('label("start", function()',
                     '  say("Eileen", "Hi {nope}")',
                     "  scene('bg/missing')",
                     "  jump('a, \"b\"')",
                     "end)"),
        other: script('label("two", function() jump(store.next) end)',
                      'label("three", function() narrate("x") end)'),
    }, project='title = 1\n',
               expected=[("project.toml", 1, "warning", "project-config",
                          "'title' must be a string"),
                         (MAIN, "{nope}", "error", "bad-text-tag",
                          "unknown tag 'nope' (byte 3 of the text)"),
                         (MAIN, "bg/missing", "error", "missing-image",
                          "image 'bg/missing' not found"),
                         (MAIN, "a, ", "error", "unknown-label",
                          "jump from label 'start' goes to unknown label 'a, \"b\"'"),
                         (other, '"two"', "warning", "unreachable-label",
                          "no path from 'start' reaches label 'two'"),
                         (other, '"two"', "warning", "dynamic-edge",
                          "jump from label 'two' has a computed target"),
                         (other, '"three"', "warning", "unreachable-label",
                          "no path from 'start' reaches label 'three'"),
                         (other, '"three"', "warning", "dead-end",
                          "label 'three' ends with no jump or ret (mark it { ending = true } "
                          "if it ends the story)")])


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for case in cases():
        case.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
