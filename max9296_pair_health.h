#ifndef MAX9296_PAIR_HEALTH_H
#define MAX9296_PAIR_HEALTH_H

/* Dual-wide pair health, derived from the AP1302 HINF frame counter.
 *
 * A dual-wide pair shares one CSI link behind the GMSL serdes and its two
 * frames are stitched into a single wide frame.  If one channel stops
 * producing while the other keeps going, the composite never forms: the
 * deserializer output collapses even though every error channel stays clean.
 * Board capture (max9296 issue #65, poll_20260907_120018_slow_t7) recorded
 * exactly that - ch B's HINF counter pinned at 49 for 36 s while ch A kept
 * advancing, with R0x0006 ERROR = 0x0000, GMSL LOCKED and dmesg silent.
 *
 * The symptom is visible in telemetry the driver already reads; nothing was
 * comparing the two channels.  This decides that comparison.  It reports, it
 * does not act: the root cause is unresolved and an automatic recovery would
 * destroy the evidence a field capture needs.
 *
 * Sampling cadence is part of the contract, not advice: STALLED means "the 8-bit
 * HINF counter did not move between two reads", so a read faster than one frame
 * period reports a stall that never happened.  max9296_hinf_gap_classify() below
 * refuses such a verdict rather than leaving it to whoever runs the sampler; what
 * that refusal costs a slow reader is stated once, in docs/health-raw-v1.md,
 * beside the field it affects.
 */

enum max9296_hinf_state {
  /* Not streaming, first sample, or the counter could not be read.  The caller
   * leaves a channel here rather than deciding it, so UNKNOWN must survive
   * being neither PROGRESSING nor STALLED. */
  MAX9296_HINF_UNKNOWN = 0,
  MAX9296_HINF_PROGRESSING,
  MAX9296_HINF_STALLED,
};

enum max9296_pair_health {
  MAX9296_PAIR_NOT_APPLICABLE = 0,
  MAX9296_PAIR_ALIGNED,
  MAX9296_PAIR_DIVERGENT,
  MAX9296_PAIR_BOTH_STALLED,
};

/* Both channels must be in a decided state before the pair means anything.
 * A single unknown side makes the comparison meaningless, not benign - callers
 * must not read NOT_APPLICABLE as healthy. */
static inline enum max9296_pair_health
max9296_pair_health_decision(unsigned int dual, unsigned int streaming,
                             enum max9296_hinf_state ch0,
                             enum max9296_hinf_state ch1) {
  if (!dual || !streaming)
    return MAX9296_PAIR_NOT_APPLICABLE;

  if (ch0 == MAX9296_HINF_UNKNOWN || ch1 == MAX9296_HINF_UNKNOWN)
    return MAX9296_PAIR_NOT_APPLICABLE;

  if (ch0 == MAX9296_HINF_STALLED && ch1 == MAX9296_HINF_STALLED)
    return MAX9296_PAIR_BOTH_STALLED;

  if (ch0 != ch1)
    return MAX9296_PAIR_DIVERGENT;

  return MAX9296_PAIR_ALIGNED;
}

static inline const char *
max9296_pair_health_name(enum max9296_pair_health state) {
  switch (state) {
  case MAX9296_PAIR_ALIGNED:
    return "ALIGNED";
  case MAX9296_PAIR_DIVERGENT:
    return "DIVERGENT";
  case MAX9296_PAIR_BOTH_STALLED:
    return "BOTH_STALLED";
  default:
    return "NOT_APPLICABLE";
  }
}

/* Frames the HINF counter can advance before "unchanged" stops meaning
 * "stalled".  The counter is 8 bits (R0x0002[15:8]) and the verdict is an
 * equality test, so a gap of exactly 256 frames wraps back to the same value
 * and reads as a stall that never happened. */
#define MAX9296_HINF_COUNTER_PERIOD 256U

/* Where a sample's interval sits relative to the window in which the 8-bit
 * counter equality test carries information.  The two rejections are separate
 * outcomes because the caller must treat them differently, not merely to name
 * them: a gap that ran past the wrap window has to re-seed the baseline (nothing
 * later can be judged otherwise), while one that arrived too soon must NOT, or a
 * fast reader would keep re-seeding and starve a slower one of any usable
 * interval. */
enum max9296_hinf_gap {
  MAX9296_HINF_GAP_TOO_SHORT = 0,
  MAX9296_HINF_GAP_DECIDABLE,
  MAX9296_HINF_GAP_TOO_LONG,
};

/* Classify the interval the counter comparison spans.
 *
 * Too soon (< 2 frame periods): the counter may legitimately not have moved.
 * Two periods absorbs sampler jitter and costs nothing at the 1 Hz cadence the
 * exporter uses - 67 ms at 30 fps, 17 ms at 120.
 *
 * Too late (past 255 frame periods, less a millisecond): the counter can have
 * wrapped onto its own value.  The ceiling is one period short of the counter's
 * own period because the tick phase is unknown - see the note on the constant
 * below.  It lands at 8,499 ms for 30 fps and 2,124 ms for 120, so a plausible
 * `--interval-ms 5000` drop-in would be unreliable at high frame rates while
 * looking perfectly sane in the unit file.
 *
 * elapsed_ms is a signed 64-bit millisecond difference, deliberately not a
 * pre-narrowed 32-bit value.  An earlier revision clamped the caller's delta to
 * 60 s before this test; at 4 fps and below 60 s is still inside the 256-frame
 * window, so a gap of hours passed the wrap bound as if it were a minute and the
 * equality test then compared a counter that had wrapped thousands of times.
 * `max9296_s_frame_interval()` accepts any fps from 1, so those rates are
 * reachable.  Judge the real interval; narrow only what is exported.
 *
 * Both bounds are expressed in frames rather than milliseconds so the arithmetic
 * stays exact and rate-independent: elapsed_ms * fps is frames * 1000. */
static inline enum max9296_hinf_gap
max9296_hinf_gap_classify(long long elapsed_ms, unsigned int fps) {
  /* One period short of the counter's own period.  The tick phase relative to
   * the sample is unknown, so an interval spanning 255.96 periods still contains
   * 256 tick edges for most phases - and 256 edges bring the 8-bit counter back
   * to its own value, which the equality test reads as a stall.  Swept over
   * 1,000 phases at 30, 60 and 120 fps, every interval a 256-period ceiling
   * accepts does exactly that at some phase.  255 periods cannot, at any phase. */
  long long ceiling_x1000 = (long long)(MAX9296_HINF_COUNTER_PERIOD - 1U) * 1000LL;
  long long guard_x1000 = (long long)MAX9296_HINF_COUNTER_PERIOD * 1000LL;
  long long frames_x1000;

  if (!fps || elapsed_ms <= 0)
    return MAX9296_HINF_GAP_TOO_SHORT;
  /* Bounds the multiplies below, and is not merely defensive: 1 fps is the
   * lowest rate the driver accepts, and even there the counter has wrapped by
   * 256,000 ms, so no longer gap is decidable at any rate. */
  if (elapsed_ms > guard_x1000)
    return MAX9296_HINF_GAP_TOO_LONG;
  frames_x1000 = elapsed_ms * (long long)fps;
  if (frames_x1000 < 2000LL)
    return MAX9296_HINF_GAP_TOO_SHORT;
  /* elapsed_ms + 1 because both timestamps are truncated millisecond reads, so
   * the interval they describe can be a millisecond longer than their
   * difference.  Judge the longest interval the pair of reads can mean. */
  if ((elapsed_ms + 1LL) * (long long)fps > ceiling_x1000)
    return MAX9296_HINF_GAP_TOO_LONG;
  return MAX9296_HINF_GAP_DECIDABLE;
}

/* Narrow a 64-bit millisecond delta into the u32 the sample exports.  Saturating
 * is correct here and only here: publishing "longer than this field can say" is
 * all a consumer needs once the verdict is NOT_APPLICABLE, whereas judging a
 * narrowed value is the defect max9296_hinf_gap_classify() documents. */
static inline unsigned int
max9296_pair_gap_export_ms(long long elapsed_ms) {
  if (elapsed_ms <= 0)
    return 0U;
  return elapsed_ms > 4294967295LL ? 4294967295U : (unsigned int)elapsed_ms;
}

#endif
