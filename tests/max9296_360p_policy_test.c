#include "../max9296_360p_policy.h"

#include <stdio.h>

#ifndef MAX9296_360P_EXPECTED_MAX_FPS
#define MAX9296_360P_EXPECTED_MAX_FPS 120U
#endif

#ifndef MAX9296_HD_EXPECTED_MAX_FPS
#define MAX9296_HD_EXPECTED_MAX_FPS 60U
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

/*
 * max9296 #82.  fps belongs to a hardware identity only through the preview
 * ceiling, so rates that program nothing must compare equal.
 */
static void test_programmed_max_fps_is_the_only_fps_footprint(void) {
  /* Outside the 640x360 window nothing is derived from fps, so a cadence
   * change there is the same programmed hardware. */
  CHECK(max9296_preview_programmed_max_fps(2560U, 720U, 15U) ==
        max9296_preview_programmed_max_fps(2560U, 720U, 20U));
  CHECK(max9296_preview_programmed_max_fps(1280U, 720U, 60U) == 0U);
  CHECK(max9296_preview_programmed_max_fps(1920U, 1080U, 30U) == 0U);

  /* At or below the window, and past the negotiation ceiling, the register is
   * left untouched. */
  CHECK(max9296_preview_programmed_max_fps(640U, 360U, 20U) == 0U);
  CHECK(max9296_preview_programmed_max_fps(640U, 360U, 30U) == 0U);
  CHECK(max9296_preview_programmed_max_fps(640U, 360U, 121U) == 0U);

  /* Inside the window the exact rate is encoded.  A build whose ceiling closes
   * the window derives nothing at any rate, so every expectation below is taken
   * from the ceiling rather than from a literal and holds in both the
   * qualification and the restricted build. */
  CHECK(max9296_preview_programmed_max_fps(640U, 360U,
                                           MAX9296_360P_EXPECTED_MAX_FPS) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 31U
             ? max9296_preview_max_fps_fixed8(MAX9296_360P_EXPECTED_MAX_FPS)
             : 0U));

  /* Entering, leaving and moving within the window are each a different
   * programmed hardware wherever the window is open.  The pair for the last
   * case is derived from the ceiling so it stays inside that window. */
  CHECK((max9296_preview_programmed_max_fps(640U, 360U,
                                            MAX9296_360P_EXPECTED_MAX_FPS) !=
         max9296_preview_programmed_max_fps(640U, 360U, 20U)) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 31U));
  CHECK((max9296_preview_programmed_max_fps(640U, 360U, 20U) !=
         max9296_preview_programmed_max_fps(
             640U, 360U, MAX9296_360P_EXPECTED_MAX_FPS)) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 31U));
  CHECK((max9296_preview_programmed_max_fps(640U, 360U,
                                            MAX9296_360P_EXPECTED_MAX_FPS) !=
         max9296_preview_programmed_max_fps(
             640U, 360U, MAX9296_360P_EXPECTED_MAX_FPS - 1U)) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 32U));

  /* Trap (max9296 #82 round-1 blocker).  This helper takes an OUTPUT size, but
   * the dual 640x360 mode stores the combined width 1280.  Handing it the raw
   * stored width derives 0 at every rate, which makes two in-window rates
   * compare equal, so callers must halve the width for dual modes exactly as
   * the register writer does. */
  CHECK(max9296_preview_programmed_max_fps(1280U, 360U, 60U) == 0U);
  CHECK(max9296_preview_programmed_max_fps(1280U, 360U, 120U) == 0U);
  CHECK(max9296_preview_programmed_max_fps(1280U, 360U,
                                           MAX9296_360P_EXPECTED_MAX_FPS) == 0U);
  CHECK((max9296_preview_programmed_max_fps(640U, 360U,
                                            MAX9296_360P_EXPECTED_MAX_FPS) !=
         max9296_preview_programmed_max_fps(
             1280U, 360U, MAX9296_360P_EXPECTED_MAX_FPS)) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 31U));
}

static void test_only_360p_exposes_the_high_fps_policy(void) {
  CHECK(MAX9296_HD_MAX_FPS == MAX9296_HD_EXPECTED_MAX_FPS);
  CHECK(max9296_mode_max_fps(1920U, 1080U) == 30U);
  CHECK(max9296_mode_max_fps(3840U, 1080U) == 30U);
  CHECK(max9296_mode_max_fps(1280U, 720U) == MAX9296_HD_EXPECTED_MAX_FPS);
  CHECK(max9296_mode_max_fps(2560U, 720U) == MAX9296_HD_EXPECTED_MAX_FPS);
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

  /* Raising the HD negotiation ceiling must not pull HD into the 640x360
   * high-fps preview path: that path stays gated on the output size. */
  CHECK(max9296_preview_output_uses_high_fps(1280U, 720U, 31U) == 0U);
  CHECK(max9296_preview_output_uses_high_fps(1280U, 720U, 60U) == 0U);
  CHECK(max9296_preview_output_uses_high_fps(2560U, 720U, 60U) == 0U);
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 30U) == 0U);
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 31U) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 31U));
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 120U) ==
        (MAX9296_360P_EXPECTED_MAX_FPS >= 120U));
  CHECK(max9296_preview_output_uses_high_fps(640U, 360U, 121U) == 0U);
}

static void test_high_fps_manual_exposure_warns_without_rejection(void) {
  CHECK(max9296_exposure_policy_decision(0U, 120U, 30U) ==
        MAX9296_EXPOSURE_POLICY_INVALID);
  CHECK(max9296_exposure_policy_decision(121U, 120U, 30U) ==
        MAX9296_EXPOSURE_POLICY_INVALID);
  CHECK(max9296_exposure_policy_decision(30U, 120U, 0U) ==
        MAX9296_EXPOSURE_POLICY_INVALID);
  CHECK(max9296_exposure_policy_decision(30U, 120U, 30U) ==
        MAX9296_EXPOSURE_POLICY_ALLOW);
  CHECK(max9296_exposure_policy_decision(31U, 120U, 30U) ==
        MAX9296_EXPOSURE_POLICY_WARN);
  CHECK(max9296_exposure_policy_decision(120U, 120U, 30U) ==
        MAX9296_EXPOSURE_POLICY_WARN);
  CHECK(max9296_exposure_frame_period_us(120U) == 8333U);
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
  test_programmed_max_fps_is_the_only_fps_footprint();
  test_only_360p_exposes_the_high_fps_policy();
  test_high_fps_manual_exposure_warns_without_rejection();
  test_full_fov_roi_is_normalized();

  printf("max9296 360p policy: %u checks, %u failures -> %s\n", checks,
         failures, failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
