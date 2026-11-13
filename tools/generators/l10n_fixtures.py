#!/usr/bin/env python3
"""Generate the projects and the expected files of the `vnmaki extract-strings` CLI tests.

Usage: l10n_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.

`extract/` is a project with languages en, ja and de. Its `translations/ja.po` is a file from an
earlier version of the scripts, so the run keeps one translation, moves one as fuzzy (the text of
its line changed) and makes one obsolete (its label is gone). There is no `de.po` yet.
`extract.expected/` has the files that the run must write, and `extract.expected/stdout.txt` has
its output. `broken/` has a `.po` file that cannot be read. `lang/` is a game for `vnmaki run
--headless --lang`: its `ja.po` has two translations, a fuzzy entry (the source text shows) and no
entry for one line. `lang.en.transcript` and `lang.ja.transcript` are what the run must print.

The expected files come from the model in this script: the key hash (FNV-1a, 32 bit), the lines of
the strings (listed here by hand) and the layout of a `.po` file. Nothing here reads the engine's
scan or the engine's `.po` writer.
"""

import shutil
import sys
from pathlib import Path

HEADER_TEXT = "Content-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n"
PROJECT = 'title = "L10n fixture"\nlanguages = ["en", "ja", "de"]\n'
MAIN = "scripts/main.lua"

SCRIPT = [
    'local eileen = character { name = "Eileen" }',  # 1
    "",  # 2
    'label("start", function()',  # 3
    '  say(eileen, "You came back.")',  # 4
    '  say(nil, "Quote \\"q\\" here")',  # 5
    '  narrate("The end.\\nReally.")',  # 6
    '  local c = menu { "Yes", "No", fixed = true }',  # 7
    '  say(eileen, "You came back.")',  # 8
    "  say(eileen, store.line)",  # 9: not a literal, not extracted
    "end, { ending = true })",  # 10
    "",  # 11
    'label("second", function()',  # 12
    '  say("Bob", "Hi {b}there{/b}")',  # 13
    "end, { ending = true })",  # 14
    "",  # 15
    'screen("title", function(args)',  # 16
    '  return vbox { text { "My Game" }, button { "Start", returns = "start" } }',  # 17
    "end)",  # 18
]

# (scope, speaker, text, lines): in the order of the first line
STRINGS = [
    ("start", "eileen", "You came back.", [4, 8]),
    ("start", "", 'Quote "q" here', [5]),
    ("start", "", "The end.\nReally.", [6]),
    ("start", "", "Yes", [7]),
    ("start", "", "No", [7]),
    ("second", "Bob", "Hi {b}there{/b}", [13]),
    ("screen/title", "", "My Game", [17]),
    ("screen/title", "", "Start", [17]),
]
# What the same strings were in the version that wrote the old ja.po: the label `second` had
# another text at the same position, and a label `gone` existed.
OLD_SECOND = ("second", "Bob", "Hello {b}there{/b}")
OLD_GONE = ("gone", "", "Farewell")


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return f"{value:08x}"


def key(scope, speaker, text):
    data = speaker.encode() + bytes([0]) + text.encode()
    return f"{scope}/{fnv1a(data)}"


def quoted(text):
    out = []
    for char in text:
        if char in ('"', "\\"):
            out.append("\\" + char)
        elif char == "\n":
            out.append("\\n")
        else:
            out.append(char)
    return '"' + "".join(out) + '"'


def field(keyword, text, prefix=""):
    """`keyword "text"`, in one string for each line when the text has a line break inside."""
    parts = text.split("\n")
    if len(parts) == 1 or (len(parts) == 2 and parts[1] == ""):
        return f"{prefix}{keyword} {quoted(text)}\n"
    segments = [part + "\n" for part in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
    return f'{prefix}{keyword} ""\n' + "".join(f"{prefix}{quoted(s)}\n" for s in segments)


def entry(context, text, translation="", comments=(), extracted=(), refs=(), flags=(),
          obsolete=False):
    prefix = "#~ " if obsolete else ""
    out = "".join(f"# {note}\n" for note in comments)
    out += "".join(f"#. {note}\n" for note in extracted)
    if refs:
        out += "#: " + " ".join(refs) + "\n"
    if flags:
        out += "#, " + ", ".join(flags) + "\n"
    return (out + field("msgctxt", context, prefix) + field("msgid", text, prefix)
            + field("msgstr", translation, prefix))


def header(language, extra=""):
    text = HEADER_TEXT + (f"Language: {language}\n" if language else "") + extra
    return field("msgid", "") + field("msgstr", text)


def template_entry(scope, speaker, text, lines, translation="", comments=(), flags=()):
    return entry(key(scope, speaker, text), text, translation, comments,
                 [f"speaker: {speaker}"] if speaker else [],
                 [f"{MAIN}:{line}" for line in lines], flags)


def old_ja_po():
    return "\n".join([
        header("ja", "X-Custom: kept\n"),
        entry(key("start", "eileen", "You came back."), "You came back.", "Okaeri.",
              ["check the tone"], (), ["scripts/main.lua:99"], ["no-wrap"]),
        entry(key(*OLD_SECOND), OLD_SECOND[2], "Konnichiwa {b}minasan{/b}"),
        entry(key(*OLD_GONE), OLD_GONE[2], "Sayonara"),
        entry("start/deadbeef", "Lost long ago", "Mukashi", obsolete=True),
    ])


def new_ja_po():
    """The old file after the run: see the docstring."""
    parts = [header("ja", "X-Custom: kept\n")]
    for scope, speaker, text, lines in STRINGS:
        if text == "You came back.":
            parts.append(template_entry(scope, speaker, text, lines, "Okaeri.",
                                        ["check the tone"], ["no-wrap"]))
        elif text == "Hi {b}there{/b}":
            parts.append(template_entry(scope, speaker, text, lines,
                                        "Konnichiwa {b}minasan{/b}", (), ["fuzzy"]))
        else:
            parts.append(template_entry(scope, speaker, text, lines))
    parts.append(entry(key(*OLD_GONE), OLD_GONE[2], "Sayonara", obsolete=True))
    parts.append(entry("start/deadbeef", "Lost long ago", "Mukashi", obsolete=True))
    return "\n".join(parts)


def template(language):
    return "\n".join([header(language)] + [template_entry(*item) for item in STRINGS])


LANG_PROJECT = 'title = "Lang fixture"\nlanguages = ["en", "ja"]\n'
LANG_SCRIPT = [
    'local eileen = character { name = "Eileen" }',  # 1
    "",  # 2
    'label("start", function()',  # 3
    '  say(eileen, "Hello.")',  # 4: translated
    '  narrate("Silence.")',  # 5: fuzzy in ja.po, so the source text shows
    '  say("Bob", "Bye {b}now{/b}.")',  # 6: no entry
    '  say("Eileen", "Thanks.")',  # 7: translated (the speaker is a literal: the exact key)
    "end, { ending = true })",  # 8
]
LANG_TRANSCRIPT = [
    "label start",
    "say Eileen: {hello}",
    "narrate Silence.",
    "say Bob: Bye {{b}}now{{/b}}.",
    "say Eileen: {thanks}",
]


def lang_po():
    return "\n".join([
        header("ja"),
        entry(key("start", "eileen", "Hello."), "Hello.", "Konnichiwa.",
              extracted=["speaker: eileen"]),
        entry(key("start", "", "Silence."), "Silence.", "Shizukesa.", flags=["fuzzy"]),
        entry(key("start", "Eileen", "Thanks."), "Thanks.", "Arigatou.",
              extracted=["speaker: Eileen"]),
    ])


def write(root, files):
    for path, text in files.items():
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode("utf-8"))


def main():
    if len(sys.argv) != 2:
        print("usage: l10n_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)

    script = "\n".join(SCRIPT) + "\n"
    write(out / "extract", {"project.toml": PROJECT, MAIN: script,
                            "translations/ja.po": old_ja_po()})
    count = len(STRINGS)
    write(out / "extract.expected", {
        "translations/template.pot": template(""),
        "translations/ja.po": new_ja_po(),
        "translations/de.po": template("de"),
        "stdout.txt": f"translations/template.pot: {count} strings\n"
                      f"translations/ja.po: {count} strings, 1 fuzzy, 2 obsolete\n"
                      f"translations/de.po: {count} strings, 0 fuzzy, 0 obsolete\n",
    })
    broken_po = 'msgid "a"\nmsgstr "b"\n\nmsgid "c"\nmsgstr "d\n'
    write(out / "broken", {"project.toml": PROJECT, MAIN: script, "translations/ja.po": broken_po})
    write(out / "lang", {"project.toml": LANG_PROJECT, MAIN: "\n".join(LANG_SCRIPT) + "\n",
                         "translations/ja.po": lang_po()})
    for code, hello, thanks in (("en", "Hello.", "Thanks."), ("ja", "Konnichiwa.", "Arigatou.")):
        lines = [line.format(hello=hello, thanks=thanks) for line in LANG_TRANSCRIPT]
        write(out, {f"lang.{code}.transcript": "\n".join(lines) + "\n"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
