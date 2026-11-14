# VNMaki

An open-source visual novel engine (C++20, SDL3, Lua 5.4). The full spec is `docs/SPEC.md`. Build, test and code conventions are in `docs/CONVENTIONS.md`; read it before changing code.

## CI

- The CI is local: `tools\dev\check.cmd` (clang-format 18, debug with MSVC `/analyze`, release). Run it before each push.
- There is no CI on GitHub. Do not add GitHub workflows. Do not wait for or read GitHub checks.
- The engine targets Windows only. Do not add Linux or macOS CI jobs unless the user asks.

## Subagents

The main session orchestrates. It selects the model and effort for each subagent:

- Sonnet, medium or high effort: tasks with written acceptance criteria. No limit on the count.
- Opus, medium effort: tasks without written acceptance criteria, and the final code review at the end of a session.
- Haiku, high effort: small documentation edits, a few lines at a time. Do not use Haiku to rewrite or restructure a full document. Use Sonnet or Opus for that.
- A code change updates its own documentation.
- Do not use Fable models. Only the user uses them.

Give each writing subagent its own worktree and its own set of files. Two subagents must not change the same file.

Each subagent checks its result before it returns it:

- Build, then run only the tests for the code that it changed: `ctest --test-dir build/debug -R <pattern>`. Example: `-R unit.vfs`.
- Do not run the full test suite.
- Read the diff again.

If a Sonnet subagent fails its checks two times, give the task to Opus.

At the end of a session, the main session runs the full suite one time: `tools\dev\check.cmd` (it is the CI). Then the Opus review starts.
