#!/usr/bin/env python3
"""Drive the production hardware-identity predicate, not a reachable stand-in.

`max9296_fingerprint_equal()` and the dual-width transform it rests on are
static in max9296.c. tests/max9296_360p_policy_test.c can only reach the header
inline they call, so it stays green with the transform deleted -- the #82
round-1 blocker it claims to trap. Extract the real functions and compile them,
and take every constant and type from production too: a value retyped here is
the same class of stand-in.
"""

import re
import subprocess
import tempfile
from pathlib import Path

from max9296_exposure_replay_binding_test import extract_function


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "max9296.c"

FUNCTIONS = (
    "max9296_mode_is_dual",
    "max9296_fingerprint_preview_max_fps",
    "max9296_fingerprint_exposure_seed_route",
    "max9296_fingerprint_equal",
)


def extract_block(source: str, opener: str) -> str:
    """Extract one brace-delimited top-level declaration, terminator included."""
    at = source.find(opener)
    if at < 0:
        raise ValueError(f"{opener!r} is missing from max9296.c")
    depth = 0
    for index in range(source.index("{", at), len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                end = source.find(";", index)
                if end < 0:
                    raise ValueError(f"{opener!r} has no terminator")
                return source[at : end + 1]
    raise ValueError(f"{opener!r} is unbalanced")


def extract_define(source: str, name: str) -> str:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(.+?)\s*$", source, re.M)
    if not match:
        raise ValueError(f"#define {name} is missing from max9296.c")
    return f"#define {name} {match.group(1)}"


def build_harness(source: str) -> str:
    production = "\n\n".join(extract_function(source, name) for name in FUNCTIONS)
    safe_max = extract_define(source, "MAX9296_EXPOSURE_SAFE_MAX_FPS")
    mode_ids = extract_block(source, "enum max9296_mode_id {")
    fingerprint = extract_block(source, "struct max9296_hw_fingerprint {")
    return f"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "max9296_360p_policy.h"
#include "max9296_exposure_policy.h"

typedef uint32_t u32;

{safe_max}

{mode_ids}

/* Only the two members the extracted code reads. A rename in production breaks
 * this compile rather than silently testing a different field. */
struct max9296_mode_info {{
  enum max9296_mode_id id;
  u32 exposure_safe_max_fps;
}};

{fingerprint}

{production}

static int failures;
static int checks;

#define CHECK(expr)                                                            \\
  do {{                                                                         \\
    checks++;                                                                  \\
    if (!(expr)) {{                                                             \\
      printf("FAIL: %s (line %d)\\n", #expr, __LINE__);                         \\
      failures++;                                                              \\
    }}                                                                          \\
  }} while (0)

static const struct max9296_mode_info dual_360p = {{
    .id = MAX9296_MODE_1280x360,
    .exposure_safe_max_fps = MAX9296_EXPOSURE_SAFE_MAX_FPS,
}};
static const struct max9296_mode_info single_360p = {{
    .id = MAX9296_MODE_640x360,
    .exposure_safe_max_fps = MAX9296_EXPOSURE_SAFE_MAX_FPS,
}};
static const struct max9296_mode_info dual_hd = {{
    .id = MAX9296_MODE_2560x720,
    .exposure_safe_max_fps = MAX9296_EXPOSURE_SAFE_MAX_FPS,
}};
/* The production table holds a pair no derived axis can separate:
 * max9296_mode_data_360_R and max9296_mode_data[MAX9296_MODE_640x360] agree on
 * id, dimensions, ceiling and exposure_safe_max_fps, so the pointer comparison
 * in max9296_fingerprint_equal() is the only thing between them. A mode-axis
 * check that changes dual-ness instead tests a derived axis and passes with
 * that comparison deleted. */
static const struct max9296_mode_info single_360p_right = {{
    .id = MAX9296_MODE_640x360,
    .exposure_safe_max_fps = MAX9296_EXPOSURE_SAFE_MAX_FPS,
}};

/* enable is not free: sysfs_prepare_store() rejects anything but 3 for a dual
 * tuple and 1 or 2 for a single one, so a fingerprint carrying the wrong mask is
 * a state production never admits -- and a check built on one proves nothing
 * about the reachable ones. */
#define ENABLE_DUAL 3U
#define ENABLE_LEFT 1U
#define ENABLE_RIGHT 2U

static struct max9296_hw_fingerprint make(const struct max9296_mode_info *mode,
                                          u32 width, u32 height, u32 fps,
                                          u32 enable) {{
  struct max9296_hw_fingerprint fingerprint = {{
      .mode = mode, .width = width, .height = height,
      .code = 0x2006U, .fps = fps, .enable = enable, .crop_enable = false,
  }};
  return fingerprint;
}}

int main(void) {{
  /* The dual mode stores the combined width. The transform under test halves it
   * before asking the register model, so a dual 1280x360 derives what the
   * writer programs for each 640x360 output. Without the halving every rate
   * derives 0, which is what let two in-window rates compare equal. */
  struct max9296_hw_fingerprint dual_120 = make(&dual_360p, 1280U, 360U, 120U, ENABLE_DUAL);
  struct max9296_hw_fingerprint dual_60 = make(&dual_360p, 1280U, 360U, 60U, ENABLE_DUAL);
  struct max9296_hw_fingerprint single_120 = make(&single_360p, 640U, 360U, 120U, ENABLE_LEFT);

  CHECK(max9296_fingerprint_preview_max_fps(&dual_120) != 0U);
  CHECK(max9296_fingerprint_preview_max_fps(&dual_120) ==
        max9296_preview_max_fps_fixed8(120U));
  CHECK(max9296_fingerprint_preview_max_fps(&dual_120) ==
        max9296_fingerprint_preview_max_fps(&single_120));
  CHECK(max9296_fingerprint_preview_max_fps(&dual_60) !=
        max9296_fingerprint_preview_max_fps(&dual_120));

  /* Two in-window dual rates are different hardware and must not compare
   * equal. This is the round-1 blocker the policy test claims to trap. */
  CHECK(!max9296_fingerprint_equal(&dual_120, &dual_60));

  /* Equivalence, the other direction: rates that program neither register are
   * the same hardware. A comparison that fell back to raw fps fails here. */
  struct max9296_hw_fingerprint hd_30 = make(&dual_hd, 2560U, 720U, 30U, ENABLE_DUAL);
  struct max9296_hw_fingerprint hd_20 = make(&dual_hd, 2560U, 720U, 20U, ENABLE_DUAL);
  CHECK(max9296_fingerprint_preview_max_fps(&hd_30) == 0U);
  CHECK(max9296_fingerprint_preview_max_fps(&hd_20) == 0U);
  CHECK(max9296_fingerprint_equal(&hd_30, &hd_20));

  /* The seed route is the other derived axis, and 720p isolates it: the preview
   * ceiling is 0 at every 720p rate, so only the exposure route can differ. */
  struct max9296_hw_fingerprint hd_40 =
      make(&dual_hd, 2560U, 720U, MAX9296_EXPOSURE_SAFE_MAX_FPS + 10U, ENABLE_DUAL);
  CHECK(max9296_fingerprint_preview_max_fps(&hd_40) == 0U);
  CHECK(max9296_fingerprint_exposure_seed_route(&hd_40) !=
        max9296_fingerprint_exposure_seed_route(&hd_30));
  CHECK(!max9296_fingerprint_equal(&hd_30, &hd_40));

  /* The left and right single tables, which share an id and every field the
   * derived axes read, so only the mode pointer tells the tables themselves
   * apart.  That does NOT isolate the pointer comparison: among fingerprints
   * production admits, enable already separates the pair, because
   * max9296_resolve_prepare_mode_locked() selects the right-hand table FROM
   * enable and sysfs_prepare_store() rejects any other mask.  So
   * left->mode == right->mode is defensive rather than load-bearing here, and a
   * check that forced the pair to share an enable would be asserting against a
   * state that cannot occur -- it would fail and report a regression that is
   * not one.  What is assertable is that the reachable pair is unequal.
   *
   * So this file does NOT pin left->mode == right->mode: deleting that term
   * leaves every check here green, and deliberately so.  The driver documents
   * why the term exists -- max9296_resolve_prepare_mode_locked() notes that the
   * right-hand tables share their public mode ids -- and that is where the
   * reason lives, not in a check built on a tuple sysfs rejects. */
  struct max9296_hw_fingerprint left_30 =
      make(&single_360p, 640U, 360U, 30U, ENABLE_LEFT);
  struct max9296_hw_fingerprint right_30 =
      make(&single_360p_right, 640U, 360U, 30U, ENABLE_RIGHT);
  CHECK(max9296_fingerprint_preview_max_fps(&left_30) ==
        max9296_fingerprint_preview_max_fps(&right_30));
  CHECK(max9296_fingerprint_exposure_seed_route(&left_30) ==
        max9296_fingerprint_exposure_seed_route(&right_30));
  CHECK(!max9296_fingerprint_equal(&left_30, &right_30));

  /* Which fields the predicate compares at all, one synthetic single-field delta
   * each.  These are deltas of a fingerprint, not requests production would
   * admit -- enable in particular is correlated with the mode pointer for every
   * reachable tuple -- so what they pin is the predicate's term list, not a
   * reachable distinction. Both are worth pinning; only the second would be
   * worth claiming. */
  struct max9296_hw_fingerprint other;
  other = hd_30; other.width = 1280U;         CHECK(!max9296_fingerprint_equal(&hd_30, &other));
  other = hd_30; other.height = 1080U;        CHECK(!max9296_fingerprint_equal(&hd_30, &other));
  other = hd_30; other.code = 0x2007U;        CHECK(!max9296_fingerprint_equal(&hd_30, &other));
  other = hd_30; other.enable = 1U;           CHECK(!max9296_fingerprint_equal(&hd_30, &other));
  other = hd_30; other.crop_enable = true;    CHECK(!max9296_fingerprint_equal(&hd_30, &other));
  CHECK(max9296_fingerprint_equal(&hd_30, &hd_30));

  printf("max9296 fingerprint identity: %d checks, %d failures -> %s\\n", checks,
         failures, failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}}
"""


def main() -> int:
    harness = build_harness(SOURCE.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory() as workspace:
        source = Path(workspace) / "identity.c"
        binary = Path(workspace) / "identity"
        source.write_text(harness, encoding="utf-8")
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
             str(source), "-o", str(binary)],
            check=True,
        )
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
