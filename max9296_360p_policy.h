#ifndef MAX9296_360P_POLICY_H
#define MAX9296_360P_POLICY_H

#define MAX9296_360P_SENSOR_MODE_KEEP 0xffU

#ifndef MAX9296_360P_SENSOR_MODE
#define MAX9296_360P_SENSOR_MODE MAX9296_360P_SENSOR_MODE_KEEP
#endif

#if MAX9296_360P_SENSOR_MODE != MAX9296_360P_SENSOR_MODE_KEEP && \
    MAX9296_360P_SENSOR_MODE > 15
#error "MAX9296_360P_SENSOR_MODE must be 0..15 or KEEP"
#endif

#define MAX9296_PREVIEW_SENSOR_MODE_CROP_MASK 0x3000U
#define MAX9296_PREVIEW_SENSOR_MODE_INDEX_MASK 0x000fU

#define MAX9296_PREVIEW_ROI_X0 0x0000U
#define MAX9296_PREVIEW_ROI_Y0 0x0000U
#define MAX9296_PREVIEW_ROI_X1 0x4000U
#define MAX9296_PREVIEW_ROI_Y1 0x4000U
#define MAX9296_PREVIEW_ASPECT 0x1000U

static inline unsigned int max9296_preview_sensor_mode(
    unsigned int current_value, unsigned int crop_ctrl,
    unsigned int sensor_mode) {
  return (current_value & ~(MAX9296_PREVIEW_SENSOR_MODE_CROP_MASK |
                            MAX9296_PREVIEW_SENSOR_MODE_INDEX_MASK)) |
         ((crop_ctrl & 0x3U) << 12) |
         (sensor_mode & MAX9296_PREVIEW_SENSOR_MODE_INDEX_MASK);
}

static inline unsigned int max9296_preview_uses_high_fps(unsigned int fps) {
  return fps > 30U && fps <= 120U;
}

static inline unsigned int max9296_preview_max_fps_fixed8(unsigned int fps) {
  return fps << 8;
}

static inline unsigned int max9296_mode_max_fps(unsigned int width,
                                                 unsigned int height) {
  if (height == 360U && (width == 640U || width == 1280U))
    return 120U;

  if ((height == 720U && (width == 1280U || width == 2560U)) ||
      (height == 1080U && (width == 1920U || width == 3840U)))
    return 30U;

  return 0U;
}

static inline unsigned int
max9296_preview_sensor_mode_override(unsigned int width,
                                     unsigned int height) {
  return width == 640U && height == 360U
             ? MAX9296_360P_SENSOR_MODE
             : MAX9296_360P_SENSOR_MODE_KEEP;
}

static inline unsigned int max9296_preview_output_uses_high_fps(
    unsigned int width, unsigned int height, unsigned int fps) {
  return width == 640U && height == 360U &&
         max9296_preview_uses_high_fps(fps);
}

#endif
