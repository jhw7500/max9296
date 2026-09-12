#!/usr/bin/env python3
"""Exercise production exposure writes, crop replay and STREAMON I/O errors."""

import argparse
import subprocess
import tempfile
from pathlib import Path

from max9296_exposure_replay_binding_test import extract_function


ROOT = Path(__file__).resolve().parents[1]


def build_harness(source: str) -> str:
    names = [
        "max9296_require_exposure_reinit_locked",
        "max9296_invalidate_exposure_hardware_locked",
        "max9296_apply_cached_crop",
        "max9296_set_exposure_cluster",
        "max9296_stream_commit_locked",
        "max9296_s_stream",
    ]
    # Include the old failure helper when testing the pre-fix revision.
    if "max9296_revoke_exposure_stream_locked" in source:
        names.insert(1, "max9296_revoke_exposure_stream_locked")
    production = "\n\n".join(extract_function(source, name) for name in names)
    return r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include "max9296_exposure_policy.h"

typedef uint32_t u32;
typedef uint64_t u64;
struct v4l2_ctrl { int val; bool is_new; };
struct v4l2_subdev { int unused; };
struct max9296_ctrls { struct v4l2_ctrl *exp_time, *exposure_ch0, *exposure_ch1; };
struct max9296_hw_fingerprint { unsigned int enable, fps; };
struct max9296_channel_ctrl { int exposure, dz_x, dz_y; };
struct max9296_ctrl_cache {
  int exposure, dz;
  struct max9296_channel_ctrl ch0, ch1;
  unsigned int exposure_override_mask;
  bool exposure_reinit_required, exposure_session_resetting, firmware_ready;
  bool crop_enable;
};
struct test_adapter { int nr; };
struct test_client { struct test_adapter *adapter; };
struct max9296_dev {
  struct max9296_ctrls ctrls;
  struct max9296_ctrl_cache ctrl_cache;
  struct max9296_hw_fingerprint initialized_fingerprint;
  struct test_client *i2c_client;
  int lock, prepare_request_lock, prepare_state, worker_errno, power_count;
  unsigned int enable;
  bool pending_mode_change, pending_fmt_change, hardware_valid;
  bool dying, prepare_releasing, streaming;
  u64 initialized_epoch, stream_commit_epoch;
  int stream_on, restart;
  struct { int fsync; } state;
  struct { struct max9296_dev *sensor; } shared;
};

#define AP1302_I2C_ADDR 0x3cU
#define AP1302_CH0_I2C_ADDR 0x11U
#define AP1302_CH1_I2C_ADDR 0x12U
#define MAX9296_PREP_PREPARING 1
#define MAX9296_STATE_IDLE 0
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define lockdep_assert_held(x) ((void)(x))
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define mutex_trylock(x) ((void)(x), true)
#define printk(...) ((void)0)

static int max9296_power_lock, debug;
static u64 max9296_hw_epoch = 7;
static struct max9296_dev *active_sensor;
static unsigned int write_count, fail_at, init_count, hw_ch0, hw_ch1;
static unsigned int crop_write_count, crop_fail_at;
static int crop_hw_x[2], crop_hw_y[2], crop_hw_zoom[2];
static int replay_error;

static struct max9296_dev *to_max9296_dev(struct v4l2_subdev *sd) {
  (void)sd; return active_sensor;
}
static bool max9296_hw_is_dual(const struct max9296_dev *sensor) {
  return sensor->initialized_fingerprint.enable == 3U;
}
static void max9296_fmt_ch(char *buf, unsigned int size,
                           struct max9296_dev *sensor, unsigned int addr) {
  (void)sensor; snprintf(buf, size, "%x", addr);
}
static void max9296_mark_prepare_stale_locked(struct max9296_dev *sensor) {
  sensor->prepare_state = 2;
}
static int max9296_preflight_exposure(struct max9296_dev *sensor,
                                      const char *channel, u32 value) {
  (void)sensor; (void)channel; (void)value; return 0;
}
static int max9296_write_exposure(struct max9296_dev *sensor, u32 addr,
                                  const char *channel, u32 value) {
  (void)sensor; (void)channel;
  if (++write_count == fail_at)
    return -EIO;
  if (addr == AP1302_CH0_I2C_ADDR || addr == AP1302_I2C_ADDR) hw_ch0 = value;
  if (addr == AP1302_CH1_I2C_ADDR || addr == AP1302_I2C_ADDR) hw_ch1 = value;
  return 0;
}
static int max9296_normalize_fingerprint_locked(
    struct max9296_dev *sensor, struct max9296_hw_fingerprint *fingerprint) {
  *fingerprint = sensor->initialized_fingerprint; return 0;
}
static int max9296_preflight_prepare_locked(
    struct max9296_dev *sensor, const struct max9296_hw_fingerprint *fingerprint) {
  (void)sensor; (void)fingerprint; return 0;
}
static int max9296_update_shared_fsync_locked(struct max9296_dev *sensor,
                                              unsigned int fps, bool reserve) {
  (void)sensor; (void)fps; (void)reserve; return 0;
}
static bool max9296_prepare_matches_locked(
    struct max9296_dev *sensor, const struct max9296_hw_fingerprint *fingerprint) {
  return sensor->initialized_fingerprint.enable == fingerprint->enable &&
         sensor->initialized_fingerprint.fps == fingerprint->fps;
}
static int max9296_prepare_hardware_locked(
    struct max9296_dev *sensor, const struct max9296_hw_fingerprint *fingerprint) {
  (void)sensor; (void)fingerprint;
  init_count++;
  return -EIO; /* No hardware initialization is allowed in these warm cases. */
}
static void max9296_drop_fsync_contract_locked(struct max9296_dev *sensor) {
  (void)sensor;
}
static int max9296_write_zoom_channel(
    struct max9296_dev *sensor, unsigned int addr, const char *channel,
    const struct max9296_channel_ctrl *ctrl, int zoom) {
  (void)channel;
  if (++crop_write_count == crop_fail_at)
    return -EIO;
  unsigned int index = addr == AP1302_CH1_I2C_ADDR ||
      (addr == AP1302_I2C_ADDR && sensor->enable == 2U);
  crop_hw_x[index] = ctrl->dz_x;
  crop_hw_y[index] = ctrl->dz_y;
  crop_hw_zoom[index] = zoom;
  return 0;
}
static int max9296_apply_cached_controls(struct max9296_dev *sensor) {
  sensor->ctrl_cache.firmware_ready = replay_error == 0;
  return replay_error;
}
static void max9296_health_forget_pair(struct max9296_dev *sensor) {
  (void)sensor;
}
static int max9296_disable_stream_mipi(struct max9296_dev *sensor) {
  (void)sensor; return 0;
}
''' + production + r'''

static unsigned int checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static struct max9296_dev fixture(struct v4l2_ctrl *ctrls) {
  static struct test_adapter adapter = {.nr = 2};
  static struct test_client client = {.adapter = &adapter};
  struct max9296_dev sensor = {
    .ctrls = {&ctrls[0], &ctrls[1], &ctrls[2]},
    .ctrl_cache = {.exposure = 7000, .ch0 = {.exposure = 7000},
                   .ch1 = {.exposure = 7000},
                   .firmware_ready = true},
    .initialized_fingerprint = {.enable = 3, .fps = 30},
    .i2c_client = &client, .power_count = 1, .enable = 3,
    .hardware_valid = true, .initialized_epoch = 7, .stream_commit_epoch = 7,
    .streaming = true,
  };
  ctrls[0] = (struct v4l2_ctrl){.val = 7000};
  ctrls[1] = (struct v4l2_ctrl){.val = 7000};
  ctrls[2] = (struct v4l2_ctrl){.val = 7000};
  write_count = fail_at = init_count = 0;
  crop_write_count = crop_fail_at = 0;
  for (unsigned int ch = 0; ch < 2; ch++)
    crop_hw_x[ch] = crop_hw_y[ch] = crop_hw_zoom[ch] = 0;
  replay_error = 0;
  hw_ch0 = hw_ch1 = 7000;
  return sensor;
}

int main(void) {
  struct v4l2_ctrl ctrls[3];
  struct v4l2_subdev sd = {0};
  struct max9296_dev sensor;
  /* Shared failure, each individual channel, and partial two-channel writes. */
  for (unsigned int kind = 0; kind < 4; kind++) {
    sensor = fixture(ctrls);
    active_sensor = &sensor;
    if (kind < 3) {
      ctrls[kind].is_new = true;
      ctrls[kind].val = 5000;
      fail_at = 1;
    } else {
      ctrls[1] = (struct v4l2_ctrl){.is_new = true, .val = 5000};
      ctrls[2] = (struct v4l2_ctrl){.is_new = true, .val = 6000};
      fail_at = 2;
    }
    CHECK(max9296_set_exposure_cluster(&sensor) == -EIO);
    CHECK(sensor.ctrl_cache.exposure == 7000);
    CHECK(sensor.ctrl_cache.ch0.exposure == 7000);
    CHECK(sensor.ctrl_cache.ch1.exposure == 7000);
    CHECK(sensor.ctrl_cache.exposure_override_mask == 0);
    CHECK(!sensor.ctrl_cache.exposure_reinit_required);
    CHECK(sensor.ctrl_cache.firmware_ready);
    CHECK(sensor.hardware_valid && sensor.initialized_epoch == 7);
    CHECK(sensor.streaming && sensor.stream_commit_epoch == 7);
    CHECK(hw_ch0 == (kind == 3 ? 5000U : 7000U) && hw_ch1 == 7000);
    CHECK(max9296_s_stream(&sd, 0) == 0);
    CHECK(max9296_s_stream(&sd, 1) == 0);
    CHECK(init_count == 0);
  }

  /* STREAMON replay failure must be retryable without reloading the firmware. */
  sensor = fixture(ctrls);
  active_sensor = &sensor;
  sensor.streaming = false;
  sensor.stream_commit_epoch = 0;
  replay_error = -EIO;
  CHECK(max9296_s_stream(&sd, 1) == -EIO);
  CHECK(!sensor.streaming);
  CHECK(sensor.hardware_valid && sensor.initialized_epoch == 7);
  CHECK(!sensor.ctrl_cache.exposure_reinit_required);
  replay_error = 0;
  CHECK(max9296_s_stream(&sd, 1) == 0);
  CHECK(sensor.streaming && sensor.stream_commit_epoch == 7);
  CHECK(init_count == 0);

  /* A post-initialization crop failure must not force another cold attempt.
   * Exercise prepared and stopped streams, including a partial dual write. */
  for (unsigned int mode = 1; mode <= 3; mode++) {
    for (unsigned int fps = 30; fps <= 120; fps += 90) {
      for (unsigned int ready = 0; ready <= 1; ready++) {
        unsigned int channel_count = mode == 3 ? 2 : 1;
        for (unsigned int failure = 1; failure <= channel_count; failure++) {
          sensor = fixture(ctrls);
          active_sensor = &sensor;
          sensor.enable = sensor.initialized_fingerprint.enable = mode;
          sensor.initialized_fingerprint.fps = fps;
          sensor.streaming = false;
          sensor.stream_commit_epoch = 0;
          sensor.ctrl_cache.firmware_ready = ready;
          sensor.ctrl_cache.crop_enable = true;
          sensor.ctrl_cache.dz = 0x0180;
          sensor.ctrl_cache.ch0.dz_x = 100;
          sensor.ctrl_cache.ch0.dz_y = 200;
          sensor.ctrl_cache.ch1.dz_x = 300;
          sensor.ctrl_cache.ch1.dz_y = 400;
          crop_fail_at = failure;

          CHECK(max9296_s_stream(&sd, 1) == -EIO);
          CHECK(!sensor.streaming && sensor.stream_commit_epoch == 0);
          CHECK(sensor.hardware_valid && sensor.initialized_epoch == 7);
          CHECK(sensor.ctrl_cache.firmware_ready == (bool)ready);
          CHECK(!sensor.ctrl_cache.exposure_reinit_required);
          CHECK(crop_write_count == failure && init_count == 0);
          CHECK(crop_hw_x[0] == (failure == 2 ? 100 : 0));
          CHECK(crop_hw_x[1] == 0);

          crop_fail_at = 0;
          CHECK(max9296_s_stream(&sd, 1) == 0);
          CHECK(sensor.streaming && sensor.stream_commit_epoch == 7);
          CHECK(init_count == 0);
          CHECK(crop_write_count == failure + channel_count);
          CHECK(crop_hw_x[0] == ((mode & 1) ? 100 : 0));
          CHECK(crop_hw_y[0] == ((mode & 1) ? 200 : 0));
          CHECK(crop_hw_x[1] == ((mode & 2) ? 300 : 0));
          CHECK(crop_hw_y[1] == ((mode & 2) ? 400 : 0));
          CHECK(crop_hw_zoom[0] == ((mode & 1) ? 0x0180 : 0));
          CHECK(crop_hw_zoom[1] == ((mode & 2) ? 0x0180 : 0));
        }
      }
    }
  }

  /* A successful explicit retry commits the requested exposure normally. */
  sensor = fixture(ctrls);
  ctrls[2] = (struct v4l2_ctrl){.is_new = true, .val = 5000};
  fail_at = 1;
  CHECK(max9296_set_exposure_cluster(&sensor) == -EIO);
  fail_at = 0;
  CHECK(max9296_set_exposure_cluster(&sensor) == 0);
  CHECK(hw_ch1 == 5000 && sensor.ctrl_cache.ch1.exposure == 5000);
  CHECK(sensor.ctrl_cache.exposure_override_mask == MAX9296_EXPOSURE_OVERRIDE_CH1);

  /* A rejected update preserves an existing override, including its ownership. */
  sensor = fixture(ctrls);
  sensor.ctrl_cache.ch1.exposure = 5000;
  sensor.ctrl_cache.exposure_override_mask = MAX9296_EXPOSURE_OVERRIDE_CH1;
  ctrls[2] = (struct v4l2_ctrl){.is_new = true, .val = 6000};
  fail_at = 1;
  CHECK(max9296_set_exposure_cluster(&sensor) == -EIO);
  CHECK(sensor.ctrl_cache.ch1.exposure == 5000);
  CHECK(sensor.ctrl_cache.exposure_override_mask == MAX9296_EXPOSURE_OVERRIDE_CH1);
  CHECK(!sensor.ctrl_cache.exposure_reinit_required);

  /* A write failure must not erase an already pending session reset. */
  sensor = fixture(ctrls);
  sensor.ctrl_cache.exposure_reinit_required = true;
  ctrls[0] = (struct v4l2_ctrl){.is_new = true, .val = 9000};
  fail_at = 1;
  CHECK(max9296_set_exposure_cluster(&sensor) == -EIO);
  CHECK(sensor.ctrl_cache.exposure_reinit_required);

  printf("max9296 exposure failure binding: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT / "max9296.c")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="max9296-exposure-failure-") as tmp:
        harness = Path(tmp) / "failure.c"
        binary = Path(tmp) / "failure"
        harness.write_text(build_harness(args.source.read_text()))
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
             "-I", str(ROOT),
             str(harness), "-o", str(binary)], check=True,
        )
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
