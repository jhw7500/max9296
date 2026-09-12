#!/usr/bin/env python3
"""Run production prepare and mode tables against retained serializer addresses.

Only I/O, firmware transfer, V4L2 cache plumbing and lease scheduling are mocked.
This proves admission/order and error propagation, not physical reset timing.
"""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path

from max9296_exposure_replay_binding_test import extract_function


ROOT = Path(__file__).resolve().parents[1]


def build_harness(source: str) -> str:
    tables = "\n".join(re.findall(
        r"static const struct reg_value max9296_init_setting_[^;]+;", source
    ))
    hardware = "\n".join(extract_function(source, name) for name in (
        "max9296_load_regs", "max9296_set_mode",
        "max9296_require_exposure_reinit_locked",
        "max9296_invalidate_exposure_hardware_locked",
        "max9296_prepare_hardware_locked",
    ))
    request = extract_function(source, "max9296_prepare_request")
    return r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include "max9296_exposure_policy.h"
typedef uint32_t u32;
typedef uint64_t u64;
struct reg_value { unsigned slave_addr, reg_addr, reg_byte, val, val_byte, delay_ms; };
struct max9296_mode_info {
  unsigned width, height;
  const struct reg_value *reg_data;
  unsigned reg_data_size;
};
struct max9296_hw_fingerprint {
  const struct max9296_mode_info *mode;
  unsigned enable, fps;
};
struct i2c_client { int unused; };
struct max9296_dev {
  struct i2c_client *i2c_client;
  struct { unsigned ch_shift; int disconnect; } link_status;
  struct { int init, firmware, enable; } state;
  struct {
    bool firmware_ready, exposure_reinit_required, exposure_session_resetting;
    unsigned exposure_override_mask, exposure, ch0, ch1;
    u64 exposure_session_generation;
  } ctrl_cache;
  struct { struct max9296_dev *exp_time; } ctrls;
  struct { bool probe_ready; } shared;
  int lock, prepare_request_lock, prepare_lease_timeout, power_count;
  int prepare_state, prepare_errno;
  bool dying, prepare_releasing, streaming, prepare_lease_held, hardware_valid;
  u64 prepare_generation, prepare_lease_generation;
  u64 initialized_epoch, stream_commit_epoch, cold_init_epoch;
  unsigned enable;
  struct max9296_hw_fingerprint initialized_fingerprint, prepare_fingerprint;
  const struct max9296_mode_info *last_mode;
};
#define SERDES_3GBPS
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x) = (v))
#define lockdep_assert_held(x) ((void)(x))
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define mutex_trylock(x) ((void)(x), true)
#define printk(...) ((void)0)
#define dev_err(...) ((void)0)
#define WARN_ON(x) (x)
#define MAX9296_STATE_IDLE 0
#define MAX9296_STATE_RUNNING 1
#define MAX9296_STATE_DONE 2
#define MAX9296_STATE_FAILED 3
#define MAX9296_PREP_PREPARING 1
#define MAX9296_PREP_READY 2
#define MAX9296_PREP_FAILED 3
#define HZ 100
static int debug, max9296_power_lock, system_wq;
static u64 max9296_hw_epoch = 7;
static unsigned addresses[2], visible, writes, fail_write, fw_loads, fw_error;
static unsigned preflight_error, post_error, crop_error, control_error;
static void usleep_range(unsigned a, unsigned b) { (void)a; (void)b; }
static int maxim_ops_i2c_write(struct max9296_dev *s, unsigned dest,
                             unsigned reg, unsigned val, unsigned rb, unsigned vb) {
  (void)s; (void)rb; (void)vb;
  if (++writes == fail_write) return -EIO;
  if (!dest) {
    if (reg == 0x10) visible = (val & 3) == 1 ? 1 : (val & 3) == 2 ? 2 : 3;
    return 0;
  }
  unsigned targets = 0;
  for (unsigned i = 0; i < 2; i++)
    if ((visible & (1U << i)) && addresses[i] == dest) targets |= 1U << i;
  if (!targets) return -ENXIO;
  for (unsigned i = 0; i < 2; i++)
    if ((targets & (1U << i)) && reg == 0) addresses[i] = val >> 1;
  return 0;
}
static int max9296_preflight_prepare_locked(struct max9296_dev *s,
    const struct max9296_hw_fingerprint *fp) {
  (void)s; (void)fp; return -(int)preflight_error;
}
static int max9296_loadfw(struct i2c_client *c) {
  (void)c; fw_loads++; return -(int)fw_error;
}
static int max9296_post_firmware_program_locked(struct max9296_dev *s,
    const struct max9296_hw_fingerprint *fp) {
  (void)s; (void)fp; return -(int)post_error;
}
static int max9296_apply_cached_crop(struct max9296_dev *s) {
  (void)s; return -(int)crop_error;
}
static int max9296_apply_cached_controls(struct max9296_dev *s) {
  s->ctrl_cache.firmware_ready = !control_error; return -(int)control_error;
}
static void max9296_mark_prepare_stale_locked(struct max9296_dev *s) {
  s->prepare_state = 4;
}
''' + tables + '\n' + hardware + r'''
static bool max9296_fingerprint_equal(const struct max9296_hw_fingerprint *a,
                                     const struct max9296_hw_fingerprint *b) {
  return a->mode == b->mode && a->enable == b->enable && a->fps == b->fps;
}
static bool max9296_prepare_matches_locked(struct max9296_dev *s,
    const struct max9296_hw_fingerprint *fp) {
  return max9296_fingerprint_equal(&s->initialized_fingerprint, fp);
}
static int __v4l2_ctrl_s_ctrl(struct max9296_dev *s, unsigned exposure) {
  s->ctrl_cache.ch0 = s->ctrl_cache.ch1 = exposure;
  s->ctrl_cache.exposure_override_mask = 0;
  return 0;
}
static void cancel_delayed_work_sync(int *work) { (void)work; }
static bool queue_delayed_work(int queue, int *work, unsigned timeout) {
  (void)queue; (void)work; (void)timeout; return true;
}
static int max9296_update_shared_fsync_locked(struct max9296_dev *s,
                                              unsigned fps, bool reserve) {
  (void)s; (void)fps; (void)reserve; return 0;
}
static void max9296_apply_prepare_fingerprint_locked(struct max9296_dev *s,
    const struct max9296_hw_fingerprint *fp) { s->enable = fp->enable; }
static int max9296_queue_prepare_lease_locked(struct max9296_dev *s, u64 gen) {
  (void)s; (void)gen; return 0;
}
static int max9296_prepare_existing_lease_locked(struct max9296_dev *s,
    const struct max9296_hw_fingerprint *fp, u64 gen) {
  s->prepare_generation = gen;
  s->prepare_fingerprint = *fp;
  int ret = max9296_prepare_hardware_locked(s, fp);
  s->prepare_state = ret ? MAX9296_PREP_FAILED : MAX9296_PREP_READY;
  return ret;
}
static int max9296_prepare_request_locked(struct max9296_dev *s,
    const struct max9296_hw_fingerprint *fp, u64 gen) {
  s->prepare_lease_held = true;
  return max9296_prepare_existing_lease_locked(s, fp, gen);
}
static bool max9296_prepare_lease_can_arm_locked(struct max9296_dev *s, u64 gen) {
  (void)s; (void)gen; return true;
}
static void max9296_drop_fsync_contract_locked(struct max9296_dev *s) { (void)s; }
static int max9296_set_power(struct max9296_dev *s, bool on) {
  /* A peer retains power; release cannot advance the board epoch. */
  (void)s; (void)on; return 0;
}
''' + request + r'''
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void reset_bus(void) {
  addresses[0] = addresses[1] = 0x40;
  visible = 3;
  writes = fw_loads = fail_write = 0;
  preflight_error = fw_error = post_error = crop_error = control_error = 0;
}
static struct max9296_dev fixture(unsigned enable) {
  static struct i2c_client client;
  reset_bus();
  return (struct max9296_dev) {
    .i2c_client = &client, .enable = enable, .shared.probe_ready = true,
    .prepare_lease_held = true, .ctrl_cache.exposure = 7000,
  };
}

int main(void) {
  const struct max9296_mode_info modes[] = {
    {1280, 360, max9296_init_setting_1080p_crop_720p_2ch_30fps,
      ARRAY_SIZE(max9296_init_setting_1080p_crop_720p_2ch_30fps)},
    {640, 360, max9296_init_setting_720p_30fps_L,
      ARRAY_SIZE(max9296_init_setting_720p_30fps_L)},
    {640, 360, max9296_init_setting_720p_30fps_R,
      ARRAY_SIZE(max9296_init_setting_720p_30fps_R)},
  };
  const unsigned enables[] = {3, 1, 2};
  for (unsigned mode = 0; mode < ARRAY_SIZE(modes); mode++) {
    for (unsigned fps = 30; fps <= 120; fps += 90) {
      struct max9296_hw_fingerprint fp = {&modes[mode], enables[mode], fps};
      struct max9296_dev s = fixture(fp.enable);
      s.ctrls.exp_time = &s;
      CHECK(max9296_prepare_request(&s, &fp, 100) == 0);
      CHECK(s.hardware_valid && s.initialized_epoch == max9296_hw_epoch);
      unsigned before = writes;
      CHECK(fw_loads == 1);
      /* Same session retains a channel override without cold initialization. */
      s.ctrl_cache.exposure_override_mask = 2;
      s.ctrl_cache.ch1 = 5000;
      CHECK(max9296_prepare_request(&s, &fp, 100) == 0);
      CHECK(s.ctrl_cache.ch1 == 5000 && writes == before && fw_loads == 1);
      /* New process discards overrides, but must not replay a cold table. */
      CHECK(max9296_prepare_request(&s, &fp, 101) == -ESTALE);
      CHECK(writes == before && fw_loads == 1);
      CHECK(!s.hardware_valid && !s.ctrl_cache.firmware_ready);
      CHECK(s.ctrl_cache.exposure_override_mask == 0);
      CHECK(s.ctrl_cache.ch0 == 7000 && s.ctrl_cache.ch1 == 7000);
      CHECK(s.ctrl_cache.exposure_reinit_required);
      CHECK(max9296_prepare_request(&s, &fp, 102) == -ESTALE);
      CHECK(writes == before && fw_loads == 1);
      /* Invalid readiness must not allow a different tuple to replay either. */
      struct max9296_hw_fingerprint changed = fp;
      changed.mode = &modes[(mode + 1) % ARRAY_SIZE(modes)];
      CHECK(max9296_prepare_hardware_locked(&s, &changed) == -ESTALE);
      CHECK(writes == before && fw_loads == 1);
      max9296_hw_epoch++;
      reset_bus();
      CHECK(max9296_prepare_request(&s, &fp, 103) == 0);
      CHECK(s.hardware_valid && !s.ctrl_cache.exposure_reinit_required);
      CHECK(s.initialized_epoch == max9296_hw_epoch && fw_loads == 1);
    }
  }
  /* With no override to discard, a new process still reuses current hardware. */
  struct max9296_hw_fingerprint fp = {&modes[0], 3, 120};
  struct max9296_dev s = fixture(3);
  s.ctrls.exp_time = &s;
  CHECK(max9296_prepare_request(&s, &fp, 200) == 0);
  unsigned before = writes;
  CHECK(max9296_prepare_request(&s, &fp, 201) == 0);
  CHECK(s.hardware_valid && writes == before && fw_loads == 1);

  /* An initial table/firmware/post/crop/control failure may have changed HW. */
  for (unsigned stage = 0; stage < 5; stage++) {
    s = fixture(3);
    if (stage == 0) fail_write = 25; /* After the serializer address remap. */
    if (stage == 1) fw_error = EIO;
    if (stage == 2) post_error = EIO;
    if (stage == 3) crop_error = EIO;
    if (stage == 4) control_error = EIO;
    CHECK(max9296_prepare_hardware_locked(&s, &fp) == -EIO);
    CHECK(!s.hardware_valid && !s.ctrl_cache.firmware_ready);
    before = writes;
    unsigned loads = fw_loads;
    fail_write = fw_error = post_error = crop_error = control_error = 0;
    CHECK(max9296_prepare_hardware_locked(&s, &fp) == -ESTALE);
    CHECK(writes == before && fw_loads == loads);
    max9296_hw_epoch++;
    reset_bus();
    CHECK(max9296_prepare_hardware_locked(&s, &fp) == 0);
  }
  /* Read-only preflight failure does not consume the cold initialization. */
  s = fixture(3);
  preflight_error = EINVAL;
  CHECK(max9296_prepare_hardware_locked(&s, &fp) == -EINVAL);
  CHECK(writes == 0 && fw_loads == 0);
  preflight_error = 0;
  CHECK(max9296_prepare_hardware_locked(&s, &fp) == 0);
  printf("max9296 cold init epoch binding: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT / "max9296.c")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="max9296-cold-init-") as tmp:
        harness = Path(tmp) / "cold_init.c"
        binary = Path(tmp) / "cold_init"
        harness.write_text(build_harness(args.source.read_text()))
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
            str(harness), "-o", str(binary),
        ], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
