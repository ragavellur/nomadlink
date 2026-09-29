# ADR-001 — The repository is the source of truth

- **Status:** Accepted
- **Date:** 2026-09-29

## Context

Long-running firmware work on this project was previously carried in
conversational context. That produced a specific failure: a working cellular
LBS feature was declared absent and written into status tables as a settled
finding, because the failing test was in the conversation and the working
result was not. There was no durable record to contradict it.

Session continuity was also poor. New sessions had to re-derive the pin map,
the AT command gotchas, and which capabilities were actually proven.

## Decision

The Git repository is the persistent project memory. Every fact needed to
continue work — requirements, architecture, decisions, backlog, sprint board,
test results, defects, risks, verification evidence — lives in version-controlled
files.

Concretely:

1. No project state is held only in conversation. If it is not in the repo, it
   does not exist.
2. `docs/project/*.json` is the machine-readable source of truth. HTML
   dashboards are **generated** from it, never hand-edited.
3. Verification evidence is committed alongside the claim it supports.
4. A new session must be able to answer "where are we, what is blocked, what
   was verified, and what is next" by reading files only.

## Alternatives considered

- **Continue relying on conversational context.** Rejected: this is precisely
  what caused the LBS misdiagnosis and it does not survive session boundaries.
- **Use an external issue tracker instead of in-repo files.** Rejected for now:
  the user asked for in-repo JSON plus HTML viewable at a git URL, and a single
  repository keeps requirements, code and status in one history. Can be
  revisited; in-repo remains the baseline.

## Consequences

- Every meaningful state change is a commit, so history becomes the audit trail.
- The repository grows documentation alongside code. This is the intended cost.
- Merging the 8.5% of the codebase that is documentation churns more often than
  a typical code repository.
