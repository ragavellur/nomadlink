# ADR-003: JSON is the source of truth, HTML is generated

- **Status:** Accepted
- **Date:** 2026-09-29
- **Deciders:** project owner, coding agent
- **Relates to:** ADR-001

## Context

A project board has to answer, at any moment: what is the current state, what is
blocked, and what has actually been verified. The temptation is to maintain
several views by hand — a `STATUS.md`, a `board.html`, a `PROJECT_STATUS.json`,
a summary in the README. That approach fails in a specific and predictable way.

Hand-maintained views drift from each other. They cannot disagree loudly; they
just quietly disagree. During this project's bootstrap the drift was real and
caught only by accident:

- `project-status.json` claimed 14 tasks while `backlog.json` held 13.
- It claimed 3 verified requirements while `requirements.json` held 2.
- Prose documents asserted those same wrong numbers, so the error propagated
  from the data into the narrative.

None of these would have been caught by reading the documents. They were caught
by a validator that compared the files.

## Decision

1. **JSON files under `docs/` are the single source of truth.** Requirements,
   backlog, tests, bugs, risks, sprints, releases and decisions are JSON.
2. **Every HTML page is generated.** `scripts/project-tracker/render.py` reads the
   JSON and writes the dashboards. No HTML file is edited by hand.
3. **Derived numbers are never hand-maintained.** `render.py` rewrites the
   `progress` block of `project-status.json` on every run, so a stale count is
   impossible by construction rather than by discipline.
4. **`validate.py` runs before every commit** and fails on contradictions between
   files, including orphan ID references.
5. **Prose documents may not restate a number that the JSON already holds.** If a
   document needs a count, it is generated.

## Alternatives considered

**A. Hand-maintained Markdown status documents.** Rejected. Cannot detect its own
drift; every number is a future lie. This is the failure mode we actually hit.

**B. A hosted tool (Jira, Linear, GitHub Projects) as the source of truth.**
Rejected for now. It cannot be committed, so it cannot be reviewed, diffed or
restored with the code, and it would not be available offline. The JSON files
can be imported into such a tool later if the project outgrows them.

**C. Generate everything, including the prose documents.** Rejected as too rigid.
Specifications and ADRs need judgment and narrative; only the numbers are
generated.

## Consequences

- A generated page can always be trusted to match the JSON it came from.
- Anyone editing an HTML file by hand is introducing an inconsistency that the
  next `render.py` run silently discards.
- Adding a field to a JSON file is cheap; surfacing it on a dashboard requires
  editing `render.py`. This is deliberate friction, not an oversight.
- The validator must be kept honest. It was proven against a fixture seeded with
  deliberate violations rather than assumed correct (TEST-002).
