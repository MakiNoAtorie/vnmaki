#!/usr/bin/env python3
"""Generate the projects and the expected output of the `vnmaki graph` CLI tests.

Usage: graph_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.

Each project is described as data: scripts, with labels, with statements (say, jump, call, ret,
menu). One emitter writes the Lua and notes the line of each call while it writes, and a small
model of the rules in engine/tools/story_graph.hpp (reachability from `start`, dead ends) gives
the expected CSV (and DOT) text. So the expectation does not come from the engine's tokenizer.

- story/: a small hand-made project. Its say calls carry decoys: calls inside strings, short and
  long comments, long strings that span lines. It has a label with a comma and a quote in its
  name (CSV and DOT quoting), a file that is not a script, and a script in a folder.
- random/: labels, edges and menus drawn from a seed, in several scripts and folders.
- template.csv: the graph of templates/default, from the table TEMPLATE_STORY below. The lines are
  found by searching the template's scripts, so no number is written here.
Output: <project>/ (a game folder), <project>.csv and, for story, <project>.dot.
"""

import csv
import io
import random
import shutil
import sys
from pathlib import Path

RANDOM_SEED = 34
RANDOM_LABEL_COUNT = 40
RANDOM_SCRIPTS = ["scripts/a.lua", "scripts/b.lua", "scripts/chapter1/c.lua", "scripts/chapter1/d.lua"]
MAX_STATEMENTS = 5

TEMPLATE_DIR = Path(__file__).resolve().parents[2] / "templates" / "default"
# (script, label, ending, edges as (kind, target, the text of the call)), in the order of the files.
TEMPLATE_STORY = [
    ("scripts/content.lua", "content", True, []),
    ("scripts/main.lua", "start", False, [("jump", "classroom", 'jump("classroom")')]),
    ("scripts/main.lua", "classroom", False, [("menu", "warm", 'jump("warm")'),
                                              ("menu", "cold", 'jump("cold")'),
                                              ("menu", "presentation", 'jump("presentation")'),
                                              ("menu", "content", 'jump("content")')]),
    ("scripts/main.lua", "warm", True, []),
    ("scripts/main.lua", "cold", True, []),
    ("scripts/presentation.lua", "presentation", True, []),
]


def label(name, statements, ending=False):
    return {"name": name, "statements": statements, "ending": ending}


# Statements: ("say",), ("ret",), ("jump", target or None), ("call", target or None),
# ("menu", [("jump" | "call", target), ...]). A target of None is computed at run time.
STORY = {
    "scripts/main.lua": [
        label("start", [("say",), ("menu", [("jump", "intro"), ("call", "side"),
                                            ("jump", 'odd, "name"')]), ("jump", "ending")]),
        label("intro", [("say",), ("say",), ("jump", None)]),
        label("side", [("say",)]),
        label('odd, "name"', [("say",), ("ret",)]),
    ],
    "scripts/extra/more.lua": [
        label("ending", [("say",)], ending=True),
        label("stuck", [("say",), ("say",)]),
        label("lonely", [("say",), ("jump", "stuck"), ("call", None)]),
        label("returns", [("ret",)]),
    ],
}
# A file that is not a script: its label call must not count.
NOT_A_SCRIPT = ("scripts/notes.txt", 'label("hidden", function() jump("start") end)\n')


class Emitter:
    """Writes the Lua of one script and records the lines of the edges."""

    def __init__(self, path, counter):
        self.path = path
        self.counter = counter  # a one-item list: decoys and forms rotate over the project
        self.lines = []
        self.labels = []
        self.edges = []

    def add(self, text):
        first = len(self.lines) + 1
        self.lines.extend(text.split("\n"))
        return first

    def next_form(self, count):
        self.counter[0] += 1
        return self.counter[0] % count

    def literal(self, call, target):
        # The same text four ways: ("t"), ('t'), "t" and ([[t]]).
        def quoted(quote):
            return quote + target.replace("\\", "\\\\").replace(quote, "\\" + quote) + quote

        form = self.next_form(4)
        if form == 0:
            return f"{call}({quoted(chr(34))})"
        if form == 1:
            return f"{call}({quoted(chr(39))})"
        if form == 2:
            return f"{call} {quoted(chr(34))}"
        return f"{call}([[{target}]])"

    def computed(self, call):
        return [f"{call}(store.next)", f'{call}("part_" .. suffix)', f"{call}(pick({{ 1, 2 }}))"][
            self.next_form(3)]

    def say(self):
        n = self.next_form(4)
        if n == 0:
            self.add('  say("Eileen", "She said: jump(\\"fake\\") and call(\'fake\')")')
        elif n == 1:
            self.add('  -- call("commented_out")\n  say("Eileen", "Plain.")')
        elif n == 2:
            self.add('  --[==[ jump("block comment")\n  ]] call("still the comment")\n  ]==]\n'
                     '  narrate("After the block.")')
        else:
            self.add('  local note = [[\n  jump("long string")\n  call("more")]]\n'
                     '  narrate(note)')

    def call_statement(self, info, kind, target, indent, edge_kind):
        text = self.literal(kind, target) if target is not None else self.computed(kind)
        line = self.add(indent + text)
        self.edges.append({"kind": edge_kind or kind, "from": info["name"],
                           "to": target or "", "script": self.path, "line": line,
                           "dynamic": target is None})

    def emit(self, info):
        info["script"] = self.path
        info["line"] = self.add(f'label({lua_string(info["name"])}, function()')
        info["exit"] = False
        info["calls"] = []
        for statement in info["statements"]:
            kind = statement[0]
            if kind == "say":
                self.say()
            elif kind == "ret":
                self.add("  ret()")
                info["exit"] = True
            elif kind == "menu":
                self.add("  local c = menu { " + ", ".join(
                    f'"Option {i + 1}"' for i in range(len(statement[1]))) + " }")
                for i, (branch, target) in enumerate(statement[1]):
                    self.add(f"  {'if' if i == 0 else 'elseif'} c == {i + 1} then")
                    self.call_statement(info, branch, target, "    ", "menu")
                    self.note(info, branch, target)
                self.add("  end")
            else:
                self.call_statement(info, kind, statement[1], "  ", None)
                self.note(info, kind, statement[1])
        self.add("end, { ending = true })" if info["ending"] else "end)")
        self.labels.append(info)

    @staticmethod
    def note(info, kind, target):
        if kind == "jump":
            info["exit"] = True
        elif target is not None:
            info["calls"].append(target)


def lua_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_project(root, scripts):
    """Writes the scripts and returns (labels, edges) in the order of the script paths."""
    counter = [0]
    labels, edges = [], []
    (root / "scripts").mkdir(parents=True, exist_ok=True)
    (root / "project.toml").write_bytes(b'title = "Graph fixture"\n')
    for path in sorted(scripts):
        emitter = Emitter(path, counter)
        for info in scripts[path]:
            emitter.emit(info)
            emitter.add("")
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(("\n".join(emitter.lines)).encode("utf-8"))
        labels.extend(emitter.labels)
        edges.extend(emitter.edges)
    return labels, edges


def analyse(labels, edges):
    """The rules of engine/tools/story_graph.hpp: reachable from start; dead ends."""
    reachable = {"start"} if any(i["name"] == "start" for i in labels) else set()
    changed = True
    while changed:
        changed = False
        for edge in edges:
            if edge["from"] in reachable and not edge["dynamic"] and edge["to"] not in reachable:
                if any(i["name"] == edge["to"] for i in labels):
                    reachable.add(edge["to"])
                    changed = True
    called = {target for info in labels for target in info["calls"]}
    for info in labels:
        info["reachable"] = info["name"] in reachable
        info["dead_end"] = not info["ending"] and not info["exit"] and info["name"] not in called


def template_graph():
    """The labels and edges of TEMPLATE_STORY, with the lines found in the template's scripts."""
    labels, edges = [], []
    for script, name, ending, calls in TEMPLATE_STORY:
        lines = (TEMPLATE_DIR / script).read_text(encoding="utf-8").splitlines()

        def find(text, start):
            for number in range(start, len(lines) + 1):
                if text in lines[number - 1]:
                    return number
            raise SystemExit(f"{script} has no {text!r} from line {start}")

        line = find(f"label({lua_string(name)}", 1)
        labels.append({"name": name, "script": script, "line": line, "ending": ending,
                       "exit": any(text.startswith("jump") for _, _, text in calls), "calls": []})
        for kind, target, text in calls:
            edges.append({"kind": kind, "from": name, "to": target, "script": script,
                          "line": find(text, line), "dynamic": False})
    return labels, edges


def make_csv(labels, edges):
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(["type", "from", "to", "script", "line", "flags"])
    for info in labels:
        flags = [name for name, on in (("ending", info["ending"]),
                                       ("unreachable", not info["reachable"]),
                                       ("dead_end", info["dead_end"])) if on]
        writer.writerow(["label", info["name"], "", info["script"], info["line"], ";".join(flags)])
    for edge in edges:
        writer.writerow([edge["kind"], edge["from"], edge["to"], edge["script"], edge["line"],
                         "dynamic" if edge["dynamic"] else ""])
    return out.getvalue()


def dot_quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def make_dot(labels, edges):
    lines = ["digraph story {", "  rankdir=LR;", "  node [shape=box];"]
    for info in labels:
        attributes = []
        if not info["reachable"]:
            attributes.append("color=red")
        if info["dead_end"]:
            attributes += ["style=filled", "fillcolor=lightgray"]
        if info["ending"]:
            attributes.append("peripheries=2")
        suffix = f" [{', '.join(attributes)}]" if attributes else ""
        lines.append(f"  {dot_quote(info['name'])}{suffix};")
    for edge in edges:
        attributes = [f"label={dot_quote(edge['kind'])}"]
        if edge["kind"] == "menu":
            attributes.append("color=blue")
        if edge["dynamic"]:
            attributes.append("style=dotted")
        elif edge["kind"] == "call":
            attributes.append("style=dashed")
        target = "(dynamic)" if edge["dynamic"] else edge["to"]
        lines.append(f"  {dot_quote(edge['from'])} -> {dot_quote(target)} "
                     f"[{', '.join(attributes)}];")
    if any(edge["dynamic"] for edge in edges):
        lines.append(f"  {dot_quote('(dynamic)')} [shape=plaintext];")
    lines.append("}")
    return "\n".join(lines) + "\n"


def random_scripts(rng):
    names = ["start"] + [f"label_{i}" for i in range(1, RANDOM_LABEL_COUNT)]
    scripts = {path: [] for path in RANDOM_SCRIPTS}
    for position, name in enumerate(names):
        statements = []
        for _ in range(rng.randrange(MAX_STATEMENTS)):
            kind = rng.choice(["say", "say", "jump", "call", "ret", "menu", "menu"])
            target = None if rng.random() < 0.1 else rng.choice(names)
            if kind in ("jump", "call"):
                statements.append((kind, target))
            elif kind == "menu":
                branches = [(rng.choice(["jump", "call"]),
                             None if rng.random() < 0.1 else rng.choice(names))
                            for _ in range(rng.randrange(1, 4))]
                statements.append(("menu", branches))
            else:
                statements.append((kind,))
        # `start` is always in the first script, so the file order is fixed.
        path = RANDOM_SCRIPTS[0] if position == 0 else rng.choice(RANDOM_SCRIPTS)
        scripts[path].append(label(name, statements, ending=rng.random() < 0.1))
    return scripts


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: graph_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    rng = random.Random(RANDOM_SEED)

    story = out / "story"
    labels, edges = write_project(story, STORY)
    (story / NOT_A_SCRIPT[0]).write_bytes(NOT_A_SCRIPT[1].encode("utf-8"))
    analyse(labels, edges)
    (out / "story.csv").write_bytes(make_csv(labels, edges).encode("utf-8"))
    (out / "story.dot").write_bytes(make_dot(labels, edges).encode("utf-8"))

    labels, edges = write_project(out / "random", random_scripts(rng))
    analyse(labels, edges)
    (out / "random.csv").write_bytes(make_csv(labels, edges).encode("utf-8"))

    labels, edges = template_graph()
    analyse(labels, edges)
    (out / "template.csv").write_bytes(make_csv(labels, edges).encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
