#!/usr/bin/env python3
"""Generate random stories for the replay property test.

Usage: replay_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.
Each story is OUT_DIR/case_NN/scripts/main.lua. A story is a chain of labels
that jump forward (so it always ends) and call two shared labels. Its
statements mix lines, menus, `rand` draws that decide how many lines are said,
locals that live across yield points, and writes to nested `store` tables.
"""

import random
import shutil
import sys
from pathlib import Path

CASES = 8
SEED = 20261013


class Story:
    def __init__(self, rng: random.Random) -> None:
        self.rng = rng
        self.names = 0

    def name(self, prefix: str) -> str:
        self.names += 1
        return f"{prefix}{self.names}"

    def line(self, label: str) -> str:
        text = f"{label} {self.name('line')}"
        if self.rng.random() < 0.5:
            return f'narrate("{text}")'
        return f'say("{self.rng.choice(["Eileen", "Lucy"])}", "{text}")'

    def menu(self, label: str) -> list[str]:
        choice = self.name("choice")
        options = ", ".join(f'"{choice} {i}"' for i in range(1, self.rng.randint(2, 4) + 1))
        return [
            f"local {choice} = menu({{ {options} }})",
            f"store.picks = store.picks .. {choice}",
            f"if {choice} == 1 then",
            "  store.n = store.n + rand(10)",
            "else",
            f"  {self.line(label)}",
            "end",
        ]

    def statement(self, label: str, may_call: bool) -> list[str]:
        kinds = ["line", "line", "menu", "draw", "kept", "maybe", "loop", "flag", "ratio"]
        if may_call:
            kinds += ["call", "call"]
        kind = self.rng.choice(kinds)
        if kind == "line":
            return [self.line(label)]
        if kind == "menu":
            return self.menu(label)
        if kind == "draw":
            return ["store.n = store.n + rand(1, 6)"]
        if kind == "kept":
            kept = self.name("kept")
            return [
                f"local {kept} = rand(1, 100)",
                self.line(label),
                f"store.trail[#store.trail + 1] = {kept}",
            ]
        if kind == "maybe":
            return ["if rand() < 0.5 then", f"  {self.line(label)}", "end"]
        if kind == "loop":
            return [
                "for i = 1, math.random(1, 3) do",
                f'  narrate("{label} {self.name("loop")} " .. i)',
                "end",
            ]
        if kind == "flag":
            return ['store.flags["f" .. rand(1, 4)] = rand(0, 1) == 1']
        if kind == "ratio":
            return ["store.ratio = rand()"]
        return [f'call("{self.rng.choice(["aside", "detour"])}")']

    def label(self, name: str, first: list[str], count: int, may_call: bool,
              last: list[str]) -> list[str]:
        body = list(first)
        for _ in range(count):
            body += self.statement(name, may_call)
        body += last
        return [f'label("{name}", function()'] + [f"  {text}" for text in body] + ["end)", ""]

    def text(self) -> str:
        chain = ["start"] + [f"part{i}" for i in range(1, self.rng.randint(2, 3) + 1)]
        lines: list[str] = []
        for index, name in enumerate(chain):
            first: list[str] = []
            if index == 0:
                first = [
                    "store.n = 0",
                    "store.trail = {}",
                    "store.flags = {}",
                    'store.picks = ""',
                ]
                first += self.menu(name)
                first += ["store.n = store.n + rand(1, 6)"]
            later = chain[index + 1:]
            last: list[str] = []
            if len(later) == 1:
                last = [f'jump("{later[0]}")']
            elif later:
                last = [
                    "if rand(1, 2) == 1 then",
                    f'  jump("{later[0]}")',
                    "end",
                    f'jump("{later[1]}")',
                ]
            lines += self.label(name, first, self.rng.randint(3, 5), True, last)
        lines += self.label("aside", [self.line("aside")], self.rng.randint(1, 3), False, [])
        lines += self.label("detour", [], self.rng.randint(1, 3), False,
                            [self.line("detour"), "ret()"])
        return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: replay_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    for case in range(CASES):
        path = out / f"case_{case:02d}" / "scripts" / "main.lua"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(Story(random.Random(SEED + case)).text().encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
