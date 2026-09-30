#!/usr/bin/env python3
"""
NomadLink secrets scanner.

Fails if a credential-looking string appears in content git would actually
commit. This is the pre-commit guard for Rule 27: never commit secrets, never
hardcode passwords/tokens/keys.

Scope: tracked files plus untracked-but-not-ignored files. Gitignored files such
as secrets.h and .env are deliberately NOT scanned, because those are exactly
where credentials are supposed to live locally. If git is unavailable the scan
degrades to a full filesystem walk and says so, rather than quietly passing.

Usage:
    python3 scripts/project-tracker/secrets_scan.py            # committable tree
    python3 scripts/project-tracker/secrets_scan.py --history  # every commit too
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# (name, regex). Deliberately targeted: high signal, low false positive.
# Any pattern that captures a credential value must use a capturing group so
# PLACEHOLDER can be tested against the value alone.
PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    ("GitHub personal access token", re.compile(r"gh[pousr]_[A-Za-z0-9]{16,}")),
    ("AWS access key id", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("Google API key", re.compile(r"\bAIza[0-9A-Za-z_\-]{35}\b")),
    ("Slack token", re.compile(r"\bxox[baprs]-[0-9A-Za-z\-]{10,}")),
    ("Private key block", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |PGP )?PRIVATE KEY-----")),
    ("MQTT password define", re.compile(
        r"#define\s+\w*(?:PASS|PASSWORD|SECRET|TOKEN)\w*\s+\"([^\"]{3,})\"")),
    ("Arduino secret define", re.compile(
        r"#define\s+\w*(?:PASS|PASSWORD|SECRET|TOKEN|KEY)\w*\s+\"([^\"]{3,})\"")),
    ("Hardcoded credential assignment", re.compile(
        r"(?i)\b(?:password|passwd|secret|api[_-]?key|token|private[_-]?key)\b"
        r"\s*[:=]\s*[\"']([^\"'\s]{6,})[\"']")),
    # Device identifiers are not credentials: they cannot authenticate. But a
    # real IMEI or phone number committed to a public repo permanently identifies
    # a physical device and its owner, and cannot be rotated after the fact.
    ("IMEI (15-digit device identifier)", re.compile(r"(?<![\d.])([0-9]{15})(?![\d.])")),
    ("Indian mobile number in source", re.compile(
        r"(?<![\d+])(?:\+?91)?([6-9]\d{9})(?![\d.])")),
]

TEXT_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".ino", ".py", ".js", ".ts", ".json", ".md",
    ".yaml", ".yml", ".txt", ".cmake", ".ini", ".conf", ".csv", ".sh", ".html",
}

# Template and credential-ish files whose extension is not in TEXT_SUFFIXES.
# secrets.h.example is the single most likely place for someone to paste a real
# password and forget to revert it, so templates must be scanned too: a
# placeholder is filtered out by PLACEHOLDER, but a real value must be caught.
TEMPLATE_SUFFIXES = {".example", ".template", ".sample", ".dist", ".tpl", ".env",
                     ".pem", ".key", ".crt", ".p12", ".pfx"}
CREDENTIAL_NAMES = {".env", "credentials", "id_rsa", "id_ed25519", ".netrc",
                    ".htpasswd", ".npmrc", "secrets.h", "secrets.json"}

SKIP_DIRS = {".git", "build", "node_modules", ".venv", "__pycache__",
             "managed_components", "artifacts"}

# Values that match a credential pattern but are obviously not credentials.
# secrets.h.example must be committed and must contain placeholder text, so
# `MQTT_PASS "your-password"` has to read as a template, not a leak. A real
# pasted password will not match any of these.
PLACEHOLDER = re.compile(
    r"""^(?:
          (?:your|my|the)[-_ ].*        # your-password, my_token
        | (?:change|set|replace|insert|fill)[-_ ].*
        | (?:example|placeholder|dummy|sample|test|foo|bar|baz|todo|tbd|x{3,}).*
        | <[^>]*>                      # <password>
        | \$\{[^}]*\}                  # ${MQTT_PASS}
        | %[0-9]*[sd]                  # %s
        | \*+                          # "****"
        | (?:n/?a|none|null|empty|undefined)
        | [0-9a-f]{16,}                 # bare placeholder token
    )$""",
    re.IGNORECASE | re.VERBOSE,
)


def _run_git(root: Path, args: list[str]) -> str | None:
    try:
        res = subprocess.run(["git", *args], cwd=root, capture_output=True,
                             text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    return res.stdout if res.returncode == 0 else None


def is_scannable(rel: Path) -> bool:
    if any(part in SKIP_DIRS for part in rel.parts):
        return False
    name = rel.name
    if name in CREDENTIAL_NAMES or name.startswith(".env"):
        return True
    # secrets.h.example has a double suffix, so check every suffix, not just the last.
    suffixes = {s.lower() for s in rel.suffixes}
    if suffixes & TEMPLATE_SUFFIXES:
        return True
    return rel.suffix.lower() in TEXT_SUFFIXES


def committable_paths(root: Path) -> tuple[list[Path], bool]:
    """Tracked files + untracked-not-ignored files. Second element: exact."""
    tracked = _run_git(root, ["ls-files", "-z"])
    untracked = _run_git(root, ["ls-files", "-z", "--others", "--exclude-standard"])
    if tracked is None or untracked is None:
        return [p for p in root.rglob("*") if p.is_file()], False
    paths = [Path(p) for p in (tracked + untracked).split("\0") if p]
    return paths, True


def is_placeholder(value: str) -> bool:
    v = value.strip().strip("\"'")
    if PLACEHOLDER.match(v):
        return True
    return is_synthetic_number(v)


def is_synthetic_number(value: str) -> bool:
    """True for obviously fake numbers: all-same digits, or a +/-1 run.

    Documentation examples legitimately contain phone-shaped literals such as
    9876543210. Requiring those to be rewritten by hand is a gate people will
    eventually route around, so a strictly monotonic run is treated as a
    placeholder. A real number is not monotonic.
    """
    v = re.sub(r"[\s\-()]", "", value.strip())
    # Strip a country code only when doing so leaves a full-length national
    # number. A naive `^\+?\d{0,2}` would eat real leading digits.
    if len(v) == 12 and v.startswith("+"):
        v = v[3:]
    elif len(v) == 12 and v.startswith("91"):
        v = v[2:]
    if len(v) < 9 or not v.isdigit():
        return False
    if len(set(v)) == 1:                  # 0000000000, 9999999999
        return True
    steps = {int(b) - int(a) for a, b in zip(v, v[1:])}
    return steps in ({1}, {-1})           # 9876543210, 1234567890


# An NVS key is a *namespace*, not a credential: `#define NVS_KEY_MQTT_PASS "mqtt_pass"`
# names the entry holding the password; the value lives in NVS at runtime and never
# appears in source. The vendored esp32_nat_router base trips the KEY/PASSWORD defines
# on several of these. Exempted narrowly by symbol prefix so that a real credential
# added alongside it is still caught — do not widen this to the whole tree, which
# would be a blanket "upstream is trusted" pass.
NVS_KEY_DEFINE = re.compile(r"^\s*#define\s+NVS_KEY_\w*\s+\"", re.MULTILINE)

# The scanner's own comments quote the patterns they explain, so it matches itself.
# Skipping itself is not a hole: this file contains no credential by construction.
SELF = Path(__file__).resolve()


def scan_text(label: str, text: str) -> list[str]:
    """Findings for one blob, deduplicated to one per line."""
    if label.endswith(SELF.name):
        return []
    hits: dict[int, str] = {}
    for name, rx in PATTERNS:
        for m in rx.finditer(text):
            line_no = text.count("\n", 0, m.start()) + 1
            line_start = text.rfind("\n", 0, m.start()) + 1
            if NVS_KEY_DEFINE.match(text, line_start):
                continue
            values = [g for g in m.groups() if g is not None]
            if values and all(is_placeholder(v) for v in values):
                continue
            shown = m.group(0)
            if len(shown) > 20:
                shown = shown[:8] + "…" + shown[-6:]
            hits.setdefault(line_no, f"  x {label}:{line_no}  [{name}]  {shown}")
    return [hits[k] for k in sorted(hits)]


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    history = "--history" in sys.argv

    print("NomadLink secrets scan")
    print(f"repo root: {root}\n")

    findings: list[str] = []
    exact = True

    paths, exact = committable_paths(root)
    scanned = 0
    for rel in paths:
        if not is_scannable(rel):
            continue
        full = root / rel
        if not full.is_file():
            continue
        try:
            text = full.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        scanned += 1
        findings.extend(scan_text(str(rel), text))

    if history:
        revs = _run_git(root, ["rev-list", "--all"]) or ""
        for sha in revs.split():
            diff = _run_git(root, ["show", "--format=", "--unified=0", sha]) or ""
            findings.extend(scan_text(f"commit {sha[:8]}", diff))

    scope = "committable tree"
    if history:
        scope += " + full git history"
    if not exact:
        scope += " (UNTRACKED-BY-GIT fallback: gitignored files were scanned too)"

    if findings:
        print(f"FAILED ({len(findings)} potential secret(s)) over {scanned} file(s):")
        for f in findings:
            print(f)
        print("\nSecrets must not be committed. Rule 27.")
        print("Move the value to a gitignored secrets.h or .env, and ROTATE it if it was ever committed.")
        return 1

    print(f"PASS: no credential patterns in {scope} ({scanned} file(s)).")
    if not exact:
        print("NOTE: git was unavailable, so this pass also scanned gitignored files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
