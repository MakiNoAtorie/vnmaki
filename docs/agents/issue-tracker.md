# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues in `MakiNoAtorie/vnmaki`.

Use the `gh` CLI where it is available. In cloud sessions without `gh`, use the GitHub MCP tools (`mcp__github__*`) for the same operations.

## Conventions

- **Create an issue**: `gh issue create --title "..." --body "..."` (MCP: `issue_write` with `method: create`).
- **Read an issue**: `gh issue view <number> --json number,title,body,labels,comments` (MCP: `issue_read`).
- **List issues**: `gh issue list --state open --label ready-for-agent` (MCP: `list_issues`).
- **Make an issue a sub-issue of a parent**: `gh issue create --parent <parent> ...` (MCP: `issue_write` with `parent_issue_number`, or `sub_issue_write` with the child's database id).
- **Comment on an issue**: `gh issue comment <number> --body "..."` (MCP: `add_issue_comment`).
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`.
- **Close**: `gh issue close <number> --comment "..."`. A PR body line `Closes #<n>` closes the issue on merge.

## Blocking edges

Each ticket body has a `## Blocked by` section listing `#<n>` references. A ticket is on the **frontier** when every issue it lists is closed. Where GitHub's native issue dependencies are available, they may be added too (`gh api --method POST repos/MakiNoAtorie/vnmaki/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`); the body section stays the source of truth.

## Spec

The engine spec lives in `docs/SPEC.md`. Each batch of tickets has a parent tracking issue; tickets are its sub-issues.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Read it as in **Read an issue** above.
