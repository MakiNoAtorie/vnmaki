#!/usr/bin/env python3
"""Generate the project and the DAP sessions of the `vnmaki debug --headless` CLI tests.

Usage: debug_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.

project/        a small game: two labels, a `store` value, a local table.
<name>.in.dap   the requests of a client, framed with Content-Length. The client sends them all at
                once and then closes stdin, so the answers cannot depend on timing: the debugger
                reads nothing after a request that resumes the story until the story stops again.
<name>.out.dap  the framed responses and events that `vnmaki debug` must write. Written here from
                the Debug Adapter Protocol and the rules of docs/CONVENTIONS.md (Debugger), not
                taken from the engine: each body is spelled out below. JSON keys are sorted and
                there are no spaces, as nlohmann-json writes them.

template.in.dap, template.out.dap: a session on templates/default itself (the Phase 4 demo): a
                breakpoint in `start`, a step, `store.affection`, and a goto to `warm`. Its lines are
                found by searching main.lua of the template.

The headless player answers each line at once, so the story runs on until it stops at a breakpoint
or ends. Line numbers of start.lua are found by searching for a marker, so editing the script does
not leave a wrong number behind.
"""

import json
import os
import shutil
import sys
from pathlib import Path

RNG_SEED = 0x5EED0F7E570B5E  # store.__rng of a new game (kDefaultRandomSeed)

START = '''label("start", function()
  local mood = "calm"
  store.affection = 3
  say("Maki", "Hello")
  store.affection = store.affection + 1
  local pair = { kind = "pair", count = 2 }  -- BREAK
  say("Maki", "Again")  -- NEXT
  jump("warm")
end)

label("warm", function()
  say("Maki", "Warm")
  say("Maki", "Warmer")  -- WARMER
  say("Maki", "Done")
end, { ending = true })
'''


TEMPLATE_MAIN = Path(__file__).resolve().parents[2] / "templates" / "default" / "scripts" / "main.lua"


def line_of(marker, text=START):
    for number, line in enumerate(text.splitlines(), 1):
        if marker in line:
            return number
    raise SystemExit(f"no line with {marker!r}")


def compact(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def frame(value):
    body = compact(value).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


class Session:
    """A list of requests, and the messages the debugger must answer them with."""

    def __init__(self, source_path):
        self.requests = []
        self.output = []  # (kind, ...) in the order they must appear
        self.source_path = source_path

    def request(self, command, arguments=None, body=None, success=True, message=None):
        """Sends a request. `body` (or `message` when it fails) is what the response must say."""
        seq = len(self.requests) + 1
        self.requests.append({"seq": seq, "type": "request", "command": command,
                              "arguments": arguments or {}})
        response = {"type": "response", "request_seq": seq, "success": success,
                    "command": command}
        if success:
            response["body"] = body if body is not None else {}
        else:
            response["message"] = message
        self.output.append(response)

    def event(self, name, body=None):
        message = {"type": "event", "event": name}
        if body is not None:
            message["body"] = body
        self.output.append(message)

    def stopped(self, reason):
        self.event("stopped", {"reason": reason, "threadId": 1, "allThreadsStopped": True})

    def frames(self, line, name):
        path = self.source_path
        return {"stackFrames": [{"id": 1, "name": name, "line": line, "column": 1,
                                 "source": {"name": Path(path).name, "path": path}}],
                "totalFrames": 1}

    def write(self, out, name):
        (out / f"{name}.in.dap").write_bytes(b"".join(frame(r) for r in self.requests))
        stream = b""
        for seq, message in enumerate(self.output, 1):
            stream += frame({"seq": seq, **message})
        (out / f"{name}.out.dap").write_bytes(stream)


CAPABILITIES = {"supportsConfigurationDoneRequest": True, "supportsEvaluateForHovers": True}
SCOPES = {"scopes": [{"name": "Locals", "variablesReference": 1, "expensive": False},
                     {"name": "store", "variablesReference": 2, "expensive": False}]}


def variable(name, value, reference=0, type_name=None):
    entry = {"name": name, "value": value, "variablesReference": reference}
    if type_name:
        entry["type"] = type_name
    return entry


def debug_session(project):
    """Breakpoint, locals, `store`, watches, step over, goto from the stop, and the end."""
    source = project + "/scripts/start.lua"
    s = Session(source)
    s.request("initialize", {"adapterID": "vnmaki"}, CAPABILITIES)
    s.event("initialized")
    s.request("launch")
    s.request("setBreakpoints",
              {"source": {"path": source}, "breakpoints": [{"line": line_of("BREAK")}]},
              {"breakpoints": [{"verified": True, "line": line_of("BREAK")}]})
    s.request("configurationDone")
    s.stopped("breakpoint")  # the story starts: line 6 comes after the first line is shown
    s.request("threads", body={"threads": [{"id": 1, "name": "story"}]})
    s.request("stackTrace", {"threadId": 1}, s.frames(line_of("BREAK"), "start"))
    s.request("scopes", {"frameId": 1}, SCOPES)
    s.request("variables", {"variablesReference": 1},
              {"variables": [variable("mood", '"calm"', type_name="string")]})
    s.request("variables", {"variablesReference": 2},
              {"variables": [variable("__rng", str(RNG_SEED)), variable("affection", "4")]})
    s.request("evaluate", {"expression": "store.affection", "frameId": 1},
              {"result": "4", "variablesReference": 0})
    s.request("evaluate", {"expression": "mood", "frameId": 1},
              {"result": '"calm"', "variablesReference": 0})
    s.request("evaluate", {"expression": "store.nothing.here"}, success=False,
              message="repl:1: attempt to index a nil value (field 'nothing')")
    s.request("next", {"threadId": 1})
    s.stopped("step")
    s.request("stackTrace", {"threadId": 1}, s.frames(line_of("NEXT"), "start"))
    s.request("scopes", {"frameId": 1}, SCOPES)
    # `pair` is a local now; its table is the next reference, after the two scopes.
    s.request("variables", {"variablesReference": 1},
              {"variables": [variable("mood", '"calm"', type_name="string"),
                             variable("pair", "table", 3, "table")]})
    s.request("variables", {"variablesReference": 3},
              {"variables": [variable("count", "2", type_name="number"),
                             variable("kind", '"pair"', type_name="string")]})
    # The story stops inside a run, so the goto cuts the run and goes on once it has unwound.
    s.request("vnmaki/goto", {"label": "warm", "step": 2}, {"label": "warm", "step": 2})
    s.stopped("goto")
    s.request("stackTrace", {"threadId": 1}, s.frames(line_of("WARMER"), "warm"))
    s.request("evaluate", {"expression": "store.affection"},
              {"result": "4", "variablesReference": 0})
    s.request("vnmaki/goto", {"label": "nowhere"}, success=False, message="no label 'nowhere'")
    s.request("continue", {"threadId": 1}, {"allThreadsContinued": True})
    s.event("exited", {"exitCode": 0})
    s.event("terminated")
    s.request("disconnect")
    return s


def finish_session(project):
    """No breakpoint: the story runs to its end, and an unknown request is refused."""
    s = Session(project + "/scripts/start.lua")
    s.request("initialize", {"adapterID": "vnmaki"}, CAPABILITIES)
    s.event("initialized")
    s.request("launch")
    s.request("setBreakpoints", {"source": {"path": s.source_path}, "breakpoints": []},
              {"breakpoints": []})
    s.request("configurationDone")
    s.event("exited", {"exitCode": 0})
    s.event("terminated")
    s.request("frobnicate", success=False, message="unsupported request 'frobnicate'")
    s.request("stackTrace", {"threadId": 1}, success=False, message="the story has ended")
    s.request("disconnect")
    return s


def template_session(template):
    """The template: a breakpoint in `start`, a step, `store.affection` and a goto to `warm`."""
    source = template + "/scripts/main.lua"
    text = TEMPLATE_MAIN.read_text(encoding="utf-8")
    first = line_of('call_screen("title"', text)  # the first line of `start`
    second = line_of('jump("classroom")', text)
    warm = line_of("I'm glad", text)  # the first say of `warm`
    s = Session(source)
    s.request("initialize", {"adapterID": "vnmaki"}, CAPABILITIES)
    s.event("initialized")
    s.request("launch")
    s.request("setBreakpoints", {"source": {"path": source}, "breakpoints": [{"line": first}]},
              {"breakpoints": [{"verified": True, "line": first}]})
    s.request("configurationDone")
    s.stopped("breakpoint")
    s.request("stackTrace", {"threadId": 1}, s.frames(first, "start"))
    s.request("scopes", {"frameId": 1}, SCOPES)
    s.request("variables", {"variablesReference": 1}, {"variables": []})  # `start` has no local
    s.request("variables", {"variablesReference": 2},
              {"variables": [variable("__rng", str(RNG_SEED)), variable("affection", "0")]})
    s.request("next", {"threadId": 1})  # the headless player closes the title screen at once
    s.stopped("step")
    s.request("stackTrace", {"threadId": 1}, s.frames(second, "start"))
    s.request("evaluate", {"expression": "store.affection", "frameId": 1},
              {"result": "0", "variablesReference": 0})
    s.request("vnmaki/goto", {"label": "warm"}, {"label": "warm", "step": 1})
    s.stopped("goto")
    s.request("stackTrace", {"threadId": 1}, s.frames(warm, "warm"))
    s.request("evaluate", {"expression": "store.affection"},
              {"result": "0", "variablesReference": 0})
    s.request("continue", {"threadId": 1}, {"allThreadsContinued": True})
    s.event("exited", {"exitCode": 0})
    s.event("terminated")
    s.request("disconnect")
    return s


def main():
    out = Path(sys.argv[1])
    if out.exists():
        shutil.rmtree(out)
    (out / "project" / "scripts").mkdir(parents=True)
    (out / "project" / "project.toml").write_bytes(b'title = "Debug fixture"\n')
    (out / "project" / "scripts" / "start.lua").write_bytes(START.encode("utf-8"))
    project = os.path.abspath(out / "project").replace("\\", "/")
    debug_session(project).write(out, "session")
    finish_session(project).write(out, "finish")
    template = str(TEMPLATE_MAIN.parents[1]).replace("\\", "/")
    template_session(template).write(out, "template")


main()
