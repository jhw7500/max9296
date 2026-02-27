// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 * Pointimage drivers/media/i2c/max9296.c support
 *
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 * Copyright (C) 2022 Pointimage, Inc. All rights reserved.
 *
 * Author:
 *   Junggyu Lee <youstar02@gmail.com>, 2022/02/05
 *
 * Description:
 *    This program is free software; you can redistribute  it and/or modify it
 *    under  the terms of  the GNU General  Public License as published by the
 *    Free Software Foundation;  either version 2 of the  License, or (at your
 *    option) any later version.
 */

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define SW_VERSION "2.0"
#define SERDES_3GBPS
#define SERDES_STPx
#define _FILE_                                                                 \
  (strrchr(__FILE__, '/') ? (strrchr(__FILE__, '/') + 1) : (__FILE__))
#define KEYWORD "I2C"

static int debug;

#define DEFAULT_FRAMERATE_FPS (120)
#define DEFAULT_RESOLUTION_WIDTH (2560)
#define DEFAULT_RESOLUTION_HEIGHT (720)

#define MAX9296_REG_CHIP_ID 0x000d

/* AP1302 ISP I2C slave address and register map */
#define AP1302_I2C_ADDR 0x3c
#define AP1302_CH0_I2C_ADDR 0x11
#define AP1302_CH1_I2C_ADDR 0x12
#define AP1302_REG_ROTATION 0x100c
#define AP1302_REG_AE_CTRL 0x5002
#define AP1302_REG_AE_GAIN 0x5006
#define AP1302_REG_EXP_TIME 0x500c
#define AP1302_REG_AWB_CTRL 0x5100
#define AP1302_REG_LSC_CTRL 0x54a0

/* Image tuning (fixed-point) */
#define AP1302_REG_BRIGHTNESS 0x7000
#define AP1302_REG_CONTRAST 0x7002
#define AP1302_REG_SATURATION 0x7006

#define AP1302_AE_CTRL_AUTO 0x0299
#define AP1302_AE_CTRL_MANUAL 0x0290
#define AP1302_AWB_CTRL_AUTO 0x115f

/* Custom V4L2 controls for per-channel settings in dual-channel mode */
#define V4L2_CID_EXPOSURE_AUTO_CH0 (V4L2_CID_USER_BASE + 0x1000)
#define V4L2_CID_EXPOSURE_AUTO_CH1 (V4L2_CID_USER_BASE + 0x1001)
#define V4L2_CID_AUTO_WHITE_BALANCE_CH0 (V4L2_CID_USER_BASE + 0x1002)
#define V4L2_CID_AUTO_WHITE_BALANCE_CH1 (V4L2_CID_USER_BASE + 0x1003)
#define V4L2_CID_AUTOGAIN_CH0 (V4L2_CID_USER_BASE + 0x1004)
#define V4L2_CID_AUTOGAIN_CH1 (V4L2_CID_USER_BASE + 0x1005)
#define V4L2_CID_GAIN_CH0 (V4L2_CID_USER_BASE + 0x1006)
#define V4L2_CID_GAIN_CH1 (V4L2_CID_USER_BASE + 0x1007)
#define V4L2_CID_HFLIP_CH0 (V4L2_CID_USER_BASE + 0x1008)
#define V4L2_CID_HFLIP_CH1 (V4L2_CID_USER_BASE + 0x1009)
#define V4L2_CID_VFLIP_CH0 (V4L2_CID_USER_BASE + 0x100A)
#define V4L2_CID_VFLIP_CH1 (V4L2_CID_USER_BASE + 0x100B)

/* Shared controls */
#define V4L2_CID_LSC (V4L2_CID_USER_BASE + 0x100C)

/* Per-channel tuning controls (fixed12 u16) */
#define V4L2_CID_BRIGHTNESS_CH0 (V4L2_CID_USER_BASE + 0x100D)
#define V4L2_CID_BRIGHTNESS_CH1 (V4L2_CID_USER_BASE + 0x100E)
#define V4L2_CID_CONTRAST_CH0 (V4L2_CID_USER_BASE + 0x100F)
#define V4L2_CID_CONTRAST_CH1 (V4L2_CID_USER_BASE + 0x1010)
#define V4L2_CID_SATURATION_CH0 (V4L2_CID_USER_BASE + 0x1011)
#define V4L2_CID_SATURATION_CH1 (V4L2_CID_USER_BASE + 0x1012)

/* Exposure value per-channel (u32 in HW, V4L2 INTEGER is s32) */
#define V4L2_CID_EXPOSURE_CH0 (V4L2_CID_USER_BASE + 0x1013)
#define V4L2_CID_EXPOSURE_CH1 (V4L2_CID_USER_BASE + 0x1014)

/* Exposure time shared control: exp_time (maps to 0x500c) */
#define V4L2_CID_EXP_TIME (V4L2_CID_USER_BASE + 0x1015)

/* LSC strength per-channel (fixed12 u16) */
#define V4L2_CID_LSC_CH0 (V4L2_CID_USER_BASE + 0x1016)
#define V4L2_CID_LSC_CH1 (V4L2_CID_USER_BASE + 0x1017)

enum max9296_mode_id {
  MAX9296_MODE_2560x720 = 0,
  MAX9296_MODE_1280x720,
  MAX9296_MODE_3840x1080,
  MAX9296_MODE_1920x1080,
  MAX9296_NUM_MODES,
};

enum max9296_frame_rate {
  MAX9296_30_FPS = 0,
  MAX9296_NUM_FRAMERATES,
};

enum max9296_state {
  MAX9296_STATE_IDLE = 0,
  MAX9296_STATE_RUNNING,
  MAX9296_STATE_DONE,
  MAX9296_STATE_MAX,
};

struct max9296_pixfmt {
  u32 code;
  u32 colorspace;
};

static const struct max9296_pixfmt max9296_formats[] = {
    {
        MEDIA_BUS_FMT_UYVY8_2X8,
        V4L2_COLORSPACE_SRGB,
    },
};

static const int max9296_framerates[] = {
    [MAX9296_30_FPS] = 30,
};

struct reg_value {
  u32 slave_addr;
  u32 reg_addr;
  u32 reg_byte;
  u32 val;
  u32 val_byte;
  u32 delay_ms;
};

struct max9296_mode_info {
  enum max9296_mode_id id;
  u32 width;
  u32 height;
  const struct reg_value *reg_data;
  u32 reg_data_size;
  u32 max_fps;
};

struct max9296_ctrls {
  struct v4l2_ctrl_handler handler;
  struct v4l2_ctrl *pixel_rate;
  struct v4l2_ctrl *exp_time;
  struct v4l2_ctrl *light_freq;
  struct v4l2_ctrl *hue;

  /* Per-channel controls for dual-channel mode */
  struct v4l2_ctrl *auto_exp_ch0;
  struct v4l2_ctrl *auto_exp_ch1;
  struct v4l2_ctrl *auto_wb_ch0;
  struct v4l2_ctrl *auto_wb_ch1;
  struct v4l2_ctrl *auto_gain_ch0;
  struct v4l2_ctrl *auto_gain_ch1;
  struct v4l2_ctrl *gain_ch0;
  struct v4l2_ctrl *gain_ch1;
  struct v4l2_ctrl *exposure_ch0;
  struct v4l2_ctrl *exposure_ch1;
  struct v4l2_ctrl *hflip_ch0;
  struct v4l2_ctrl *hflip_ch1;
  struct v4l2_ctrl *vflip_ch0;
  struct v4l2_ctrl *vflip_ch1;
  struct v4l2_ctrl *lsc_ch0;
  struct v4l2_ctrl *lsc_ch1;
  struct v4l2_ctrl *brightness_ch0;
  struct v4l2_ctrl *brightness_ch1;
  struct v4l2_ctrl *contrast_ch0;
  struct v4l2_ctrl *contrast_ch1;
  struct v4l2_ctrl *saturation_ch0;
  struct v4l2_ctrl *saturation_ch1;
};

/* Per-channel control settings */
struct max9296_channel_ctrl {
  int ae_on;      /* V4L2_CID_EXPOSURE_AUTO_CHx (bool; 1=auto, 0=manual) */
  int awb;        /* V4L2_CID_AUTO_WHITE_BALANCE_CHx */
  int gain_auto;  /* V4L2_CID_AUTOGAIN_CHx */
  int gain;       /* V4L2_CID_GAIN_CHx */
  int exposure;   /* V4L2_CID_EXPOSURE_CHx (u32, stored as int) */
  int hflip;      /* V4L2_CID_HFLIP_CHx */
  int vflip;      /* V4L2_CID_VFLIP_CHx */
  int lsc;        /* V4L2_CID_LSC_CHx (fixed12 u16) */
  int brightness; /* V4L2_CID_BRIGHTNESS_CHx (fixed12 u16) */
  int contrast;   /* V4L2_CID_CONTRAST_CHx (fixed12 u16) */
  int saturation; /* V4L2_CID_SATURATION_CHx (fixed12 u16) */
};

struct max9296_ctrl_cache {
  bool firmware_ready;
  bool pending;

  /* Channel-specific settings */
  struct max9296_channel_ctrl ch0;
  struct max9296_channel_ctrl ch1;

  /* Shared setting value, applied to both channels when set */
  int exposure; /* V4L2_CID_EXP_TIME - exp_time (u32) */
};

struct max9296_dev {
  struct i2c_client *i2c_client;
  struct v4l2_subdev sd;
  struct media_pad pad;
  struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */

  struct gpio_desc *reset_gpio;
  struct gpio_desc *pwdn_gpio;
  struct gpio_desc *fsync_gpio;
  struct task_struct *thread_fsync;
  struct task_struct *thread_en;
  struct task_struct *thread_fw_init;

  // state
  struct {
    unsigned int init;
    unsigned int firmware;
    unsigned int enable;
    unsigned int setup;
    unsigned int fsync;
    unsigned int power;
  } state;

  // for firmware
  struct work_struct fw_work;
  wait_queue_head_t fw_wait;

  /* lock to protect all members below */
  struct mutex lock;

  int power_count;

  struct v4l2_mbus_framefmt fmt;
  bool pending_fmt_change;

  const struct max9296_mode_info *current_mode;
  const struct max9296_mode_info *last_mode;
  struct v4l2_fract frame_interval;

  struct max9296_ctrls ctrls;
  struct max9296_ctrl_cache ctrl_cache;

  bool pending_mode_change;
  bool streaming;
  unsigned int stream_on;

  unsigned int fps;
  unsigned int rotate;
  unsigned int enable;
  unsigned int restart;

  // for fsync
  struct {
    unsigned int fsync;
    struct device_node *np;
    struct i2c_client *client;
    struct v4l2_subdev *sd;
    struct max9296_dev *sensor;
    struct task_struct *thread_shared_init;
  } shared;
};

static inline struct max9296_dev *to_max9296_dev(struct v4l2_subdev *sd) {
  return container_of(sd, struct max9296_dev, sd);
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl) {
  return &container_of(ctrl->handler, struct max9296_dev, ctrls.handler)->sd;
}

/*
 * FIXME: all of these register tables are likely filled with
 * entries that set the register to their power-on default values,
 * and which are otherwise not touched by this driver. Those entries
 * should be identified and removed to speed register load time
 * over i2c.
 */

static const struct reg_value max9296_init_setting_1080p_crop_720p_2ch_30fps[] =
    {

        // CSI out disable
        {0x00, 0x0313, 2, 0x00, 1, 10},
        // auto link
        {0x00, 0x0010, 2, 0x31, 1, 200},

        // CSI port B start video
        {0x40, 0x0311, 2, 0xf0, 1, 200},
        // CSI port A/B enable, portUZYZ B
        {0x40, 0x0308, 2, 0x7f, 1, 10},

        //
        {0x40, 0x0318, 2, 0x5E, 1, 10},
        {0x40, 0x0002, 2, 0x43, 1, 10},

        //
        {0x40, 0x0010, 2, 0x21, 1, 100},

        //
        {0x00, 0x0332, 2, 0x30, 1, 10},
        {0x00, 0x0331, 2, 0xF0, 1, 10},

        //
        {0x40, 0x0010, 2, 0x21, 1, 100},

        //
        {0x00, 0x0010, 2, 0x32, 1, 200},

        //
        {0x40, 0x0000, 2, 0xC0, 1, 10},
        {0x60, 0x0010, 2, 0x31, 1, 100},

        {0x60, 0x006b, 2, 0x12, 1, 10},
        {0x60, 0x0073, 2, 0x13, 1, 10},
        {0x60, 0x007b, 2, 0x32, 1, 10},
        {0x60, 0x0083, 2, 0x32, 1, 10},
        {0x60, 0x0093, 2, 0x32, 1, 10},
        {0x60, 0x009b, 2, 0x32, 1, 10},
        {0x60, 0x00a3, 2, 0x32, 1, 10},
        {0x60, 0x00ab, 2, 0x32, 1, 10},
        {0x60, 0x008b, 2, 0x32, 1, 10},

        {0x00, 0x0010, 2, 0x23, 1, 100},

        //
        {0x40, 0x0318, 2, 0x5e, 1, 10},
        {0x40, 0x0002, 2, 0x43, 1, 10},
        {0x40, 0x0042, 2, 0x22, 1, 10},
        {0x40, 0x0043, 2, 0x78, 1, 10},

        //
        {0x60, 0x0318, 2, 0x5e, 1, 10},
        {0x60, 0x0002, 2, 0x43, 1, 10},
        {0x60, 0x0053, 2, 0x13, 1, 10},
        {0x60, 0x005b, 2, 0x10, 1, 10},
        {0x60, 0x0042, 2, 0x24, 1, 10},
        {0x60, 0x0043, 2, 0x78, 1, 10},
        {0x60, 0x0010, 2, 0x21, 1, 100},

        //
        {0X00, 0X0003, 2, 0x80, 1, 10},
        {0X00, 0X0330, 2, 0x04, 1, 10},
        {0X00, 0X0050, 2, 0x00, 1, 10},
        {0X00, 0X0051, 2, 0x02, 1, 10},
        {0X00, 0X0052, 2, 0x01, 1, 10},
        {0X00, 0X0333, 2, 0x4E, 1, 10},
        {0X00, 0X0334, 2, 0xE4, 1, 10},
        {0X00, 0X044A, 2, 0xC0, 1, 10},

        {0X00, 0X0332, 2, 0x30, 1, 10},

        {0X00, 0X0320, 2, 0x27, 1, 10},
        {0X00, 0X0318, 2, 0x1E, 1, 10},
        {0x00, 0x0331, 2, 0xF0, 1, 10},
        {0x00, 0x0314, 2, 0x00, 1, 10},
        {0x00, 0x0319, 2, 0x10, 1, 10},
        {0x00, 0x0316, 2, 0x5E, 1, 10},
        {0x00, 0x0317, 2, 0x0E, 1, 10},

#ifdef SERDES_3GBPS // 3Gbps
        {0x40, 0x0001, 2, 0x04, 1, 10},
        {0x60, 0x0001, 2, 0x04, 1, 10},
        {0x00, 0x0001, 2, 0x01, 1, 10},
#endif

#ifdef SERDES_STP // STP drive
        {0x40, 0x0011, 2, 0x02, 1, 10},
        {0x60, 0x0011, 2, 0x02, 1, 10},
        {0x00, 0x0011, 2, 0x0A, 1, 10},
#endif

        {0x00, 0x031D, 2, 0xEF, 1, 10},
        {0x00, 0x0010, 2, 0x23, 1, 100},

        //
        {0x40, 0x03F1, 2, 0x85, 1, 100},
        {0x60, 0x03F1, 2, 0x85, 1, 100},
};

// 0x00 == 0x48 slave address
static const struct reg_value max9296_init_setting_720p_30fps_L[] = {
    // step 1
    {0x00, 0x0010, 2, 0x22, 1, 300},

    // step 2
    {0x40, 0x0010, 2, 0x22, 1, 310},

    // step 3
    {0x00, 0x0313, 2, 0x00, 1, 10},
    {0x00, 0x0330, 2, 0x04, 1, 10},
    {0x00, 0x0051, 2, 0x02, 1, 10},
    {0x00, 0x0052, 2, 0x01, 1, 10},
    {0x00, 0x0333, 2, 0x4E, 1, 10},
    {0x00, 0x0334, 2, 0xE4, 1, 10},
    {0x00, 0x044A, 2, 0xC0, 1, 10},
    {0x00, 0x0332, 2, 0x30, 1, 10},
    //{ 0x00, 0x0320, 2, 0x26, 1, 10},// mipi phy1 frequency
    {0x00, 0x0320, 2, 0x24, 1, 10}, // mipi phy1 frequency

#ifdef SERDES_3GBPS // 3Gbps
    {0x40, 0x0001, 2, 0x04, 1, 10},
    {0x00, 0x0001, 2, 0x01, 1, 10},
#endif

#ifdef SERDES_STP // STP drive
    {0x40, 0x0011, 2, 0x02, 1, 10},
    {0x00, 0x0011, 2, 0x0A, 1, 10},
#endif

    {0x00, 0x0010, 2, 0x22, 1, 300},

    // step 4
    {0x40, 0x0010, 2, 0x22, 1, 300},
};

// 0x00 == 0x48 slave address
static const struct reg_value max9296_init_setting_720p_30fps_R[] = {
    // step 1
    {0x00, 0x0010, 2, 0x21, 1, 300},

    // step 2
    {0x40, 0x0010, 2, 0x21, 1, 310},

    // step 3
    {0x00, 0x0313, 2, 0x00, 1, 10},
    {0x00, 0x0330, 2, 0x04, 1, 10},
    {0x00, 0x0051, 2, 0x02, 1, 10},
    {0x00, 0x0052, 2, 0x01, 1, 10},
    {0x00, 0x0333, 2, 0x4E, 1, 10},
    {0x00, 0x0334, 2, 0xE4, 1, 10},
    {0x00, 0x044A, 2, 0xC0, 1, 10},
    {0x00, 0x0332, 2, 0x30, 1, 10},
    //{ 0x00, 0x0320, 2, 0x26, 1, 10},// mipi phy1 frequency
    {0x00, 0x0320, 2, 0x24, 1, 10}, // mipi phy1 frequency

#ifdef SERDES_3GBPS // 3Gbps
    {0x40, 0x0001, 2, 0x04, 1, 10},
    {0x00, 0x0001, 2, 0x01, 1, 10},
#endif

#ifdef SERDES_STP // STP drive
    {0x40, 0x0011, 2, 0x02, 1, 10},
    {0x00, 0x0011, 2, 0x0A, 1, 10},
#endif

    {0x00, 0x0010, 2, 0x21, 1, 300},

    // step 4
    {0x40, 0x0010, 2, 0x21, 1, 300},
};

/* power-on sensor init reg table */
static const struct max9296_mode_info max9296_mode_init_data = {
    MAX9296_MODE_2560x720,
    2560,
    720,
    max9296_init_setting_1080p_crop_720p_2ch_30fps,
    ARRAY_SIZE(max9296_init_setting_1080p_crop_720p_2ch_30fps),
    MAX9296_30_FPS,
};

static const struct max9296_mode_info max9296_mode_data[MAX9296_NUM_MODES] = {
    {
        MAX9296_MODE_2560x720,
        2560,
        720,
        max9296_init_setting_1080p_crop_720p_2ch_30fps,
        ARRAY_SIZE(max9296_init_setting_1080p_crop_720p_2ch_30fps),
        MAX9296_30_FPS,
    },
    {
        MAX9296_MODE_1280x720,
        1280,
        720,
        max9296_init_setting_720p_30fps_L,
        ARRAY_SIZE(max9296_init_setting_720p_30fps_L),
        MAX9296_30_FPS,
    },
    {
        MAX9296_MODE_3840x1080,
        3840,
        1080,
        max9296_init_setting_1080p_crop_720p_2ch_30fps,
        ARRAY_SIZE(max9296_init_setting_1080p_crop_720p_2ch_30fps),
        MAX9296_30_FPS,
    },
    {
        MAX9296_MODE_1920x1080,
        1920,
        1080,
        max9296_init_setting_720p_30fps_L,
        ARRAY_SIZE(max9296_init_setting_720p_30fps_L),
        MAX9296_30_FPS,
    },
};
static const struct max9296_mode_info max9296_mode_data_HD_R = {
    MAX9296_MODE_1280x720,
    1280,
    720,
    max9296_init_setting_720p_30fps_R,
    ARRAY_SIZE(max9296_init_setting_720p_30fps_R),
    MAX9296_30_FPS,
};

static const struct max9296_mode_info max9296_mode_data_FHD_R = {
    MAX9296_MODE_1920x1080,
    1920,
    1080,
    max9296_init_setting_720p_30fps_R,
    ARRAY_SIZE(max9296_init_setting_720p_30fps_R),
    MAX9296_30_FPS,
};
//-------------------------------------------------------------------------
static int maxim_ops_i2c_write(struct max9296_dev *sensor,
                               unsigned int slave_addr, unsigned int reg,
                               unsigned int val, unsigned int reg_byte,
                               unsigned int val_byte) {
  int ret = 0, index = 0, i = 0;
  struct i2c_client *client = sensor->i2c_client;
  unsigned char buf[8];
  struct i2c_msg msg;
  unsigned int retry = 10;

  msg.addr = (slave_addr == 0 ? client->addr : slave_addr);
  msg.flags = 0;
  msg.len = reg_byte + val_byte;
  msg.buf = buf;

  for (i = 0; i < reg_byte; ++i)
    buf[i] = (reg >> ((reg_byte - i - 1) << 3)) & 0xff;

  index = i;
  for (i = 0; i < val_byte; ++i)
    buf[index + i] = (val >> ((val_byte - i - 1) << 3)) & 0xff;

  do {
    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret < 0) {
      printk(KERN_ERR "[%s:%d][%s:%d] retry:%d Error i2c write reg : [0x%x] "
                      "reg=0x%x(%d byte), val=0x%x(%d byte)",
             KEYWORD, client->adapter->nr, _FILE_, __LINE__, retry, slave_addr,
             reg, reg_byte, val, val_byte);
    } else
      break;
  } while (--retry);

  if ((retry == 0) && (ret < 0))
    return ret;

  /*
   * i2c_transfer() returns the number of messages transferred on success.
   * For this single-message write path, success is ret == 1.
   * V4L2 ctrl callbacks must return 0 on success, so normalize here.
   */
  if (ret != 1) {
    return -EIO;
  }
  if (1)
    printk(KERN_INFO "[%s:%d][%s:%d] Success!! i2c write reg : [0x%x] "
                       "reg=0x%x(%d byte), val=0x%x(%d byte)(ret:%d)\n",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__, slave_addr, reg,
           reg_byte, val, val_byte, ret);

  return 0;
}

//-------------------------------------------------------------------------
static int maxim_ops_i2c_read(struct max9296_dev *sensor,
                              unsigned int slave_addr, unsigned int reg,
                              unsigned int reg_byte, unsigned int val_byte,
                              unsigned int *val) {
  int ret = 0, i = 0;
  struct i2c_client *client = sensor->i2c_client;
  unsigned char buf[4] =
      {
          0,
      },
                r_buf[4] = {
                    0,
                };
  struct i2c_msg msg[2] = {
      0,
  };
  unsigned int retry = 10;

  if (val)
    *val = 0;

  msg[0].addr = (slave_addr == 0 ? client->addr : slave_addr);
  msg[0].flags = 0;
  msg[0].len = reg_byte;
  msg[0].buf = buf;

  for (i = 0; i < reg_byte; ++i)
    buf[i] = (reg >> ((reg_byte - i - 1) << 3)) & 0xff;

  msg[1].addr = (slave_addr == 0 ? client->addr : slave_addr);
  msg[1].flags = I2C_M_RD;
  msg[1].len = val_byte;
  msg[1].buf = r_buf;

  do {
    ret = i2c_transfer(client->adapter, msg, 2);
    if (ret < 0) {
      printk(KERN_ERR "[%s:%d][%s:%d] Error i2c read reg : [0x%x] "
                      "reg=0x%x(%d byte),(read %d byte)",
             KEYWORD, client->adapter->nr, _FILE_, __LINE__,
             (slave_addr == 0 ? client->addr : slave_addr), reg, reg_byte,
             val_byte);
    } else
      break;
  } while (--retry);

  if ((retry == 0) && (ret < 0)) {
    printk(KERN_ERR "[%s:%d][%s:%d] Error i2c read - slave: 0x%x, reg: "
                    "0x%x(%d byte, %d data byte",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__,
           (slave_addr == 0 ? client->addr : slave_addr), reg, reg_byte,
           val_byte);
    return (-EPERM);
  }

  if (val) {
    for (i = 0; i < val_byte; ++i)
      *val |= ((r_buf[i] & 0xff) << ((val_byte - i - 1) << 3));
    if (debug)
      printk(KERN_NOTICE "[%s:%d][%s:%d] i2c read reg : [0x%x] reg=0x%x(%d "
                         "byte), val=0x%x(%d byte)",
             KEYWORD, client->adapter->nr, _FILE_, __LINE__,
             (slave_addr == 0 ? client->addr : slave_addr), reg, reg_byte, *val,
             val_byte);
  }

  return 0;
}

static int max9296_check_valid_mode(struct max9296_dev *sensor,
                                    const struct max9296_mode_info *mode,
                                    enum max9296_frame_rate rate) {
  struct i2c_client *client = sensor->i2c_client;
  int ret = 0;
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s (mode->id:%u)", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           mode->id);
  switch (mode->id) {
  case MAX9296_MODE_1280x720:
  case MAX9296_MODE_2560x720:
  case MAX9296_MODE_3840x1080:
  case MAX9296_MODE_1920x1080:
    if ((rate != MAX9296_30_FPS))
      ret = -EINVAL;
    break;
  default:
    printk(KERN_CRIT "[%s:%d][%s:%d] Invalid mode (%u)", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, mode->id);

    ret = -EINVAL;
  }

  return ret;
}

static int max9296_load_regs(struct max9296_dev *sensor,
                             const struct max9296_mode_info *mode) {
  const struct reg_value *regs = mode->reg_data;
  unsigned int i;
  int ret = 0;
  u32 slave_addr, reg_addr, reg_byte, val, val_byte, delay_ms;
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  for (i = 0; i < mode->reg_data_size; ++i, ++regs) {
    slave_addr = regs->slave_addr;
    reg_addr = regs->reg_addr;
    reg_byte = regs->reg_byte;
    val = regs->val;
    val_byte = regs->val_byte;
    delay_ms = regs->delay_ms;

    ret = maxim_ops_i2c_write(sensor, slave_addr, reg_addr, val, reg_byte,
                              val_byte);
    if (delay_ms)
      usleep_range(1000 * delay_ms, 1000 * delay_ms + 1000 * delay_ms / 10);
  }
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return ret;
}

static int max9296_set_autoexposure(struct max9296_dev *sensor, bool on) {
  return 0;
}

/* read exposure, in number of line periods */
static int max9296_get_exposure(struct max9296_dev *sensor) { return 0; }

static int max9296_get_gain(struct max9296_dev *sensor) { return 0; }

static int max9296_set_gain(struct max9296_dev *sensor, int gain) { return 0; }

static int max9296_set_autogain(struct max9296_dev *sensor, bool on) {
  return 0;
}

static int max9296_set_stream_mipi(struct max9296_dev *sensor, bool on) {
  int ret;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s (%s)", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           on ? "on" : "off");
  if (on) {
    if (sensor->current_mode->id == MAX9296_MODE_1280x720)
      ret = maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x02, 2, 1);
    else
      ret = maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x82, 2, 1);
  } else {
    ret = maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x00, 2, 1);
  }

  msleep(100);

  return ret;
}

static int max9296_set_bandingfilter(struct max9296_dev *sensor) { return 0; }

static int max9296_get_binning(struct max9296_dev *sensor) { return 0; }

static int max9296_set_binning(struct max9296_dev *sensor, bool enable) {
  return 0;
}

static const struct max9296_mode_info *
max9296_find_mode(struct max9296_dev *sensor, int width, int height,
                  bool nearest) {
  const struct max9296_mode_info *mode;

  mode =
      v4l2_find_nearest_size(max9296_mode_data, ARRAY_SIZE(max9296_mode_data),
                             width, height, width, height);
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s (id:%u width:%u height:%u "
                     "reg_data_size:%u max_fps:%u)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, mode->id, mode->width, mode->height,
           mode->reg_data_size, mode->max_fps);

  if (!mode || (!nearest && (mode->width != width || mode->height != height)))
    return NULL;

  return mode;
}

//-------------------------------------------------------------------------

static u64 max9296_calc_pixel_rate(struct max9296_dev *sensor) {
  u64 rate;

  rate = sensor->current_mode->width * sensor->current_mode->height;
  rate *= sensor->fps;
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (rate:%llu)", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__, rate);
  return rate;
}

static void max9296_power(struct max9296_dev *sensor, bool enable) {
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s (pwdn %s)", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           enable ? "high" : "low");
  gpiod_set_value_cansleep(sensor->pwdn_gpio, enable ? 0 : 1);

  if (sensor->shared.sensor != NULL) {
    if (debug)
      printk(KERN_NOTICE "[%s:%d][%s:%d] %s (shared pwdn %s)", "RST",
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
             enable ? "high" : "low");
    gpiod_set_value_cansleep(sensor->shared.sensor->pwdn_gpio, enable ? 0 : 1);
  }
  usleep_range(10000, 11000);
}

static void max9296_reset(struct max9296_dev *sensor) {
  struct gpio_desc *reset_gpio;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  if (sensor->shared.sensor != NULL)
    reset_gpio = (sensor->reset_gpio ? sensor->reset_gpio
                                     : sensor->shared.sensor->reset_gpio);
  else
    reset_gpio = sensor->reset_gpio;

  max9296_power(sensor, false);
  ssleep(1);

  if (reset_gpio) {
    if (debug)
      printk(KERN_NOTICE "[%s:%d][%s:%d] %s (reset low)", "RST",
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    gpiod_set_value_cansleep(reset_gpio, 1);
    ssleep(2);
  }

  if (reset_gpio) {
    if (debug)
      printk(KERN_NOTICE "[%s:%d][%s:%d] %s (reset high)", "RST",
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    gpiod_set_value_cansleep(reset_gpio, 0);
    ssleep(1);
  }

  /* camera power cycle */
  max9296_power(sensor, true);
  ssleep(1);

  // ssleep(2);
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
}

static int max9296_set_power_on(struct max9296_dev *sensor) {
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  usleep_range(10000, 11000);
  max9296_reset(sensor);
  usleep_range(10000, 11000);
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;
}

static void max9296_set_power_off(struct max9296_dev *sensor) {
  struct gpio_desc *reset_gpio;
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s start", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  if (sensor->shared.sensor != NULL)
    reset_gpio = (sensor->reset_gpio ? sensor->reset_gpio
                                     : sensor->shared.sensor->reset_gpio);
  else
    reset_gpio = sensor->reset_gpio;

  if (reset_gpio) {
    if (debug)
      printk(KERN_NOTICE "[%s:%d][%s:%d] %s (reset low)", "RST",
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    gpiod_set_value_cansleep(reset_gpio, 1);
  }

  max9296_power(sensor, false);
  sensor->streaming = false;
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
}

static int max9296_set_power(struct max9296_dev *sensor, bool on) {
  int ret = 0;
  int shared_power_ctl = 0;
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  if (sensor->shared.sensor != NULL) {
    while (sensor->shared.sensor->state.power == MAX9296_STATE_RUNNING) {
      shared_power_ctl = 1;
      msleep(100);
    }

    if ((sensor->shared.sensor->state.power != MAX9296_STATE_DONE) &&
        (shared_power_ctl == 0)) {
      sensor->state.power = MAX9296_STATE_RUNNING;
      sensor->shared.sensor->state.power = MAX9296_STATE_RUNNING;

      if (on) {
        ret = max9296_set_power_on(sensor);
        if (ret)
          return ret;
      }

      if (!on)
        max9296_set_power_off(sensor);

      sensor->state.power = MAX9296_STATE_DONE;
      sensor->shared.sensor->state.power = MAX9296_STATE_DONE;
    }
  } else {
    sensor->state.power = MAX9296_STATE_RUNNING;

    if (on) {
      ret = max9296_set_power_on(sensor);
      if (ret)
        return ret;
    }

    if (!on)
      max9296_set_power_off(sensor);

    sensor->state.power = MAX9296_STATE_DONE;
  }
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;
}

/* --------------- Subdev Operations --------------- */

static int max9296_s_power(struct v4l2_subdev *sd, int on) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  int ret = 0;
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (%d)", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__, on);

  mutex_lock(&sensor->lock);

  /* Update the power count. */
  if ((sensor->power_count == 0) && on) {
    // on
    ret = max9296_set_power(sensor, 1);
  }

  sensor->power_count += on ? 1 : -1;

  WARN_ON(sensor->power_count < 0);

  if (sensor->power_count == 0) {
    // off
    ret = max9296_set_power(sensor, 0);
    sensor->state.power = MAX9296_STATE_IDLE;
    if (sensor->shared.sensor != NULL)
      sensor->shared.sensor->state.power = MAX9296_STATE_IDLE;
    ssleep(5);
  }

  mutex_unlock(&sensor->lock);
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end(%d)", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__, on);
  return ret;
}

static int max9296_get_fmt(struct v4l2_subdev *sd,
                           struct v4l2_subdev_pad_config *cfg,
                           struct v4l2_subdev_format *format) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct v4l2_mbus_framefmt *fmt;

  if (format->pad != 0) {
    printk(KERN_WARNING "[%s:%d][%s:%d] %s return -EINVAL", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    return -EINVAL;
  }

  mutex_lock(&sensor->lock);

  if (format->which == V4L2_SUBDEV_FORMAT_TRY)
    fmt = v4l2_subdev_get_try_format(&sensor->sd, cfg, format->pad);
  else
    fmt = &sensor->fmt;

  fmt->reserved[1] = sensor->fps;
  format->format = *fmt;
  if (debug)
    printk(KERN_DEBUG
           "[%s:%d][%s:%d] %s (width:%u height:%u code:0x%x field:%u "
           "colorspace:%u ycbcr_enc:%u quantization:%u xfer_func:%u fps:%u)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, fmt->width, fmt->height, fmt->code, fmt->field,
           fmt->colorspace, fmt->ycbcr_enc, fmt->quantization, fmt->xfer_func,
           fmt->reserved[1]);
  mutex_unlock(&sensor->lock);
  return 0;
}

static int max9296_try_fmt_internal(struct v4l2_subdev *sd,
                                    struct v4l2_mbus_framefmt *fmt,
                                    const struct max9296_mode_info **new_mode) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  const struct max9296_mode_info *mode;
  int i;

  mode = max9296_find_mode(sensor, fmt->width, fmt->height, true);
  if (!mode) {
    printk(KERN_WARNING "[%s:%d][%s:%d] %s return -EINVAL", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    return -EINVAL;
  }

  fmt->width = mode->width;
  fmt->height = mode->height;
  memset(fmt->reserved, 0, sizeof(fmt->reserved));

  if (new_mode)
    *new_mode = mode;

  for (i = 0; i < ARRAY_SIZE(max9296_formats); i++)
    if (max9296_formats[i].code == fmt->code)
      break;

  if (i >= ARRAY_SIZE(max9296_formats))
    i = 0;

  fmt->code = max9296_formats[i].code;
  fmt->colorspace = max9296_formats[i].colorspace;
  fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
  fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
  fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s (width:%u height:%u code:0x%x field:%u "
                     "colorspace:%u ycbcr_enc:%u quantization:%u xfer_func:%u)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, fmt->width, fmt->height, fmt->code, fmt->field,
           fmt->colorspace, fmt->ycbcr_enc, fmt->quantization, fmt->xfer_func);
  return 0;
}

static int max9296_set_fmt(struct v4l2_subdev *sd,
                           struct v4l2_subdev_pad_config *cfg,
                           struct v4l2_subdev_format *format) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  const struct max9296_mode_info *new_mode;
  struct v4l2_mbus_framefmt *mbus_fmt = &format->format;
  struct v4l2_mbus_framefmt *fmt;
  int ret;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  if (format->pad != 0) {
    printk(KERN_WARNING "[%s:%d][%s:%d] %s return -EINVAL", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    return -EINVAL;
  }

  mutex_lock(&sensor->lock);

  if (sensor->streaming) {
    printk(KERN_WARNING "[%s:%d][%s:%d] %s goto out", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    ret = -EBUSY;
    goto out;
  }

  ret = max9296_try_fmt_internal(sd, mbus_fmt, &new_mode);
  if (ret) {
    printk(KERN_WARNING "[%s:%d][%s:%d] %s goto out", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    goto out;
  }

  if (format->which == V4L2_SUBDEV_FORMAT_TRY)
    fmt = v4l2_subdev_get_try_format(sd, cfg, 0);
  else
    fmt = &sensor->fmt;

  *fmt = *mbus_fmt;

  if (new_mode != sensor->current_mode) {
    sensor->current_mode = new_mode;
    sensor->pending_mode_change = true;
  }
  if (mbus_fmt->code != sensor->fmt.code)
    sensor->pending_fmt_change = true;

  __v4l2_ctrl_s_ctrl_int64(sensor->ctrls.pixel_rate,
                           max9296_calc_pixel_rate(sensor));

  if (sensor->pending_mode_change || sensor->pending_fmt_change)
    sensor->fmt = *mbus_fmt;
out:

  sensor->state.setup = MAX9296_STATE_DONE;
  mutex_unlock(&sensor->lock);
  return ret;
}

/*
 * Sensor Controls.
 */

/* Write a register to per-channel addresses in dual mode, or global in single
 * mode */
static int max9296_write_per_channel(struct max9296_dev *sensor,
                                     unsigned int reg, unsigned int val,
                                     unsigned int reg_byte,
                                     unsigned int val_byte) {
  bool dual = (sensor->current_mode->id == MAX9296_MODE_2560x720 ||
               sensor->current_mode->id == MAX9296_MODE_3840x1080);
  int ret;

  if (dual) {
    ret = maxim_ops_i2c_write(sensor, AP1302_CH0_I2C_ADDR, reg, val, reg_byte,
                              val_byte);
    if (!ret)
      ret = maxim_ops_i2c_write(sensor, AP1302_CH1_I2C_ADDR, reg, val, reg_byte,
                                val_byte);
  } else {
    ret = maxim_ops_i2c_write(sensor, AP1302_I2C_ADDR, reg, val, reg_byte,
                              val_byte);
  }
  return ret;
}

static int max9296_set_ctrl_hue(struct max9296_dev *sensor, int value) {
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s value:%d", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         value);
  return 0;
}

static int max9296_set_ctrl_lsc(struct max9296_dev *sensor, int value) {
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s value:%d", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         value);
  return max9296_write_per_channel(sensor, AP1302_REG_LSC_CTRL, value, 2, 2);
}

static int max9296_set_ctrl_white_balance(struct max9296_dev *sensor, int awb) {
  int ret;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s awb:%d", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__, awb);

  if (awb)
    ret = max9296_write_per_channel(sensor, AP1302_REG_AWB_CTRL,
                                    AP1302_AWB_CTRL_AUTO, 2, 2);
  else
    ret = max9296_write_per_channel(sensor, AP1302_REG_AWB_CTRL, 0x0000, 2, 2);

  return ret;
}

static int
max9296_set_ctrl_exposure(struct max9296_dev *sensor,
                          enum v4l2_exposure_auto_type auto_exposure) {
  int ret;
  u16 ae_val = (auto_exposure == V4L2_EXPOSURE_AUTO) ? AP1302_AE_CTRL_AUTO
                                                     : AP1302_AE_CTRL_MANUAL;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s auto_exposure:%d ae_val:0x%04x",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         __FUNCTION__, auto_exposure, ae_val);

  /* AE mode: per-channel (0x11/0x12 in dual) */
  ret = max9296_write_per_channel(sensor, AP1302_REG_AE_CTRL, ae_val, 2, 2);
  if (ret < 0)
    return ret;

  /* When switching to manual, apply the current exposure value */
  if (auto_exposure != V4L2_EXPOSURE_AUTO) {
    int exp_val = sensor->ctrl_cache.firmware_ready
                      ? (sensor->ctrls.exp_time ? sensor->ctrls.exp_time->val
                                                : sensor->ctrl_cache.exposure)
                      : sensor->ctrl_cache.exposure;
    /* exp_time: per-channel in dual mode (0x11/0x12), global in single (0x3c)
     */
    ret = max9296_write_per_channel(sensor, AP1302_REG_EXP_TIME, exp_val, 2, 4);
  }

  return ret;
}

static int max9296_set_ctrl_gain(struct max9296_dev *sensor, bool auto_gain) {
  int ret = 0;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s auto_gain:%d", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         auto_gain);

  /* When manual gain, write gain value to AP1302 ISP (per-channel in dual) */
  if (!auto_gain) {
    int gain_val = sensor->ctrl_cache.ch0.gain;
    ret = max9296_write_per_channel(sensor, AP1302_REG_AE_GAIN, gain_val, 2, 2);
  }

  return ret;
}

static int max9296_set_ctrl_test_pattern(struct max9296_dev *sensor,
                                         int value) {
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s value:%d", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         value);
  return 0;
}

static int max9296_set_ctrl_light_freq(struct max9296_dev *sensor, int value) {
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s value:%d", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         value);
  return 0;
}

static int max9296_set_ctrl_pixelrate(struct max9296_dev *sensor, int value) {
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s value:%d", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         value);
  return 0;
}

static int max9296_g_volatile_ctrl(struct v4l2_ctrl *ctrl) {
  struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  /*
   * Do not override cached V4L2 control values from volatile reads.
   * The legacy get_* helpers return 0 and would clobber defaults
   * (gain/exposure).
   */
  return 0;
}

static void max9296_cache_ctrl(struct max9296_dev *sensor,
                               struct v4l2_ctrl *ctrl) {
  sensor->ctrl_cache.pending = true;
  switch (ctrl->id) {
  /* Shared controls */
  case V4L2_CID_EXP_TIME:
    /* Shared exposure time: keep both channels in sync by default */
    sensor->ctrl_cache.exposure = ctrl->val;
    sensor->ctrl_cache.ch0.exposure = ctrl->val;
    sensor->ctrl_cache.ch1.exposure = ctrl->val;
    break;

  /* Per-channel controls - channel 0 */
  case V4L2_CID_EXPOSURE_AUTO_CH0:
    sensor->ctrl_cache.ch0.ae_on = ctrl->val ? 1 : 0;
    break;
  case V4L2_CID_AUTO_WHITE_BALANCE_CH0:
    sensor->ctrl_cache.ch0.awb = ctrl->val;
    break;
  case V4L2_CID_AUTOGAIN_CH0:
    sensor->ctrl_cache.ch0.gain_auto = ctrl->val;
    break;
  case V4L2_CID_GAIN_CH0:
    sensor->ctrl_cache.ch0.gain = ctrl->val;
    break;
  case V4L2_CID_EXPOSURE_CH0:
    sensor->ctrl_cache.ch0.exposure = ctrl->val;
    break;
  case V4L2_CID_HFLIP_CH0:
    sensor->ctrl_cache.ch0.hflip = ctrl->val;
    break;
  case V4L2_CID_VFLIP_CH0:
    sensor->ctrl_cache.ch0.vflip = ctrl->val;
    break;
  case V4L2_CID_LSC_CH0:
    sensor->ctrl_cache.ch0.lsc = ctrl->val;
    break;
  case V4L2_CID_BRIGHTNESS_CH0:
    sensor->ctrl_cache.ch0.brightness = ctrl->val;
    break;
  case V4L2_CID_CONTRAST_CH0:
    sensor->ctrl_cache.ch0.contrast = ctrl->val;
    break;
  case V4L2_CID_SATURATION_CH0:
    sensor->ctrl_cache.ch0.saturation = ctrl->val;
    break;

  /* Per-channel controls - channel 1 */
  case V4L2_CID_EXPOSURE_AUTO_CH1:
    sensor->ctrl_cache.ch1.ae_on = ctrl->val ? 1 : 0;
    break;
  case V4L2_CID_AUTO_WHITE_BALANCE_CH1:
    sensor->ctrl_cache.ch1.awb = ctrl->val;
    break;
  case V4L2_CID_AUTOGAIN_CH1:
    sensor->ctrl_cache.ch1.gain_auto = ctrl->val;
    break;
  case V4L2_CID_GAIN_CH1:
    sensor->ctrl_cache.ch1.gain = ctrl->val;
    break;
  case V4L2_CID_EXPOSURE_CH1:
    sensor->ctrl_cache.ch1.exposure = ctrl->val;
    break;
  case V4L2_CID_HFLIP_CH1:
    sensor->ctrl_cache.ch1.hflip = ctrl->val;
    break;
  case V4L2_CID_VFLIP_CH1:
    sensor->ctrl_cache.ch1.vflip = ctrl->val;
    break;
  case V4L2_CID_LSC_CH1:
    sensor->ctrl_cache.ch1.lsc = ctrl->val;
    break;
  case V4L2_CID_BRIGHTNESS_CH1:
    sensor->ctrl_cache.ch1.brightness = ctrl->val;
    break;
  case V4L2_CID_CONTRAST_CH1:
    sensor->ctrl_cache.ch1.contrast = ctrl->val;
    break;
  case V4L2_CID_SATURATION_CH1:
    sensor->ctrl_cache.ch1.saturation = ctrl->val;
    break;

  default:
    break;
  }
  if (debug) {
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s cached ctrl 0x%x = %d\n", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           ctrl->id, ctrl->val);
  }
}

/* Helper function to apply settings for one channel */
static void max9296_apply_channel_controls(struct max9296_dev *sensor,
                                           u32 i2c_addr,
                                           struct max9296_channel_ctrl *ch_ctrl,
                                           const char *ch_name) {
  u16 ae_val, awb_val, rot;
  u32 exp_seed;
  u16 gain_seed;
  int ret, err = 0;

  /* STEP 1: Initialize AE to manual mode first */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_CTRL,
                            AP1302_AE_CTRL_MANUAL, 2, 2);
  if (ret)
    err = ret;
  msleep(100);

  /*
   * Seed exposure time while in manual mode.
   * Some FW revisions need a non-zero seed before switching to AE auto.
   */
  exp_seed =
      ch_ctrl->exposure
          ? ch_ctrl->exposure
          : (sensor->ctrl_cache.exposure ? sensor->ctrl_cache.exposure : 10000);
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_EXP_TIME, exp_seed, 2,
                            4);
  if (ret)
    err = ret;
  msleep(100);

  /* STEP 2: Apply configured AE mode (auto/manual) */
  ae_val = ch_ctrl->ae_on ? AP1302_AE_CTRL_AUTO : AP1302_AE_CTRL_MANUAL;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_CTRL, ae_val, 2, 2);
  if (ret)
    err = ret;
  if (ch_ctrl->ae_on)
    msleep(100);

  /* AWB (auto/manual) */
  awb_val = ch_ctrl->awb ? AP1302_AWB_CTRL_AUTO : 0x0000;
  ret =
      maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AWB_CTRL, awb_val, 2, 2);
  if (ret)
    err = ret;

  /* Gain value (always set, used when switching to manual) */
  gain_seed = ch_ctrl->gain ? ch_ctrl->gain : 256;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_GAIN, gain_seed, 2,
                            2);
  if (ret)
    err = ret;

  /* Rotation (hflip + vflip combined) */
  rot = (ch_ctrl->hflip ? 0x01 : 0x00) | (ch_ctrl->vflip ? 0x02 : 0x00);
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_ROTATION, rot, 2, 2);
  if (ret)
    err = ret;

  /* Per-channel tuning values */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_LSC_CTRL, ch_ctrl->lsc,
                            2, 2);
  if (ret)
    err = ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_BRIGHTNESS,
                            ch_ctrl->brightness, 2, 2);
  if (ret)
    err = ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_CONTRAST,
                            ch_ctrl->contrast, 2, 2);
  if (ret)
    err = ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_SATURATION,
                            ch_ctrl->saturation, 2, 2);
  if (ret)
    err = ret;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s %s applied (i2c:0x%02x ae:%s awb:%d "
                     "gain:%d exp_seed:%u rot:0x%02x) ret:%d\n",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         __FUNCTION__, ch_name, i2c_addr, ch_ctrl->ae_on ? "auto" : "manual",
         ch_ctrl->awb, gain_seed, exp_seed, rot, err);
}

static void max9296_apply_cached_controls(struct max9296_dev *sensor) {
  bool dual = (sensor->current_mode->id == MAX9296_MODE_2560x720 ||
               sensor->current_mode->id == MAX9296_MODE_3840x1080);
  int i2c_nr = sensor->i2c_client->adapter->nr;
  const char *ch0_name, *ch1_name;

  if (!sensor->ctrl_cache.pending)
    return;

  /* Determine channel names based on I2C adapter number */
  if (i2c_nr == 1) {
    ch0_name = "ch2";
    ch1_name = "ch3";
  } else {
    ch0_name = "ch0";
    ch1_name = "ch1";
  }

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s applying cached controls (dual:%d)",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         __FUNCTION__, dual);

  /* Exposure seeding is handled in max9296_apply_channel_controls(). */

  /* Shared tuning values */

  if (dual) {
    /* Dual-channel mode: apply each channel's settings separately */
    max9296_apply_channel_controls(sensor, AP1302_CH0_I2C_ADDR,
                                   &sensor->ctrl_cache.ch0, ch0_name);
    max9296_apply_channel_controls(sensor, AP1302_CH1_I2C_ADDR,
                                   &sensor->ctrl_cache.ch1, ch1_name);
  } else {
    /* Single-channel mode: apply ch0 settings to global address */
    max9296_apply_channel_controls(sensor, AP1302_I2C_ADDR,
                                   &sensor->ctrl_cache.ch0, "single");
  }

  sensor->ctrl_cache.pending = false;
  sensor->ctrl_cache.firmware_ready = true;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s cached controls applied (exp:%d)",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         __FUNCTION__, sensor->ctrl_cache.exposure);
}

static int max9296_s_ctrl(struct v4l2_ctrl *ctrl) {
  struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  int ret;
  bool dual = (sensor->current_mode->id == MAX9296_MODE_2560x720 ||
               sensor->current_mode->id == MAX9296_MODE_3840x1080);
  u32 ch0_addr = dual ? AP1302_CH0_I2C_ADDR : AP1302_I2C_ADDR;
  u32 ch1_addr = dual ? AP1302_CH1_I2C_ADDR : AP1302_I2C_ADDR;

  if (debug)
    printk(
        KERN_NOTICE
        "[%s:%d][%s:%d] %s ctrl->id:0x%x ctrl->val:%d fw_ready:%d pw_cnt:%d\n",
        KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
        __FUNCTION__, ctrl->id, ctrl->val, sensor->ctrl_cache.firmware_ready,
        sensor->power_count);
  /* v4l2_ctrl_lock() locks our own mutex */

  /* Always update cache (even if powered off) */
  max9296_cache_ctrl(sensor, ctrl);

  /*
   * If the device is not powered up by the host driver do
   * not apply any controls to H/W at this time. Instead
   * the controls will be restored right after power-up.
   */
  if (sensor->power_count == 0) {
    return 0;
  }

  /* If firmware not ready, just cache (apply later) */
  if (!sensor->ctrl_cache.firmware_ready) {
    return 0;
  }

  /* Firmware ready: apply immediately to hardware */
  switch (ctrl->id) {
  case V4L2_CID_EXP_TIME:
    /* Shared exposure time */
    ret =
        max9296_write_per_channel(sensor, AP1302_REG_EXP_TIME, ctrl->val, 2, 4);
    break;
  case V4L2_CID_HUE:
    ret = max9296_set_ctrl_hue(sensor, ctrl->val);
    break;
  case V4L2_CID_TEST_PATTERN:
    ret = max9296_set_ctrl_test_pattern(sensor, ctrl->val);
    break;
  case V4L2_CID_POWER_LINE_FREQUENCY:
    ret = max9296_set_ctrl_light_freq(sensor, ctrl->val);
    break;
  case V4L2_CID_PIXEL_RATE:
    ret = max9296_set_ctrl_pixelrate(sensor, ctrl->val);
    break;

  /* Per-channel controls - Channel 0 */
  case V4L2_CID_EXPOSURE_AUTO_CH0: {
    u16 ae_val = ctrl->val ? AP1302_AE_CTRL_AUTO : AP1302_AE_CTRL_MANUAL;
    ret =
        maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_AE_CTRL, ae_val, 2, 2);
    if (!ret && !ctrl->val) {
      u32 exp_val =
          sensor->ctrl_cache.ch0.exposure
              ? sensor->ctrl_cache.ch0.exposure
              : (sensor->ctrl_cache.exposure ? sensor->ctrl_cache.exposure
                                             : 10000);
      ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_EXP_TIME, exp_val,
                                2, 4);
    }
    break;
  }
  case V4L2_CID_AUTO_WHITE_BALANCE_CH0: {
    u16 awb_val = ctrl->val ? AP1302_AWB_CTRL_AUTO : 0x0000;
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_AWB_CTRL, awb_val, 2,
                              2);
    break;
  }
  case V4L2_CID_AUTOGAIN_CH0:
    /* Gain auto mode handled implicitly via AE mode */
    ret = 0;
    break;
  case V4L2_CID_GAIN_CH0:
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_AE_GAIN, ctrl->val,
                              2, 2);
    break;
  case V4L2_CID_LSC_CH0:
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_LSC_CTRL, ctrl->val,
                              2, 2);
    break;
  case V4L2_CID_EXPOSURE_CH0:
    if (!sensor->ctrl_cache.ch0.ae_on) {
      ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_EXP_TIME,
                                ctrl->val, 2, 4);
    } else {
      ret = 0;
    }
    break;
  case V4L2_CID_HFLIP_CH0:
  case V4L2_CID_VFLIP_CH0: {
    unsigned int rot = (sensor->ctrl_cache.ch0.hflip ? 0x01 : 0x00) |
                       (sensor->ctrl_cache.ch0.vflip ? 0x02 : 0x00);
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_ROTATION, rot, 2, 2);
    break;
  }
  case V4L2_CID_BRIGHTNESS_CH0:
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_BRIGHTNESS,
                              ctrl->val, 2, 2);
    break;
  case V4L2_CID_CONTRAST_CH0:
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_CONTRAST, ctrl->val,
                              2, 2);
    break;
  case V4L2_CID_SATURATION_CH0:
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_SATURATION,
                              ctrl->val, 2, 2);
    break;

  /* Per-channel controls - Channel 1 */
  case V4L2_CID_EXPOSURE_AUTO_CH1: {
    u16 ae_val = ctrl->val ? AP1302_AE_CTRL_AUTO : AP1302_AE_CTRL_MANUAL;
    ret =
        maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_AE_CTRL, ae_val, 2, 2);
    if (!ret && !ctrl->val) {
      u32 exp_val =
          sensor->ctrl_cache.ch1.exposure
              ? sensor->ctrl_cache.ch1.exposure
              : (sensor->ctrl_cache.exposure ? sensor->ctrl_cache.exposure
                                             : 10000);
      ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_EXP_TIME, exp_val,
                                2, 4);
    }
    break;
  }
  case V4L2_CID_AUTO_WHITE_BALANCE_CH1: {
    u16 awb_val = ctrl->val ? AP1302_AWB_CTRL_AUTO : 0x0000;
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_AWB_CTRL, awb_val, 2,
                              2);
    break;
  }
  case V4L2_CID_AUTOGAIN_CH1:
    /* Gain auto mode handled implicitly via AE mode */
    ret = 0;
    break;
  case V4L2_CID_GAIN_CH1:
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_AE_GAIN, ctrl->val,
                              2, 2);
    break;
  case V4L2_CID_LSC_CH1:
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_LSC_CTRL, ctrl->val,
                              2, 2);
    break;
  case V4L2_CID_EXPOSURE_CH1:
    if (!sensor->ctrl_cache.ch1.ae_on) {
      ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_EXP_TIME,
                                ctrl->val, 2, 4);
    } else {
      ret = 0;
    }
    break;
  case V4L2_CID_HFLIP_CH1:
  case V4L2_CID_VFLIP_CH1: {
    unsigned int rot = (sensor->ctrl_cache.ch1.hflip ? 0x01 : 0x00) |
                       (sensor->ctrl_cache.ch1.vflip ? 0x02 : 0x00);
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_ROTATION, rot, 2, 2);
    break;
  }
  case V4L2_CID_BRIGHTNESS_CH1:
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_BRIGHTNESS,
                              ctrl->val, 2, 2);
    break;
  case V4L2_CID_CONTRAST_CH1:
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_CONTRAST, ctrl->val,
                              2, 2);
    break;
  case V4L2_CID_SATURATION_CH1:
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_SATURATION,
                              ctrl->val, 2, 2);
    break;

  default:
    printk(KERN_CRIT "[%s:%d][%s:%d] %s return", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    ret = -EINVAL;
    break;
  }

  return ret;
}

static const struct v4l2_ctrl_ops max9296_ctrl_ops = {
    .g_volatile_ctrl = max9296_g_volatile_ctrl,
    .s_ctrl = max9296_s_ctrl,
};

static const char *max9296_devm_ctrl_name(struct max9296_dev *sensor,
                                          const char *fmt, int idx) {
  char tmp[64];

  snprintf(tmp, sizeof(tmp), fmt, idx);
  return devm_kstrdup(&sensor->i2c_client->dev, tmp, GFP_KERNEL);
}

/* Per-channel control descriptor for table-driven registration */
struct max9296_ctrl_desc {
  u32 cid_ch0;
  u32 cid_ch1;
  enum v4l2_ctrl_type type;
  const char *name;
  s64 min;
  s64 max;
  s64 def;
  size_t offset_ch0; /* offset into struct max9296_ctrls */
  size_t offset_ch1;
};

#define CTRL_DESC(_cid0, _cid1, _type, _name, _min, _max, _def, _m0, _m1)      \
  {_cid0,                                                                      \
   _cid1,                                                                      \
   _type,                                                                      \
   _name,                                                                      \
   _min,                                                                       \
   _max,                                                                       \
   _def,                                                                       \
   offsetof(struct max9296_ctrls, _m0),                                        \
   offsetof(struct max9296_ctrls, _m1)}

static const struct max9296_ctrl_desc max9296_per_ch_ctrls[] = {
    CTRL_DESC(V4L2_CID_EXPOSURE_AUTO_CH0, V4L2_CID_EXPOSURE_AUTO_CH1,
              V4L2_CTRL_TYPE_BOOLEAN, "AE On", 0, 1, 1, auto_exp_ch0,
              auto_exp_ch1),
    CTRL_DESC(V4L2_CID_AUTO_WHITE_BALANCE_CH0, V4L2_CID_AUTO_WHITE_BALANCE_CH1,
              V4L2_CTRL_TYPE_BOOLEAN, "Auto White Balance", 0, 1, 1,
              auto_wb_ch0, auto_wb_ch1),
    CTRL_DESC(V4L2_CID_AUTOGAIN_CH0, V4L2_CID_AUTOGAIN_CH1,
              V4L2_CTRL_TYPE_BOOLEAN, "Auto Gain", 0, 1, 1, auto_gain_ch0,
              auto_gain_ch1),
    CTRL_DESC(V4L2_CID_GAIN_CH0, V4L2_CID_GAIN_CH1, V4L2_CTRL_TYPE_INTEGER,
              "Gain", 0, 65535, 256, gain_ch0, gain_ch1),
    CTRL_DESC(V4L2_CID_EXPOSURE_CH0, V4L2_CID_EXPOSURE_CH1,
              V4L2_CTRL_TYPE_INTEGER, "Exp Time", 0, INT_MAX, 10000,
              exposure_ch0, exposure_ch1),
    CTRL_DESC(V4L2_CID_HFLIP_CH0, V4L2_CID_HFLIP_CH1, V4L2_CTRL_TYPE_BOOLEAN,
              "HFlip", 0, 1, 0, hflip_ch0, hflip_ch1),
    CTRL_DESC(V4L2_CID_VFLIP_CH0, V4L2_CID_VFLIP_CH1, V4L2_CTRL_TYPE_BOOLEAN,
              "VFlip", 0, 1, 0, vflip_ch0, vflip_ch1),
    CTRL_DESC(V4L2_CID_LSC_CH0, V4L2_CID_LSC_CH1, V4L2_CTRL_TYPE_INTEGER, "LSC",
              0, 65535, 0x3fff, lsc_ch0, lsc_ch1),
    CTRL_DESC(V4L2_CID_BRIGHTNESS_CH0, V4L2_CID_BRIGHTNESS_CH1,
              V4L2_CTRL_TYPE_INTEGER, "Brightness", 0, 65535, 0, brightness_ch0,
              brightness_ch1),
    CTRL_DESC(V4L2_CID_CONTRAST_CH0, V4L2_CID_CONTRAST_CH1,
              V4L2_CTRL_TYPE_INTEGER, "Contrast", 0, 65535, 0, contrast_ch0,
              contrast_ch1),
    CTRL_DESC(V4L2_CID_SATURATION_CH0, V4L2_CID_SATURATION_CH1,
              V4L2_CTRL_TYPE_INTEGER, "Saturation", 0, 65535, 4096,
              saturation_ch0, saturation_ch1),
};

static int max9296_init_controls(struct max9296_dev *sensor) {
  const struct v4l2_ctrl_ops *ops = &max9296_ctrl_ops;
  struct max9296_ctrls *ctrls = &sensor->ctrls;
  struct v4l2_ctrl_handler *hdl = &ctrls->handler;
  int ret;
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  v4l2_ctrl_handler_init(hdl, 48);

  /* we can use our own mutex for the ctrl lock */
  hdl->lock = &sensor->lock;

  /* Clock related controls */
  ctrls->pixel_rate =
      v4l2_ctrl_new_std(hdl, ops, V4L2_CID_PIXEL_RATE, 0, INT_MAX, 1,
                        max9296_calc_pixel_rate(sensor));

  /* exp_time: exposure time (same HW register 0x500c) */
  {
    static const struct v4l2_ctrl_config cfg_exp_time = {
        .ops = &max9296_ctrl_ops,
        .id = V4L2_CID_EXP_TIME,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "Exp Time",
        .min = 0,
        .max = INT_MAX,
        .def = 10000,
        .step = 1,
    };
    ctrls->exp_time = v4l2_ctrl_new_custom(hdl, &cfg_exp_time, NULL);
  }
  ctrls->hue = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HUE, 0, 359, 1, 0);
  ctrls->light_freq =
      v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_POWER_LINE_FREQUENCY,
                             V4L2_CID_POWER_LINE_FREQUENCY_AUTO, 0,
                             V4L2_CID_POWER_LINE_FREQUENCY_50HZ);

  /* Per-channel controls: table-driven registration */
  {
    bool second = (sensor->i2c_client->adapter->nr == 1);
    int ch0_num = second ? 2 : 0;
    int ch1_num = second ? 3 : 1;
    int i;

    for (i = 0; i < ARRAY_SIZE(max9296_per_ch_ctrls); i++) {
      const struct max9296_ctrl_desc *d = &max9296_per_ch_ctrls[i];
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .type = d->type,
          .min = d->min,
          .max = d->max,
          .def = d->def,
          .step = 1,
      };
      struct v4l2_ctrl **p0 =
          (struct v4l2_ctrl **)((char *)ctrls + d->offset_ch0);
      struct v4l2_ctrl **p1 =
          (struct v4l2_ctrl **)((char *)ctrls + d->offset_ch1);

      cfg.id = d->cid_ch0;
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL, "%s CH%d",
                                d->name, ch0_num);
      if (!cfg.name)
        return -ENOMEM;
      *p0 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);

      cfg.id = d->cid_ch1;
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL, "%s CH%d",
                                d->name, ch1_num);
      if (!cfg.name)
        return -ENOMEM;
      *p1 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
  }

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (pixel_rate:%d exp_time:%d)", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         ctrls->pixel_rate->val, ctrls->exp_time ? ctrls->exp_time->val : 0);
  printk(KERN_NOTICE
         "[%s:%d][%s:%d] %s (gain_ch0:%d awb_ch0:%d sat_ch0:%d hue:%d "
         "con_ch0:%d hflip_ch0:%d vflip_ch0:%d light_freq:%d)",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         __FUNCTION__, ctrls->gain_ch0 ? ctrls->gain_ch0->val : 0,
         ctrls->auto_wb_ch0 ? ctrls->auto_wb_ch0->val : 0,
         ctrls->saturation_ch0 ? ctrls->saturation_ch0->val : 0,
         ctrls->hue->val, ctrls->contrast_ch0 ? ctrls->contrast_ch0->val : 0,
         ctrls->hflip_ch0 ? ctrls->hflip_ch0->val : 0,
         ctrls->vflip_ch0 ? ctrls->vflip_ch0->val : 0, ctrls->light_freq->val);

  if (hdl->error) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s hdl->error", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    ret = hdl->error;
    goto free_ctrls;
  }

  ctrls->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
  /* Remove VOLATILE flags to allow userspace writes */
  /* ctrls->exp_time->flags |= V4L2_CTRL_FLAG_VOLATILE; */

  /* Don't use auto_cluster - allow independent control */
  sensor->sd.ctrl_handler = hdl;
  return 0;

free_ctrls:
  v4l2_ctrl_handler_free(hdl);
  return ret;
}

static int max9296_enum_frame_size(struct v4l2_subdev *sd,
                                   struct v4l2_subdev_pad_config *cfg,
                                   struct v4l2_subdev_frame_size_enum *fse) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  if (debug)
    printk(KERN_DEBUG "[%s:%d][%s:%d] %s (fse->index:%d min_width:%u "
                      "max_width:%u min_height:%u max_height:%u)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, fse->index, fse->min_width, fse->max_width,
           fse->min_height, fse->max_height);

  if (fse->pad != 0) {
    if (debug)
      printk(KERN_DEBUG "[%s:%d][%s:%d] %s fse->pad:%d return", KEYWORD,
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
             fse->pad);
    return -EINVAL;
  }
  if (fse->index >= MAX9296_NUM_MODES) {
    if (debug)
      printk(KERN_DEBUG "[%s:%d][%s:%d] %s fse->index:%d return", KEYWORD,
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
             fse->index);
    return -EINVAL;
  }

  fse->min_width = max9296_mode_data[fse->index].width;
  fse->max_width = fse->min_width;
  fse->min_height = max9296_mode_data[fse->index].height;
  fse->max_height = fse->min_height;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s (fse->index:%d min_width:%u "
                     "max_width:%u min_height:%u max_height:%u)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, fse->index, fse->min_width, fse->max_width,
           fse->min_height, fse->max_height);
  return 0;
}

static int
max9296_enum_frame_interval(struct v4l2_subdev *sd,
                            struct v4l2_subdev_pad_config *cfg,
                            struct v4l2_subdev_frame_interval_enum *fie) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  int i, count;

  if (fie->pad != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s fie->pad:%d return err", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           fie->pad);
    return -EINVAL;
  }
  if (fie->index >= DEFAULT_FRAMERATE_FPS) {
    if (debug)
      printk(KERN_CRIT "[%s:%d][%s:%d] %s fie->index:%d return err", KEYWORD,
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
             fie->index);
    return -EINVAL;
  }

  if (fie->width == 0 || fie->height == 0 || fie->code == 0) {
    // pr_warn("Please assign pixel format, width and height.\n");
    printk(KERN_CRIT
           "[%s:%d][%s:%d] %s Please assign pixel format, width and heigh",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__);
    return -EINVAL;
  }

  fie->interval.numerator = 1;
  fie->interval.denominator = fie->index + 1; /* 1, 2, 3, ..., 30 fps */

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s fie->index:%d fie->width:%d "
                       "fie->height:%d fie->code:%d denominator:%d return",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, fie->index, fie->width, fie->height, fie->code,
           fie->interval.denominator);
  return 0;
}

static int max9296_g_frame_interval(struct v4l2_subdev *sd,
                                    struct v4l2_subdev_frame_interval *fi) {
  struct max9296_dev *sensor = to_max9296_dev(sd);

  mutex_lock(&sensor->lock);
  fi->interval = sensor->frame_interval;
  mutex_unlock(&sensor->lock);
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s (numerator:%u denominator:%u)", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           fi->interval.numerator, fi->interval.denominator);
  return 0;
}

static int max9296_s_frame_interval(struct v4l2_subdev *sd,
                                    struct v4l2_subdev_frame_interval *fi) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  int ret = 0;

  if (fi->pad != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s return err", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    return -EINVAL;
  }

  unsigned int fps;

  mutex_lock(&sensor->lock);

  if (fi->interval.numerator == 0 || fi->interval.denominator == 0) {
    ret = -EINVAL;
    goto out;
  }

  fps = fi->interval.denominator / fi->interval.numerator;

  if (fps < 1 || fps > 120) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s invalid fps %u (valid: 1~120)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, fps);
    ret = -EINVAL;
    goto out;
  }

  sensor->frame_interval.numerator = 1;
  sensor->frame_interval.denominator = fps;
  sensor->fps = fps;

  if (sensor->shared.sensor) {
    sensor->shared.sensor->frame_interval = sensor->frame_interval;
    sensor->shared.sensor->fps = fps;
  }

  //if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s (numerator:%u denominator:%u)", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           fi->interval.numerator, fi->interval.denominator);

  __v4l2_ctrl_s_ctrl_int64(sensor->ctrls.pixel_rate,
                           max9296_calc_pixel_rate(sensor));

out:
  mutex_unlock(&sensor->lock);
  return ret;
}

static int max9296_enum_mbus_code(struct v4l2_subdev *sd,
                                  struct v4l2_subdev_pad_config *cfg,
                                  struct v4l2_subdev_mbus_code_enum *code) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  if (code->pad != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s return err", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    return -EINVAL;
  }
  if (code->index >= ARRAY_SIZE(max9296_formats)) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s return err", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    return -EINVAL;
  }

  code->code = max9296_formats[code->index].code;
  return 0;
}

#define MAX9296_FIRMWARE "v4l-ap1302-ar0234.fw"
#define FWSEND (256) // 8258 = (256 * 32) + 66;
#define FWDEV(x) &((x)->dev)
#define BURST_SIZE (8192)
#define INIT_HEADER (0x8000)

static char *firmware = "";

module_param(firmware, charp, 0444);

MODULE_PARM_DESC(firmware, "Firmware image to load");

static int start_fw_load(struct i2c_client *client) {
  int ret = 0;
  struct v4l2_subdev *sd = i2c_get_clientdata(client);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  ret = maxim_ops_i2c_write(sensor, 0x3c, 0xf05a, 0x0014, 2, 2);
  ret = maxim_ops_i2c_write(sensor, 0x3c, 0x6024, 0x00300000, 2, 4);
  ret = maxim_ops_i2c_write(sensor, 0x3c, 0x6034, 0x012c0000, 2, 4);

  msleep(100);
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s complete", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  return ret;
}

static int end_fw_load(struct i2c_client *client) {
  int ret = 0;
  struct v4l2_subdev *sd = i2c_get_clientdata(client);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  ret = maxim_ops_i2c_write(sensor, 0x3c, 0x6002, 0xffff, 2, 2);

  msleep(100);
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  return ret;
}

static int fw_write(struct i2c_client *client, const u8 *data, int size) {
  unsigned short addr = client->addr; /* chip address - NOTE: 7bit	*/

  client->addr = 0x3c;

  if (i2c_master_send(client, data, size) < size) {
    client->addr = addr;
    v4l_err(client, "firmware load i2c failure\n");
    return -ENOSYS;
  }

  client->addr = addr;

  return 0;
}

static const char *get_fw_name(struct i2c_client *client) {
  if (firmware[0])
    return firmware;
  else
    return MAX9296_FIRMWARE;
}
int max9296_loadfw(struct i2c_client *client) {
  struct v4l2_subdev *sd = i2c_get_clientdata(client);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  const struct firmware *fw = NULL;
  u8 buffer[FWSEND + 1];
  const u8 *ptr;
  const char *fwname = get_fw_name(client);
  int size, retval;
  int max_buf_size = FWSEND, burst_size = BURST_SIZE;
  u32 header = INIT_HEADER;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  if (request_firmware(&fw, fwname, FWDEV(client)) != 0) {
    // v4l_err(client, "unable to open firmware %s\n", fwname);
    printk(KERN_CRIT "[%s:%d][%s:%d] unable to open firmware %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, fwname);
    return -EINVAL;
  }

  retval = start_fw_load(client);
  if (retval < 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] start firmware load i2c failure", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__);
    return retval;
  }

  size = fw->size;
  ptr = fw->data;

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s size:%d", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           size);

  while (size > 0) {
    int len = min(max_buf_size - 2, size);
    len = min(burst_size, len);

    memset(buffer, 0x00, sizeof(u8) * FWSEND + 1);

    header = INIT_HEADER + (BURST_SIZE - burst_size);

    buffer[0] = (header >> 8) & 0xff;
    buffer[1] = header & 0xff;

    memcpy(buffer + 2, ptr, len);

    retval = fw_write(client, buffer, len + 2);
    if (retval < 0) {
      printk(KERN_CRIT "[%s:%d][%s:%d] firmware write i2c failure", KEYWORD,
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__);
      release_firmware(fw);
      return retval;
    }

    size -= len;
    ptr += len;
    burst_size -= len;

    if (burst_size == 0) {
      burst_size = BURST_SIZE;
      usleep_range(5000, 5500);
    }
  }

  retval = end_fw_load(client);
  if (retval < 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] end firmware load i2c failure", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__);
    return retval;
  }

  size = fw->size;
  release_firmware(fw);

  // v4l_info(client, "loaded %s firmware (%d bytes)\n", get_fw_name(client),
  // size);

  // printk(KERN_NOTICE "[%s:%d][%s:%d] mipi 400M", "ISP",
  // sensor->i2c_client->adapter->nr, _FILE_, __LINE__,get_fw_name(client),
  // size);
  printk(KERN_NOTICE "[%s:%d][%s:%d] loaded %s firmware (%d bytes)", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, get_fw_name(client),
         size);
  sensor->state.firmware = MAX9296_STATE_DONE;

  return 0;
}

MODULE_FIRMWARE(MAX9296_FIRMWARE);

//-------------------------------------------------------------------------------------
static void max9296_fw_work_handler(struct work_struct *work) {
  struct max9296_dev *sensor = container_of(work, struct max9296_dev, fw_work);

  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  if (max9296_loadfw(sensor->i2c_client) != 0)
    sensor->state.firmware = MAX9296_STATE_DONE;

  wake_up(&sensor->fw_wait);
}

//-------------------------------------------------------------------------
static int max9296_load_firmware(struct v4l2_subdev *sd) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct workqueue_struct *q;
  char str[64] = {
      0,
  };
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  DEFINE_WAIT(wait);

  INIT_WORK(&sensor->fw_work, max9296_fw_work_handler);
  init_waitqueue_head(&sensor->fw_wait);
  snprintf(str, sizeof(str), "max9296_fw_%s",
           dev_name(&sensor->i2c_client->dev));
  q = create_singlethread_workqueue(str);
  if (q) {
    prepare_to_wait(&sensor->fw_wait, &wait, TASK_UNINTERRUPTIBLE);
    queue_work(q, &sensor->fw_work);
    schedule();
    finish_wait(&sensor->fw_wait, &wait);
    destroy_workqueue(q);
  }

  return 0;
}

//-------------------------------------------------------------------------
static int max9296_set_mode(struct max9296_dev *sensor) {
  const struct max9296_mode_info *mode = sensor->current_mode;
  int ret = 0;
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s width:%d, height:%d, enable:0x%02x",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, mode->width, mode->height, sensor->enable);
  sensor->state.init = MAX9296_STATE_RUNNING;

  if (sensor->enable == 0x02) {
    if (debug)
      printk(KERN_NOTICE "[%s:%d][%s:%d] mode change : right", KEYWORD,
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    if (mode->width == 1280 && mode->height == 720) {
      sensor->current_mode = &max9296_mode_data_HD_R;
    } else if (mode->width == 1920 && mode->height == 1080) {
      sensor->current_mode = &max9296_mode_data_FHD_R;
    }
  }
  mode = sensor->current_mode;
  sensor->last_mode = mode;

  ret = max9296_load_regs(sensor, mode);

  sensor->state.init = MAX9296_STATE_DONE;

  return ret;
}

//-------------------------------------------------------------------------
static int max9296_fw_init(void *data) {
  struct max9296_dev *sensor = (struct max9296_dev *)data;
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  while (!kthread_should_stop()) {
    set_current_state(TASK_INTERRUPTIBLE);

    if (sensor->state.firmware == MAX9296_STATE_DONE) {
      schedule();
      continue;
    }

    if (sensor->state.firmware == MAX9296_STATE_IDLE)
      sensor->state.firmware = MAX9296_STATE_RUNNING;

    max9296_load_firmware(&sensor->sd);

    if (sensor->state.firmware != MAX9296_STATE_DONE)
      msleep(300);
  }

  return 0;
}

static int max9296_s_stream(struct v4l2_subdev *sd, int enable) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  int ret = 0;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (%d)", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         enable);

  mutex_lock(&sensor->lock);

  if (enable) {
    // init deserializer & serializer, load firmware

    if ((sensor->restart == 0)) {
      ret = max9296_set_mode(sensor);
      if (ret < 0) {
        printk(KERN_CRIT "[%s:%d][%s:%d] %s fail!", KEYWORD,
               sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
        // goto out;
      }

      sensor->thread_fw_init =
          kthread_run(max9296_fw_init, sensor, "max9296_fw_init");
      if (IS_ERR(sensor->thread_fw_init)) {
        printk(KERN_CRIT "[%s:%d][%s:%d] %s goto", KEYWORD,
               sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
        goto out;
      }

      while (sensor->state.firmware != MAX9296_STATE_DONE)
        msleep(100);

      sensor->state.enable = MAX9296_STATE_RUNNING;

      if ((sensor->current_mode->id != MAX9296_MODE_3840x1080) &&
          (sensor->current_mode->id != MAX9296_MODE_1920x1080)) {
        maxim_ops_i2c_write(sensor, 0x3c, 0x1184, 0x0001, 2, 2);
        usleep_range(10000, 11000);
        maxim_ops_i2c_write(sensor, 0x3c, 0x2000, 0x0500, 2, 2);
        usleep_range(10000, 11000);
        maxim_ops_i2c_write(sensor, 0x3c, 0x2002, 0x02d0, 2, 2);
        usleep_range(10000, 11000);
        maxim_ops_i2c_write(sensor, 0x3c, 0x1184, 0x0013, 2, 2);
        usleep_range(10000, 11000);
        maxim_ops_i2c_write(sensor, 0x3c, 0x1010, 0x0140, 2, 2);
        usleep_range(10000, 11000);
      }
      // maxim_ops_i2c_write(sensor, 0x3c, 0x2012, 0x0040, 2, 2); //format
      // RGB888 default YUV422 maxim_ops_i2c_write(sensor, 0x3c, 0x2012, 0x0041,
      // 2, 2, 100); //format RGB565 default YUV422

      maxim_ops_i2c_write(sensor, 0x3c, 0x1186, 0x038A, 2, 2);

      if ((sensor->current_mode->id == MAX9296_MODE_2560x720) ||
          (sensor->current_mode->id == MAX9296_MODE_3840x1080)) {
        maxim_ops_i2c_write(sensor, 0x00, 0x0471, 0x83, 2, 1);
        msleep(500);
      }

      sensor->state.enable = MAX9296_STATE_DONE;

      sensor->stream_on = 1;
    } else {
      /* Stream restart: reapply V4L2 controls */
      if (sensor->ctrl_cache.firmware_ready) {
        sensor->ctrl_cache.pending = true; /* Force re-apply */
        max9296_apply_cached_controls(sensor);
      }

      sensor->stream_on = 1;
      sensor->restart = 0;
    }
  } else {
    ret = max9296_set_stream_mipi(sensor, enable);

    sensor->restart = 1;

    sensor->state.fsync = MAX9296_STATE_IDLE;

    if (sensor->shared.sensor != NULL) {
      sensor->shared.sensor->state.fsync = MAX9296_STATE_IDLE;
    }
  }

  sensor->streaming = enable;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
out:
  mutex_unlock(&sensor->lock);
  return ret;
}

static const struct v4l2_subdev_core_ops max9296_core_ops = {
    .s_power = max9296_s_power,
    .log_status = v4l2_ctrl_subdev_log_status,
    .subscribe_event = v4l2_ctrl_subdev_subscribe_event,
    .unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops max9296_video_ops = {
    .g_frame_interval = max9296_g_frame_interval,
    .s_frame_interval = max9296_s_frame_interval,
    .s_stream = max9296_s_stream,
};

static const struct v4l2_subdev_pad_ops max9296_pad_ops = {
    .enum_mbus_code = max9296_enum_mbus_code,
    .get_fmt = max9296_get_fmt,
    .set_fmt = max9296_set_fmt,
    .enum_frame_size = max9296_enum_frame_size,
    .enum_frame_interval = max9296_enum_frame_interval,
};

static const struct v4l2_subdev_ops max9296_subdev_ops = {
    .core = &max9296_core_ops,
    .video = &max9296_video_ops,
    .pad = &max9296_pad_ops,
};

static int max9296_link_setup(struct media_entity *entity,
                              const struct media_pad *local,
                              const struct media_pad *remote, u32 flags) {
  return 0;
}

static const struct media_entity_operations max9296_sd_media_ops = {
    .link_setup = max9296_link_setup,
};

//-------------------------------------------------------------------------
static int max9296_fsync(void *data) {
  struct max9296_dev *sensor = (struct max9296_dev *)data;
  unsigned int high = 1000, low = 0;
  static unsigned int restart_cnt = 0;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  while (1) {
    set_current_state(TASK_INTERRUPTIBLE);

    if (kthread_should_stop())
      break;

    if (sensor->restart == 1) {
      restart_cnt = 1;
      msleep_interruptible(300);
      continue;
    }

    if (restart_cnt > 0) {
      --restart_cnt;
      msleep_interruptible(1000);
      continue;
    }

    // single mode
    if (sensor->shared.sensor == NULL) {
      if (sensor->state.enable == MAX9296_STATE_DONE) {
        if (sensor->state.fsync != MAX9296_STATE_RUNNING)
          msleep_interruptible(1000);

        low = (1000000 / sensor->fps) - high;

        gpiod_set_value_cansleep(sensor->fsync_gpio, 1);
        usleep_range(high, high + high / 10);
        gpiod_set_value_cansleep(sensor->fsync_gpio, 0);
        usleep_range(low, low + low / 10);
        sensor->state.fsync = MAX9296_STATE_RUNNING;
      } else {
        usleep_range(10000, 11000);
      }
    } else /* default dual mode */
    {
      /* dual mode init */
      if ((sensor->state.init == MAX9296_STATE_DONE) &&
          (sensor->shared.sensor->state.init == MAX9296_STATE_DONE)) {
        if ((sensor->state.enable == MAX9296_STATE_DONE) &&
            (sensor->shared.sensor->state.enable == MAX9296_STATE_DONE)) {
          if (sensor->state.fsync != MAX9296_STATE_RUNNING)
            msleep_interruptible(1000);

          if (sensor->shared.sensor->state.fsync != MAX9296_STATE_RUNNING)
            msleep_interruptible(1000);

          low = (1000000 / sensor->fps) - high;

          pr_notice_once("[%s:%d][%s:%d] %s fps : %d, low : %d, high : "
                         "%d\n",
                         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_,
                         __LINE__, __FUNCTION__, sensor->fps, low, high);
          gpiod_set_value_cansleep(sensor->fsync_gpio, 1);
          usleep_range(high, high + high / 10);
          gpiod_set_value_cansleep(sensor->fsync_gpio, 0);
          usleep_range(low, low + low / 10);
          sensor->shared.sensor->state.fsync = sensor->state.fsync =
              MAX9296_STATE_RUNNING;
        } else {
          usleep_range(10000, 11000);
        }
      }
      /* single mode init */
      else {
        int start = 0, fps = 30;
        unsigned int *fsync_state = NULL;
        if ((sensor->state.init == MAX9296_STATE_DONE) &&
            (sensor->state.enable == MAX9296_STATE_DONE) &&
            (sensor->shared.sensor->state.setup != MAX9296_STATE_DONE)) {
          fsync_state = &sensor->state.fsync;
          fps = sensor->fps;
          start = 1;
        } else if ((sensor->shared.sensor->state.init == MAX9296_STATE_DONE) &&
                   (sensor->shared.sensor->state.enable ==
                    MAX9296_STATE_DONE) &&
                   (sensor->state.setup != MAX9296_STATE_DONE)) {
          fsync_state = &sensor->shared.sensor->state.fsync;
          fps = sensor->shared.sensor->fps;
          start = 1;
        } else {
          usleep_range(10000, 11000);
        }

        if (start) {
          if (*fsync_state != MAX9296_STATE_RUNNING)
            msleep_interruptible(1000);

          low = (1000000 / fps) - high;

          pr_notice_once("[%s:%d][%s:%d] %s fps : %d, low : %d, "
                         "high : %d\n",
                         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_,
                         __LINE__, __FUNCTION__, fps, low, high);
          gpiod_set_value_cansleep(sensor->fsync_gpio, 1);
          usleep_range(high, high + high / 10);
          gpiod_set_value_cansleep(sensor->fsync_gpio, 0);
          usleep_range(low, low + low / 10);
          *fsync_state = MAX9296_STATE_RUNNING;
        }
      }
    }
  }
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;
}

//-------------------------------------------------------------------------
static int max9296_enable(void *data) {
  struct max9296_dev *sensor = (struct max9296_dev *)data;

  // pr_emerg("\x1b[34m%s() %d line in %s file : \x1b[0m --- %s\n",
  // __FUNCTION__, __LINE__, __FILE__, dev_name(&sensor->i2c_client->dev));
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  while (1) {
    set_current_state(TASK_INTERRUPTIBLE);

    if (kthread_should_stop())
      break;

    if ((sensor->stream_on == 1) &&
        (sensor->state.fsync == MAX9296_STATE_RUNNING)) {
      // pr_emerg("\x1b[34m%s() %d line in %s file : \x1b[0m --- %s\n",
      // __FUNCTION__, __LINE__, __FILE__, dev_name(&sensor->i2c_client->dev));
      msleep_interruptible(300);
      if (sensor->shared.sensor != NULL) {
        if (sensor->shared.sensor->state.setup == MAX9296_STATE_DONE) {
          if (sensor->fsync_gpio != NULL) {
            if (sensor->current_mode->id == MAX9296_MODE_1280x720) {
              maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x02, 2, 1);
              // usleep_range(10000, 11000);
              maxim_ops_i2c_write(sensor->shared.sensor, 0x00, 0x0313, 0x02, 2,
                                  1);
              usleep_range(10000, 11000);
            } else {
              maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x82, 2, 1);
              // usleep_range(10000, 11000);
              maxim_ops_i2c_write(sensor->shared.sensor, 0x00, 0x0313, 0x82, 2,
                                  1);
              usleep_range(10000, 11000);
            }

            // ae - per-channel (init to manual first)
            max9296_write_per_channel(sensor, AP1302_REG_AE_CTRL,
                                      AP1302_AE_CTRL_MANUAL, 2, 2);
            max9296_write_per_channel(sensor->shared.sensor, AP1302_REG_AE_CTRL,
                                      AP1302_AE_CTRL_MANUAL, 2, 2);
            msleep_interruptible(100);

            // awb - per-channel
            max9296_write_per_channel(sensor, AP1302_REG_AWB_CTRL,
                                      AP1302_AWB_CTRL_AUTO, 2, 2);
            max9296_write_per_channel(sensor->shared.sensor,
                                      AP1302_REG_AWB_CTRL, AP1302_AWB_CTRL_AUTO,
                                      2, 2);
            msleep_interruptible(100);

            // lsc - per-channel
            max9296_write_per_channel(sensor, AP1302_REG_LSC_CTRL, 0x3fff, 2,
                                      2);
            max9296_write_per_channel(sensor->shared.sensor,
                                      AP1302_REG_LSC_CTRL, 0x3fff, 2, 2);
          }
        } else {
          if (sensor->current_mode->id == MAX9296_MODE_1280x720) {
            maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x02, 2, 1);
            usleep_range(10000, 11000);
          } else {
            maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x82, 2, 1);
            usleep_range(10000, 11000);
          }

          // ae - per-channel (init to manual first, then auto)
          max9296_write_per_channel(sensor, AP1302_REG_AE_CTRL,
                                    AP1302_AE_CTRL_MANUAL, 2, 2);
          msleep_interruptible(100);
          max9296_write_per_channel(sensor, AP1302_REG_AE_CTRL,
                                    AP1302_AE_CTRL_AUTO, 2, 2);
          msleep_interruptible(100);

          // awb - per-channel
          max9296_write_per_channel(sensor, AP1302_REG_AWB_CTRL,
                                    AP1302_AWB_CTRL_AUTO, 2, 2);
          msleep_interruptible(100);

          // lsc - per-channel
          max9296_write_per_channel(sensor, AP1302_REG_LSC_CTRL, 0x3fff, 2, 2);
        }
      } else {
        if (sensor->current_mode->id == MAX9296_MODE_1280x720) {
          maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x02, 2, 1);
          usleep_range(10000, 11000);
        } else {
          maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x82, 2, 1);
          usleep_range(10000, 11000);
        }

        // ae - per-channel (init to manual first, then auto)
        max9296_write_per_channel(sensor, AP1302_REG_AE_CTRL,
                                  AP1302_AE_CTRL_MANUAL, 2, 2);
        msleep_interruptible(100);
        max9296_write_per_channel(sensor, AP1302_REG_AE_CTRL,
                                  AP1302_AE_CTRL_AUTO, 2, 2);
        msleep_interruptible(100);

        // awb - per-channel
        max9296_write_per_channel(sensor, AP1302_REG_AWB_CTRL,
                                  AP1302_AWB_CTRL_AUTO, 2, 2);
        msleep_interruptible(100);

        // lsc - per-channel
        max9296_write_per_channel(sensor, AP1302_REG_LSC_CTRL, 0x3fff, 2, 2);
      }
      sensor->stream_on = 0;

      /* Override hardcoded AE/AWB init with V4L2 cached controls */
      mutex_lock(&sensor->lock);
      max9296_apply_cached_controls(sensor);
      mutex_unlock(&sensor->lock);

      usleep_range(10000, 11000);
      // pr_emerg("\x1b[34m%s() %d line in %s file : \x1b[0m --- %s\n",
      // __FUNCTION__, __LINE__, __FILE__, dev_name(&sensor->i2c_client->dev));
    } else
      usleep_range(10000, 11000);
  }
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;
}

//-------------------------------------------------------------------------
static int max9296_shared_init(void *data) {
  struct max9296_dev *sensor = (struct max9296_dev *)data;

  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  while (1) {
    set_current_state(TASK_INTERRUPTIBLE);

    if (kthread_should_stop())
      break;

    if (sensor->shared.sensor == NULL) {
      if (sensor->shared.client == NULL) {
        sensor->shared.client = of_find_i2c_device_by_node(sensor->shared.np);
        if (sensor->shared.client == NULL) {
          // dev_warn(&sensor->i2c_client->dev, "warning not found i2c_client
          // from handle.. this device works in single mode..\n");
          usleep_range(10000, 11000);
        } else {
          sensor->shared.sd = i2c_get_clientdata(sensor->shared.client);
          if (sensor->shared.sd == NULL)
            usleep_range(10000, 11000);
          else {
            sensor->shared.sensor = to_max9296_dev(sensor->shared.sd);
            if (sensor->shared.sensor == NULL)
              usleep_range(10000, 11000);
          }
        }
      } else {
        sensor->shared.sd = i2c_get_clientdata(sensor->shared.client);
        if (sensor->shared.sd == NULL)
          usleep_range(10000, 11000);
        else {
          sensor->shared.sensor = to_max9296_dev(sensor->shared.sd);
          if (sensor->shared.sensor == NULL)
            usleep_range(10000, 11000);
        }
      }
    } else
      break;
  }
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;
}

//-------------------------------------------------------------------------
static ssize_t sysfs_rotate_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] sensor rotate : 0x%x", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, sensor->rotate);
  return snprintf(buf, PAGE_SIZE, "%u\n", sensor->rotate);
}

static ssize_t sysfs_rotate_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);
  unsigned long val;
  int ret;

  ret = kstrtoul(buf, 10, &val);
  if (ret)
    return ret;

  sensor->rotate = val;

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] sensor rotate : 0x%x", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, sensor->rotate);

  return count;
}
static DEVICE_ATTR(rotate, 0664, sysfs_rotate_show, sysfs_rotate_store);
//-------------------------------------------------------------------------
static ssize_t sysfs_enable_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] sensor enable : 0x%x", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, sensor->enable);
  return snprintf(buf, PAGE_SIZE, "%u\n", sensor->enable);
}

static ssize_t sysfs_enable_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);
  unsigned long val;
  int ret;

  ret = kstrtoul(buf, 10, &val);
  if (ret)
    return ret;

  sensor->enable = val;

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] sensor enable : 0x%x", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, sensor->enable);

  return count;
}
static DEVICE_ATTR(enable, 0664, sysfs_enable_show, sysfs_enable_store);
//-------------------------------------------------------------------------
static int max9296_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct fwnode_handle *endpoint;
  struct max9296_dev *sensor;
  struct v4l2_mbus_framefmt *fmt;
  char str[128] = {
      0,
  };
  int ret;

  printk(KERN_ALERT "[%s:%d][%s:%d] max9296 version : %s", KEYWORD,
         client->adapter->nr, _FILE_, __LINE__, SW_VERSION);

  sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
  if (!sensor)
    return -ENOMEM;

  sensor->i2c_client = client;
  sensor->state.init = MAX9296_STATE_IDLE;
  sensor->state.firmware = MAX9296_STATE_IDLE;
  sensor->state.enable = MAX9296_STATE_IDLE;
  sensor->state.fsync = MAX9296_STATE_IDLE;
  sensor->state.setup = MAX9296_STATE_IDLE;
  sensor->state.power = MAX9296_STATE_IDLE;
  sensor->stream_on = 0;
  sensor->ctrl_cache.firmware_ready = false;
  sensor->ctrl_cache.pending = false;
  /* Per-channel cache defaults are set after max9296_init_controls() below */

  sensor->fps = DEFAULT_FRAMERATE_FPS;

  /*
   * default init sequence initialize sensor to
   * YUV422 UYVY 2560x720@30fps
   */
  fmt = &sensor->fmt;
  fmt->code = MEDIA_BUS_FMT_UYVY8_2X8; // 1X16;
  fmt->colorspace = V4L2_COLORSPACE_SRGB;
  fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
  fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
  fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
  fmt->width = DEFAULT_RESOLUTION_WIDTH;
  fmt->height = DEFAULT_RESOLUTION_HEIGHT;
  fmt->field = V4L2_FIELD_NONE;

  sensor->frame_interval.numerator = 1;
  sensor->frame_interval.denominator = sensor->fps;
  sensor->current_mode = &max9296_mode_data[MAX9296_MODE_2560x720];
  sensor->last_mode = sensor->current_mode;

  endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
  if (!endpoint) {
    // dev_err(dev, "endpoint node not found\n");
    printk(KERN_CRIT "[%s:%d][%s:%d] endpoint node not found", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    return -EINVAL;
  }

  ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
  fwnode_handle_put(endpoint);
  if (ret) {
    // dev_err(dev, "Could not parse endpoint\n");
    printk(KERN_CRIT "[%s:%d][%s:%d] Could not parse endpoint(%d)", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, ret);
    return ret;
  }

  if (sensor->ep.bus_type != V4L2_MBUS_PARALLEL &&
      sensor->ep.bus_type != V4L2_MBUS_CSI2_DPHY &&
      sensor->ep.bus_type != V4L2_MBUS_BT656) {
    // dev_err(dev, "Unsupported bus type %d\n", sensor->ep.bus_type);
    printk(KERN_CRIT "[%s:%d][%s:%d] Unsupported bus type %d", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, sensor->ep.bus_type);
    return -EINVAL;
  }

  /* request optional power down pin */
  sensor->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown", GPIOD_OUT_HIGH);
  if (IS_ERR(sensor->pwdn_gpio)) {
    printk(KERN_CRIT "[%s:%d][%s:%d] pwdn gpio error", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    return PTR_ERR(sensor->pwdn_gpio);
  }

  /* request optional reset pin */
  sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
  if (IS_ERR(sensor->reset_gpio)) {
    printk(KERN_CRIT "[%s:%d][%s:%d] reset gpio error", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    return PTR_ERR(sensor->reset_gpio);
  }
  if (!sensor->reset_gpio) {
    dev_info(dev, "warning reset gpio...\n");
  }

  sensor->fsync_gpio = devm_gpiod_get_optional(dev, "fsync", GPIOD_OUT_LOW);
  if (IS_ERR(sensor->fsync_gpio)) {
    printk(KERN_CRIT "[%s:%d][%s:%d] fsync gpio error", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    return PTR_ERR(sensor->fsync_gpio);
  }
  if (!sensor->fsync_gpio) {
    dev_info(dev, "warning fsync gpio...\n");
  }

  /* check fsync shared handle */
  if (of_property_read_bool(dev->of_node, "fsync,shared")) {
    sensor->shared.fsync = 1;

    sensor->shared.np =
        of_parse_phandle(dev->of_node, "fsync-shared-handle", 0);
    if (sensor->shared.np == NULL) {
      sensor->shared.fsync = 0;
      printk(KERN_CRIT "[%s:%d][%s:%d] warning not found fsync shared "
                       "handle.. this device works in single mode..",
             KEYWORD, client->adapter->nr, _FILE_, __LINE__);
    } else {
      printk(KERN_NOTICE "[%s:%d][%s:%d] shared Init", KEYWORD,
             client->adapter->nr, _FILE_, __LINE__);
      sensor->shared.thread_shared_init =
          kthread_run(max9296_shared_init, sensor, "max9296_shared_init");
      if (IS_ERR(sensor->shared.thread_shared_init)) {
        printk(KERN_CRIT
               "[%s:%d][%s:%d] sensor->shared.thread_shared_init error",
               KEYWORD, client->adapter->nr, _FILE_, __LINE__);
        goto free_ctrls;
      }
      get_task_struct(sensor->shared.thread_shared_init);
    }
  }

  // max9296_set_power(sensor, 1);
  // sensor->thread_fw_init = kthread_run(max9296_fw_init, sensor,
  // "max9296_fw_init");

  v4l2_i2c_subdev_init(&sensor->sd, client, &max9296_subdev_ops);

  sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
  sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
  sensor->sd.entity.ops = &max9296_sd_media_ops;
  sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
  ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
  if (ret) {
    printk(KERN_CRIT "[%s:%d][%s:%d] media_entity_pads_init error(%d)", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, ret);
    return ret;
  }

  mutex_init(&sensor->lock);

  ret = max9296_init_controls(sensor);
  if (ret) {
    printk(KERN_CRIT "[%s:%d][%s:%d] max9296_init_controls error(%d)", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, ret);
    goto entity_cleanup;
  } else
    ret = v4l2_ctrl_handler_setup(&sensor->ctrls.handler);

  /* Initialize cache with V4L2 control default values for both channels */
  sensor->ctrl_cache.ch0.ae_on = sensor->ctrls.auto_exp_ch0
                                     ? (sensor->ctrls.auto_exp_ch0->val ? 1 : 0)
                                     : 1;
  sensor->ctrl_cache.ch0.awb =
      sensor->ctrls.auto_wb_ch0 ? sensor->ctrls.auto_wb_ch0->val : 1;
  sensor->ctrl_cache.ch0.gain_auto =
      sensor->ctrls.auto_gain_ch0 ? sensor->ctrls.auto_gain_ch0->val : 1;
  sensor->ctrl_cache.ch0.gain =
      sensor->ctrls.gain_ch0 ? sensor->ctrls.gain_ch0->val : 256;
  sensor->ctrl_cache.ch0.exposure =
      sensor->ctrls.exposure_ch0 ? sensor->ctrls.exposure_ch0->val : 10000;
  sensor->ctrl_cache.ch0.hflip =
      sensor->ctrls.hflip_ch0 ? sensor->ctrls.hflip_ch0->val : 0;
  sensor->ctrl_cache.ch0.vflip =
      sensor->ctrls.vflip_ch0 ? sensor->ctrls.vflip_ch0->val : 0;
  sensor->ctrl_cache.ch0.lsc =
      sensor->ctrls.lsc_ch0 ? sensor->ctrls.lsc_ch0->val : 0x3fff;
  sensor->ctrl_cache.ch0.brightness =
      sensor->ctrls.brightness_ch0 ? sensor->ctrls.brightness_ch0->val : 0;
  sensor->ctrl_cache.ch0.contrast =
      sensor->ctrls.contrast_ch0 ? sensor->ctrls.contrast_ch0->val : 0;
  sensor->ctrl_cache.ch0.saturation =
      sensor->ctrls.saturation_ch0 ? sensor->ctrls.saturation_ch0->val : 4096;

  sensor->ctrl_cache.ch1.ae_on = sensor->ctrls.auto_exp_ch1
                                     ? (sensor->ctrls.auto_exp_ch1->val ? 1 : 0)
                                     : 1;
  sensor->ctrl_cache.ch1.awb =
      sensor->ctrls.auto_wb_ch1 ? sensor->ctrls.auto_wb_ch1->val : 1;
  sensor->ctrl_cache.ch1.gain_auto =
      sensor->ctrls.auto_gain_ch1 ? sensor->ctrls.auto_gain_ch1->val : 1;
  sensor->ctrl_cache.ch1.gain =
      sensor->ctrls.gain_ch1 ? sensor->ctrls.gain_ch1->val : 256;
  sensor->ctrl_cache.ch1.exposure =
      sensor->ctrls.exposure_ch1 ? sensor->ctrls.exposure_ch1->val : 10000;
  sensor->ctrl_cache.ch1.hflip =
      sensor->ctrls.hflip_ch1 ? sensor->ctrls.hflip_ch1->val : 0;
  sensor->ctrl_cache.ch1.vflip =
      sensor->ctrls.vflip_ch1 ? sensor->ctrls.vflip_ch1->val : 0;
  sensor->ctrl_cache.ch1.lsc =
      sensor->ctrls.lsc_ch1 ? sensor->ctrls.lsc_ch1->val : 0x3fff;
  sensor->ctrl_cache.ch1.brightness =
      sensor->ctrls.brightness_ch1 ? sensor->ctrls.brightness_ch1->val : 0;
  sensor->ctrl_cache.ch1.contrast =
      sensor->ctrls.contrast_ch1 ? sensor->ctrls.contrast_ch1->val : 0;
  sensor->ctrl_cache.ch1.saturation =
      sensor->ctrls.saturation_ch1 ? sensor->ctrls.saturation_ch1->val : 4096;

  sensor->ctrl_cache.exposure =
      sensor->ctrls.exp_time ? sensor->ctrls.exp_time->val : 10000;
  sensor->ctrl_cache.pending = true; /* Mark as having cached values */

  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s cache initialized: ch0(ae:%d awb:%d "
                     "gain_auto:%d gain:%d hflip:%d vflip:%d) ch1(ae:%d awb:%d "
                     "gain_auto:%d gain:%d hflip:%d vflip:%d) exp:%d",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, sensor->ctrl_cache.ch0.ae_on,
           sensor->ctrl_cache.ch0.awb, sensor->ctrl_cache.ch0.gain_auto,
           sensor->ctrl_cache.ch0.gain, sensor->ctrl_cache.ch0.hflip,
           sensor->ctrl_cache.ch0.vflip, sensor->ctrl_cache.ch1.ae_on,
           sensor->ctrl_cache.ch1.awb, sensor->ctrl_cache.ch1.gain_auto,
           sensor->ctrl_cache.ch1.gain, sensor->ctrl_cache.ch1.hflip,
           sensor->ctrl_cache.ch1.vflip, sensor->ctrl_cache.exposure);

  ret = v4l2_async_register_subdev_sensor_common(&sensor->sd);
  if (ret) {
    printk(KERN_CRIT
           "[%s:%d][%s:%d] v4l2_async_register_subdev_sensor_common error(%d)",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__, ret);
    goto free_ctrls;
  }

  if (sensor->fsync_gpio != NULL) {
    sensor->thread_fsync = kthread_run(max9296_fsync, sensor, "max9296_fsync");
    if (IS_ERR(sensor->thread_fsync)) {
      printk(KERN_CRIT "[%s:%d][%s:%d] sensor thread fsync error", KEYWORD,
             client->adapter->nr, _FILE_, __LINE__);
      goto free_ctrls;
    }
  }

  snprintf(str, sizeof(str), "max9296_enable_%s",
           dev_name(&sensor->i2c_client->dev));
  sensor->thread_en = kthread_run(max9296_enable, sensor, str);
  if (IS_ERR(sensor->thread_en)) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sensor thread enable error", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    goto free_ctrls;
  }
  if (device_create_file(&client->dev, &dev_attr_rotate) != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sysfs rotate entry failed", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    ret = (-EINVAL);
    goto free_ctrls;
  }
  if (device_create_file(&client->dev, &dev_attr_enable) != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sysfs enable entry failed", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    ret = (-EINVAL);
    goto free_ctrls;
  }

  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;

free_ctrls:
  /* Stop threads that were already started */
  if (sensor->thread_en && !IS_ERR(sensor->thread_en))
    kthread_stop(sensor->thread_en);
  if (sensor->thread_fsync && !IS_ERR(sensor->thread_fsync))
    kthread_stop(sensor->thread_fsync);
  if (sensor->shared.thread_shared_init &&
      !IS_ERR(sensor->shared.thread_shared_init)) {
    kthread_stop(sensor->shared.thread_shared_init);
    put_task_struct(sensor->shared.thread_shared_init);
  }
  sensor->shared.thread_shared_init = NULL;

  v4l2_ctrl_handler_free(&sensor->ctrls.handler);
entity_cleanup:
  media_entity_cleanup(&sensor->sd.entity);
  mutex_destroy(&sensor->lock);
  return ret;
}

static int max9296_remove(struct i2c_client *client) {
  struct v4l2_subdev *sd = i2c_get_clientdata(client);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct max9296_dev *peer = sensor->shared.sensor;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  /* Phase 1: Stop peer threads that reference our memory */
  if (peer && peer->shared.sensor == sensor) {
    WRITE_ONCE(peer->shared.sensor, NULL);
    if (peer->thread_fsync && !IS_ERR(peer->thread_fsync)) {
      kthread_stop(peer->thread_fsync);
      peer->thread_fsync = NULL;
    }
    if (peer->thread_en && !IS_ERR(peer->thread_en)) {
      kthread_stop(peer->thread_en);
      peer->thread_en = NULL;
    }
  }

  /* Phase 2: Stop our own threads */
  if (sensor->thread_en && !IS_ERR(sensor->thread_en)) {
    kthread_stop(sensor->thread_en);
    sensor->thread_en = NULL;
  }
  if (sensor->thread_fsync && !IS_ERR(sensor->thread_fsync)) {
    kthread_stop(sensor->thread_fsync);
    sensor->thread_fsync = NULL;
  }
  if (sensor->thread_fw_init && !IS_ERR(sensor->thread_fw_init)) {
    kthread_stop(sensor->thread_fw_init);
    sensor->thread_fw_init = NULL;
  }
  if (sensor->shared.thread_shared_init &&
      !IS_ERR(sensor->shared.thread_shared_init)) {
    kthread_stop(sensor->shared.thread_shared_init);
    put_task_struct(sensor->shared.thread_shared_init);
  }
  sensor->shared.thread_shared_init = NULL;

  /* Phase 3: Clean up shared references */
  sensor->shared.sensor = NULL;
  if (sensor->shared.client) {
    put_device(&sensor->shared.client->dev);
    sensor->shared.client = NULL;
  }

  /* Phase 4: V4L2/media cleanup */
  device_remove_file(&client->dev, &dev_attr_rotate);
  device_remove_file(&client->dev, &dev_attr_enable);
  v4l2_async_unregister_subdev(&sensor->sd);
  media_entity_cleanup(&sensor->sd.entity);
  v4l2_ctrl_handler_free(&sensor->ctrls.handler);
  mutex_destroy(&sensor->lock);

  return 0;
}

static const struct i2c_device_id max9296_id[] = {
    {"max9296", 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, max9296_id);

static const struct of_device_id max9296_dt_ids[] = {
    {.compatible = "maxim,max9296"}, {/* sentinel */}};
MODULE_DEVICE_TABLE(of, max9296_dt_ids);

static struct i2c_driver max9296_i2c_driver = {
    .driver =
        {
            .name = "max9296",
            .of_match_table = max9296_dt_ids,
        },
    .id_table = max9296_id,
    .probe_new = max9296_probe,
    .remove = max9296_remove,
};

module_i2c_driver(max9296_i2c_driver);

module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable debug messages (default: 1)");

MODULE_DESCRIPTION("MAX9296 MIPI Camera Subdev Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(SW_VERSION);
