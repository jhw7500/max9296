#ifndef MAX9296_EXPOSURE_POLICY_H
#define MAX9296_EXPOSURE_POLICY_H

#define MAX9296_EXPOSURE_OVERRIDE_CH0 (1U << 0)
#define MAX9296_EXPOSURE_OVERRIDE_CH1 (1U << 1)

#define MAX9296_EXPOSURE_UPDATE_CH0 MAX9296_EXPOSURE_OVERRIDE_CH0
#define MAX9296_EXPOSURE_UPDATE_CH1 MAX9296_EXPOSURE_OVERRIDE_CH1
#define MAX9296_EXPOSURE_UPDATE_SHARED (1U << 2)

enum max9296_exposure_seed_route {
  MAX9296_EXPOSURE_SEED_SKIP = 0,
  MAX9296_EXPOSURE_SEED_PAIR,
  MAX9296_EXPOSURE_SEED_CHANNEL,
};

struct max9296_exposure_replay_decision {
  enum max9296_exposure_seed_route route;
  unsigned int value;
  unsigned int has_runtime_override;
};

struct max9296_exposure_control_state {
  unsigned int shared;
  unsigned int ch0;
  unsigned int ch1;
  unsigned int override_mask;
};

static inline struct max9296_exposure_control_state
max9296_exposure_control_update(
    struct max9296_exposure_control_state previous, unsigned int update_mask,
    unsigned int requested_shared, unsigned int requested_ch0,
    unsigned int requested_ch1) {
  struct max9296_exposure_control_state desired = previous;

  if (update_mask & MAX9296_EXPOSURE_UPDATE_SHARED) {
    desired.shared = requested_shared;
    desired.ch0 = requested_shared;
    desired.ch1 = requested_shared;
    desired.override_mask = 0U;
  }

  if (update_mask & MAX9296_EXPOSURE_UPDATE_CH0) {
    desired.ch0 = requested_ch0;
    if (desired.ch0 == desired.shared)
      desired.override_mask &= ~MAX9296_EXPOSURE_OVERRIDE_CH0;
    else
      desired.override_mask |= MAX9296_EXPOSURE_OVERRIDE_CH0;
  }

  if (update_mask & MAX9296_EXPOSURE_UPDATE_CH1) {
    desired.ch1 = requested_ch1;
    if (desired.ch1 == desired.shared)
      desired.override_mask &= ~MAX9296_EXPOSURE_OVERRIDE_CH1;
    else
      desired.override_mask |= MAX9296_EXPOSURE_OVERRIDE_CH1;
  }

  return desired;
}

/* Decide only the cached replay operation. Shared EXP_TIME establishes the
 * normal pair baseline. An explicit EXPOSURE_CHx write changes the contract
 * for the current userspace prepare generation: replay restores each channel
 * exactly, even when that can make a dual-wide pair unusable. */
static inline struct max9296_exposure_replay_decision
max9296_exposure_replay_decision(
    unsigned int dual, unsigned int local_channel,
    unsigned int override_mask, unsigned int pair_ae_on, unsigned int fps,
    unsigned int safe_max_fps, unsigned int shared_value,
    unsigned int ch0_value, unsigned int ch1_value) {
  struct max9296_exposure_replay_decision decision;
  unsigned int channel_override =
      local_channel ? MAX9296_EXPOSURE_OVERRIDE_CH1
                    : MAX9296_EXPOSURE_OVERRIDE_CH0;
  unsigned int has_runtime_override =
      dual ? override_mask != 0U : (override_mask & channel_override) != 0U;

  decision.has_runtime_override = has_runtime_override;
  decision.value = has_runtime_override
                       ? (local_channel ? ch1_value : ch0_value)
                       : shared_value;

  if (!has_runtime_override && pair_ae_on && fps > safe_max_fps)
    decision.route = MAX9296_EXPOSURE_SEED_SKIP;
  else if (dual && !has_runtime_override)
    decision.route = MAX9296_EXPOSURE_SEED_PAIR;
  else
    decision.route = MAX9296_EXPOSURE_SEED_CHANNEL;

  return decision;
}

static inline unsigned int max9296_exposure_channel_is_active(
    unsigned int dual, unsigned int active_local_channel,
    unsigned int requested_local_channel) {
  return dual || active_local_channel == requested_local_channel;
}

static inline unsigned int max9296_exposure_hardware_can_apply(
    unsigned int session_resetting, unsigned int pending_mode_change,
    unsigned int pending_fmt_change, unsigned int topology_matches,
    unsigned int powered, unsigned int firmware_ready,
    unsigned int hardware_valid, unsigned long long initialized_epoch,
    unsigned long long current_epoch) {
  return !session_resetting && !pending_mode_change && !pending_fmt_change &&
         topology_matches && powered && firmware_ready && hardware_valid &&
         initialized_epoch == current_epoch;
}

static inline unsigned int max9296_exposure_override_was_cleared(
    unsigned int previous_mask, unsigned int desired_mask) {
  return (previous_mask & ~desired_mask) != 0U;
}

static inline unsigned int max9296_exposure_session_should_reset(
    unsigned long long current_generation,
    unsigned long long requested_generation) {
  return requested_generation != 0ULL &&
         requested_generation != current_generation;
}

static inline unsigned int max9296_exposure_session_requires_reinit(
    unsigned long long current_generation,
    unsigned long long requested_generation, unsigned int override_mask) {
  return override_mask != 0U &&
         max9296_exposure_session_should_reset(current_generation,
                                               requested_generation);
}

#endif
