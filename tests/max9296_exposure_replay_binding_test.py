#!/usr/bin/env python3
"""Compile the production exposure-replay decision against exhaustive states."""

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def extract_function(source: str, name: str) -> str:
    """Extract one complete static C function with lexical brace matching."""
    definition = re.search(
        rf"^static\b[^;{{}}]*\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{",
        source,
        re.MULTILINE,
    )
    if not definition:
        raise ValueError(f"static function definition for {name} is missing")

    static_start = definition.start()
    opening = definition.end() - 1

    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "line_comment":
            if current == "\n":
                state = "code"
        elif state == "block_comment":
            if current == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif current == "\\":
                escaped = True
            elif (state == "string" and current == '"') or (
                state == "character" and current == "'"
            ):
                state = "code"
        elif current == "/" and following == "/":
            state = "line_comment"
            index += 1
        elif current == "/" and following == "*":
            state = "block_comment"
            index += 1
        elif current == '"':
            state = "string"
        elif current == "'":
            state = "character"
        elif current == "{":
            depth += 1
        elif current == "}":
            depth -= 1
            if depth == 0:
                return source[static_start : index + 1]
        index += 1

    raise ValueError(f"unterminated function body for {name}")


def verify_extractor() -> None:
    """Reject prototypes and unrelated bodies before the requested definition."""
    fixture = """static int target(int value);
static int unrelated(void) { return 0; }
static int
target(int value) { return value + 1; }
"""
    extracted = extract_function(fixture, "target")
    if "unrelated" in extracted or "return value + 1;" not in extracted:
        raise ValueError("function extractor did not select the target definition")


def build_harness(source: str) -> str:
    slave_to_global_ch = extract_function(source, "max9296_slave_to_global_ch")
    format_channel = extract_function(source, "max9296_fmt_ch")
    cached_value = extract_function(source, "max9296_cached_exposure_value")
    replay_decision = extract_function(
        source, "max9296_cached_exposure_replay_decision"
    )
    apply_channel_controls = extract_function(
        source, "max9296_apply_channel_controls"
    )

    return f"""
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "max9296_exposure_policy.h"

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define AP1302_I2C_ADDR 0x3cU
#define AP1302_CH0_I2C_ADDR 0x11U
#define AP1302_CH1_I2C_ADDR 0x12U
#define MAX9295_SER_ADDR_CH0 0x40U
#define MAX9295_SER_ADDR_CH1 0x60U
#define AP1302_REG_AE_CTRL 0x5002U
#define AP1302_REG_AWB_CTRL 0x5100U
#define AP1302_REG_AE_GAIN 0x5006U
#define AP1302_REG_ROTATION 0x100cU
#define AP1302_REG_LSC_CTRL 0x3784U
#define AP1302_REG_BRIGHTNESS 0x7000U
#define AP1302_REG_CONTRAST 0x7002U
#define AP1302_REG_SATURATION 0x7006U
#define AR0234_REG_LED_FLASH_CONTROL 0x3270U
#define AP1302_AE_CTRL_AUTO 0x000cU
#define AP1302_AE_CTRL_MANUAL 0x0000U
#define AP1302_AWB_CTRL_FROM_MODE(value) ((u16)(value))
#define KERN_NOTICE ""
#define KERN_ERR ""
#define KEYWORD "MAX9296"
#define _FILE_ "binding-test"
#define scnprintf snprintf

struct max9296_mode_info {{
  u32 exposure_safe_max_fps;
}};

struct max9296_channel_ctrl {{
  unsigned int ae_on;
  unsigned int awb;
  unsigned int gain;
  unsigned int exposure;
  unsigned int hflip;
  unsigned int vflip;
  unsigned int lsc;
  unsigned int brightness;
  unsigned int contrast;
  unsigned int saturation;
  unsigned int led_flash;
}};

struct max9296_ctrl_cache {{
  struct max9296_channel_ctrl ch0;
  struct max9296_channel_ctrl ch1;
  unsigned int exposure;
  unsigned int exposure_override_mask;
  unsigned int exposure_reinit_required;
  unsigned int mcp4018_wiper;
  unsigned int mcp4018_wiper_ch1;
}};

struct test_adapter {{
  int nr;
}};

struct test_client {{
  struct test_adapter *adapter;
}};

struct max9296_dev {{
  struct max9296_ctrl_cache ctrl_cache;
  const struct max9296_mode_info *current_mode;
  struct test_client *i2c_client;
  struct {{
    int ch_shift;
  }} link_status;
  u32 fps;
  unsigned int enable;
  unsigned int test_dual;
}};

#define READ_ONCE(value) (value)

static bool max9296_hw_is_dual(const struct max9296_dev *sensor) {{
  return sensor->test_dual != 0U;
}}

{slave_to_global_ch}

{format_channel}

{cached_value}

{replay_decision}

static unsigned int exposure_write_count;
static unsigned int exposure_write_addr;
static unsigned int exposure_write_value;
static int exposure_write_error;
static char exposure_write_name[16];
static unsigned int exposure_preflight_count;
static unsigned int ae_write_count;
static unsigned int ae_write_addr[4];
static unsigned int ae_write_value[4];
static char event_log[8];
static unsigned int event_count;

static int test_printk(const char *format, ...) {{
  va_list args;

  va_start(args, format);
  va_end(args);
  return 0;
}}

#define printk test_printk

static const char *awb_mode_name(int mode) {{
  (void)mode;
  return "test";
}}

static int max9296_preflight_exposure(
    struct max9296_dev *sensor, const char *channel, u32 exposure) {{
  (void)sensor;
  (void)channel;
  (void)exposure;
  exposure_preflight_count++;
  return 0;
}}

static int max9296_write_exposure(
    struct max9296_dev *sensor, u32 i2c_addr, const char *channel,
    u32 exposure) {{
  (void)sensor;
  exposure_write_count++;
  exposure_write_addr = i2c_addr;
  exposure_write_value = exposure;
  snprintf(exposure_write_name, sizeof(exposure_write_name), "%s", channel);
  event_log[event_count++] = 'E';
  event_log[event_count] = '\\0';
  return exposure_write_error;
}}

static int maxim_ops_i2c_write(
    struct max9296_dev *sensor, unsigned int i2c_addr, unsigned int reg,
    unsigned int value, unsigned int reg_bytes, unsigned int value_bytes) {{
  (void)sensor;
  (void)reg_bytes;
  (void)value_bytes;
  if (reg == AP1302_REG_AE_CTRL) {{
    if (ae_write_count < sizeof(ae_write_addr) / sizeof(ae_write_addr[0])) {{
      ae_write_addr[ae_write_count] = i2c_addr;
      ae_write_value[ae_write_count] = value;
    }}
    ae_write_count++;
    event_log[event_count++] = 'A';
    event_log[event_count] = '\\0';
  }}
  return 0;
}}

static void msleep(unsigned int milliseconds) {{
  (void)milliseconds;
}}

static int max9296_dma_write_reg(
    struct max9296_dev *sensor, unsigned int i2c_addr, unsigned int reg,
    u16 value) {{
  (void)sensor;
  (void)i2c_addr;
  (void)reg;
  (void)value;
  return 0;
}}

static int max9295_mfp4_set(
    struct max9296_dev *sensor, u8 serializer, bool enabled) {{
  (void)sensor;
  (void)serializer;
  (void)enabled;
  return 0;
}}

static int mcp4018_write_wiper(
    struct max9296_dev *sensor, u8 host, u8 wiper, u8 serializer) {{
  (void)sensor;
  (void)host;
  (void)wiper;
  (void)serializer;
  return 0;
}}

#define max9296_require_exposure_reinit_locked(sensor) \\
  ((sensor)->ctrl_cache.exposure_reinit_required = true)

{apply_channel_controls}

struct cache_case {{
  unsigned int shared;
  unsigned int ch0;
  unsigned int ch1;
  unsigned int override_mask;
}};

static const struct cache_case cache_cases[] = {{
    {{7000U, 7000U, 7000U, 0U}},
    {{7000U, 5000U, 7000U, 0U}},
    {{7000U, 7000U, 5000U, 0U}},
    {{7000U, 5000U, 7000U, MAX9296_EXPOSURE_OVERRIDE_CH0}},
    {{7000U, 7000U, 5000U, MAX9296_EXPOSURE_OVERRIDE_CH1}},
    {{7000U, 5000U, 6000U,
      MAX9296_EXPOSURE_OVERRIDE_CH0 | MAX9296_EXPOSURE_OVERRIDE_CH1}},
    {{7000U, 0U, 7000U, MAX9296_EXPOSURE_OVERRIDE_CH0}},
    {{7000U, 7000U, 0U, MAX9296_EXPOSURE_OVERRIDE_CH1}},
    {{0U, 4000U, 5000U,
      MAX9296_EXPOSURE_OVERRIDE_CH0 | MAX9296_EXPOSURE_OVERRIDE_CH1}},
}};

static const unsigned int fps_cases[] = {{30U, 31U, 120U}};

int main(void) {{
  struct max9296_mode_info mode = {{.exposure_safe_max_fps = 30U}};
  struct test_adapter adapter = {{.nr = 2}};
  struct test_client client = {{.adapter = &adapter}};
  struct max9296_dev sensor = {{
      .current_mode = &mode,
      .i2c_client = &client,
  }};
  unsigned int configurations = 0U;
  unsigned int dual_configurations = 0U;
  unsigned int failures = 0U;
  unsigned int dual;
  unsigned int local_channel;
  unsigned int ae0;
  unsigned int ae1;
  unsigned int fps_index;
  unsigned int cache_index;
  char channel_label[8];

  sensor.test_dual = 1U;
  sensor.enable = 0x01U;
  sensor.link_status.ch_shift = 0;
  max9296_fmt_ch(channel_label, sizeof(channel_label), &sensor,
                 AP1302_I2C_ADDR);
  if (strcmp(channel_label, "pair") != 0) {{
    fprintf(stderr, "FAIL dual broadcast label=%s want=pair\\n", channel_label);
    failures++;
  }}

  sensor.test_dual = 0U;
  sensor.enable = 0x02U;
  sensor.link_status.ch_shift = 2;
  max9296_fmt_ch(channel_label, sizeof(channel_label), &sensor,
                 AP1302_I2C_ADDR);
  if (strcmp(channel_label, "ch3") != 0) {{
    fprintf(stderr, "FAIL single broadcast label=%s want=ch3\\n", channel_label);
    failures++;
  }}

  for (dual = 0U; dual <= 1U; dual++) {{
    for (local_channel = 0U; local_channel <= 1U; local_channel++) {{
      for (ae0 = 0U; ae0 <= 1U; ae0++) {{
        for (ae1 = 0U; ae1 <= 1U; ae1++) {{
          for (fps_index = 0U;
               fps_index < sizeof(fps_cases) / sizeof(fps_cases[0]);
               fps_index++) {{
            for (cache_index = 0U;
                 cache_index < sizeof(cache_cases) / sizeof(cache_cases[0]);
                 cache_index++) {{
              const struct cache_case *cache = &cache_cases[cache_index];
              const struct max9296_channel_ctrl *channel;
              struct max9296_exposure_replay_decision actual;
              enum max9296_exposure_seed_route expected_route;
              unsigned int channel_bit;
              unsigned int expected_override;
              unsigned int expected_pair_ae;
              unsigned int expected_value;
              unsigned int expected_seed_count;
              unsigned int expected_ae_count;
              unsigned int i2c_addr;
              unsigned int mismatch;
              const char *channel_name;
              const char *expected_event;
              const char *expected_name;
              int apply_ret;

              sensor.test_dual = dual;
              sensor.fps = fps_cases[fps_index];
              sensor.ctrl_cache.exposure = cache->shared;
              sensor.ctrl_cache.ch0.exposure = cache->ch0;
              sensor.ctrl_cache.ch1.exposure = cache->ch1;
              sensor.ctrl_cache.ch0.ae_on = ae0;
              sensor.ctrl_cache.ch1.ae_on = ae1;
              sensor.ctrl_cache.exposure_override_mask = cache->override_mask;
              channel = local_channel ? &sensor.ctrl_cache.ch1
                                      : &sensor.ctrl_cache.ch0;

              actual = max9296_cached_exposure_replay_decision(&sensor,
                                                                channel);
              channel_bit = local_channel ? MAX9296_EXPOSURE_OVERRIDE_CH1
                                          : MAX9296_EXPOSURE_OVERRIDE_CH0;
              expected_override = dual ? cache->override_mask != 0U
                                       : (cache->override_mask & channel_bit) != 0U;
              expected_pair_ae = dual ? ae0 && ae1
                                      : (local_channel ? ae1 : ae0);
              expected_value = expected_override
                                   ? (local_channel ? cache->ch1 : cache->ch0)
                                   : cache->shared;
              if (!expected_override && expected_pair_ae &&
                  sensor.fps > mode.exposure_safe_max_fps)
                expected_route = MAX9296_EXPOSURE_SEED_SKIP;
              else if (dual && !expected_override)
                expected_route = MAX9296_EXPOSURE_SEED_PAIR;
              else
                expected_route = MAX9296_EXPOSURE_SEED_CHANNEL;

              exposure_write_count = 0U;
              exposure_write_addr = 0U;
              exposure_write_value = 0U;
              exposure_write_name[0] = '\\0';
              exposure_preflight_count = 0U;
              ae_write_count = 0U;
              memset(ae_write_addr, 0, sizeof(ae_write_addr));
              memset(ae_write_value, 0, sizeof(ae_write_value));
              event_count = 0U;
              event_log[0] = '\\0';
              i2c_addr = dual ? (local_channel ? 0x12U : 0x11U)
                               : AP1302_I2C_ADDR;
              channel_name = local_channel ? "ch1" : "ch0";
              apply_ret = max9296_apply_channel_controls(
                  &sensor, i2c_addr,
                  local_channel ? &sensor.ctrl_cache.ch1
                                : &sensor.ctrl_cache.ch0,
                  0x40U, 0x2fU, 0U, channel_name,
                  dual ? "dual" : "single");
              expected_seed_count =
                  expected_route == MAX9296_EXPOSURE_SEED_SKIP ? 0U : 1U;
              expected_ae_count = expected_seed_count ? 2U : 1U;
              expected_event = expected_seed_count ? "AEA" : "A";
              expected_name =
                  expected_route == MAX9296_EXPOSURE_SEED_PAIR
                      ? "dual-pair"
                      : channel_name;

              mismatch = actual.route != expected_route ||
                         actual.value != expected_value ||
                         actual.has_runtime_override != expected_override ||
                         apply_ret != 0 ||
                         exposure_preflight_count != expected_seed_count ||
                         exposure_write_count != expected_seed_count ||
                         ae_write_count != expected_ae_count ||
                         strcmp(event_log, expected_event) != 0;
              if (expected_seed_count &&
                  (exposure_write_addr !=
                       (expected_route == MAX9296_EXPOSURE_SEED_PAIR
                            ? AP1302_I2C_ADDR
                            : i2c_addr) ||
                   exposure_write_value != expected_value ||
                   strcmp(exposure_write_name, expected_name) != 0))
                mismatch = 1U;
              if (ae_write_count > 0U &&
                  (ae_write_addr[0] != i2c_addr ||
                   ae_write_value[0] !=
                       (expected_seed_count ? AP1302_AE_CTRL_MANUAL
                                            : (channel->ae_on
                                                   ? AP1302_AE_CTRL_AUTO
                                                   : AP1302_AE_CTRL_MANUAL))))
                mismatch = 1U;
              if (ae_write_count > 1U &&
                  (ae_write_addr[1] != i2c_addr ||
                   ae_write_value[1] !=
                       (channel->ae_on ? AP1302_AE_CTRL_AUTO
                                       : AP1302_AE_CTRL_MANUAL)))
                mismatch = 1U;

              configurations++;
              if (dual)
                dual_configurations++;
              if (mismatch) {{
                if (failures < 12U)
                  fprintf(stderr,
                          "FAIL dual=%u local=%u ae=(%u,%u) fps=%u case=%u "
                          "decision=(%u,%u,%u)/(%u,%u,%u) "
                          "seed=%u@0x%x/%u ae=%u events=%s\\n",
                          dual, local_channel, ae0, ae1, sensor.fps,
                          cache_index, actual.route, actual.value,
                          actual.has_runtime_override, expected_route,
                          expected_value, expected_override,
                          exposure_write_count, exposure_write_addr,
                          exposure_write_value, ae_write_count, event_log);
                failures++;
              }}
            }}
          }}
        }}
      }}
    }}
  }}

  if (configurations != 432U || dual_configurations != 216U) {{
    fprintf(stderr, "FAIL coverage=%u dual=%u want=432/216\\n",
            configurations, dual_configurations);
    failures++;
  }}

  /* An override replay error is returned without requesting initialization.
   * An independently pending session reset must remain pending on failure. */
  for (unsigned int pending_reset = 0U; pending_reset <= 1U; pending_reset++) {{
    sensor.test_dual = 1U;
    sensor.fps = 30U;
    sensor.ctrl_cache.ch1.exposure = 5000U;
    sensor.ctrl_cache.exposure_override_mask = MAX9296_EXPOSURE_OVERRIDE_CH1;
    sensor.ctrl_cache.exposure_reinit_required = pending_reset;
    exposure_write_error = -5;
    event_count = ae_write_count = 0U;
    if (max9296_apply_channel_controls(
            &sensor, 0x12U, &sensor.ctrl_cache.ch1, 0x60U, 0x2fU, 0U,
            "ch1", "dual") != -5 ||
        sensor.ctrl_cache.exposure_reinit_required != pending_reset ||
        sensor.ctrl_cache.ch1.exposure != 5000U) {{
      fprintf(stderr, "FAIL exposure replay error changed reset/cache state "
                      "pending_reset=%u\\n", pending_reset);
      failures++;
    }}
  }}

  printf("max9296 exposure replay binding: %u configurations (%u dual), "
         "%u failures -> %s\\n", configurations, dual_configurations,
         failures, failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT / "max9296.c")
    args = parser.parse_args()

    try:
        verify_extractor()
        harness = build_harness(args.source.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}")
        return 1

    with tempfile.TemporaryDirectory(prefix="max9296-exposure-binding-") as tmp:
        tmp_dir = Path(tmp)
        harness_path = tmp_dir / "exposure_replay_binding.c"
        binary_path = tmp_dir / "exposure_replay_binding"
        harness_path.write_text(harness, encoding="utf-8")

        compile_result = subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT),
                str(harness_path),
                "-o",
                str(binary_path),
            ],
            check=False,
            text=True,
            capture_output=True,
        )
        if compile_result.returncode:
            print(compile_result.stdout, end="")
            print(compile_result.stderr, end="")
            return compile_result.returncode

        return subprocess.run([str(binary_path)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
