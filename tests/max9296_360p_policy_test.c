#include "../max9296_360p_policy.h"

#include <stdio.h>

#ifndef MAX9296_360P_EXPECTED_MAX_FPS
#define MAX9296_360P_EXPECTED_MAX_FPS 120U
#endif

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

static void test_sensor_mode_preserves_unowned_bits(void) {
  CHECK(MAX9296_360P_SENSOR_MODE_KEEP == 0xffU);
  CHECK(max9296_preview_sensor_mode(0xc5eaU, 2U, 5U) == 0xe5e5U);
  CHECK(max9296_preview_sensor_mode(0xffffU, 0U, 0U) == 0xcff0U);
}

static void test_high_fps_policy_uses_fixed8_values(void) {
  CHECK(MAX9296_360P_MAX_FPS == MAX9296_360P_EXPECTED_MAX_FPS);
  CHECK(max9296_preview_uses_high_fps(30U) == 0U);
  CHECK(max9296_preview_uses_high_fps(31U) == 1U);
  CHECK(max9296_preview_uses_high_fps(120U) == 1U);
  CHECK(max9296_preview_uses_high_fps(121U) == 0U);
  CHECK(max9296_preview_max_fps_fixed8(31U) == 0x1f00U);
  CHECK(max9296_preview_max_fps_fixed8(60U) == 0x3c00U);
  CHECK(max9296_preview_max_fps_fixed8(120U) == 0x7800U);
}

static void test_only_360p_exposes_the_high_fps_policy(void) {
  CHECK(max9296_mode_max_fps(1920U, 1080U) == 30U);
  CHECK(max9296_mode_max_fps(1280U, 720U) == 30U);
  CHECK(max9296_mode_max_fps(640U, 360U) ==
        MAX9296_360P_EXPECTED_MAX_FPS);
  CHECK(max9296_mode_max_fps(1280U, 360U) ==
        MAX9296_360P_EXPECTED_MAX_FPS);
  CHECK(max9296_mode_max_fps(640U, 480U) == 0U);

  CHECK(max9296_preview_sensor_mode_override(1920U, 1080U) ==
        MAX9296_360P_SENSOR_MODE_KEEP);
  CHECK(max9296_preview_sensor_mode_override(1280U, 720U) ==
        MAX9296_360P_SENSOR_MODE_KEEP);
  CHECK(max9296_preview_sensor_mode_override(640U, 360U) ==
        MAX9296_360P_SENSOR_MODE);

  CHECK(max9296_preview_output_uses_high_fps(1280U, 720U, 31U) == 0U);
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 30U) == 0U);
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 31U) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 31U));
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 120U) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 120U));
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 121U) == 0U);
}

static void test_full_fov_roi_is_normalized(void) {
  CHECK(MAX9296_PREVIEW_ROI_X0 == 0x0000U);
  CHECK(MAX9296_PREVIEW_ROI_Y0 == 0x0000U);
  CHECK(MAX9296_PREVIEW_ROI_X1 == 0x4000U);
  CHECK(MAX9296_PREVIEW_ROI_Y1 == 0x4000U);
  CHECK(MAX9296_PREVIEW_ASPECT == 0x1000U);
}

int main(void) {
  test_sensor_mode_preserves_unowned_bits();
  test_high_fps_policy_uses_fixed8_values();
  test_only_360p_exposes_the_high_fps_policy();
  test_full_fov_roi_is_normalized();

  printf("max9296 360p policy: %u checks, %u failures -> %s\n", checks,
         failures, failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
