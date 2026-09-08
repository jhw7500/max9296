/* Linux defines current as get_current(); keep this freestanding policy
 * header safe to include after kernel headers, as max9296.c does. */
#define current get_current()
#include "../max9296_exposure_policy.h"
#undef current

#include <stdio.h>

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

static void test_shared_baseline_keeps_pair_policy(void) {
  struct max9296_exposure_replay_decision decision;

  decision = max9296_exposure_replay_decision(
      1U, 0U, 0U, 1U, 120U, 30U, 7000U, 7000U, 7000U);
  CHECK(decision.route == MAX9296_EXPOSURE_SEED_SKIP);
  CHECK(decision.has_runtime_override == 0U);

  decision = max9296_exposure_replay_decision(
      1U, 1U, 0U, 0U, 120U, 30U, 7000U, 7000U, 7000U);
  CHECK(decision.route == MAX9296_EXPOSURE_SEED_PAIR);
  CHECK(decision.value == 7000U);
}

static void test_dual_runtime_override_replays_each_channel(void) {
  struct max9296_exposure_replay_decision ch0;
  struct max9296_exposure_replay_decision ch1;

  ch0 = max9296_exposure_replay_decision(
      1U, 0U, MAX9296_EXPOSURE_OVERRIDE_CH1, 1U, 120U, 30U,
      7000U, 7000U, 5000U);
  ch1 = max9296_exposure_replay_decision(
      1U, 1U, MAX9296_EXPOSURE_OVERRIDE_CH1, 1U, 120U, 30U,
      7000U, 7000U, 5000U);

  CHECK(ch0.route == MAX9296_EXPOSURE_SEED_CHANNEL);
  CHECK(ch0.value == 7000U);
  CHECK(ch0.has_runtime_override == 1U);
  CHECK(ch1.route == MAX9296_EXPOSURE_SEED_CHANNEL);
  CHECK(ch1.value == 5000U);
  CHECK(ch1.has_runtime_override == 1U);
}

static void test_single_runtime_override_uses_active_channel(void) {
  struct max9296_exposure_replay_decision decision;

  decision = max9296_exposure_replay_decision(
      0U, 1U, MAX9296_EXPOSURE_OVERRIDE_CH1, 1U, 120U, 30U,
      7000U, 7000U, 5000U);
  CHECK(decision.route == MAX9296_EXPOSURE_SEED_CHANNEL);
  CHECK(decision.value == 5000U);
  CHECK(decision.has_runtime_override == 1U);

  decision = max9296_exposure_replay_decision(
      0U, 0U, MAX9296_EXPOSURE_OVERRIDE_CH1, 1U, 120U, 30U,
      7000U, 7000U, 5000U);
  CHECK(decision.route == MAX9296_EXPOSURE_SEED_SKIP);
  CHECK(decision.value == 7000U);
  CHECK(decision.has_runtime_override == 0U);
}

static void test_single_live_write_targets_only_the_active_channel(void) {
  CHECK(max9296_exposure_channel_is_active(1U, 0U, 0U));
  CHECK(max9296_exposure_channel_is_active(1U, 0U, 1U));
  CHECK(max9296_exposure_channel_is_active(0U, 0U, 0U));
  CHECK(!max9296_exposure_channel_is_active(0U, 0U, 1U));
  CHECK(!max9296_exposure_channel_is_active(0U, 1U, 0U));
  CHECK(max9296_exposure_channel_is_active(0U, 1U, 1U));
}

static void test_power_cycle_makes_pre_stream_control_cache_only(void) {
  CHECK(max9296_exposure_hardware_can_apply(
      0U, 0U, 0U, 1U, 1U, 1U, 1U, 7ULL, 7ULL));

  /* A real last-off/first-on transition advances the board epoch before
   * STREAMON downloads firmware again. Stale firmware_ready must not make a
   * control write touch that reset hardware. */
  CHECK(!max9296_exposure_hardware_can_apply(
      0U, 0U, 0U, 1U, 1U, 1U, 1U, 7ULL, 8ULL));

  CHECK(!max9296_exposure_hardware_can_apply(
      0U, 0U, 0U, 1U, 1U, 1U, 0U, 8ULL, 8ULL));
  CHECK(max9296_exposure_hardware_can_apply(
      0U, 0U, 0U, 1U, 1U, 1U, 1U, 8ULL, 8ULL));
}

static void test_zero_values_are_explicit_not_default_sentinels(void) {
  struct max9296_exposure_replay_decision shared;
  struct max9296_exposure_replay_decision overridden;

  shared = max9296_exposure_replay_decision(
      1U, 0U, 0U, 0U, 30U, 30U, 0U, 7000U, 7000U);
  CHECK(shared.route == MAX9296_EXPOSURE_SEED_PAIR);
  CHECK(shared.value == 0U);

  overridden = max9296_exposure_replay_decision(
      1U, 1U, MAX9296_EXPOSURE_OVERRIDE_CH1, 0U, 30U, 30U,
      7000U, 7000U, 0U);
  CHECK(overridden.route == MAX9296_EXPOSURE_SEED_CHANNEL);
  CHECK(overridden.value == 0U);
}

static void test_prepare_generation_defines_session_boundary(void) {
  CHECK(max9296_exposure_session_should_reset(0ULL, 100ULL));
  CHECK(!max9296_exposure_session_should_reset(100ULL, 100ULL));
  CHECK(max9296_exposure_session_should_reset(100ULL, 101ULL));
  CHECK(!max9296_exposure_session_should_reset(100ULL, 0ULL));
}

static void test_new_session_with_override_requires_fresh_hardware(void) {
  CHECK(max9296_exposure_session_requires_reinit(
      100ULL, 101ULL, MAX9296_EXPOSURE_OVERRIDE_CH1));
  CHECK(!max9296_exposure_session_requires_reinit(100ULL, 101ULL, 0U));
  CHECK(!max9296_exposure_session_requires_reinit(
      100ULL, 100ULL, MAX9296_EXPOSURE_OVERRIDE_CH1));
  CHECK(!max9296_exposure_session_requires_reinit(
      100ULL, 0ULL, MAX9296_EXPOSURE_OVERRIDE_CH1));
}

static void test_clearing_any_override_requires_reconciliation(void) {
  CHECK(max9296_exposure_override_was_cleared(
      MAX9296_EXPOSURE_OVERRIDE_CH1, 0U));
  CHECK(max9296_exposure_override_was_cleared(
      MAX9296_EXPOSURE_OVERRIDE_CH0 | MAX9296_EXPOSURE_OVERRIDE_CH1,
      MAX9296_EXPOSURE_OVERRIDE_CH1));
  CHECK(!max9296_exposure_override_was_cleared(
      MAX9296_EXPOSURE_OVERRIDE_CH1, MAX9296_EXPOSURE_OVERRIDE_CH1));
  CHECK(!max9296_exposure_override_was_cleared(
      0U, MAX9296_EXPOSURE_OVERRIDE_CH1));
}

static void test_shared_write_clears_override_even_when_value_is_unchanged(void) {
  struct max9296_exposure_control_state current = {
      .shared = 7000U,
      .ch0 = 7000U,
      .ch1 = 5000U,
      .override_mask = MAX9296_EXPOSURE_OVERRIDE_CH1,
  };
  struct max9296_exposure_control_state desired =
      max9296_exposure_control_update(
          current, MAX9296_EXPOSURE_UPDATE_SHARED, 7000U, 7000U, 5000U);

  CHECK(desired.shared == 7000U);
  CHECK(desired.ch0 == 7000U);
  CHECK(desired.ch1 == 7000U);
  CHECK(desired.override_mask == 0U);
}

static void test_channel_write_sets_and_clears_override_against_baseline(void) {
  struct max9296_exposure_control_state current = {
      .shared = 7000U,
      .ch0 = 7000U,
      .ch1 = 7000U,
      .override_mask = 0U,
  };
  struct max9296_exposure_control_state desired;

  desired = max9296_exposure_control_update(
      current, MAX9296_EXPOSURE_UPDATE_CH1, 7000U, 7000U, 5000U);
  CHECK(desired.shared == 7000U);
  CHECK(desired.ch0 == 7000U);
  CHECK(desired.ch1 == 5000U);
  CHECK(desired.override_mask == MAX9296_EXPOSURE_OVERRIDE_CH1);

  current = desired;
  desired = max9296_exposure_control_update(
      current, MAX9296_EXPOSURE_UPDATE_CH1, 7000U, 7000U, 7000U);
  CHECK(desired.ch1 == 7000U);
  CHECK(desired.override_mask == 0U);
}

static void test_shared_and_channel_batch_applies_override_after_baseline(void) {
  struct max9296_exposure_control_state current = {
      .shared = 9000U,
      .ch0 = 9000U,
      .ch1 = 9000U,
      .override_mask = 0U,
  };
  struct max9296_exposure_control_state desired =
      max9296_exposure_control_update(
          current,
          MAX9296_EXPOSURE_UPDATE_SHARED | MAX9296_EXPOSURE_UPDATE_CH1,
          7000U, 7000U, 5000U);

  CHECK(desired.shared == 7000U);
  CHECK(desired.ch0 == 7000U);
  CHECK(desired.ch1 == 5000U);
  CHECK(desired.override_mask == MAX9296_EXPOSURE_OVERRIDE_CH1);
}

int main(void) {
  test_shared_baseline_keeps_pair_policy();
  test_dual_runtime_override_replays_each_channel();
  test_single_runtime_override_uses_active_channel();
  test_single_live_write_targets_only_the_active_channel();
  test_power_cycle_makes_pre_stream_control_cache_only();
  test_zero_values_are_explicit_not_default_sentinels();
  test_prepare_generation_defines_session_boundary();
  test_new_session_with_override_requires_fresh_hardware();
  test_clearing_any_override_requires_reconciliation();
  test_shared_write_clears_override_even_when_value_is_unchanged();
  test_channel_write_sets_and_clears_override_against_baseline();
  test_shared_and_channel_batch_applies_override_after_baseline();

  printf("max9296 exposure policy: %u checks, %u failures -> %s\n", checks,
         failures, failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
