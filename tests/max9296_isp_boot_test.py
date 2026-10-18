#!/usr/bin/env python3
"""Run the production AP1302 boot checks (#110) against scripted ISP reads.

The judgement and record functions are extracted from max9296.c and compiled
with only the I2C read, sleep and log calls replaced, so the rules below are the
driver's own.  Their placement around the firmware download and the health_raw
no-read contract are checked on the source, because no host harness reaches
max9296_loadfw() or the sysfs show path.  Physical boot timing is the board's
to prove (issue #110 AC1-AC3), not this file's.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from max9296_exposure_replay_binding_test import extract_function


ROOT = Path(__file__).resolve().parents[1]
DEFINES = (
    "AP1302_I2C_ADDR", "AP1302_CH0_I2C_ADDR", "AP1302_CH1_I2C_ADDR",
    "AP1302_REG_PREVIEW_WIDTH", "AP1302_PREVIEW_WIDTH_ROM",
    "MAX9296_ISP_BOOT_REREADS", "MAX9296_ISP_BOOT_REREAD_MS",
)
FUNCTIONS = (
    "max9296_isp_output_width", "max9296_isp_boot_stage_name",
    "max9296_isp_boot_status_name", "max9296_isp_boot_value_ok",
    "max9296_isp_boot_begin_locked", "max9296_isp_boot_check_locked",
)


def extract_block(source: str, pattern: str) -> str:
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"missing block: {pattern}")
    return match.group(0)


def build_harness(source: str) -> str:
    defines = []
    for name in DEFINES:
        match = re.search(rf"^#define {name} .*$", source, re.MULTILINE)
        if not match:
            raise ValueError(f"missing define {name}")
        defines.append(match.group(0))
    stage = extract_block(source, r"^enum max9296_isp_boot_stage \{.*?^\};")
    record = extract_block(source, r"^struct max9296_isp_boot \{.*?^\};")
    functions = "\n".join(extract_function(source, name) for name in FUNCTIONS)
    return r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define lockdep_assert_held(x) ((void)(x))
''' + "\n".join(defines) + "\n" + stage + "\n" + record + r'''
struct max9296_mode_info { unsigned width; bool dual; };
struct max9296_hw_fingerprint { const struct max9296_mode_info *mode; u32 enable; };
struct i2c_client { int dev; };
struct max9296_dev {
  struct i2c_client *i2c_client;
  int lock;
  struct { unsigned ch_shift; } link_status;
  struct max9296_isp_boot isp_boot;
};
static bool max9296_mode_is_dual(const struct max9296_mode_info *m) { return m->dual; }

/* Scripted ISP replies: each address answers from its own list, repeating the
 * last entry once the list runs out. */
struct reply { int err; unsigned value; };
static struct reply replies[3][16];
static unsigned reply_count[3], reads[3], sleeps, err_lines, notice_lines;
static int slot(unsigned addr) {
  return addr == AP1302_CH0_I2C_ADDR ? 0 : addr == AP1302_CH1_I2C_ADDR ? 1
       : addr == AP1302_I2C_ADDR ? 2 : -1;
}
static int max9296_health_i2c_read_once(struct max9296_dev *s, unsigned addr,
    unsigned reg, unsigned reg_byte, unsigned val_byte, unsigned *val) {
  (void)s;
  int i = slot(addr);
  if (i < 0 || reg != AP1302_REG_PREVIEW_WIDTH || reg_byte != 2 || val_byte != 2)
    return -EINVAL;
  unsigned n = reads[i]++;
  const struct reply *r = &replies[i][n < reply_count[i] ? n : reply_count[i] - 1];
  *val = r->err ? 0 : r->value;
  return r->err;
}
static void msleep(unsigned ms) { (void)ms; sleeps++; }
#define dev_err(dev, ...) ((void)(dev), err_lines++)
#define dev_notice(dev, ...) ((void)(dev), notice_lines++)
''' + functions + r'''
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void script(unsigned addr, unsigned n, const struct reply *list) {
  int i = slot(addr);
  memcpy(replies[i], list, n * sizeof(*list));
  reply_count[i] = n;
}
static void quiet(void) {
  memset(reads, 0, sizeof(reads));
  sleeps = err_lines = notice_lines = 0;
}

static const struct max9296_mode_info dual_720 = {2560, true};
static const struct max9296_mode_info single_720 = {1280, false};
static struct i2c_client client;

static struct max9296_dev begin(const struct max9296_mode_info *mode,
                                unsigned enable) {
  struct max9296_dev s = {.i2c_client = &client, .link_status.ch_shift = 2};
  struct max9296_hw_fingerprint fp = {mode, enable};
  s.isp_boot.err[0] = -EIO; /* begin must clear the previous attempt */
  max9296_isp_boot_begin_locked(&s, &fp, 9);
  quiet();
  return s;
}

int main(void) {
  const struct reply rom = {0, AP1302_PREVIEW_WIDTH_ROM}, fw = {0, 0x0780},
                     cfg = {0, 1280}, zero = {0, 0}, nack = {-ENXIO, 0};
  const unsigned rereads = MAX9296_ISP_BOOT_REREADS;

  /* Judgement per stage: only the values a failed boot leaves are refused. */
  CHECK(!max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_PRE_DOWNLOAD, 0, 1280));
  CHECK(max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_PRE_DOWNLOAD, 0x0280, 1280));
  CHECK(max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_PRE_DOWNLOAD, 0x0780, 1280));
  CHECK(!max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_POST_DOWNLOAD, 0, 1280));
  CHECK(!max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_POST_DOWNLOAD, 0x0280, 1280));
  CHECK(max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_POST_DOWNLOAD, 0x0780, 1280));
  CHECK(max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_POST_DOWNLOAD, 0x0500, 1280));
  CHECK(max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_POST_CONFIG, 1280, 1280));
  CHECK(!max9296_isp_boot_value_ok(MAX9296_ISP_BOOT_POST_CONFIG, 0x0780, 1280));

  /* begin: dual checks both ISPs at the per-ISP width; single checks the one
   * enabled channel; the previous attempt's record is gone. */
  struct max9296_dev s = begin(&dual_720, 3);
  CHECK(s.isp_boot.epoch == 9 && s.isp_boot.dual && s.isp_boot.mask == 3);
  CHECK(s.isp_boot.expected_width == 1280 && s.isp_boot.stage == -1);
  CHECK(s.isp_boot.err[0] == 0 && !s.isp_boot.failed);
  CHECK(!strcmp(max9296_isp_boot_status_name(&s.isp_boot), "INCOMPLETE"));
  s = begin(&single_720, 2);
  CHECK(!s.isp_boot.dual && s.isp_boot.mask == 2 && s.isp_boot.expected_width == 1280);
  struct max9296_isp_boot never = {0};
  CHECK(!strcmp(max9296_isp_boot_status_name(&never), "NOT_RUN"));

  /* Healthy dual boot: one read per ISP per stage, no sleep, and pass lines
   * only after post_config. */
  s = begin(&dual_720, 3);
  script(AP1302_CH0_I2C_ADDR, 1, &rom);
  script(AP1302_CH1_I2C_ADDR, 1, &rom);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_PRE_DOWNLOAD) == 0);
  script(AP1302_CH0_I2C_ADDR, 1, &fw);
  script(AP1302_CH1_I2C_ADDR, 1, &fw);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_POST_DOWNLOAD) == 0);
  CHECK(notice_lines == 0);
  script(AP1302_CH0_I2C_ADDR, 1, &cfg);
  script(AP1302_CH1_I2C_ADDR, 1, &cfg);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_POST_CONFIG) == 0);
  CHECK(reads[0] == 3 && reads[1] == 3 && reads[2] == 0 && sleeps == 0);
  CHECK(err_lines == 0 && notice_lines == 2);
  CHECK(!strcmp(max9296_isp_boot_status_name(&s.isp_boot), "OK"));
  CHECK(s.isp_boot.value[0] == 1280 && s.isp_boot.value_valid[1]);

  /* Type B: an ISP that does not answer before the download fails at once,
   * with the read's own errno, and its partner is still judged. */
  s = begin(&dual_720, 3);
  script(AP1302_CH0_I2C_ADDR, 1, &nack);
  script(AP1302_CH1_I2C_ADDR, 1, &rom);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_PRE_DOWNLOAD) == -ENXIO);
  CHECK(reads[0] == 1 && reads[1] == 1 && sleeps == 0 && err_lines == 1);
  CHECK(s.isp_boot.err[0] == -ENXIO && !s.isp_boot.value_valid[0]);
  CHECK(s.isp_boot.err[1] == 0 && s.isp_boot.value[1] == AP1302_PREVIEW_WIDTH_ROM);
  CHECK(!strcmp(max9296_isp_boot_status_name(&s.isp_boot), "FAILED"));
  CHECK(!strcmp(max9296_isp_boot_stage_name(s.isp_boot.stage), "pre_download"));
  s = begin(&dual_720, 3);
  script(AP1302_CH0_I2C_ADDR, 1, &rom);
  script(AP1302_CH1_I2C_ADDR, 1, &zero);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_PRE_DOWNLOAD) == -EIO);
  CHECK(reads[1] == 1 && sleeps == 0 && s.isp_boot.value_valid[1]);

  /* Type A: an ISP left in its ROM after the download fails once the reread
   * budget is spent; the other ISP is read exactly once. */
  s = begin(&dual_720, 3);
  script(AP1302_CH0_I2C_ADDR, 1, &rom);
  script(AP1302_CH1_I2C_ADDR, 1, &fw);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_POST_DOWNLOAD) == -EIO);
  CHECK(reads[0] == 1 + rereads && reads[1] == 1 && sleeps == rereads);
  CHECK(s.isp_boot.err[0] == -EIO && s.isp_boot.err[1] == 0 && s.isp_boot.failed);
  CHECK(err_lines == 1 && notice_lines == 0);

  /* A healthy ISP caught mid-transition passes on a reread. */
  s = begin(&dual_720, 3);
  const struct reply settling[] = {rom, fw};
  script(AP1302_CH0_I2C_ADDR, 2, settling);
  script(AP1302_CH1_I2C_ADDR, 1, &fw);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_POST_DOWNLOAD) == 0);
  CHECK(reads[0] == 2 && sleeps == 1 && !s.isp_boot.failed);

  /* post_config compares with the width written, after the same budget. */
  s = begin(&dual_720, 3);
  script(AP1302_CH0_I2C_ADDR, 1, &cfg);
  script(AP1302_CH1_I2C_ADDR, 1, &fw);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_POST_CONFIG) == -EIO);
  CHECK(reads[1] == 1 + rereads && notice_lines == 0 && err_lines == 1);
  CHECK(!strcmp(max9296_isp_boot_stage_name(s.isp_boot.stage), "post_config"));

  /* Single mode reads the one ISP through the broadcast address only. */
  s = begin(&single_720, 2);
  script(AP1302_I2C_ADDR, 1, &cfg);
  CHECK(max9296_isp_boot_check_locked(&s, MAX9296_ISP_BOOT_POST_CONFIG) == 0);
  CHECK(reads[0] == 0 && reads[1] == 0 && reads[2] == 1 && notice_lines == 1);
  CHECK(!s.isp_boot.value_valid[0] && s.isp_boot.value[1] == 1280);

  printf("max9296 isp boot checks: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
'''


def ordered(body: str, *needles: str) -> bool:
    position = -1
    for needle in needles:
        found = body.find(needle, position + 1)
        if found < 0:
            return False
        position = found
    return True


def source_checks(source: str) -> list:
    """Placement contracts no host harness reaches.  Each entry: (ok, what)."""
    loadfw = extract_function(source, "max9296_loadfw")
    hardware = extract_function(source, "max9296_prepare_hardware_locked")
    collect = extract_function(source, "max9296_collect_health_locked")
    show = extract_function(source, "sysfs_health_raw_show")
    pre = "max9296_isp_boot_check_locked(sensor, MAX9296_ISP_BOOT_PRE_DOWNLOAD)"
    post_dl = re.compile(
        r"max9296_isp_boot_check_locked\(\s*sensor,\s*MAX9296_ISP_BOOT_POST_DOWNLOAD\)")
    post_dl_match = post_dl.search(loadfw)
    post_dl_text = post_dl_match.group(0) if post_dl_match else "<missing>"

    def releases_after(call: str) -> bool:
        index = loadfw.find(call)
        if index < 0:
            return False
        block = loadfw[index:loadfw.find("}", index)]
        return "release_firmware(fw);" in block and "return retval;" in block

    return [
        (ordered(loadfw, "request_firmware(", pre, "start_fw_load(client)"),
         "pre_download reads after request_firmware and before start_fw_load"),
        (ordered(loadfw, "end_fw_load(client)", post_dl_text, "size = fw->size;"),
         "post_download reads after end_fw_load succeeds"),
        (releases_after(pre) and releases_after(post_dl_text),
         "a failed download-stage check releases the firmware"),
        (ordered(hardware, "if (sensor->cold_init_epoch == epoch)",
                 "sensor->cold_init_epoch = epoch;",
                 "max9296_isp_boot_begin_locked(sensor, fingerprint, epoch);",
                 "max9296_set_mode("),
         "the record resets only after the cold-attempt guard admits"),
        (ordered(hardware, "max9296_post_firmware_program_locked(",
                 "MAX9296_ISP_BOOT_POST_CONFIG", "max9296_apply_cached_crop("),
         "post_config reads after post-firmware programming"),
        (source.count("max9296_isp_boot_check_locked(") == 4,
         "the check runs from exactly three call sites (plus its definition)"),
        ("sample->isp_boot = sensor->isp_boot;" in collect
         and "isp_boot_check" not in collect and "isp_boot_check" not in show,
         "health_raw copies the record and never re-reads an ISP for it"),
        (ordered(show, "mutex_unlock(&sensor->lock);", "\\\"isp_boot\\\":"),
         "health_raw prints the record after dropping the lock"),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT / "max9296.c")
    args = parser.parse_args()
    source = args.source.read_text()

    results = source_checks(source)
    failed = 0
    for ok, what in results:
        if not ok:
            failed += 1
            print(f"FAIL source: {what}", file=sys.stderr)
    print(f"max9296 isp boot source checks: {len(results)} checks, "
          f"{failed} failures")

    with tempfile.TemporaryDirectory(prefix="max9296-isp-boot-") as tmp:
        harness = Path(tmp) / "isp_boot.c"
        binary = Path(tmp) / "isp_boot"
        harness.write_text(build_harness(source))
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(harness), "-o", str(binary),
        ], check=True)
        rc = subprocess.run([str(binary)], check=False).returncode
    return 1 if failed or rc else 0


if __name__ == "__main__":
    raise SystemExit(main())
