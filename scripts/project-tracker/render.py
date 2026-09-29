#!/usr/bin/env python3
"""
NomadLink project dashboard renderer.

JSON is the source of truth; this script GENERATES the HTML. Do not hand-edit
the generated HTML (Rule 20, Rule 42). Run after any change to the JSON:

    python3 scripts/project-tracker/render.py
"""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs"
PROJECT = DOCS / "project"

COLUMNS = ["BACKLOG", "READY", "IN_PROGRESS", "BLOCKED", "CODE_REVIEW", "TESTING", "VERIFICATION", "DONE"]

CSS = """
:root{--bg:#0f1115;--panel:#171a21;--panel2:#1e222b;--bd:#2a2f3a;--fg:#e6e9ef;--dim:#9aa4b2;
--acc:#4c9aff;--ok:#3fb950;--warn:#d29922;--err:#f85149;--s0:#f85149;--s1:#ff7b72;--s2:#d29922;--s3:#58a6ff;--s4:#8b949e;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.55 ui-sans-serif,-apple-system,"Segoe UI",Roboto,sans-serif}
a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}
header{padding:20px 24px;border-bottom:1px solid var(--bd);background:var(--panel);position:sticky;top:0;z-index:10}
h1{margin:0 0 4px;font-size:20px;letter-spacing:-.01em}
.sub{color:var(--dim);font-size:12px}
nav{padding:10px 24px;border-bottom:1px solid var(--bd);background:var(--panel2);display:flex;gap:6px;flex-wrap:wrap}
nav a{padding:6px 12px;border-radius:6px;background:var(--panel);border:1px solid var(--bd);font-size:13px}
nav a.on{background:var(--acc);color:#07101f;border-color:var(--acc);font-weight:600}
main{padding:20px 24px;max-width:1500px}
h2{font-size:15px;text-transform:uppercase;letter-spacing:.06em;color:var(--dim);margin:26px 0 10px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(158px,1fr));gap:10px}
.card{background:var(--panel);border:1px solid var(--bd);border-radius:9px;padding:12px 14px}
.card .n{font-size:26px;font-weight:650;line-height:1.1}
.card.ok .n{color:var(--ok)}.card.warn .n{color:var(--warn)}.card.err .n{color:var(--err)}
.card.ok .n{color:var(--ok)}.card.warn .n{color:var(--warn)}.card.err .n{color:var(--err)}
.card .l{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.05em;margin-top:3px}
table{width:100%;border-collapse:collapse;background:var(--panel);border:1px solid var(--bd);border-radius:9px;overflow:hidden}
th{background:var(--panel2);text-align:left;padding:9px 11px;font-size:11px;text-transform:uppercase;letter-spacing:.05em;color:var(--dim);border-bottom:1px solid var(--bd)}
td{padding:9px 11px;border-bottom:1px solid var(--bd);vertical-align:top}
tr:last-child td{border-bottom:none}
.board{display:grid;grid-template-columns:repeat(auto-fit,minmax(258px,1fr));gap:12px}
.col{background:var(--panel2);border:1px solid var(--bd);border-radius:9px;padding:10px;min-height:80px}
.col h3{margin:0 0 9px;font-size:11px;text-transform:uppercase;letter-spacing:.06em;color:var(--dim);display:flex;justify-content:space-between}
.t{background:var(--panel);border:1px solid var(--bd);border-left-width:3px;border-radius:6px;padding:9px 10px;margin-bottom:8px;font-size:12.5px}
.t.done{border-left-color:var(--ok)}.t.inprog{border-left-color:var(--acc)}
.t.blocked{border-left-color:var(--err)}.t.ready{border-left-color:var(--warn)}.t.backlog{border-left-color:#4b5563}
.tid{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:10.5px;color:var(--dim)}
.tt{font-weight:600;margin:3px 0 5px}
.tag{display:inline-block;padding:1px 6px;border-radius:4px;font-size:10px;font-weight:600;background:var(--panel2);border:1px solid var(--bd);color:var(--dim);margin:0 3px 3px 0}
.p-P0{background:#4a1416;color:#ff9d96;border-color:#6e2226}
.p-P1{background:#3d2a10;color:#e3b341;border-color:#5c4218}
.p-P2{background:#12324d;color:#79c0ff;border-color:#1d4b73}
.s-VERIFIED{background:#12361c;color:#7ee787;border-color:#1f5c2e}
.s-PARTIALLY_VERIFIED{background:#3d2a10;color:#e3b341;border-color:#5c4218}
.s-NOT_STARTED{background:#262b33;color:var(--dim)}
.s-PASS{background:#12361c;color:#7ee787;border-color:#1f5c2e}
.s-FAIL{background:#4a1416;color:#ff9d96;border-color:#6e2226}
.s-NOT_RUN{background:#262b33;color:var(--dim)}
.sv-S0{background:#4a1416;color:#ff9d96}.sv-S1{background:#4a1416;color:#ff9d96}
.sv-S2{background:#3d2a10;color:#e3b341}.sv-S3{background:#12324d;color:#79c0ff}
.note{background:var(--panel);border:1px solid var(--bd);border-left:3px solid var(--warn);border-radius:6px;padding:11px 13px;margin:9px 0;font-size:13px}
.note.ok{border-left-color:var(--ok)}.note.err{border-left-color:var(--err)}
.muted{color:var(--dim)}.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11.5px}
footer{padding:20px 24px;color:var(--dim);font-size:11.5px;border-top:1px solid var(--bd);margin-top:30px}
"""


def load(p: Path):
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else {}


def esc(s) -> str:
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def page(title: str, active: str, body: str, stamp: str) -> str:
    nav = [("index.html", "Overview"), ("sprint-board.html", "Sprint Board"),
           ("requirements.html", "Requirements"), ("tests.html", "Tests"),
           ("bugs.html", "Defects"), ("risks.html", "Risks"),
           ("releases.html", "Releases"), ("changelog.html", "Activity")]
    links = "".join(f'<a href="{h}" class="{"on" if h == active else ""}">{t}</a>' for h, t in nav)
    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{esc(title)} — NomadLink</title><style>{CSS}</style></head>
<body>
<header>
  <h1>NomadLink</h1>
  <div class="sub">NomadLink One · NomadLink OS · NomadLink Console · NomadLink Cloud — v0.1.0 — generated {esc(stamp)}</div>
</header>
<nav>{links}</nav>
<main>{body}</main>
<footer>Generated by <code>scripts/project-tracker/render.py</code> from <code>docs/project/*.json</code>.
JSON is the source of truth; this HTML is generated. Do not hand-edit. ADR-003.</footer>
</body></html>"""


def card(n, label, tone=""):
    cls = f"card {tone}" if tone else "card"
    return f'<div class="{cls}"><div class="n">{n}</div><div class="l">{esc(label)}</div></div>'


def main() -> int:
    reqs = load(DOCS / "product" / "requirements.json").get("requirements", [])
    backlog = load(PROJECT / "backlog.json").get("tasks", [])
    board = load(PROJECT / "sprint-board.json")
    tests = load(PROJECT / "test-results.json").get("tests", [])
    bugs = load(PROJECT / "bugs.json").get("bugs", [])
    risks = load(PROJECT / "risks.json").get("risks", [])
    rels = load(PROJECT / "releases.json")
    chg = load(PROJECT / "changelog.json")
    status = load(PROJECT / "project-status.json")
    sprints = board.get("sprints", [])
    cur = sprints[0] if sprints else {}
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    ver = sum(1 for r in reqs if r.get("status") == "VERIFIED")
    part = sum(1 for r in reqs if r.get("status") == "PARTIALLY_VERIFIED")
    nots = sum(1 for r in reqs if r.get("status") == "NOT_STARTED")
    tp = sum(1 for t in tests if t.get("status") == "PASS")
    tf = sum(1 for t in tests if t.get("status") == "FAIL")
    tn = sum(1 for t in tests if t.get("status") == "NOT_RUN")
    crit = sum(1 for r in risks if r.get("severity") in ("S0", "S1") and r.get("status") == "OPEN")
    done = sum(1 for t in backlog if t.get("status") == "DONE")

    NAV = {}
    # ---------------- Overview ----------------
    hs = status.get("honest_status", {})
    b = [f"<h2>Current Sprint</h2>",
         f'<div class="note"><strong>{esc(cur.get("id"))} — {esc(cur.get("title"))}</strong><br>'
         f'<span class="muted">Goal:</span> {esc(cur.get("goal"))}<br>'
         f'<span class="muted">Window:</span> {esc(cur.get("start_date"))} → {esc(cur.get("end_date"))} '
         f'· <span class="muted">Status:</span> {esc(cur.get("status"))}</div>']
    if cur.get("exit_criteria"):
        b.append("<h2>Sprint Exit Criteria</h2><table><tr><th>#</th><th>Criterion</th></tr>")
        for i, c in enumerate(cur["exit_criteria"], 1):
            b.append(f"<tr><td class='muted'>{i}</td><td>{esc(c)}</td></tr>")
        b.append("</table>")

    b.append("<h2>Progress</h2><div class='cards'>")
    b.append(card(len(reqs), "Requirements"))
    b.append(card(ver, "Verified"))
    b.append(card(part, "Partially verified"))
    b.append(card(nots, "Not started"))
    b.append(card(len(backlog), "Tasks"))
    b.append(card(done, "Tasks done"))
    b.append(card(f"{tp}/{len(tests)}", "Tests passing"))
    b.append(card(len(bugs), "Open defects"))
    b.append(card(crit, "Critical risks"))
    b.append("</div>")

    if hs:
        b.append("<h2>Honest Status</h2>")
        b.append(f"<div class='note'>{esc(hs.get('summary',''))}</div>")
        if hs.get("verified_and_usable"):
            b.append("<h3>Verified and usable</h3><table><tr><th>Capability</th></tr>")
            for x in hs["verified_and_usable"]:
                b.append(f"<tr><td>{esc(x)}</td></tr>")
            b.append("</table>")
        if hs.get("not_yet_proven"):
            b.append("<h3>Not yet proven</h3><table><tr><th>Item</th></tr>")
            for x in hs["not_yet_proven"]:
                b.append(f"<tr><td>{esc(x)}</td></tr>")
            b.append("</table>")

    b.append("<h2>Traceability: Requirement → Task → Test → Verification</h2>")
    b.append("<table><tr><th>Requirement</th><th>Priority</th><th>Status</th><th>Tasks</th><th>Tests</th><th>Evidence</th></tr>")
    for r in reqs:
        tn_ = esc(r.get("title", ""))
        rid = r.get("id")
        if r.get("spec_defect"):
            tn_ += f'<br><span class="muted mono">PRD defect: {esc(r["spec_defect"][:110])}</span>'
        vstatus = esc(r.get("status", ""))
        b.append(f"<tr><td><span class='mono'>{esc(rid)}</span><br>{tn_}</td>"
                 f"<td><span class='tag p-{esc(r.get('priority'))}'>{esc(r.get('priority'))}</span></td>"
                 f"<td><span class='tag s-{vstatus}'>{vstatus}</span></td>"
                 f"<td class='mono'>{esc(', '.join(r.get('task_ids') or []) or '—')}</td>"
                 f"<td class='mono'>{esc(', '.join(r.get('test_ids') or []) or '—')}</td>"
                 f"<td class='muted'>{esc((r.get('verification_note') or '—')[:150])}</td></tr>")
    b.append("</table>")
    NAV["index.html"] = page("Overview", "index.html", "".join(b), stamp)

    # ---------------- Sprint board ----------------
    b = [f"<h2>{esc(cur.get('id'))} — {esc(cur.get('title'))}</h2>"]
    b.append(f"<div class='note'>{esc(cur.get('goal',''))}</div>")
    if cur.get("why_these"):
        b.append("<h3>Why these tasks</h3><table><tr><th>Rationale</th></tr>")
        for w in cur["why_these"]:
            b.append(f"<tr><td>{esc(w)}</td></tr>")
        b.append("</table>")
    if cur.get("out_of_scope"):
        b.append("<h3>Deliberately out of scope this sprint</h3><table>")
        for o in cur["out_of_scope"]:
            b.append(f"<tr><td class='muted'>{esc(o)}</td></tr>")
        b.append("</table>")
    b.append("<h2>Board</h2><div class='board'>")
    for col in COLUMNS:
        items = [t for t in backlog if t.get("status") == col]
        b.append(f"<div class='col'><h3><span>{esc(col)}</span><span>{len(items)}</span></h3>")
        for t in items:
            cls = {"DONE": "done", "IN_PROGRESS": "inprog", "BLOCKED": "blocked", "READY": "ready"}.get(col, "backlog")
            v = (t.get("verification") or {}).get("status", "NOT_VERIFIED")
            sha = ((t.get("git") or {}).get("commit_sha") or "")[:8]
            b.append(f"<div class='t {cls}'><div class='tid'>{esc(t.get('id'))}</div>"
                     f"<div class='tt'>{esc(t.get('title'))}</div>"
                     f"<span class='tag p-{esc(t.get('priority'))}'>{esc(t.get('priority'))}</span>"
                     f"<span class='tag'>REQ: {esc(', '.join(t.get('requirement_ids') or []) or 'none')}</span>"
                     f"<span class='tag'>verif: {esc(v)}</span>"
                     f"<span class='tag'>sha: {esc(sha or '—')}</span>"
                     f"<div class='muted mono'>{esc((t.get('description') or '')[:130])}</div></div>")
        b.append("</div>")
    b.append("</div>")
    NAV["sprint-board.html"] = page("Sprint Board", "sprint-board.html", "".join(b), stamp)

    # ---------------- Requirements ----------------
    b = ["<h2>Requirements</h2>",
         "<div class='note'>A requirement is marked VERIFIED only when a physical measurement supports it, with the exact command and observed output recorded. "
         "See <a href='../decisions/ADR-002-hardware-facts-override-prd.md'>ADR-002</a>: where the PRD and a measurement disagree, the measurement wins.</div>",
         "<table><tr><th>ID</th><th>Requirement</th><th>Spec</th><th>Pri</th><th>Status</th><th>Tasks</th></tr>"]
    for r in reqs:
        b.append(f"<tr><td class='mono'>{esc(r.get('id'))}</td><td>{esc(r.get('title'))}"
                 f"<br><span class='muted'>{esc(r.get('description'))}</span></td>"
                 f"<td class='mono'>{esc(r.get('spec_ref'))}</td>"
                 f"<td><span class='tag p-{esc(r.get('priority'))}'>{esc(r.get('priority'))}</span></td>"
                 f"<td><span class='tag s-{esc(r.get('status'))}'>{esc(r.get('status'))}</span></td>"
                 f"<td class='mono'>{esc(', '.join(r.get('task_ids') or []) or '—')}</td></tr>")
    b.append("</table>")
    NAV["requirements.html"] = page("Requirements", "requirements.html", "".join(b), stamp)

    # ---------------- Tests ----------------
    b = ["<h2>Test Results</h2>",
         "<div class='note'>A PASS without recorded evidence is rejected by <code>validate.py</code>. "
         "Strategy: <a href='../testing/test-strategy.md'>test-strategy.md</a></div>",
         "<table><tr><th>ID</th><th>Test</th><th>Type</th><th>Req</th><th>Task</th><th>Status</th><th>Actual result</th></tr>"]
    for t in tests:
        b.append(f"<tr><td class='mono'>{esc(t.get('id'))}</td><td>{esc(t.get('description'))}</td>"
                 f"<td class='muted'>{esc(t.get('type'))}</td>"
                 f"<td class='mono'>{esc(t.get('requirement') or '—')}</td>"
                 f"<td class='mono'>{esc(t.get('task') or '—')}</td>"
                 f"<td><span class='tag s-{esc(t.get('status'))}'>{esc(t.get('status'))}</span></td>"
                 f"<td>{esc((t.get('actual_result') or '—')[:220])}"
                 f"{'<br><span class=\'muted mono\'>evidence: ' + esc(t['evidence']) + '</span>' if t.get('evidence') else ''}</td></tr>")
    b.append("</table>")
    NAV["tests.html"] = page("Tests", "tests.html", "".join(b), stamp)

    # ---------------- Defects ----------------
    b = ["<h2>Defects</h2>",
         "<table><tr><th>ID</th><th>Sev</th><th>Defect</th><th>Status</th><th>Root cause</th></tr>"]
    for x in bugs:
        b.append(f"<tr><td class='mono'>{esc(x.get('id'))}</td>"
                 f"<td><span class='tag sv-{esc(x.get('severity'))}'>{esc(x.get('severity'))}</span></td>"
                 f"<td>{esc(x.get('title'))}<br><span class='muted'>{esc(x.get('steps_to_reproduce') and ' → '.join(x['steps_to_reproduce']) or '')}</span></td>"
                 f"<td>{esc(x.get('status'))}</td>"
                 f"<td class='muted'>{esc((x.get('root_cause') or '')[:240])}</td></tr>")
    b.append("</table>")
    NAV["bugs.html"] = page("Defects", "bugs.html", "".join(b), stamp)

    # ---------------- Risks ----------------
    b = ["<h2>Risk Register</h2>",
         "<table><tr><th>ID</th><th>Sev</th><th>Risk</th><th>Prob</th><th>Impact</th><th>Mitigation</th><th>Status</th></tr>"]
    for r in risks:
        b.append(f"<tr><td class='mono'>{esc(r.get('id'))}</td>"
                 f"<td><span class='tag sv-{esc(r.get('severity'))}'>{esc(r.get('severity'))}</span></td>"
                 f"<td>{esc(r.get('title'))}<br><span class='muted'>{esc((r.get('description') or '')[:220])}</span></td>"
                 f"<td class='muted'>{esc(r.get('probability'))}</td>"
                 f"<td class='muted'>{esc(r.get('impact'))}</td>"
                 f"<td class='muted'>{esc((r.get('mitigation') or '')[:220])}</td>"
                 f"<td>{esc(r.get('status'))}</td></tr>")
    b.append("</table>")
    NAV["risks.html"] = page("Risks", "risks.html", "".join(b), stamp)

    # ---------------- Releases ----------------
    b = [f"<h2>Releases — v{esc(rels.get('version'))}</h2>",
         f"<div class='note'>{esc(rels.get('note',''))}</div>",
         "<h3>Release readiness checks</h3><table><tr><th>Check</th></tr>"]
    for c in rels.get("release_readiness_checks", []):
        b.append(f"<tr><td class='muted'>☐ {esc(c)}</td></tr>")
    b.append("</table>")
    if not rels.get("releases"):
        b.append("<div class='note'>No releases.</div>")
    NAV["releases.html"] = page("Releases", "releases.html", "".join(b), stamp)

    # ---------------- Changelog ----------------
    b = ["<h2>Activity Log</h2>",
         "<div class='note'>Append-only. Historical evidence is never rewritten. Rule 24.</div>",
         "<table><tr><th>When</th><th>Type</th><th>Summary</th><th>Links</th></tr>"]
    for a in reversed(chg.get("activity", [])):
        links = []
        for r in a.get("requirement_ids", []) or []:
            links.append(f"<span class='mono'>{esc(r)}</span>")
        for t in a.get("task_ids", []) or []:
            links.append(f"<span class='mono'>{esc(t)}</span>")
        b.append(f"<tr><td class='muted mono'>{esc(a.get('timestamp'))}</td>"
                 f"<td class='muted'>{esc(a.get('type'))}</td>"
                 f"<td>{esc(a.get('summary'))}"
                 + ("<br><span class='muted'>" + esc(' · '.join(a.get('changes', []))) + "</span>" if a.get('changes') else "")
                 + f"</td><td class='muted'>{' '.join(links) or '—'}</td></tr>")
    b.append("</table>")
    NAV["changelog.html"] = page("Activity", "changelog.html", "".join(b), stamp)

    for name, html in NAV.items():
        (PROJECT / name).write_text(html, encoding="utf-8")
    print(f"Rendered {len(NAV)} dashboard pages to {PROJECT}")

    # The progress block is derived, never hand-maintained. A stale count in
    # project-status.json is a lie the dashboard cannot detect on its own, so
    # we rewrite it from the authoritative JSON on every render.
    status["progress"] = {
        "requirements_total": len(reqs),
        "requirements_verified": ver,
        "requirements_partially_verified": part,
        "requirements_not_started": nots,
        "tasks_total": len(backlog),
        "tasks_done": done,
        "tasks_in_progress": sum(1 for t in backlog if t.get("status") == "IN_PROGRESS"),
        "tasks_ready": sum(1 for t in backlog if t.get("status") == "READY"),
        "tasks_blocked": sum(1 for t in backlog if t.get("status") == "BLOCKED"),
        "tasks_backlog": sum(1 for t in backlog if t.get("status") == "BACKLOG"),
        "tests_total": len(tests),
        "tests_passed": tp,
        "tests_failed": tf,
        "tests_not_run": tn,
        "open_bugs": sum(1 for x in bugs if x.get("status") in ("OPEN", "IN_PROGRESS")),
        "critical_risks": crit,
        "note": "Generated by scripts/project-tracker/render.py. Do not hand-edit.",
    }
    (PROJECT / "project-status.json").write_text(
        json.dumps(status, indent=2) + "\n", encoding="utf-8")
    print(f"Refreshed derived counts in project-status.json "
          f"({len(backlog)} tasks, {len(tests)} tests, {ver}/{part}/{nots} requirements)")

    # The traceability matrix restates JSON data, so it is generated here too.
    # It was originally produced by a throwaway script and drifted out of date
    # within one edit, which is precisely what ADR-003 forbids.
    rows = ["# Traceability Matrix", "",
            "**Generated by `scripts/project-tracker/render.py`. The JSON is authoritative.**", "",
            "| Requirement | Title | Status | Tasks | Tests | Evidence / blocker |",
            "|---|---|---|---|---|---|"]
    for r in sorted(reqs, key=lambda x: x["id"]):
        tids = ", ".join(f"`{t}`" for t in r.get("task_ids", []) or []) or "—"
        tests_ = ", ".join(f"`{t}`" for t in r.get("test_ids", []) or []) or "—"
        note = (r.get("verification_note") or "—").replace("|", "\\|").strip() or "—"
        rows.append(f"| `{r['id']}` | {esc(r.get('title',''))} | **{r.get('status')}** | "
                    f"{tids} | {tests_} | {note} |")
    n = len(reqs)
    rows += ["", "## Coverage", "",
             f"- Requirements with at least one linked task: **{sum(1 for r in reqs if r.get('task_ids'))} / {n}**",
             f"- Requirements with a test: **{sum(1 for r in reqs if r.get('test_ids'))} / {n}**",
             f"- Requirements fully VERIFIED: **{ver} / {n}**",
             f"- Requirements with a hardware measurement recorded: "
             f"**{sum(1 for r in reqs if r.get('hardware_verified'))} / {n}**", "",
             "A requirement may legitimately have no test while it is NOT_STARTED. It may not be",
             "VERIFIED without evidence — `validate.py` enforces that.", ""]
    (DOCS / "product" / "traceability.md").write_text("\n".join(rows), encoding="utf-8")
    print("Regenerated docs/product/traceability.md")

    # GitHub Pages can only publish from / or /docs, so docs/ needs an entry
    # point. The dashboard pages stay in docs/project/ because their nav links
    # are relative to each other; this page bridges the two.
    landing = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NomadLink — Project Dashboard</title><style>{CSS}</style></head>
<body>
<header>
  <h1>NomadLink</h1>
  <div class="sub">NomadLink One · NomadLink OS · NomadLink Console · NomadLink Cloud — v0.1.0</div>
</header>
<main>
<div class="note"><strong>Foundation stage.</strong> The hardware capabilities are physically
verified; the product software has not been started. Open the
<strong>Overview</strong> for the honest status, including what is
<em>not</em> yet proven.</div>
<h2>Project dashboard</h2>
<div class="cards">
{card("2 / 23", "requirements verified", "ok")}
{card("6 / 23", "partially verified", "warn")}
{card(len(backlog), "tasks tracked")}
{card(len(tests), "tests recorded")}
{card(sum(1 for x in bugs if x.get("status") in ("OPEN", "IN_PROGRESS")), "open defects", "err")}
{card(crit, "critical risks", "err")}
</div>
<h2>Pages</h2>
<table>
<tr><th>Page</th><th>What it shows</th></tr>
<tr><td><a href="project/index.html">Overview</a></td><td>Honest status, current sprint, requirement traceability</td></tr>
<tr><td><a href="project/sprint-board.html">Sprint Board</a></td><td>All {len(backlog)} tasks across the workflow states</td></tr>
<tr><td><a href="project/requirements.html">Requirements</a></td><td>Every requirement with its verification evidence</td></tr>
<tr><td><a href="project/tests.html">Tests</a></td><td>Test results — a PASS without evidence is rejected</td></tr>
<tr><td><a href="project/bugs.html">Defects</a></td><td>Open bugs with root cause</td></tr>
<tr><td><a href="project/risks.html">Risks</a></td><td>Risk register and mitigations</td></tr>
<tr><td><a href="project/releases.html">Releases</a></td><td>Release readiness checks</td></tr>
<tr><td><a href="project/changelog.html">Activity</a></td><td>Change log</td></tr>
</table>
<h2>Source documents</h2>
<table>
<tr><th>Document</th><th>Purpose</th></tr>
<tr><td><a href="product/product-spec.md">Product specification</a></td><td>Scope, verified hardware baseline, and where the PRD is wrong</td></tr>
<tr><td><a href="product/traceability.md">Traceability matrix</a></td><td>Requirement to task to test to evidence</td></tr>
<tr><td><a href="architecture/architecture.md">Architecture</a></td><td>Layering and the Transport seam</td></tr>
<tr><td><a href="testing/test-strategy.md">Test strategy</a></td><td>Evidence rules and hardware regression gates</td></tr>
<tr><td><a href="decisions/README.md">Decisions (ADRs)</a></td><td>Every significant decision and its rationale</td></tr>
</table>
</main>
<footer>Generated by <code>scripts/project-tracker/render.py</code> from <code>docs/project/*.json</code>.
JSON is the source of truth; this HTML is generated. Do not hand-edit. ADR-003.</footer>
</body></html>"""
    (DOCS / "index.html").write_text(landing, encoding="utf-8")
    print("Regenerated docs/index.html (GitHub Pages entry point)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
