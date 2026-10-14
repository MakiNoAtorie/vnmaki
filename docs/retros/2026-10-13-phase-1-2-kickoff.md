# Retro: Phase 1–2 kickoff session (2026-10-13)

**Scope:** `/to-tickets` and `/implement-spec` on `docs/SPEC.md`, run as one cloud session.

**Outcome:**
- 19 tickets published (#2–#20) under tracking issue #1, plus draft PR #21.
- Tickets 01–04 merged into `vnmaki/phase-1-2`.
- Tickets 05 and 06 pushed and waiting on CI.

**Cost:** six implementer subagents used about 1.65M tokens and 1,033 tool calls in total. Per ticket: 01 198k, 02 180k, 03 208k, 04 295k, 05 420k, 06 354k.

The candidates below are ranked by severity. Each one states the evidence, the fix, and whether the fix is already in place.

## 1. Local checks differ from CI, so CI-only failures cost extra rounds (environment / information access)

**Evidence:**
- vcpkg can't build in this container: GitHub source archives return 403 unless the repository is attached to the session.
- Implementers therefore verified with `system-*` presets against apt packages and libraries built from git into `/opt/vnmaki-deps`. CI uses vcpkg.
- Three of six tickets failed only in CI, each costing at least one more push-and-wait cycle:
  - 01: MSVC couldn't print `std::string_view` in doctest checks.
  - 03: a TSan race through `std::future` exceptions.
  - 06: vcpkg's FindLua wrapper sets no version variables.
- `/opt/vnmaki-deps` disappears when the container is reclaimed. Rebuilding it takes about 10 minutes and depends on a script in the scratchpad, outside the repo.

**Fix (not applied):**
- Commit the dependency script as `tools/dev/bootstrap-deps.sh`.
- Run it from a SessionStart hook (the `session-start-hook` skill) so every cloud session starts with the same prefix.
- Longer term: make the container use vcpkg too, through a vcpkg asset-cache `x-script` that fetches sources with `git` (allowed) instead of archive URLs (refused). Local and CI builds would then come from one dependency source.

## 2. GitHub Actions MCP calls were the largest single tool results (tool economy)

**Evidence:**
- In every implementer, the biggest tool results were `actions_list list_workflow_jobs` (18–24 KB per call) and `get_job_logs` (18–32 KB per call).
- The orchestrator made the same `list_workflow_jobs` call once (about 25 KB) just to learn "8 of 11 passed".
- Implementers polled CI themselves, repeating these calls.

**Fix (partly applied mid-session):**
- Only the orchestrator checks CI, once per ticket, at merge.
- It uses run-level status (`list_workflow_runs`, `perPage: 1`, filtered by branch).
- It fetches logs only for failed runs, with `get_job_logs failed_only: true, tail_lines: 60`.

**Still to do:** write this rule down where every future orchestrator sees it, e.g. a line in `docs/agents/issue-tracker.md`, or a step in the `implement-spec` skill (see candidate 3).

## 3. The `implement-spec` and `tdd` skills conflict in background runs (steering)

**Evidence:**
- `tdd` requires confirming seams with the user before any test is written.
- `implement-spec` runs implementers as background subagents, which cannot ask the user.
- Every implementer prompt had to override this ("seams are pre-agreed by the orchestrator").
- `implement-spec` also asks for a merger subagent for every merge. All four merges so far were fast-forwards that took one `git merge --ff-only`.

**Fix (not applied):** amend `.claude/skills/implement-spec/SKILL.md`:
- Step 4: the orchestrator names each ticket's seams in the implementer prompt; that counts as the `tdd` confirmation.
- Step 4: the orchestrator picks Opus or Sonnet per ticket by difficulty.
- Step 5: fast-forward merges go straight onto the branch; a merger subagent is only for conflicts.
- Steps 4–5: implementers run local checks and push; the orchestrator checks CI once at merge (candidate 2).

## 4. Each implementer re-explored the same files at start (navigation)

**Evidence:**
- Every implementer opened with large `sed -n` ranges of `docs/SPEC.md` (7–14 KB).
- Each then ran `cat` over many headers, CMake files and CI workflows (10–23 KB).
- The shared scratch notes file had grown to 25 KB, and every agent read all of it (7–19 KB per read).

**Fix (applied mid-session):**
- `docs/CONVENTIONS.md` (about 8 KB): build, test and code rules plus a contract per module, pointed to from `CLAUDE.md`.
- The scratch notes are trimmed to container-only facts (about 2 KB).
- Implementers no longer write to shared notes; they report new conventions in at most 10 bullet points, and the orchestrator folds them in.

**Still to do:** keep each ticket's spec pointer to exact § numbers (already done), and keep `CONVENTIONS.md` short. Its header carries the "delete lines that stop being true" rule against sediment.

## 5. MSVC-only warnings are found only in CI (automated checks)

**Evidence:**
- Ticket 06 had to guess at MSVC's unreachable-code warning C4702 in tests.
- The doctest `string_view` problem (ticket 01) also surfaced only on Windows.
- No MSVC exists in the container.

**Fix (not applied):** add `-Wunreachable-code` to the Clang flags in `cmake/VnmakiOptions.cmake`. Clang then flags locally the same class of code C4702 rejects. It is a one-line mechanical guardrail; a written rule would not substitute for it.

## 6. Agent worktrees show up as untracked files (repo hygiene)

**Evidence:**
- The stop hook flagged `.claude/worktrees/` as uncommitted work.
- The orchestrator excluded it with `.git/info/exclude`, which only exists in this clone. The next session will hit the same thing.

**Fix (not applied):** add `.claude/worktrees/` to the repo `.gitignore`.

## 7. Misdiagnosing the GitHub 403 cost the user a settings change (information access)

**Evidence:**
- The 403 on GitHub archive URLs was first reported to the user as a network-policy block.
- The user switched the environment to Full access. GitHub archives were still refused, because the real cause was repository scope.
- The 403 response body says so: "GitHub access to this repository is not enabled for this session".
- Other hosts (e.g. savannah) were the only real network-policy blocks.

**Fix (lesson):** before asking the user to change environment settings, read the denied response's body, not only its status code, and check the `read_documentation` page for that topic. Repository-scope refusals are answered by `add_repo` or by `git clone`, not by network settings.

## 8. Tracker label missing (tracker setup)

**Evidence:**
- `to-tickets` applies `ready-for-agent`, but the label doesn't exist in the repo.
- The GitHub MCP tools can't create labels, so every ticket body says `Status: ready-for-agent` instead.

**Fix (needs the user):**
- Create the five triage labels in GitHub settings, or run `gh label create` from a machine that has `gh`.
- Then apply `ready-for-agent` to #2–#20.

## 9. No reviewer standards file yet (coding standards)

**Evidence:**
- `code-review` runs after all tickets merge, and the repo has no `CODING_STANDARDS.md`.
- The mechanical rules are already enforced by tools: warnings as errors, clang-tidy with every finding an error, clang-format, and the no-SDL-outside-platform lint test. That is the right place for them.

**Fix (later):** add `CODING_STANDARDS.md` only when a review finds a judgement-call rule worth keeping, e.g. module boundaries or what belongs in the Lua prelude versus C++. Mechanical findings get a lint or test instead.

## What worked

- **Tracer-bullet tickets with § pointers:** each implementer built its ticket from the issue plus the spec, with few questions.
- **Guardrails from ticket 01:**
  - Strict warnings, sanitizers and clang-tidy caught real bugs before merge, including a TSan race.
  - Fuzz targets caught real bugs too, including stb_image undefined behaviour.
  - Implementers planted bugs to prove the guardrails fire.
- **Parallel worktrees:** shared CMake files were only ever appended to, so concurrent tickets merged with trivial conflicts.
- **Headless seams:** the headless platform, the transcript runner and lavapipe golden images let rendering and scripting be tested without a display.
