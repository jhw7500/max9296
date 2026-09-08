#!/usr/bin/env python3
"""Static source checks for the dual-pair HINF verdict in max9296.c.

The pure decisions in max9296_pair_health.h are covered by a host C test.  The
composition in max9296_pair_verdict_locked() and its caller is not, and cannot
be: no host test instantiates a struct max9296_dev.  That gap is where the defects this file
guards actually lived - the header refused a two-hour interval correctly all
along while the driver narrowed the caller's delta before handing it over, and
the verdict was later found timing one interval while comparing another.  So the
invariants below are asserted against the source text, following the same
convention as max9296_prepare_test.py.

These checks are anchored to identifiers and to block structure, not to whole
statements, but they are still source-text checks: renaming `delta_ms`, `now_ms`
or the gap enumerators reddens this file even though behaviour is unchanged.
That is the deliberate trade - a rename is a deliberate act and updating the
anchor with it is cheap, whereas the defects above were each invisible to every
executable test in the tree.  Two earlier revisions of this file were weaker than
they read: an ordering comparison passed while the assignment it guarded moved
out of its branch entirely, and a window that started after the declaration
missed the assignment on the line before it.  Two more were weaker than they
read for a different reason - they matched a token where they meant a family, so
a pr_warn() in the locked path and an unterminated format beside a decoy literal
both stayed green.  All four are now structural.

The scan is deliberately raw-source: an earlier revision stripped comments first,
and the guard that was supposed to make that safe could be desynchronised by a
stray quote inside a comment, hiding a real call.  The cost is an authoring rule -
prose inside max9296_collect_health_locked() or max9296_pair_verdict_locked() must
not name a logging function in call syntax, or the logging check reports that the
code logs under the lock when it does not.  Name it without parentheses.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DRIVER = REPO / "max9296.c"

checks = 0
failures: list[str] = []


def check(condition: bool, label: str) -> None:
    global checks
    checks += 1
    if condition:
        print(f"  OK   {label}")
    else:
        failures.append(label)
        print(f"  FAIL {label}")


def _close(source: str, opening: int, what: str) -> str:
    depth = 1
    pos = opening + 1
    while pos < len(source) and depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    if depth:
        raise SystemExit(f"unterminated block: {what}")
    return source[opening + 1 : pos - 1]


def enclosing_block(source: str, at: int) -> str:
    """Body of the innermost brace-delimited block containing offset `at`.

    A previous revision deleted this as a one-caller helper.  It is back because
    the lifecycle contract needs a scope tighter than the function:
    max9296_s_stream() holds both the start and the stop path, so a per-function
    check stays green when the stop path alone loses its forget.
    """
    depth = 0
    pos = at
    while pos > 0:
        pos -= 1
        if source[pos] == "}":
            depth += 1
        elif source[pos] == "{":
            if not depth:
                return _close(source, pos, "enclosing block")
            depth -= 1
    raise SystemExit("no enclosing block")


def braced_block(source: str, marker: str) -> str:
    """Return the body of the brace-delimited block that `marker` opens."""
    at = source.find(marker)
    if at < 0:
        raise SystemExit(f"missing block: {marker}")
    return _close(source, source.find("{", at), marker)


def main() -> int:
    source = DRIVER.read_text(encoding="utf-8")
    sampler = braced_block(source, "static void max9296_collect_health_locked(")
    verdict = braced_block(source, "static void max9296_pair_verdict_locked(")
    decidable = braced_block(verdict, "if (gap == MAX9296_HINF_GAP_DECIDABLE)")

    print("=== max9296 pair health source contract ===")

    # The interval that is judged must be the interval that is compared, and it
    # must reach the classifier unmodified.  An earlier revision clamped it to
    # 60,000 ms first; at 4 fps and below that landed back inside the 256-frame
    # wrap window.  Counting assignments catches every spelling of that clamp -
    # a bare literal, an if-guard, a ternary, min_t() or clamp_t() - where a
    # pattern for any one of them does not.
    assignments = re.findall(r"\bdelta_ms\s*=(?!=)", verdict)
    check(
        len(assignments) == 1,
        "delta_ms is assigned exactly once, at its declaration",
    )
    check(
        re.search(r"max9296_hinf_gap_classify\(\s*delta_ms\s*,", verdict) is not None,
        "the gap classifier receives delta_ms directly",
    )
    check(
        "max9296_pair_gap_export_ms(delta_ms)" in verdict,
        "only the exported gap is narrowed, through the helper",
    )

    # The timestamp must be taken next to the counters it times, not at the top
    # of the sample: the HINF reads happen after the deserializer and per-channel
    # probes, and at 120 fps the whole lower bound is two 8.3 ms frames.  Scan
    # the whole verdict block - an earlier window began after the declaration
    # and so could not see the assignment site it was named for.
    check(
        "sample->observed_ms" not in verdict,
        "the verdict does not time itself from the sample-start timestamp",
    )
    check(
        verdict.count("ktime_to_ms(ktime_get_boottime())") == 1,
        "the verdict takes its own timestamp beside the counter reads",
    )

    # Report-only: neither the sampler nor the verdict may drive recovery.
    for forbidden in ("max9296_reset", "max9296_power(", "max9296_set_mode"):
        check(
            forbidden not in sampler and forbidden not in verdict,
            f"the health sampler performs no recovery action ({forbidden})",
        )

    # printk in process context can take the console lock and drain the pending
    # buffer, and this runs under sensor->lock on a path an unprivileged reader
    # schedules.  The line is staged here and emitted by the caller.  Match the
    # whole logging family, not the token "printk": pr_warn() slipped past an
    # earlier spelling of this check.  The kernel ratelimit helper is on the list
    # because it prints its own "callbacks suppressed" line from inside the
    # caller's context, and WARN_ON is because it expands to printk plus
    # dump_stack - this file already uses it seven times elsewhere.
    logging = re.compile(
        r"\b(printk\w*|pr_[a-z_]+|dev_[a-z_]+|__ratelimit"
        r"|WARN|WARN_ON|WARN_ONCE|WARN_ON_ONCE|BUG|BUG_ON|dump_stack)\s*\("
    )
    for where, name in ((sampler, "sampler"), (verdict, "verdict")):
        found = sorted({m.group(1) for m in logging.finditer(where)})
        check(not found, f"the {name} logs nothing while it holds sensor->lock")

    # Each format is checked on its own: counting escape sequences across the
    # whole helper let a decoy literal cover for an unterminated format.
    report = braced_block(source, "static void max9296_health_report_pair(")
    formats = re.findall(r"(KERN_\w+)\s+(MAX9296_PAIR_LINE_FMT)", report)
    fmt_macro = re.search(
        r"#define\s+MAX9296_PAIR_LINE_FMT\b((?:[^\n]*\\\n)*[^\n]*)", source
    )
    if fmt_macro is None:
        raise SystemExit("missing macro: MAX9296_PAIR_LINE_FMT")
    fmt_macro = fmt_macro.group(1)
    check(
        {level for level, _ in formats} == {"KERN_WARNING", "KERN_NOTICE"}
        and report.count("printk(") == 1,
        "both severities reach the log through the one shared format",
    )
    check(
        fmt_macro.rstrip().endswith('\\n"'),
        "the verdict format ends in a newline",
    )
    check(
        "seq=%llu" in fmt_macro and "held=%u" in fmt_macro,
        "the line carries the sample sequence and the held count",
    )
    # An unread counter must not render as 0 - that is a value an AP1302 really
    # reports.  Pin the behaviour, not the branch count: any renderer emitting the
    # same strings for the same validity combinations should stay green.
    render = braced_block(source, "static void max9296_pair_render_hinf(")
    check(
        "hinf %s" in fmt_macro and "hinf %u" not in fmt_macro,
        "the counters reach the line through the renderer, not as raw numbers",
    )
    check(
        re.search(r"pair_baseline_valid\[i\]", render) is not None
        and re.search(r"channel\[i\]\.hinf_valid", render) is not None
        and '"?"' in render,
        "the renderer consults both validity bits and has a non-numeric marker",
    )
    # The marker is worthless if nobody fills the field it reads, and the staged
    # snapshot must be taken before the baseline advances in the same call - that
    # ordering is the whole point of staging rather than reading the live fields.
    staged = verdict.find("sample->pair_baseline_valid[i] = "
                          "sensor->health.pair_hinf_valid[i];")
    advanced = verdict.find("sensor->health.pair_hinf[i] = "
                            "sample->channel[i].hinf_count;")
    check(
        0 <= staged < advanced,
        "the baseline snapshot is staged from health, before health advances",
    )
    # pair_changed is only ever set true, on the emit path.  Everything that makes
    # the other paths safe is the sampler's opening memset plus the fact that the
    # one early return never reaches the report: without both, a stack-garbage
    # byte would print a line built from uninitialised fields.
    check(
        re.match(
            r"\s*(?:[^\n;{}]*;\s*)*\s*memset\(sample, 0, sizeof\(\*sample\)\);",
            sampler,
        )
        is not None,
        "the sampler zeroes the sample before anything reads it",
    )
    show = braced_block(source, "static ssize_t sysfs_health_raw_show(")
    check(
        show.index("return scnprintf") < show.index("max9296_collect_health_locked")
        and "max9296_health_report_pair" not in show[: show.index("max9296_collect_health_locked")],
        "the busy early return never reaches the verdict report",
    )
    unlocks = [m.start() for m in re.finditer(r"mutex_unlock\(&sensor->lock\)", show)]
    reported = show.find("max9296_health_report_pair(sensor, &sample)")
    check(
        len(unlocks) == 1 and 0 <= unlocks[0] < reported,
        "the caller reports the verdict only after dropping the lock",
    )

    # Change-only and rate-bounded must be one decision.  A held line that
    # advanced the state anyway would be lost for good, because the change-only
    # gate then treats that transition as already reported.
    # The gate is a three-way on the verdict: unchanged, held, emitted.  A held
    # transition must not advance pair_state - the change-only gate would then
    # treat it as reported - and must be counted once, not once per sample.
    resolved = re.search(
        r"if\s*\(\s*pair\s*==\s*sensor->health\.pair_state\s*\)", decidable)
    check(resolved is not None, "the gate branches on the verdict being unchanged")
    # Pin the comparison, not the token: a held branch guarded by "0 &&" still
    # mentions MAX9296_PAIR_LOG_MIN_MS while gating nothing, and every transition
    # would then emit immediately.
    held_cond = re.search(
        r"\}\s*else\s+if\s*\((?P<cond>[^{]*)\)\s*\{", decidable, re.S)
    at_held = decidable.find("MAX9296_PAIR_LOG_MIN_MS")
    check(
        held_cond is not None
        and re.search(
            r"sensor->health\.pair_log_ms\s*&&\s*now_ms\s*-\s*"
            r"sensor->health\.pair_log_ms\s*<\s*MAX9296_PAIR_LOG_MIN_MS",
            held_cond.group("cond"),
        )
        is not None,
        "a minimum interval separates two verdict lines",
    )
    held_body = emit_body = ""
    if at_held >= 0:
        opening = decidable.index("{", at_held)
        held_body = _close(decidable, opening, "held branch")
        rest = decidable[decidable.index("}", opening + len(held_body)) :]
        emit_body = _close(rest, rest.index("{", rest.index("else")), "emit branch")
    check(
        "sensor->health.pair_state = pair;" not in held_body
        and "sensor->health.pair_state = pair;" in emit_body,
        "a held line does not advance the reported verdict",
    )
    check(
        "pair_log_suppressed++" in held_body,
        "a held line is counted so the next one can report it",
    )
    # Counting the sample instead of the transition inflates held by the sampling
    # rate: at 120 fps one held transition would report 59.
    check(
        re.search(
            r"if\s*\(\s*pair\s*!=\s*sensor->health\.pair_held\s*\)",
            held_body,
        )
        is not None
        and "sensor->health.pair_held = pair;" in held_body,
        "the held count counts transitions, not the samples that carry them",
    )
    check(
        "sensor->health.pair_held = pair;" in emit_body,
        "emitting resets what has already been counted",
    )

    # The gate state is per device, not a shared static: a limiter shared by both
    # deserializers would let a reader of one spend the other's budget.
    check(
        "sensor->health.pair_log_ms" in verdict
        and not re.search(r"\bstatic\b[^;\n]*\b(ratelimit|pair_log)", verdict),
        "the log gate keeps its state in the device, not in a static",
    )

    # The verdict state moves only where a verdict was actually reached.
    check(
        verdict.count("sensor->health.pair_state = pair;") == 1,
        "pair_state is written exactly once",
    )

    # Baseline policy: recover from a wrapped interval, but never re-seed on a
    # gap too short to judge, or a fast reader would keep resetting the window.
    check(
        re.search(
            r"if\s*\(\s*seeding\s*\|\|\s*gap\s*!=\s*MAX9296_HINF_GAP_TOO_SHORT\s*\)",
            verdict,
        )
        is not None,
        "the baseline advances except on a too-short gap",
    )
    check(
        verdict.count("sensor->health.pair_sample_ms = now_ms;") == 1,
        "the baseline timestamp advances with the counters, not separately",
    )
    # The classifier converts one interval at one rate, and nothing else clears
    # this baseline when max9296_s_frame_interval() changes the rate mid-interval.
    check(
        re.search(
            r"if\s*\(\s*!seeding\s*&&\s*fps\s*!=\s*sensor->health\.pair_fps\s*\)"
            r"\s*\n?\s*gap\s*=\s*MAX9296_HINF_GAP_TOO_LONG;",
            verdict,
        )
        is not None,
        "a rate change between baseline and sample refuses the verdict",
    )
    check(
        verdict.count("sensor->health.pair_fps = fps;") == 1
        and verdict.index("sensor->health.pair_fps = fps;")
        > verdict.index("sensor->health.pair_sample_ms = now_ms;") - 200,
        "the baseline records the rate it was taken at",
    )

    # Lifecycle: a stale verdict must not silence the first recurrence.  The
    # contract is not "three call sites" - it is that stopping the stream forgets
    # the pair, wherever that happens.  A forced peer shutdown in max9296_remove()
    # was once a fourth such site with no forget, so count the property, not the
    # calls: every function that clears ->streaming must forget the same device.
    stops = list(re.finditer(r"\b(\w+)->streaming = false;", source))
    check(len(stops) >= 3, "every known stream-stop site is still found")
    missing = [
        m.group(1)
        for m in stops
        if f"max9296_health_forget_pair({m.group(1)})"
        not in enclosing_block(source, m.start())
    ]
    check(not missing, "every site that stops streaming forgets the pair baseline")
    check(
        source.count("memset(sensor->health.hinf_valid") == 1,
        "the raw baseline memset lives only inside the helper",
    )
    forget = braced_block(source, "static void max9296_health_forget_pair(")
    check(
        "pair_state = MAX9296_PAIR_NOT_APPLICABLE" in forget
        and "pair_sample_ms = 0" in forget
        and "pair_log_ms = 0" in forget
        and "pair_held = MAX9296_PAIR_NOT_APPLICABLE" in forget
        and "pair_fps = 0" in forget
        and "pair_hinf_valid" in forget
        and "hinf_valid" in forget,
        "forgetting clears both baselines, the verdict, its timestamp and the log gate",
    )

    print(
        f"\nmax9296 pair health source: {checks} checks, {len(failures)} failures -> "
        f"{'FAILED' if failures else 'PASSED'}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
