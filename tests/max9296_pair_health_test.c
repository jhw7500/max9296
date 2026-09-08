#include "../max9296_pair_health.h"

#include <stdio.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

#define CHECK(_condition)                                                   \
  do {                                                                      \
    checks++;                                                               \
    if (!(_condition)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #_condition); \
      failures++;                                                           \
    }                                                                       \
  } while (0)

#define DECIDE(_dual, _streaming, _ch0, _ch1) \
  max9296_pair_health_decision((_dual), (_streaming), (_ch0), (_ch1))

/* The #65 signature: one channel's HINF counter pinned while the other keeps
 * advancing.  This is the case the whole check exists for, and it must fire in
 * both channel orders - the captured board case had B stalled, but nothing
 * guarantees which side goes first. */
static void test_one_sided_stall_is_divergent(void) {
  CHECK(DECIDE(1U, 1U, MAX9296_HINF_PROGRESSING, MAX9296_HINF_STALLED) ==
        MAX9296_PAIR_DIVERGENT);
  CHECK(DECIDE(1U, 1U, MAX9296_HINF_STALLED, MAX9296_HINF_PROGRESSING) ==
        MAX9296_PAIR_DIVERGENT);
}

/* Both sides stalled is a total stop, not a composition failure.  Keeping it
 * distinct matters: the operator response differs, and #65's captured case was
 * explicitly one-sided while other snapshots showed both moving together. */
static void test_both_stalled_is_its_own_verdict(void) {
  CHECK(DECIDE(1U, 1U, MAX9296_HINF_STALLED, MAX9296_HINF_STALLED) ==
        MAX9296_PAIR_BOTH_STALLED);
  CHECK(DECIDE(1U, 1U, MAX9296_HINF_PROGRESSING, MAX9296_HINF_PROGRESSING) ==
        MAX9296_PAIR_ALIGNED);
}

/* An undecided side makes the comparison meaningless.  NOT_APPLICABLE must not
 * be read as healthy, so it is deliberately a separate verdict from ALIGNED. */
static void test_unknown_side_blocks_the_verdict(void) {
  CHECK(DECIDE(1U, 1U, MAX9296_HINF_UNKNOWN, MAX9296_HINF_STALLED) ==
        MAX9296_PAIR_NOT_APPLICABLE);
  CHECK(DECIDE(1U, 1U, MAX9296_HINF_PROGRESSING, MAX9296_HINF_UNKNOWN) ==
        MAX9296_PAIR_NOT_APPLICABLE);
  CHECK(DECIDE(1U, 1U, MAX9296_HINF_UNKNOWN, MAX9296_HINF_UNKNOWN) ==
        MAX9296_PAIR_NOT_APPLICABLE);
  CHECK(MAX9296_PAIR_NOT_APPLICABLE != MAX9296_PAIR_ALIGNED);
}

/* Single-channel mode has no pair, and a stopped stream is expected to have a
 * still counter.  Neither may raise a warning. */
static void test_single_and_idle_never_warn(void) {
  CHECK(DECIDE(0U, 1U, MAX9296_HINF_PROGRESSING, MAX9296_HINF_STALLED) ==
        MAX9296_PAIR_NOT_APPLICABLE);
  CHECK(DECIDE(1U, 0U, MAX9296_HINF_PROGRESSING, MAX9296_HINF_STALLED) ==
        MAX9296_PAIR_NOT_APPLICABLE);
  CHECK(DECIDE(0U, 0U, MAX9296_HINF_STALLED, MAX9296_HINF_PROGRESSING) ==
        MAX9296_PAIR_NOT_APPLICABLE);
}

/* NOT_APPLICABLE == 0 is load-bearing: struct max9296_dev comes from
 * devm_kzalloc, so health.pair_state starts at "not judged" rather than at a
 * verdict nobody reached.  The two rejection outcomes must stay distinct
 * values, because the driver treats them differently - a too-long gap re-seeds
 * the baseline, a too-short one deliberately does not - and aliasing them is a
 * merge no equality assertion elsewhere in this file can see. */
static void test_zero_values_and_distinct_rejections(void) {
  CHECK(MAX9296_PAIR_NOT_APPLICABLE == 0);
  CHECK(MAX9296_HINF_GAP_TOO_SHORT != MAX9296_HINF_GAP_TOO_LONG);
  CHECK(MAX9296_HINF_GAP_DECIDABLE != MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(MAX9296_HINF_GAP_DECIDABLE != MAX9296_HINF_GAP_TOO_LONG);
}

/* These four strings are ABI: they go into health_raw as pair.status verbatim
 * and are the status table in docs/health-raw-v1.md.  Compare them whole - a
 * first-character check leaves DIVERGENT -> DEGRADED green while it breaks both
 * documented consumers. */
static void test_names_are_the_documented_abi(void) {
  CHECK(!strcmp(max9296_pair_health_name(MAX9296_PAIR_DIVERGENT), "DIVERGENT"));
  CHECK(!strcmp(max9296_pair_health_name(MAX9296_PAIR_BOTH_STALLED),
                "BOTH_STALLED"));
  CHECK(!strcmp(max9296_pair_health_name(MAX9296_PAIR_ALIGNED), "ALIGNED"));
  CHECK(!strcmp(max9296_pair_health_name(MAX9296_PAIR_NOT_APPLICABLE),
                "NOT_APPLICABLE"));
}

/* Sampling-gap classification.  Why the two rejections must stay distinct is
 * argued once, beside enum max9296_hinf_gap. */
#define GAP(_ms, _fps) max9296_hinf_gap_classify((_ms), (_fps))

/* Board-measured failure before any gate existed (pim-camera-v016, 30 fps): ten
 * back-to-back health_raw reads emitted BOTH_STALLED while the counters plainly
 * advanced 27,28 -> 28,29 -> 29,30, oscillating against ALIGNED once per read. */
static void test_gap_too_short_is_refused(void) {
  CHECK(GAP(0LL, 30U) == MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(GAP(66LL, 30U) == MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(GAP(67LL, 30U) == MAX9296_HINF_GAP_DECIDABLE);

  CHECK(GAP(16LL, 120U) == MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(GAP(17LL, 120U) == MAX9296_HINF_GAP_DECIDABLE);

  /* A backwards or zero clock difference is not a gap, and no frame rate means
   * no frame period to measure against. */
  CHECK(GAP(-1LL, 30U) == MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(GAP(1000LL, 0U) == MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(GAP(0LL, 0U) == MAX9296_HINF_GAP_TOO_SHORT);
}

/* 256 frames wraps the 8-bit counter onto its own value, so the ceiling is 255
 * periods, not 256: the counter's tick phase relative to the sample is unknown,
 * and an interval spanning 255.96 periods already contains 256 edges at most
 * phases.  One further millisecond comes off for the truncation in both
 * timestamps.  At 120 fps the ceiling lands at 2,124 ms - still close enough to
 * a plausible --interval-ms 2000 drop-in that an open upper end would
 * manufacture false stalls from a sane-looking unit file. */
static void test_gap_too_long_is_refused(void) {
  CHECK(GAP(2124LL, 120U) == MAX9296_HINF_GAP_DECIDABLE);
  CHECK(GAP(2125LL, 120U) == MAX9296_HINF_GAP_TOO_LONG);
  /* What a 256-period ceiling would have accepted, and why it must not: at some
   * phase this interval carries the counter through 256 edges, back to its own
   * value, and the equality test then reads a healthy channel as stalled. */
  CHECK(GAP(2133LL, 120U) == MAX9296_HINF_GAP_TOO_LONG);

  CHECK(GAP(8499LL, 30U) == MAX9296_HINF_GAP_DECIDABLE);
  CHECK(GAP(8500LL, 30U) == MAX9296_HINF_GAP_TOO_LONG);
  CHECK(GAP(8533LL, 30U) == MAX9296_HINF_GAP_TOO_LONG);

  /* The shipped 1 Hz cadence is decidable from 2 fps up.  At 1 fps the floor is
   * two whole seconds, so a 1 Hz reader sits below it - and because a too-short
   * gap pins the baseline rather than re-seeding it, that reader is decidable on
   * every second read instead. */
  CHECK(GAP(1000LL, 1U) == MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(GAP(1000LL, 2U) == MAX9296_HINF_GAP_DECIDABLE);
}

/* Regression for the clamp that defeated the wrap bound.  An earlier revision
 * narrowed the caller's delta to 60,000 ms before classifying, so at 4 fps and
 * below the narrowed value landed back inside the window and a gap of hours was
 * judged decidable; the equality test then compared a counter that had wrapped
 * thousands of times.  max9296_s_frame_interval() accepts any fps from 1. */
static void test_gap_judges_the_real_interval(void) {
  /* Only the rates where the clamped value re-enters the window discriminate:
   * at 30 fps 60,000 ms is already past the wrap, so that assertion would be
   * green with the clamp in place and proves nothing about it. */
  CHECK(GAP(7200000LL, 1U) == MAX9296_HINF_GAP_TOO_LONG);
  CHECK(GAP(7200000LL, 4U) == MAX9296_HINF_GAP_TOO_LONG);

  /* 60,000 ms is a genuine interval at low rates and is still judged on its own
   * merits - the fix removed the clamp, it does not blanket-refuse 60 s. */
  CHECK(GAP(60000LL, 1U) == MAX9296_HINF_GAP_DECIDABLE);
  CHECK(GAP(60000LL, 4U) == MAX9296_HINF_GAP_DECIDABLE);
  CHECK(GAP(60000LL, 5U) == MAX9296_HINF_GAP_TOO_LONG);

  /* 1 fps is where the window is widest: 2 frames is 2,000 ms, the ceiling
   * 255,000 minus the truncation millisecond. */
  CHECK(GAP(1999LL, 1U) == MAX9296_HINF_GAP_TOO_SHORT);
  CHECK(GAP(2000LL, 1U) == MAX9296_HINF_GAP_DECIDABLE);
  CHECK(GAP(254999LL, 1U) == MAX9296_HINF_GAP_DECIDABLE);
  CHECK(GAP(255000LL, 1U) == MAX9296_HINF_GAP_TOO_LONG);

  /* The early millisecond bound exists to keep elapsed_ms * fps from overflowing
   * a 64-bit product, not merely to restate the wrap term.  Without it these
   * would multiply out past LLONG_MAX. */
  CHECK(GAP(9223372036854775807LL, 120U) == MAX9296_HINF_GAP_TOO_LONG);
}

/* The exported gap saturates rather than truncating, so a consumer never reads a
 * wrapped small number where the real interval was enormous.  Narrowing belongs
 * here and nowhere near the classification above. */
static void test_export_saturates_without_truncating(void) {
  CHECK(max9296_pair_gap_export_ms(-1LL) == 0U);
  CHECK(max9296_pair_gap_export_ms(4294967295LL) == 4294967295U);
  /* One past the field: truncation would publish 0 here. */
  CHECK(max9296_pair_gap_export_ms(4294967296LL) == 4294967295U);
  CHECK(max9296_pair_gap_export_ms(9223372036854775807LL) == 4294967295U);
}

int main(void) {
  test_one_sided_stall_is_divergent();
  test_both_stalled_is_its_own_verdict();
  test_unknown_side_blocks_the_verdict();
  test_single_and_idle_never_warn();
  test_zero_values_and_distinct_rejections();
  test_names_are_the_documented_abi();
  test_gap_too_short_is_refused();
  test_gap_too_long_is_refused();
  test_gap_judges_the_real_interval();
  test_export_saturates_without_truncating();

  printf("max9296 pair health: %u checks, %u failures -> %s\n", checks,
         failures, failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
