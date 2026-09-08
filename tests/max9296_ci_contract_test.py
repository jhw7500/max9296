#!/usr/bin/env python3
"""Verify that source and contract changes execute the health suite in CI."""

from __future__ import annotations

import fnmatch
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "contract-test.yml"


def event_paths(workflow: str, event: str) -> tuple[str, ...]:
    event_match = re.search(
        rf"^  {re.escape(event)}:\s*$\n(?P<body>(?:^    .*\n|^\s*$)*)",
        workflow,
        re.MULTILINE,
    )
    if not event_match:
        return ()

    paths_match = re.search(
        r"^    paths:\s*$\n(?P<paths>(?:^      - .*\n)+)",
        event_match.group("body"),
        re.MULTILINE,
    )
    if not paths_match:
        return ()

    return tuple(
        line.split("-", 1)[1].strip().strip("'\"")
        for line in paths_match.group("paths").splitlines()
    )


def main() -> int:
    failures: list[str] = []
    try:
        workflow = WORKFLOW.read_text(encoding="utf-8")
    except OSError as error:
        print(f"FAIL: contract workflow is missing: {error}")
        return 1

    representative_changes = (
        "max9296.c",
        "max9296_exposure_policy.h",
        "Makefile",
        "tests/max9296_exposure_replay_binding_test.py",
        "tools/max9296_health_export.py",
    )
    for event in ("push", "pull_request"):
        patterns = event_paths(workflow, event)
        if not patterns:
            failures.append(f"{event} has no path trigger")
            continue
        for changed_path in representative_changes:
            if not any(
                fnmatch.fnmatchcase(changed_path, pattern) for pattern in patterns
            ):
                failures.append(f"{event} ignores {changed_path}")

    if "bash tests/run_health_tests.sh" not in workflow:
        failures.append("contract job does not execute the health suite")
    if "actions/checkout@" not in workflow:
        failures.append("contract job does not check out production sources")
    if "check-workflow-enabled" in workflow or "needs.check-enabled" in workflow:
        failures.append("contract job can be silently disabled by workflow-config")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1

    print("PASS: source and test changes execute the health suite in CI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
