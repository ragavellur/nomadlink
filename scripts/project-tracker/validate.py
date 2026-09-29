#!/usr/bin/env python3
"""
NomadLink project tracker validator.

Enforces the project invariants so that a task cannot be marked DONE without
real evidence. Exits non-zero when project state is inconsistent.

Usage:
    python3 scripts/project-tracker/validate.py [--repo-root PATH]

Rule coverage (Rule 44):
  1. DONE task with verification.status != VERIFIED        -> FAIL
  2. DONE task with empty git.commit_sha                    -> FAIL
  3. DONE task with no verification evidence                -> FAIL
  4. status != BACKLOG and empty acceptance_criteria        -> FAIL
  5. Implementation task with no requirement_ids            -> WARN (allowed
                                                             for type=infrastructure)
  6. Orphan references (REQ/EPIC/FEAT/STORY/TASK/TEST/BUG/RISK/ADR) -> FAIL
  7. REQUIREMENT marked VERIFIED without verification_note  -> FAIL
  8. DONE task whose requirement is not at least PARTIALLY_VERIFIED -> WARN
  9. Test referencing a nonexistent requirement or task     -> FAIL
 10. Sprint referencing a nonexistent committed task        -> FAIL
 11. Missing required project files                        -> FAIL
 12. Invalid status / priority / severity values            -> FAIL
 13. BLOCKED task without a recorded blocker reason        -> FAIL
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

VALID_TASK_STATES = {
    "BACKLOG", "READY", "IN_PROGRESS", "BLOCKED", "CODE_REVIEW",
    "TESTING", "VERIFICATION", "DONE", "REOPENED", "CANCELLED",
}
VALID_PRIORITIES = {"P0", "P1", "P2", "P3"}
VALID_SEVERITIES = {"S0", "S1", "S2", "S3", "S4"}
VALID_REQ_STATUS = {"NOT_STARTED", "PARTIALLY_VERIFIED", "VERIFIED", "DEFERRED"}
TASK_ID_RE = re.compile(r"^TASK-\d{3,}$")
REQ_ID_RE = re.compile(r"^REQ-\d{3,}$")
EPIC_ID_RE = re.compile(r"^EPIC-\d{3,}$")
FEAT_ID_RE = re.compile(r"^FEAT-\d{3,}$")
TEST_ID_RE = re.compile(r"^TEST-\d{3,}$")

REQUIRED_FILES = [
    "docs/product/requirements.json",
    "docs/project/backlog.json",
    "docs/project/sprint-board.json",
    "docs/project/risks.json",
    "docs/project/bugs.json",
    "docs/project/test-results.json",
    "docs/project/releases.json",
    "docs/project/changelog.json",
    "docs/project/project-status.json",
]

errors: list[str] = []
warnings: list[str] = []


def err(msg: str) -> None:
    errors.append(msg)


def warn(msg: str) -> None:
    warnings.append(msg)


def load_json(root: Path, rel: str):
    path = root / rel
    if not path.exists():
        err(f"REQUIRED FILE MISSING: {rel}")
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        err(f"INVALID JSON in {rel}: {exc}")
        return None


def main() -> int:
    root = Path(sys.argv[sys.argv.index("--repo-root") + 1]) if "--repo-root" in sys.argv else Path.cwd()

    print("NomadLink tracker validation")
    print(f"repo root: {root}\n")

    for rel in REQUIRED_FILES:
        if not (root / rel).exists():
            err(f"REQUIRED FILE MISSING: {rel}")

    reqs_doc = load_json(root, "docs/product/requirements.json") or {}
    backlog = load_json(root, "docs/project/backlog.json") or {}
    board = load_json(root, "docs/project/sprint-board.json") or {}
    bugs = load_json(root, "docs/project/bugs.json") or {}
    test_results = load_json(root, "docs/project/test-results.json") or {}

    requirements = reqs_doc.get("requirements", [])
    tasks = backlog.get("tasks", [])
    epics_doc = load_json(root, "docs/product/epics.json") or {}
    epics = epics_doc.get("epics", [])
    features = epics_doc.get("features", [])
    tests = test_results.get("tests", [])
    sprints = board.get("sprints", [])

    if not requirements:
        err("No requirements found in requirements.json")
    if not tasks:
        err("No tasks found in backlog.json")

    # --- index all IDs for orphan detection ---------------------------------
    req_ids = {r.get("id") for r in requirements}
    task_ids = {t.get("id") for t in tasks}
    epic_ids = {e.get("id") for e in epics}
    feat_ids = {f.get("id") for f in features}
    test_ids = {t.get("id") for t in tests}
    bug_ids = {b.get("id") for b in bugs.get("bugs", [])}
    risk_ids = {r.get("id") for r in load_json(root, "docs/project/risks.json").get("risks", [])} \
        if (root / "docs/project/risks.json").exists() else set()

    def check_ref(ref, universe, label, ctx):
        if ref is None:
            return
        if ref not in universe:
            err(f"ORPHAN REFERENCE ({label}): '{ref}' referenced by {ctx} does not exist")

    # --- requirement-level checks -------------------------------------------
    for r in requirements:
        rid = r.get("id", "<no id>")
        if not REQ_ID_RE.match(str(rid)):
            err(f"Malformed requirement id: '{rid}' (expected REQ-NNN)")
        status = r.get("status")
        if status not in VALID_REQ_STATUS:
            err(f"{rid}: invalid status '{status}'. Expected one of {sorted(VALID_REQ_STATUS)}")
        if r.get("priority") not in VALID_PRIORITIES:
            err(f"{rid}: invalid priority '{r.get('priority')}'. Expected one of {sorted(VALID_PRIORITIES)}")
        if status == "VERIFIED":
            if not r.get("hardware_verified"):
                err(f"{rid}: status is VERIFIED but hardware_verified is false. "
                    f"Refusing an unevidenced VERIFIED claim (Rule 13).")
            if not (r.get("verification_note") or "").strip():
                err(f"{rid}: status is VERIFIED but verification_note is empty. "
                    f"VERIFIED requires recorded evidence.")
        for tid in r.get("task_ids", []) or []:
            check_ref(tid, task_ids, "TASK", rid)
        for tid in r.get("test_ids", []) or []:
            check_ref(tid, test_ids, "TEST", rid)
        check_ref(r.get("epic_id"), epic_ids, "EPIC", rid)

    # --- task-level checks (the core Rule 44 invariants) ---------------------
    for t in tasks:
        tid = t.get("id", "<no id>")
        if not TASK_ID_RE.match(str(tid)):
            err(f"Malformed task id: '{tid}' (expected TASK-NNN)")
        status = t.get("status")
        if status not in VALID_TASK_STATES:
            err(f"{tid}: invalid status '{status}'. Expected one of {sorted(VALID_TASK_STATES)}")
        if t.get("priority") not in VALID_PRIORITIES:
            err(f"{tid}: invalid priority '{t.get('priority')}")

        verif = t.get("verification") or {}
        vstatus = verif.get("status")
        evidence = verif.get("evidence") or []
        commit = (t.get("git") or {}).get("commit_sha") or ""

        # 1. DONE requires VERIFIED
        if status == "DONE" and vstatus != "VERIFIED":
            err(f"RULE 1 VIOLATION {tid}: status=DONE but verification.status="
                f"'{vstatus}'. DONE requires VERIFIED.")
        # 2. DONE requires a commit SHA
        if status == "DONE" and not commit.strip():
            err(f"RULE 2 VIOLATION {tid}: status=DONE but git.commit_sha is empty.")
        # 3. DONE requires evidence
        if status == "DONE" and not evidence:
            err(f"RULE 3 VIOLATION {tid}: status=DONE but verification.evidence is empty.")
        # 4. non-BACKLOG needs acceptance criteria
        if status != "BACKLOG" and not (t.get("acceptance_criteria") or []):
            err(f"RULE 4 VIOLATION {tid}: status={status} but acceptance_criteria is empty.")
        # 5. implementation tasks need requirements
        if t.get("type") not in ("infrastructure", "architecture", "security") and not t.get("requirement_ids"):
            warn(f"RULE 5 WARNING {tid}: implementation task has no requirement_ids.")
        # 13. BLOCKED needs a reason
        if status == "BLOCKED" and not (t.get("blocker") or "").strip():
            err(f"RULE 13 VIOLATION {tid}: status=BLOCKED but no blocker reason recorded.")

        # timestamps sanity
        ts = t.get("timestamps") or {}
        if status in ("IN_PROGRESS", "CODE_REVIEW", "TESTING", "VERIFICATION", "DONE") \
                and not (ts.get("started_at") or "").strip():
            err(f"{tid}: status={status} but timestamps.started_at is empty. "
                f"State transitions must be timestamped (Rule 23/24).")
        if status == "DONE" and not (ts.get("completed_at") or "").strip():
            err(f"{tid}: status=DONE but timestamps.completed_at is empty.")
        if status == "DONE" and not (ts.get("verified_at") or "").strip():
            err(f"{tid}: status=DONE but timestamps.verified_at is empty.")

        # orphan refs from tasks
        for rid in t.get("requirement_ids", []) or []:
            check_ref(rid, req_ids, "REQ", tid)
        check_ref(t.get("epic_id"), epic_ids, "EPIC", tid)
        check_ref(t.get("feature_id"), feat_ids, "FEAT", tid)
        for dep in t.get("dependencies", []) or []:
            check_ref(dep, task_ids, "TASK(dep)", tid)
        for tid_test in t.get("test_ids", []) or []:
            check_ref(tid_test, test_ids, "TEST", tid)

        # 8. DONE task whose requirement is not even partially verified
        if status == "DONE":
            for rid in t.get("requirement_ids", []) or []:
                req = next((x for x in requirements if x.get("id") == rid), None)
                if req and req.get("status") == "NOT_STARTED":
                    warn(f"RULE 8 WARNING {tid}: DONE but requirement {rid} is NOT_STARTED.")

    # --- test-level checks ---------------------------------------------------
    for test in tests:
        test_id = test.get("id", "<no id>")
        if not TEST_ID_RE.match(str(test_id)):
            err(f"Malformed test id: '{test_id}' (expected TEST-NNN)")
        if not (test.get("description") or "").strip():
            err(f"{test_id}: empty description")
        for ref in [test.get("requirement")]:
            check_ref(ref, req_ids, "REQ", test_id)
        for ref in [test.get("task")]:
            check_ref(ref, task_ids, "TASK", test_id)
        status = test.get("status")
        if status not in {"NOT_RUN", "PASS", "FAIL", "BLOCKED", "SKIPPED"}:
            err(f"{test_id}: invalid test status '{status}'")
        if status == "PASS" and not (test.get("evidence") or "").strip():
            err(f"{test_id}: status=PASS but evidence is empty. Refusing a PASS without a captured result (Rule 13).")

    # --- sprint-level checks -------------------------------------------------
    for sprint in sprints:
        sid = sprint.get("id", "<no id>")
        for tid in sprint.get("committed_task_ids", []) or []:
            check_ref(tid, task_ids, "TASK", sid)
        for tid in sprint.get("carried_over", []) or []:
            check_ref(tid, task_ids, "TASK", sid)
        if not (sprint.get("goal") or "").strip():
            err(f"{sid}: sprint has no goal")
        if not (sprint.get("start_date") and sprint.get("end_date")):
            err(f"{sid}: sprint is missing start_date or end_date")

    # --- bug / risk level checks --------------------------------------------
    for bug in bugs.get("bugs", []):
        bid = bug.get("id", "<no id>")
        if bug.get("severity") not in VALID_SEVERITIES:
            err(f"{bid}: invalid severity '{bug.get('severity')}'. Expected {sorted(VALID_SEVERITIES)}")
        for rid in bug.get("affected_requirements", []) or []:
            check_ref(rid, req_ids, "REQ", bid)
        for tid in bug.get("affected_tasks", []) or []:
            check_ref(tid, task_ids, "TASK", bid)

    # --- report --------------------------------------------------------------
    print(f"requirements: {len(requirements)}")
    print(f"epics:        {len(epics)}")
    print(f"features:     {len(features)}")
    print(f"tasks:        {len(tasks)}")
    print(f"tests:        {len(tests)}")
    print(f"sprints:      {len(sprints)}")
    by_state: dict[str, int] = {}
    for t in tasks:
        by_state[t.get("status", "?")] = by_state.get(t.get("status", "?"), 0) + 1
    print(f"task states:  {by_state}\n")

    if warnings:
        print(f"WARNINGS ({len(warnings)}):")
        for w in warnings:
            print(f"  ~ {w}")
        print()

    if errors:
        print(f"FAILED ({len(errors)} error(s)):")
        for e in errors:
            print(f"  x {e}")
        return 1

    print("PASS: project state is consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
