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
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "max9296_360p_policy.h"

#define SW_VERSION "2.9"
#define SERDES_3GBPS
#define SERDES_STPx
#define _FILE_                                                                 \
  (strrchr(__FILE__, '/') ? (strrchr(__FILE__, '/') + 1) : (__FILE__))
#define KEYWORD "I2C"

static int debug;

#define DEFAULT_FRAMERATE_FPS (30)
#define DEFAULT_RESOLUTION_WIDTH (2560)
#define DEFAULT_RESOLUTION_HEIGHT (720)
#define MAX9296_DEFAULT_MAX_FPS 30
#define MAX9296_360P_MAX_FPS 120
#define MAX9296_EXPOSURE_SAFE_MAX_FPS 30

#define MAX9296_REG_CHIP_ID 0x000d
#define MAX9296_CHIP_ID 0x96
#define MAX9296_REG_CTRL3 0x0013
#define MAX9296_REG_RX3 0x002f
#define MAX9295_REG_CHIP_ID 0x000d
#define MAX9295_CHIP_ID 0x91
#define AP1302_REG_FRAME_CNT 0x0002

/* RX3 link-level bits. FAILLOCK is read-clear and is deliberately excluded. */
#define MAX9296_RX3_LINK_A_UP 0x06
#define MAX9296_RX3_LINK_B_UP 0x60

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
#define AP1302_REG_ATOMIC 0x1184
#define AP1302_ATOMIC_BEGIN 0x0001
#define AP1302_ATOMIC_FINISH 0x0013
#define AP1302_REG_PREVIEW_WIDTH 0x2000
#define AP1302_REG_PREVIEW_HEIGHT 0x2002
#define AP1302_REG_PREVIEW_ROI_X0 0x2004
#define AP1302_REG_PREVIEW_ROI_Y0 0x2006
#define AP1302_REG_PREVIEW_ROI_X1 0x2008
#define AP1302_REG_PREVIEW_ROI_Y1 0x200a
#define AP1302_REG_PREVIEW_ASPECT 0x200c
#define AP1302_REG_PREVIEW_SENSOR_MODE 0x2014
#define AP1302_REG_PREVIEW_LINE_TIME 0x201c
#define AP1302_REG_PREVIEW_MAX_FPS 0x2020
#define AP1302_REG_TRIGGER_MAX_MISMATCH 0x6112
#define AP1302_REG_DZ_TGT_FCT 0x1010
#define AP1302_REG_DZ_STEP_FCT 0x1012
#define AP1302_REG_DZ_CENTER_X 0x118c
#define AP1302_REG_DZ_CENTER_Y 0x118e

#define AP1302_DZ_STEP_IMMEDIATE 0x8000
#define MAX9296_DZ_MIN 100
#define MAX9296_DZ_MAX 300
#define MAX9296_DZ_DEFAULT 100
#define MAX9296_DZ_CENTER_DEFAULT 0x8000

/* AR0234CS sensor register (accessed via AP1302 DMA) */
#define AR0234_REG_LED_FLASH_CONTROL 0x3270
#define AR0234_I2C_ADDR 0x10

/* AP1302 DMA-based sensor access registers (Basic address space) */
#define AP1302_REG_DMA_SRC 0x60a0
#define AP1302_REG_DMA_DST 0x60a4
#define AP1302_REG_DMA_SIZE 0x60a8
#define AP1302_REG_DMA_CTRL 0x60ac
#define AP1302_REG_SENSOR_SIP 0x604a

/* Image tuning (fixed-point) */
#define AP1302_REG_BRIGHTNESS 0x7000
#define AP1302_REG_CONTRAST 0x7002
#define AP1302_REG_SATURATION 0x7006

#define AP1302_AE_CTRL_AUTO 0x0299
#define AP1302_AE_CTRL_MANUAL 0x0290

/*
 * AWB_CTRL (0x5100) layout:
 *   [12]=POSTGAIN, [10:8]=undocumented, [7:6]=FACE, [4]=IMM1, [3:0]=MODE
 * AP1302_AWB_CTRL_BASE keeps the tuning flags this driver has always
 * used (POSTGAIN=1, bit8 = firmware-internal flag carried over from the
 * original 0x115f AUTO constant, FACE=ignore, IMM1=1). The caller ORs
 * the desired MODE value (0x0~0xf) into the lower nibble.
 * BASE | AUTO(0xf) == 0x115f preserves byte-for-byte compatibility with
 * the pre-change AP1302_AWB_CTRL_AUTO value.
 */
#define AP1302_AWB_CTRL_BASE    0x1150
#define AP1302_AWB_MODE_OFF     0x0  /* manual via AWB_MANUAL_QX/QY */
#define AP1302_AWB_MODE_HORIZON 0x1
#define AP1302_AWB_MODE_A       0x2
#define AP1302_AWB_MODE_CWF     0x3
#define AP1302_AWB_MODE_D50     0x4
#define AP1302_AWB_MODE_D65     0x5
#define AP1302_AWB_MODE_D75     0x6
#define AP1302_AWB_MODE_TEMP    0x7  /* user AWB_MANUAL_TEMP */
#define AP1302_AWB_MODE_MEASURE 0x8  /* one-shot */
#define AP1302_AWB_MODE_AUTO    0xf
#define AP1302_AWB_MODE_MASK    0xf
#define AP1302_AWB_CTRL_AUTO    (AP1302_AWB_CTRL_BASE | AP1302_AWB_MODE_AUTO)
#define AP1302_AWB_CTRL_FROM_MODE(m) \
  ((u16)(AP1302_AWB_CTRL_BASE | ((m) & AP1302_AWB_MODE_MASK)))

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

/* LED Flash control per-channel (AR0234CS R0x3270 via AP1302 DMA) */
#define V4L2_CID_LED_FLASH_CH0 (V4L2_CID_USER_BASE + 0x1018)
#define V4L2_CID_LED_FLASH_CH1 (V4L2_CID_USER_BASE + 0x1019)

/* MCP4018 digital potentiometer wiper control (7-bit, 0x00~0x7F) */
#define V4L2_CID_MCP4018_WIPER (V4L2_CID_USER_BASE + 0x101A)
#define V4L2_CID_MCP4018_WIPER_CH1 (V4L2_CID_USER_BASE + 0x101B)

/* Generic AR0234 sensor register access via AP1302 DMA
 * Value format (32-bit): [31:16] = register address, [15:0] = data
 * Write: set ctrl value → DMA write to AR0234
 * Read:  set ctrl value with reg addr in [31:16] → get ctrl returns data in [15:0]
 */
#define V4L2_CID_DMA_REG_WRITE_CH0 (V4L2_CID_USER_BASE + 0x101C)
#define V4L2_CID_DMA_REG_WRITE_CH1 (V4L2_CID_USER_BASE + 0x101D)
#define V4L2_CID_DMA_REG_READ_CH0 (V4L2_CID_USER_BASE + 0x101E)
#define V4L2_CID_DMA_REG_READ_CH1 (V4L2_CID_USER_BASE + 0x101F)

/* MCP4018 VCC power control via MAX9295 MFP4 GPIO (bool) */
#define V4L2_CID_MCP4018_POWER_CH0 (V4L2_CID_USER_BASE + 0x1020)
#define V4L2_CID_MCP4018_POWER_CH1 (V4L2_CID_USER_BASE + 0x1021)

/* AP1302 digital zoom: one common percent plus normalized center coordinates.
 *
 * A dual MAX9296 output concatenates both AP1302 streams. Different zoom
 * factors change their sensor readout heights and corrupt that combined frame,
 * so only the shared factor is exposed. Center coordinates remain independent
 * per channel and retain their original control IDs. */
#define V4L2_CID_DZ (V4L2_CID_USER_BASE + 0x1022)
#define V4L2_CID_DZ_X (V4L2_CID_USER_BASE + 0x1023)
#define V4L2_CID_DZ_Y (V4L2_CID_USER_BASE + 0x1024)
#define V4L2_CID_DZ_X_CH0 (V4L2_CID_USER_BASE + 0x1027)
#define V4L2_CID_DZ_X_CH1 (V4L2_CID_USER_BASE + 0x1028)
#define V4L2_CID_DZ_Y_CH0 (V4L2_CID_USER_BASE + 0x1029)
#define V4L2_CID_DZ_Y_CH1 (V4L2_CID_USER_BASE + 0x102a)
#define V4L2_CID_CROP_ENABLE (V4L2_CID_USER_BASE + 0x102b)

/*
 * MAX9295 serializer addresses (matches mcp4018_ctrl.sh channel table).
 *
 * DUAL ONLY. 0x40 is the serializer's power-on default; the 2ch init table
 * remaps one of them to 0x60 because both would otherwise collide, and that
 * remap is the sole reason 0x60 exists. In dual mode the mapping is
 * ch0 = 0x40 (PHY A) and ch1 = 0x60 (PHY B), bus1 likewise ch2/ch3.
 *
 * In SINGLE-channel mode there is no remap, so the one serializer answers at
 * 0x40 whichever channel it is - use max9296_ser_addr(), never these names.
 * The "PHY A/B" half does not carry over either: which PHY a single-channel
 * unit comes up on is a wiring property, not a channel number. Measured on a
 * ch1 unit: CTRL3(0x13) = 0xda -> LINK_MODE 01 = PHY A.
 */
#define MAX9295_SER_ADDR_CH0   0x40  /* dual: ch0 (bus2) / ch2 (bus1) */
#define MAX9295_SER_ADDR_CH1   0x60  /* dual: ch1 (bus2) / ch3 (bus1) */
#define MAX9295_REG_MFP4_CTRL  0x02ca
#define MAX9295_MFP4_POWER_ON  0x90
#define MAX9295_MFP4_POWER_OFF 0x80

/*
 * MCP4018T-503E: 7-bit single I2C digital potentiometer (50kΩ, 128 steps)
 * Connected to MAX9295 main I2C bus (I2C0, shared with AP1302 ISP)
 * MAX9295 MFP4 GPIO is the MCP4018 I2C-bus gate (HIGH=connected, LOW=isolated);
 * the wiper value is retained by the pot after the I2C gate closes.
 *
 * Port A and Port B MCP4018 share host-visible addr 0x2F without remap, so
 * userspace must enforce mutual exclusion during wiper write: open one
 * channel's MFP4 → write wiper → close it → next channel. See gstApp
 * apply_led_flash_v4l2() for the transient-gate pattern.
 *
 * I2C protocol (no register address):
 *   Write: [START][0x5E][wiper_value(0x00~0x7F)][STOP]
 *   Read:  [START][0x5F][data][STOP]
 * Power-on default: 0x3F (mid-scale, ~25kΩ)
 */
#define MCP4018_I2C_ADDR 0x2F
/* Host-visible address for Port B (same as real addr) */
#define MCP4018_HOST_ADDR MCP4018_I2C_ADDR
/* Host-visible address for Port A (dual mode) */
#define MCP4018_HOST_ADDR_CH1 MCP4018_I2C_ADDR
/*
 * Dual mode address remap: Port A MCP4018을 0x2E로 리맵하여
 * Port B(0x2F)와 호스트 주소 충돌 방지.
 * 듀얼 동시 사용 시 아래로 교체 필요:
 * #define MCP4018_HOST_ADDR_CH1 0x2E
 */
#define MCP4018_WIPER_MAX 0x7F
#define MCP4018_WIPER_DEFAULT 0x3F

/*
 * MAX9295 address translator B registers (unused pair, A is for AP1302)
 * SRC_B (0x0044): host-visible address (bits [7:1] = 7-bit addr << 1)
 * DST_B (0x0045): real device address on main I2C bus (I2C0)
 */
#define MAX9295_REG_I2C_SRC_B 0x0044
#define MAX9295_REG_I2C_DST_B 0x0045

enum max9296_mode_id {
  MAX9296_MODE_2560x720 = 0,
  MAX9296_MODE_1280x720,
  MAX9296_MODE_3840x1080,
  MAX9296_MODE_1920x1080,
  MAX9296_MODE_1280x360,
  MAX9296_MODE_640x360,
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
  MAX9296_STATE_FAILED,
  MAX9296_STATE_MAX,
};

enum max9296_prepare_request_state {
  MAX9296_PREP_IDLE = 0,
  MAX9296_PREP_PREPARING,
  MAX9296_PREP_READY,
  MAX9296_PREP_STALE,
  MAX9296_PREP_CONSUMED,
  MAX9296_PREP_FAILED,
  MAX9296_PREP_EXPIRED,
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
  u32 exposure_safe_max_fps;
};

/*
 * Normalized identity of the hardware programming performed before STREAMON.
 *
 * mode is deliberately part of the identity rather than only mode->id: the
 * left and right single-channel tables have the same mode id and dimensions,
 * but program different physical inputs.  Request generation belongs to the
 * userspace orchestration state and is therefore not part of this hardware
 * fingerprint.
 */
struct max9296_hw_fingerprint {
  const struct max9296_mode_info *mode;
  u32 width;
  u32 height;
  u32 code;
  u32 fps;
  u32 enable;
  bool crop_enable;
};

struct max9296_ctrls {
  struct v4l2_ctrl_handler handler;
  struct v4l2_ctrl *pixel_rate;
  struct v4l2_ctrl *exp_time;
  struct v4l2_ctrl *dz;
  struct v4l2_ctrl *dz_x;
  struct v4l2_ctrl *dz_y;
  struct v4l2_ctrl *crop_enable;
  struct v4l2_ctrl *crop_cluster[5];
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
  struct v4l2_ctrl *led_flash_ch0;
  struct v4l2_ctrl *led_flash_ch1;
  struct v4l2_ctrl *dz_x_ch0;
  struct v4l2_ctrl *dz_x_ch1;
  struct v4l2_ctrl *dz_y_ch0;
  struct v4l2_ctrl *dz_y_ch1;

  /* MCP4018 digital potentiometer */
  struct v4l2_ctrl *mcp4018_wiper;
  struct v4l2_ctrl *mcp4018_wiper_ch1;
  struct v4l2_ctrl *mcp4018_power;
  struct v4l2_ctrl *mcp4018_power_ch1;

  /* Generic AR0234 DMA register access */
  struct v4l2_ctrl *dma_reg_write_ch0;
  struct v4l2_ctrl *dma_reg_write_ch1;
  struct v4l2_ctrl *dma_reg_read_ch0;
  struct v4l2_ctrl *dma_reg_read_ch1;
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
  int led_flash;  /* V4L2_CID_LED_FLASH_CHx (AR0234 R0x3270, bit8=EN, bit7:0=DELAY) */
  int dz_x;       /* normalized center X (0..65535) */
  int dz_y;       /* normalized center Y (0..65535) */
};

struct max9296_ctrl_cache {
  bool firmware_ready;
  bool crop_enable;

  /* Channel-specific settings */
  struct max9296_channel_ctrl ch0;
  struct max9296_channel_ctrl ch1;

  /* Shared setting value, applied to both channels when set */
  int exposure; /* V4L2_CID_EXP_TIME - exp_time (u32) */
  int dz;
  int dz_x;
  int dz_y;

  /* MCP4018 digital potentiometer */
  int mcp4018_wiper;      /* V4L2_CID_MCP4018_WIPER - Port B (0x00~0x7F) */
  int mcp4018_wiper_ch1;  /* V4L2_CID_MCP4018_WIPER_CH1 - Port A (0x00~0x7F) */
  int mcp4018_power;      /* V4L2_CID_MCP4018_POWER_CH0 - MFP4 HIGH/LOW (bool) */
  int mcp4018_power_ch1;  /* V4L2_CID_MCP4018_POWER_CH1 - MFP4 HIGH/LOW (bool) */

  /* DMA register access: last read address per channel */
  u16 dma_read_addr_ch0;  /* AR0234 reg addr for CH0 read */
  u16 dma_read_addr_ch1;  /* AR0234 reg addr for CH1 read */
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
  /* Durable background-worker/topology failure.  -EAGAIN is the transient
   * detach admission gate; other non-zero values block STREAMON until the
   * worker set or reciprocal FSYNC owner is restored. */
  int worker_errno;

  // state
  /*
   * No 'setup' member here on purpose. It was written at max9296_set_fmt()'s
   * out: label - unconditionally, TRY and error paths included - and never
   * cleared, so it only ever meant "set_fmt was called at least once since
   * probe". Read as "this channel is configured" it silently wedged the FSYNC
   * gate and the enable thread's peer test; init+enable are the live signals.
   * Do not reintroduce it.
   */
  struct {
    unsigned int init;
    unsigned int firmware;
    unsigned int enable;
    unsigned int fsync;
    unsigned int power;
  } state;

  // Per-channel disconnect bitmask (set during load_regs)
  // bit layout matches cam_ch_bit: bit0=ch0, bit1=ch1, bit2=ch2, bit3=ch3
  struct {
      int disconnect;         // bitmask of disconnected channels, -1 = not checked
      unsigned int ch_shift;  // bit shift for Link A (0 for adapter2, 2 for adapter1)
  } link_status;

  /* On-demand health sampling state. There is intentionally no timer/work:
   * userspace owns cadence, and a sysfs read never resets or reconfigures HW. */
  struct {
    u64 sequence;
    u8 hinf_count[2];
    bool hinf_valid[2];
  } health;

  /* lock to protect all members below */
  struct mutex lock;
  /* Serializes the drain -> sensor lock -> rearm lease protocol. */
  struct mutex prepare_request_lock;

  int power_count;

  /* Userspace request identity and driver-owned power are separate from the
   * hardware fingerprint below.  Task 4 will expose these through sysfs. */
  enum max9296_prepare_request_state prepare_state;
  struct max9296_hw_fingerprint prepare_fingerprint;
  struct delayed_work prepare_lease_timeout;
  bool prepare_lease_held;
  bool prepare_releasing;
  bool dying;
  u64 prepare_generation;
  u64 prepare_lease_generation;
  int prepare_errno;

  /* One physical FSYNC net cannot serve different per-instance cadences.
   * These reservations are compared only within their board-power epoch. */
  u64 fsync_contract_epoch;
  u32 fsync_contract_fps;

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

  /* Hardware readiness is independent of a userspace prepare request. */
  struct max9296_hw_fingerprint initialized_fingerprint;
  bool hardware_valid;
  u64 initialized_epoch;
  u64 stream_commit_epoch;

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
    bool probe_ready;
  } shared;
};

static int max9296_normalize_fingerprint_locked(
    const struct max9296_dev *sensor,
    struct max9296_hw_fingerprint *fingerprint);
static bool max9296_fingerprint_equal(
    const struct max9296_hw_fingerprint *left,
    const struct max9296_hw_fingerprint *right);
static void max9296_mark_prepare_stale_locked(struct max9296_dev *sensor);
static void max9296_refresh_worker_status_locked(struct max9296_dev *sensor);

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

	//MAX9295 SER MFP4 GPIO LOW (DISABLED - MFP4 must be HIGH for MCP4018 VCC)
	{0x40, 0x02ca, 2, 0x80, 1, 10},

        //MAX9295 SER MFP4 GPIO HIGH (ENABLED - MCP4018 VCC ON)
        // Port A only - before address remap
        //{0x40, 0x02ca, 2, 0x90, 1, 10},  // Port A MFP4 HIGH
        // Configure MFP7/MFP8 for I2C SDA1/SCL1 mode
        //{0x40, 0x02d0, 2, 0x81, 1, 10},  // GPIO7_A: Enable output, GPIO_TX_EN for SDA1
        //{0x40, 0x02d3, 2, 0x81, 1, 10},  // GPIO7_A (mirror): Enable output
        //{0x40, 0x02d6, 2, 0x82, 1, 10},  // GPIO8_A: Enable GPIO_TX_EN for SCL1
        // Port B will be configured after address remap (see below)


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

        //MAX9295 SER(Link B) MFP4 GPIO LOW (DISABLED - MFP4 must be HIGH for MCP4018 VCC)
        {0x40, 0x02ca, 2, 0x80, 1, 10},

        //MAX9295 SER(Link B) MFP4 GPIO HIGH (ENABLED - MCP4018 VCC ON)
        // After address remap: 0x40 = Port B, 0x60 = Port A
        //{0x40, 0x02ca, 2, 0x90, 1, 10},  // Port B MFP4 HIGH (now 0x40 after remap)
        // Configure Port B MFP7/MFP8 for I2C SDA1/SCL1 mode
        //{0x40, 0x02d0, 2, 0x81, 1, 10},  // GPIO7_A: Enable output for Port B SDA1
        //{0x40, 0x02d3, 2, 0x81, 1, 10},  // GPIO7_A (mirror): Enable output
        //{0x40, 0x02d6, 2, 0x82, 1, 10},  // GPIO8_A: Enable GPIO_TX_EN for Port B SCL1

        //
        {0x40, 0x0318, 2, 0x5e, 1, 10},
        {0x40, 0x0002, 2, 0x43, 1, 10},
        {0x40, 0x0042, 2, 0x22, 1, 10},
        {0x40, 0x0043, 2, 0x78, 1, 10},
        // MCP4018 addr translator B (Port B): not needed when SRC=DST (no remap)
        // Enable only for dual mode with address remap (e.g., 0x2E→0x2F)
        //{0x40, 0x0044, 2, (MCP4018_HOST_ADDR << 1), 1, 10},  // SRC_B
        //{0x40, 0x0045, 2, (MCP4018_I2C_ADDR << 1), 1, 10},   // DST_B

        //
        {0x60, 0x0318, 2, 0x5e, 1, 10},
        {0x60, 0x0002, 2, 0x43, 1, 10},
        {0x60, 0x0053, 2, 0x13, 1, 10},
        {0x60, 0x005b, 2, 0x10, 1, 10},
        {0x60, 0x0042, 2, 0x24, 1, 10},
        {0x60, 0x0043, 2, 0x78, 1, 10},
        // MCP4018 addr translator B (Port A): not needed when SRC=DST (no remap)
        // Enable only for dual mode with address remap (e.g., 0x2E→0x2F)
        //{0x60, 0x0044, 2, (MCP4018_HOST_ADDR_CH1 << 1), 1, 10},  // SRC_B
        //{0x60, 0x0045, 2, (MCP4018_I2C_ADDR << 1), 1, 10},       // DST_B
        /*
         * 0x2E 리맵 시 위 SRC_B를 아래로 교체:
         * {0x60, 0x0044, 2, (0x2E << 1), 1, 10},  // SRC_B (remapped to 0x2E)
         */
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
        {0x40, 0x0001, 2, 0x04, 1, 10}, // TX_RATE=3Gbps
        {0x60, 0x0001, 2, 0x04, 1, 10}, // TX_RATE=3Gbps
        //{0x40, 0x0001, 2, 0x44, 1, 10}, // IIC_1_EN=1, TX_RATE=3Gbps
        //{0x60, 0x0001, 2, 0x44, 1, 10}, // IIC_1_EN=1, TX_RATE=3Gbps
        // MFP7/MFP8 GPIO_C OVR_RES_CFG=1 for SDA1/SCL1 alternate function
        //{0x40, 0x02d5, 2, 0x80, 1, 10}, // GPIO7_C: OVR_RES_CFG=1 (MFP7→SDA1)
        //{0x60, 0x02d5, 2, 0x80, 1, 10}, // GPIO7_C: OVR_RES_CFG=1 (link B MFP7→SDA1)
        //{0x40, 0x02d8, 2, 0x80, 1, 10}, // GPIO8_C: OVR_RES_CFG=1 (MFP8→SCL1)
        //{0x60, 0x02d8, 2, 0x80, 1, 10}, // GPIO8_C: OVR_RES_CFG=1 (link B MFP8→SCL1)
        // I2C pull-up enable for MFP7/MFP8
        //{0x40, 0x001c, 2, 0x08, 1, 10}, // I2C_PT_0: MFP7 pull-up enable (Bit3=1)
        //{0x60, 0x001c, 2, 0x08, 1, 10}, // I2C_PT_0: link B MFP7 pull-up enable
        //{0x40, 0x001d, 2, 0x08, 1, 10}, // I2C_PT_1: MFP8 pull-up enable (Bit3=1)
        //{0x60, 0x001d, 2, 0x08, 1, 10}, // I2C_PT_1: link B MFP8 pull-up enable
        {0x00, 0x0001, 2, 0x01, 1, 10},
#endif

#ifdef SERDES_STP // STP drive
        {0x40, 0x0011, 2, 0x02, 1, 10},
        {0x60, 0x0011, 2, 0x02, 1, 10},
        {0x00, 0x0011, 2, 0x0A, 1, 10},
#endif

        {0x00, 0x031D, 2, 0xEF, 1, 10},
        {0x00, 0x0010, 2, 0x23, 1, 100},
        //{0x00, 0x0320, 2, 0x26, 1, 10},
        //
        {0x40, 0x03F1, 2, 0x85, 1, 100},
        {0x60, 0x03F1, 2, 0x85, 1, 100},
};

// 0x00 == 0x48 slave address
static const struct reg_value max9296_init_setting_720p_30fps_L[] = {
    // step 1
    {0x00, 0x0010, 2, 0x22, 1, 300},

    //MAX9295 SER MFP4 GPIO LOW (DISABLED - MFP4 must be HIGH for MCP4018 VCC)
    {0x40, 0x02ca, 2, 0x80, 1, 10},

    //MAX9295 SER MFP4 GPIO HIGH (ENABLED - MCP4018 VCC ON)
    //{0x40, 0x02ca, 2, 0x90, 1, 10},
    // Configure MFP7/MFP8 for I2C SDA1/SCL1 mode
    //{0x40, 0x02d0, 2, 0x81, 1, 10},  // GPIO7_A: Enable output for SDA1
    //{0x40, 0x02d3, 2, 0x81, 1, 10},  // GPIO7_A (mirror): Enable output
    //{0x40, 0x02d6, 2, 0x82, 1, 10},  // GPIO8_A: Enable GPIO_TX_EN for SCL1
    // MCP4018 addr translator B: not needed when SRC=DST (no remap)
    //{0x40, 0x0044, 2, (MCP4018_HOST_ADDR << 1), 1, 10},  // SRC_B
    //{0x40, 0x0045, 2, (MCP4018_I2C_ADDR << 1), 1, 10},   // DST_B

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
    {0x40, 0x0001, 2, 0x04, 1, 10}, // TX_RATE=3Gbps
    //{0x40, 0x0001, 2, 0x44, 1, 10}, // IIC_1_EN=1, TX_RATE=3Gbps
    // MFP7/MFP8 GPIO_C OVR_RES_CFG=1 for SDA1/SCL1 alternate function
    //{0x40, 0x02d5, 2, 0x80, 1, 10}, // GPIO7_C: OVR_RES_CFG=1 (MFP7→SDA1)
    //{0x40, 0x02d8, 2, 0x80, 1, 10}, // GPIO8_C: OVR_RES_CFG=1 (MFP8→SCL1)
    // I2C pull-up enable for MFP7/MFP8
    //{0x40, 0x001c, 2, 0x08, 1, 10}, // I2C_PT_0: MFP7 pull-up enable (Bit3=1)
    //{0x40, 0x001d, 2, 0x08, 1, 10}, // I2C_PT_1: MFP8 pull-up enable (Bit3=1)
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

    //MAX9295 SER MFP4 GPIO LOW (DISABLED - MFP4 must be HIGH for MCP4018 VCC)
    {0x40, 0x02ca, 2, 0x80, 1, 10},

    //MAX9295 SER MFP4 GPIO HIGH (ENABLED - MCP4018 VCC ON)
    //{0x40, 0x02ca, 2, 0x90, 1, 10},
    // Configure MFP7/MFP8 for I2C SDA1/SCL1 mode
    //{0x40, 0x02d0, 2, 0x81, 1, 10},  // GPIO7_A: Enable output for SDA1
    //{0x40, 0x02d3, 2, 0x81, 1, 10},  // GPIO7_A (mirror): Enable output
    //{0x40, 0x02d6, 2, 0x82, 1, 10},  // GPIO8_A: Enable GPIO_TX_EN for SCL1
    // MCP4018 addr translator B: not needed when SRC=DST (no remap)
    //{0x40, 0x0044, 2, (MCP4018_HOST_ADDR << 1), 1, 10},  // SRC_B
    //{0x40, 0x0045, 2, (MCP4018_I2C_ADDR << 1), 1, 10},   // DST_B

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
    {0x40, 0x0001, 2, 0x04, 1, 10}, // TX_RATE=3Gbps
    //{0x40, 0x0001, 2, 0x44, 1, 10}, // IIC_1_EN=1, TX_RATE=3Gbps
    // MFP7/MFP8 GPIO_C OVR_RES_CFG=1 for SDA1/SCL1 alternate function
    //{0x40, 0x02d5, 2, 0x80, 1, 10}, // GPIO7_C: OVR_RES_CFG=1 (MFP7→SDA1)
    //{0x40, 0x02d8, 2, 0x80, 1, 10}, // GPIO8_C: OVR_RES_CFG=1 (MFP8→SCL1)
    // I2C pull-up enable for MFP7/MFP8
    //{0x40, 0x001c, 2, 0x08, 1, 10}, // I2C_PT_0: MFP7 pull-up enable (Bit3=1)
    //{0x40, 0x001d, 2, 0x08, 1, 10}, // I2C_PT_1: MFP8 pull-up enable (Bit3=1)
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
    MAX9296_DEFAULT_MAX_FPS,
    MAX9296_EXPOSURE_SAFE_MAX_FPS,
};

static const struct max9296_mode_info max9296_mode_data[MAX9296_NUM_MODES] = {
    {
        MAX9296_MODE_2560x720,
        2560,
        720,
        max9296_init_setting_1080p_crop_720p_2ch_30fps,
        ARRAY_SIZE(max9296_init_setting_1080p_crop_720p_2ch_30fps),
        MAX9296_DEFAULT_MAX_FPS,
        MAX9296_EXPOSURE_SAFE_MAX_FPS,
    },
    {
        MAX9296_MODE_1280x720,
        1280,
        720,
        max9296_init_setting_720p_30fps_L,
        ARRAY_SIZE(max9296_init_setting_720p_30fps_L),
        MAX9296_DEFAULT_MAX_FPS,
        MAX9296_EXPOSURE_SAFE_MAX_FPS,
    },
    {
        MAX9296_MODE_3840x1080,
        3840,
        1080,
        max9296_init_setting_1080p_crop_720p_2ch_30fps,
        ARRAY_SIZE(max9296_init_setting_1080p_crop_720p_2ch_30fps),
        MAX9296_DEFAULT_MAX_FPS,
        MAX9296_EXPOSURE_SAFE_MAX_FPS,
    },
    {
        MAX9296_MODE_1920x1080,
        1920,
        1080,
        max9296_init_setting_720p_30fps_L,
        ARRAY_SIZE(max9296_init_setting_720p_30fps_L),
        MAX9296_DEFAULT_MAX_FPS,
        MAX9296_EXPOSURE_SAFE_MAX_FPS,
    },
    {
        MAX9296_MODE_1280x360,
        1280,
        360,
        max9296_init_setting_1080p_crop_720p_2ch_30fps,
        ARRAY_SIZE(max9296_init_setting_1080p_crop_720p_2ch_30fps),
        MAX9296_360P_MAX_FPS,
        MAX9296_EXPOSURE_SAFE_MAX_FPS,
    },
    {
        MAX9296_MODE_640x360,
        640,
        360,
        max9296_init_setting_720p_30fps_L,
        ARRAY_SIZE(max9296_init_setting_720p_30fps_L),
        MAX9296_360P_MAX_FPS,
        MAX9296_EXPOSURE_SAFE_MAX_FPS,
    },
};
static const struct max9296_mode_info max9296_mode_data_HD_R = {
    MAX9296_MODE_1280x720,
    1280,
    720,
    max9296_init_setting_720p_30fps_R,
    ARRAY_SIZE(max9296_init_setting_720p_30fps_R),
    MAX9296_DEFAULT_MAX_FPS,
    MAX9296_EXPOSURE_SAFE_MAX_FPS,
};

static const struct max9296_mode_info max9296_mode_data_FHD_R = {
    MAX9296_MODE_1920x1080,
    1920,
    1080,
    max9296_init_setting_720p_30fps_R,
    ARRAY_SIZE(max9296_init_setting_720p_30fps_R),
    MAX9296_DEFAULT_MAX_FPS,
    MAX9296_EXPOSURE_SAFE_MAX_FPS,
};

static const struct max9296_mode_info max9296_mode_data_360_R = {
    MAX9296_MODE_640x360,
    640,
    360,
    max9296_init_setting_720p_30fps_R,
    ARRAY_SIZE(max9296_init_setting_720p_30fps_R),
    MAX9296_360P_MAX_FPS,
    MAX9296_EXPOSURE_SAFE_MAX_FPS,
};
//-------------------------------------------------------------------------
static bool max9296_mode_is_dual(const struct max9296_mode_info *mode) {
  return mode && (mode->id == MAX9296_MODE_2560x720 ||
                  mode->id == MAX9296_MODE_3840x1080 ||
                  mode->id == MAX9296_MODE_1280x360);
}

/* True when the hardware is currently programmed for both cameras.
 *
 * Deliberately reads last_mode, not current_mode. current_mode is the requested
 * format and max9296_set_fmt() moves it with no gate. last_mode is assigned in
 * max9296_set_mode() right before load_regs, so it alone says which table the
 * hardware actually received - and the serializer address is a property of
 * that, not of the pending format.
 * A dual stream followed by stream-off and an S_FMT to a single mode leaves a
 * serializer physically at 0x60 while current_mode already reads single.
 */
static bool max9296_hw_is_dual(const struct max9296_dev *sensor) {
  return max9296_mode_is_dual(sensor->last_mode);
}

/* MAX9295 I2C address for a local channel (0 or 1).
 *
 * 0x60 is NOT a hardware property of ch1 - it exists only as a side effect of
 * {0x40, 0x0000, 2, 0xC0} inside max9296_init_setting_1080p_crop_720p_2ch_30fps,
 * the only serializer self-address write in this driver. The single-channel
 * tables (max9296_init_setting_720p_30fps_L/_R) carry no such remap and address
 * the serializer exclusively at 0x40 - including MAX9295_REG_MFP4_CTRL itself.
 * The current-epoch fingerprint prevents a dual and single table from both
 * executing in one board-power lifetime; a real power reset advances the epoch
 * and restores the serializer's default address before another table may run.
 * In single-channel mode the one serializer therefore answers at its power-on
 * default 0x40, whichever local channel is active.
 *
 * Confirmed on the bench (ch1 single-channel unit):
 *   i2ctransfer -f -y -a 2 w3@0x60 0x02 0xca 0x90   -> NAK
 *   i2ctransfer -f -y -a 2 w3@0x40 0x02 0xca 0x90   -> ACK
 */
static u8 max9296_ser_addr(const struct max9296_dev *sensor,
                           unsigned int local_ch) {
  if (!max9296_hw_is_dual(sensor))
    return MAX9295_SER_ADDR_CH0;

  return local_ch ? MAX9295_SER_ADDR_CH1 : MAX9295_SER_ADDR_CH0;
}

/* Whether a per-channel MCP4018 control addresses hardware that exists.
 *
 * In single-channel mode there is one serializer at 0x40 and one pot, and both
 * MCP4018_HOST_ADDR and MCP4018_HOST_ADDR_CH1 are 0x2F - so a CH1 control would
 * be byte-identical to the CH0 one and silently retune the active channel.
 * Before the serializer address was corrected this was masked by the 0x60 write
 * simply NAKing. Gate it on the active local channel instead.
 */
static bool max9296_ch_ctrl_applies(const struct max9296_dev *sensor,
                                    unsigned int local_ch) {
  if (max9296_hw_is_dual(sensor))
    return true;

  /* Single: the CH0 slot is the documented control path (see CHANGELOG), so it
   * stays live regardless of enable; only CH1 needs the active-channel gate. */
  return local_ch == 0 || sensor->enable == 0x02;
}

/* Best-effort slave-addr -> global channel number for logging.
 *   adapter 2 : base 0 (local CH0 = global ch0, local CH1 = global ch1)
 *   adapter 1 : base 2 (local CH0 = global ch2, local CH1 = global ch3)
 * Returns -1 if not channel-specific (e.g., MAX9296 self at 0x00).
 */
static int max9296_slave_to_global_ch(struct max9296_dev *sensor,
                                      unsigned int slave_addr) {
  int base = sensor->link_status.ch_shift;
  switch (slave_addr) {
  case AP1302_CH0_I2C_ADDR:
    return base + 0;
  case MAX9295_SER_ADDR_CH0:
    /* In single-channel mode 0x40 is the only serializer whichever local
     * channel is active, so it carries no channel information on its own -
     * fall back to the enable bitmask. Labelling it "ch0" unconditionally made
     * a ch1 serializer error read as a ch0 one. */
    if (!max9296_hw_is_dual(sensor))
      return base + ((sensor->enable == 0x02) ? 1 : 0);
    return base + 0;
  case AP1302_CH1_I2C_ADDR:
  case MAX9295_SER_ADDR_CH1:
    return base + 1;
  case AP1302_I2C_ADDR:
    return base + ((sensor->enable == 0x02) ? 1 : 0);
  default:
    return -1;
  }
}

static void max9296_fmt_ch(char *buf, size_t len,
                           struct max9296_dev *sensor,
                           unsigned int slave_addr) {
  int gch = max9296_slave_to_global_ch(sensor, slave_addr);
  if (gch >= 0)
    scnprintf(buf, len, "ch%d", gch);
  else
    scnprintf(buf, len, "ch?");
}

static int maxim_ops_i2c_write(struct max9296_dev *sensor,
                               unsigned int slave_addr, unsigned int reg,
                               unsigned int val, unsigned int reg_byte,
                               unsigned int val_byte) {
  int ret = 0, index = 0, i = 0;
  struct i2c_client *client = sensor->i2c_client;
  unsigned char buf[8];
  struct i2c_msg msg;
  unsigned int attempt, max_attempts = 5;

  msg.addr = (slave_addr == 0 ? client->addr : slave_addr);
  msg.flags = 0;
  msg.len = reg_byte + val_byte;
  msg.buf = buf;

  for (i = 0; i < reg_byte; ++i)
    buf[i] = (reg >> ((reg_byte - i - 1) << 3)) & 0xff;

  index = i;
  for (i = 0; i < val_byte; ++i)
    buf[index + i] = (val >> ((val_byte - i - 1) << 3)) & 0xff;

  for (attempt = 0; attempt < max_attempts; attempt++) {
    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret >= 0)
      break;
    msleep(1);
  }

  {
    char ch_buf[8];
    max9296_fmt_ch(ch_buf, sizeof(ch_buf), sensor, msg.addr);

    if ((attempt >= max_attempts) && (ret < 0)) {
      printk(KERN_ERR "[%s:%d][%s:%d] %s Error i2c write reg : [0x%x] "
                      "reg=0x%x(%d byte), val=0x%x(%d byte)\n",
             KEYWORD, client->adapter->nr, _FILE_, __LINE__, ch_buf, msg.addr,
             reg, reg_byte, val, val_byte);

      return ret;
    }

    /*
     * i2c_transfer() returns the number of messages transferred on success.
     * For this single-message write path, success is ret == 1.
     * V4L2 ctrl callbacks must return 0 on success, so normalize here.
     */
    if (ret != 1) {
      printk_ratelimited(KERN_ERR
             "[%s:%d][%s:%d] %s i2c write short xfer (ret=%d) : [0x%x] "
             "reg=0x%x(%d byte), val=0x%x(%d byte)\n",
             KEYWORD, client->adapter->nr, _FILE_, __LINE__, ch_buf, ret,
             msg.addr, reg, reg_byte, val, val_byte);
      return -EIO;
    }
    printk(KERN_INFO "[%s:%d][%s:%d] %s Success!! i2c write reg : [0x%x] "
                     "reg=0x%x(%d byte), val=0x%x(%d byte)(ret:%d, retry:%d)\n",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__, ch_buf, msg.addr, reg,
           reg_byte, val, val_byte, ret, attempt);
  }

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
  unsigned int attempt, max_attempts = 10;

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

  for (attempt = 0; attempt < max_attempts; attempt++) {
    ret = i2c_transfer(client->adapter, msg, 2);
    if (ret >= 0)
      break;
    printk(KERN_ERR "[%s:%d][%s:%d] Error i2c read reg : [0x%x] "
                    "reg=0x%x(%d byte),(read %d byte) attempt:%d\n",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__,
           (slave_addr == 0 ? client->addr : slave_addr), reg, reg_byte,
           val_byte, attempt);
  }

  if ((attempt >= max_attempts) && (ret < 0)) {
    printk(KERN_ERR "[%s:%d][%s:%d] Error i2c read - slave: 0x%x, reg: "
                    "0x%x(%d byte, %d data byte)\n",
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

/* One-attempt, no-log I2C read for health observation.
 *
 * The control path helper above retries ten times and emits an error for each
 * failed attempt. That is appropriate for an operator control but harmful for
 * a periodic observer: a disconnected cable would monopolise the adapter and
 * flood the journal. Health reads therefore make exactly one transfer and
 * return the real errno (or -EIO for a short transfer).
 */
static int max9296_health_i2c_read_once(struct max9296_dev *sensor,
                                        unsigned int slave_addr,
                                        unsigned int reg,
                                        unsigned int reg_byte,
                                        unsigned int val_byte,
                                        unsigned int *val) {
  struct i2c_client *client = sensor->i2c_client;
  unsigned char reg_buf[4] = {0};
  unsigned char val_buf[4] = {0};
  struct i2c_msg msgs[2] = {0};
  unsigned int i;
  int ret;

  if (!val || reg_byte == 0 || reg_byte > sizeof(reg_buf) ||
      val_byte == 0 || val_byte > sizeof(val_buf))
    return -EINVAL;

  *val = 0;
  for (i = 0; i < reg_byte; i++)
    reg_buf[i] = (reg >> ((reg_byte - i - 1) * 8)) & 0xff;

  msgs[0].addr = slave_addr ? slave_addr : client->addr;
  msgs[0].flags = 0;
  msgs[0].len = reg_byte;
  msgs[0].buf = reg_buf;
  msgs[1].addr = msgs[0].addr;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = val_byte;
  msgs[1].buf = val_buf;

  ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
  if (ret < 0)
    return ret;
  if (ret != ARRAY_SIZE(msgs))
    return -EIO;

  for (i = 0; i < val_byte; i++)
    *val |= (unsigned int)val_buf[i] << ((val_byte - i - 1) * 8);
  return 0;
}

/*
 * MAX9295 MFP4 GPIO toggle — gates MCP4018 VCC.
 * Equivalent to the commented-out reg_value entries in link init tables:
 *   {ser_addr, 0x02ca, 2, 0x90 (ON) / 0x80 (OFF), 1, 10}
 */
static int max9295_mfp4_set(struct max9296_dev *sensor, u8 ser_addr, bool on) {
  return maxim_ops_i2c_write(sensor, ser_addr, MAX9295_REG_MFP4_CTRL,
                             on ? MAX9295_MFP4_POWER_ON
                                : MAX9295_MFP4_POWER_OFF,
                             2, 1);
}

/*
 * MCP4018 I2C: No register address, direct 1-byte read/write protocol.
 * Write: [START][addr+W][wiper_value][STOP]
 * Read:  [START][addr+R][data][STOP]
 *
 * @host_addr: host-visible I2C address routed through MAX9295 address translator B
 *   - Port B: MCP4018_HOST_ADDR (0x2F)
 *   - Port A: MCP4018_HOST_ADDR_CH1 (0x2F, or 0x2E when remapped)
 */
static int mcp4018_write_wiper(struct max9296_dev *sensor, u8 host_addr,
                               u8 wiper_value, u8 ser_addr) {
  struct i2c_client *client = sensor->i2c_client;
  struct i2c_msg msg;
  const unsigned int max_retry = 5;
  unsigned int attempt;
  int ret = -EIO;
  char ch_buf[8];

  max9296_fmt_ch(ch_buf, sizeof(ch_buf), sensor, ser_addr);

  if (wiper_value > MCP4018_WIPER_MAX)
    wiper_value = MCP4018_WIPER_MAX;

  msg.addr = host_addr;
  msg.flags = 0;
  msg.len = 1;
  msg.buf = &wiper_value;

  for (attempt = 1; attempt <= max_retry; attempt++) {
    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret >= 0)
      break;
    if (attempt < max_retry)
      msleep(1);
  }

  if (ret < 0) {
    printk(KERN_ERR "[%s:%d][%s:%d] %s MCP4018(0x%02x) write fail: wiper=0x%02x "
                    "gave up after %u/%u attempts ret=%d\n",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__,
           ch_buf, host_addr, wiper_value, max_retry, max_retry, ret);
    return ret;
  }
  if (ret != 1)
    return -EIO;

  printk(KERN_INFO "[%s:%d][%s:%d] %s MCP4018(0x%02x) write success: wiper=0x%02x "
                   "(%d/%d) attempt=%u/%u\n",
         KEYWORD, client->adapter->nr, _FILE_, __LINE__,
         ch_buf, host_addr, wiper_value, wiper_value, MCP4018_WIPER_MAX,
         attempt, max_retry);
  return 0;
}

static int mcp4018_read_wiper(struct max9296_dev *sensor, u8 host_addr,
                              u8 *wiper_value) {
  struct i2c_client *client = sensor->i2c_client;
  u8 buf = 0;
  struct i2c_msg msg;
  unsigned int retry = 5;
  int ret;

  msg.addr = host_addr;
  msg.flags = I2C_M_RD;
  msg.len = 1;
  msg.buf = &buf;

  do {
    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret >= 0) break;
    msleep(1);
  } while (--retry);

  if ((retry == 0) && (ret < 0)) {
    printk(KERN_ERR "[%s:%d][%s:%d] MCP4018(0x%02x) read failed: ret=%d\n",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__, host_addr, ret);
    return ret;
  }
  if (ret != 1)
    return -EIO;

  *wiper_value = buf & MCP4018_WIPER_MAX;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] MCP4018(0x%02x) wiper read: 0x%02x",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__,
           host_addr, *wiper_value);
  return 0;
}

/*
 * AP1302 DMA-based AR0234 sensor register access.
 *
 * Uses AP1302 DMA registers (Basic address space 0x60A0-0x60AC) to
 * read/write downstream AR0234 sensor registers. This approach is
 * stable unlike SIPM which suffers from AP1302 FW ADR contention.
 *
 * SENSOR_SIP (0x604A) provides sensor_id, addr_16, data_16 flags.
 * DMA_SRC/DST encode port, sensor flags, and register address.
 * DMA_CTRL triggers the transaction; poll (ctrl & 0x7) == 0 for completion.
 */

static int max9296_dma_wait_idle(struct max9296_dev *sensor, u32 ap_addr) {
  unsigned int ctrl;
  int poll;

  for (poll = 0; poll < 20; poll++) {
    ctrl = 0xFFFFFFFF;
    maxim_ops_i2c_read(sensor, ap_addr, AP1302_REG_DMA_CTRL, 2, 2, &ctrl);
    if ((ctrl & 0x7) == 0)
      return 0;
    usleep_range(5000, 6000);
  }

  printk(KERN_WARNING "[%s:%d][%s:%d] %s DMA not idle: ap=0x%02x ctrl=0x%04x\n",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         __FUNCTION__, ap_addr, ctrl);
  return -ETIMEDOUT;
}

static int max9296_dma_load_sip(struct max9296_dev *sensor, u32 ap_addr,
                                u32 *sensor_id, u32 *addr_16, u32 *data_16) {
  unsigned int sip_raw = 0;
  int ret;

  ret = maxim_ops_i2c_read(sensor, ap_addr, AP1302_REG_SENSOR_SIP, 2, 2,
                           &sip_raw);
  if (ret)
    return ret;

  *sensor_id = (sip_raw >> 1) & 0x3f;
  *addr_16 = (sip_raw & 0x0100) ? 1 : 0;
  *data_16 = (sip_raw & 0x0200) ? 1 : 0;
  return 0;
}

static int max9296_dma_read_reg(struct max9296_dev *sensor, u32 ap_addr,
                                u16 reg_addr, u16 *val) {
  u32 sensor_id, addr_16, data_16, data_size;
  u32 dma_src;
  unsigned int dma_dst_raw = 0;
  int ret;
  char ch_buf[8];

  max9296_fmt_ch(ch_buf, sizeof(ch_buf), sensor, ap_addr);

  ret = max9296_dma_load_sip(sensor, ap_addr, &sensor_id, &addr_16, &data_16);
  if (ret)
    goto fail;

  data_size = data_16 ? 2 : 1;

  ret = max9296_dma_wait_idle(sensor, ap_addr);
  if (ret)
    goto fail;

  /* DMA_SIZE = data bytes to transfer */
  ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_SIZE,
                            data_size, 2, 4);
  if (ret)
    goto fail;

  /* DMA_SRC = (port<<26)|(data_16<<25)|(addr_16<<24)|(sensor_id<<17)|reg_addr */
  dma_src = (data_16 << 25) | (addr_16 << 24) | (sensor_id << 17) |
            (reg_addr & 0xffff);
  ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_SRC, dma_src, 2, 4);
  if (ret)
    goto fail;

  /* DMA_DST = internal AP1302 address to store result */
  ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_DST,
                            0x000060a4, 2, 4);
  if (ret)
    goto fail;

  /* DMA_CTRL = 0x0032 triggers sensor-to-AP1302 read */
  ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_CTRL,
                            0x0032, 2, 2);
  if (ret)
    goto fail;

  ret = max9296_dma_wait_idle(sensor, ap_addr);
  if (ret)
    goto fail;

  /* Read result from DMA_DST — data is in upper bits of 32-bit value */
  ret = maxim_ops_i2c_read(sensor, ap_addr, AP1302_REG_DMA_DST, 2, 4,
                           &dma_dst_raw);
  if (ret)
    goto fail;

  *val = (u16)(dma_dst_raw >> (32 - data_size * 8));

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s DMA read: ap=0x%02x "
                     "reg=0x%04x val=0x%04x\n",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         ch_buf, ap_addr, reg_addr, *val);

  return 0;

fail:
  printk_ratelimited(KERN_ERR
         "[%s:%d][%s:%d] %s DMA read fail: ap=0x%02x reg=0x%04x (ret=%d)\n",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         ch_buf, ap_addr, reg_addr, ret);
  return ret;
}

static int max9296_dma_write_reg(struct max9296_dev *sensor, u32 ap_addr,
                                 u16 reg_addr, u16 val) {
  const unsigned int max_retry = 3;
  u32 sensor_id, addr_16, data_16, data_size;
  u32 dma_src, dma_dst;
  unsigned int attempt;
  int ret = -EIO;
  char ch_buf[8];

  max9296_fmt_ch(ch_buf, sizeof(ch_buf), sensor, ap_addr);

  for (attempt = 1; attempt <= max_retry; attempt++) {
    ret = max9296_dma_load_sip(sensor, ap_addr, &sensor_id, &addr_16, &data_16);
    if (ret)
      goto retry_step;

    data_size = data_16 ? 2 : 1;

    ret = max9296_dma_wait_idle(sensor, ap_addr);
    if (ret)
      goto retry_step;

    /* DMA_SIZE = data bytes to transfer */
    ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_SIZE,
                              data_size, 2, 4);
    if (ret)
      goto retry_step;

    /* DMA_SRC = (value << 16) | 0x000060a0 — data in upper 16 bits */
    dma_src = ((u32)(val & 0xffff) << 16) | 0x000060a0;
    ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_SRC, dma_src, 2, 4);
    if (ret)
      goto retry_step;

    /* DMA_DST = (port<<26)|(data_16<<25)|(addr_16<<24)|(sensor_id<<17)|reg_addr */
    dma_dst = (data_16 << 25) | (addr_16 << 24) | (sensor_id << 17) |
              (reg_addr & 0xffff);
    ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_DST, dma_dst, 2, 4);
    if (ret)
      goto retry_step;

    /* DMA_CTRL = 0x0302 triggers AP1302-to-sensor write */
    ret = maxim_ops_i2c_write(sensor, ap_addr, AP1302_REG_DMA_CTRL,
                              0x0302, 2, 2);
    if (ret)
      goto retry_step;

    ret = max9296_dma_wait_idle(sensor, ap_addr);
    if (ret)
      goto retry_step;

    printk(KERN_NOTICE "[%s:%d][%s:%d] %s DMA write success: ap=0x%02x "
                       "reg=0x%04x val=0x%04x attempt=%u/%u\n",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           ch_buf, ap_addr, reg_addr, val, attempt, max_retry);
    return 0;

retry_step:
    printk(KERN_WARNING "[%s:%d][%s:%d] %s DMA write attempt %u/%u fail: ap=0x%02x "
                        "reg=0x%04x val=0x%04x ret=%d\n",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           ch_buf, attempt, max_retry, ap_addr, reg_addr, val, ret);
    if (attempt < max_retry)
      msleep(1);
  }

  printk(KERN_ERR "[%s:%d][%s:%d] %s DMA write fail: ap=0x%02x reg=0x%04x val=0x%04x "
                  "gave up after %u attempts ret=%d\n",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         ch_buf, ap_addr, reg_addr, val, max_retry, ret);
  return ret;
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
  case MAX9296_MODE_1280x360:
  case MAX9296_MODE_640x360:
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
  int first_err = 0;
  /*
   * Despite the names these are CHANNEL-position flags, not GMSL PHY flags:
   * link_a_err -> the even channel (ch0/ch2), link_b_err -> the odd one
   * (ch1/ch3), per the shift applied when the bitmask is built below.
   *
   * The two only coincide in dual mode, where PHY A == 0x40 == even channel
   * and PHY B == 0x60 == odd channel. In single-channel mode there is one
   * serializer at 0x40 whichever PHY carries it, and the active PHY does not
   * track the channel number at all - a ch1 unit measured LINK_MODE = PHY A.
   * Read them as "even/odd channel", never as "which link".
   */
  bool link_a_err = false;
  bool link_b_err = false;
  unsigned int shift = sensor->link_status.ch_shift;
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
    if (ret < 0) {
      /* Keep walking the table so every disconnected channel is attributed,
       * but make hardware preparation truthful: any failed table write makes
       * the complete preparation fail.  Preserve the first errno rather than
       * letting later successes or failures overwrite its root cause. */
      if (!first_err)
        first_err = ret;

      if (slave_addr == 0x40) {
        /*
         * 0x40 does NOT imply a GMSL PHY here - see the note on the err flags
         * above. In single-channel mode it is the only serializer whichever
         * PHY carries it, so the channel has to come from sensor->enable:
         * 0x02 selects the odd channel, anything else the even one.
         *
         * Measured on a single ch1 unit (i2c3, enable==0x02, _R table):
         *   CTRL3 0x13 = 0xda -> LINK_MODE 01 = PHY A, LOCKED, CMU_LOCKED
         *   RX3   0x2f = 0x06 -> SYNC_LOCKED_A | WBLOCK_A
         * i.e. the active PHY was A even though the unit is channel 1. An
         * earlier version of this comment claimed Right mode puts the "Link B
         * serializer" at 0x40 - that is wrong about the PHY, though the
         * channel attribution below has always been right.
         */
        if (sensor->enable == 0x02)
          link_b_err = true;
        else
          link_a_err = true;
      } else if (slave_addr == 0x60) {
        link_b_err = true;
      }
    }

    if (delay_ms)
      usleep_range(1000 * delay_ms, 1000 * delay_ms + 1000 * delay_ms / 10);
  }

  /* Build per-channel disconnect bitmask */
  sensor->link_status.disconnect = 0;
  if (link_a_err)
    sensor->link_status.disconnect |= (1 << shift);       /* ch0 or ch2 */
  if (link_b_err)
    sensor->link_status.disconnect |= (1 << (shift + 1)); /* ch1 or ch3 */

  if (sensor->link_status.disconnect) {
    printk(KERN_WARNING "[%s:%d][%s:%d] disconnect bitmask=0x%x (ch%u=%s ch%u=%s)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           sensor->link_status.disconnect,
           shift, link_a_err ? "DISCONNECTED" : "OK",
           shift + 1, link_b_err ? "DISCONNECTED" : "OK");
  }

  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  return first_err;
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

static int max9296_disable_stream_mipi(struct max9296_dev *sensor) {
  int ret;
  if (1)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s (off)", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  ret = maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x00, 2, 1);

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
  rate *= READ_ONCE(sensor->fps);
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (rate:%llu)", KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__, rate);
  return rate;
}

/*
 * Board power is a global resource, not a per-deserializer one: a single RESET
 * net feeds every deserializer and camera module, and max9296_power() always
 * drives both PWDN lines together. The sequence must therefore run exactly once
 * for the first user and once for the last, whichever video node opens first.
 *
 * The previous handshake gated on the peer's state.power, but max9296_s_power()
 * cleared both channels' state.power to IDLE on every close - including the
 * closes where the sequence had been skipped precisely because the peer was
 * still up. Reopening that channel then saw the peer as IDLE and re-ran the
 * whole power-on: global RESET asserted ~2 s and both PWDN pulled for ~4 s,
 * killing a peer that was mid-stream. A plain refcount removes the state
 * handshake entirely.
 *
 * max9296_power_lock also serializes the sequence itself, so a concurrent open,
 * close, or reset-GPIO probe cannot race a second reset onto the same pins.
 */
static DEFINE_MUTEX(max9296_power_lock);
/* Serializes one shared-FSYNC configuration transaction across both instances.
 * It is held only for power acquisition plus the short cadence reservation;
 * firmware downloads remain parallel. */
static DEFINE_MUTEX(max9296_fsync_config_lock);
/* Protects peer publication/lifetime admission and short peer propagation. */
static DEFINE_MUTEX(max9296_shared_lock);
/* The shared.sensor links are raw devm pointers.  Serialize device removal so
 * one instance cannot free its storage while its sibling stops peer threads. */
static DEFINE_MUTEX(max9296_remove_lock);
static int max9296_power_users;
/*
 * Identifies one continuous board-power lifetime.  Fingerprints programmed in
 * an older lifetime must never be used to skip initialization.  All writers
 * hold max9296_power_lock; readers use READ_ONCE while holding sensor->lock.
 */
static u64 max9296_hw_epoch = 1;

/* The DTS declares the active-low board reset only on max9296_0.  Keep its
 * descriptor available to either instance's first/last-user transition without
 * depending on the asynchronous raw-peer publication.  The pointer is used and
 * withdrawn only under max9296_power_lock, before devres releases the GPIO. */
static struct gpio_desc *max9296_board_reset_gpio;
static struct max9296_dev *max9296_board_reset_owner;

static void max9296_put_orphaned_reset_gpio_locked(void) {
  struct gpio_desc *reset_gpio;

  lockdep_assert_held(&max9296_power_lock);
  if (max9296_power_users > 0 || max9296_board_reset_owner ||
      !max9296_board_reset_gpio)
    return;

  reset_gpio = max9296_board_reset_gpio;
  max9296_board_reset_gpio = NULL;
  gpiod_put(reset_gpio);
}

static void max9296_release_board_reset_gpio(void *data) {
  struct max9296_dev *sensor = data;

  mutex_lock(&max9296_power_lock);
  if (max9296_board_reset_owner == sensor) {
    max9296_board_reset_owner = NULL;
    sensor->reset_gpio = NULL;
    /* Keep the non-devm request across a one-sided active unbind.  This
     * preserves mux, direction, and value until a rebind adopts it or the real
     * last user has asserted reset and releases the orphan. */
    if (max9296_power_users > 0)
      goto unlock;
    max9296_put_orphaned_reset_gpio_locked();
  }
unlock:
  mutex_unlock(&max9296_power_lock);
}

/* Requesting GPIOD_OUT_HIGH asserts an active-low reset immediately.  That is
 * the safe cold-probe state, but it is destructive when the reset owner is
 * rebound while its sibling keeps the board powered.  Serialize the users
 * snapshot through the descriptor request: active-domain rebind uses ASIS and
 * the next real power transition explicitly establishes output direction. */
static int max9296_acquire_reset_gpio(struct max9296_dev *sensor) {
  struct device *dev = &sensor->i2c_client->dev;
  struct gpio_desc *reset_gpio;
  enum gpiod_flags flags;
  bool newly_requested = false;
  int ret = 0;

  if (!of_property_read_bool(dev->of_node, "reset-gpios"))
    return 0;

  mutex_lock(&max9296_power_lock);
  if (max9296_board_reset_owner) {
    ret = -EBUSY;
    goto unlock;
  }
  reset_gpio = max9296_board_reset_gpio;
  if (!reset_gpio) {
    flags = max9296_power_users > 0 ? GPIOD_ASIS : GPIOD_OUT_HIGH;
    reset_gpio = gpiod_get_optional(dev, "reset", flags);
    if (IS_ERR(reset_gpio)) {
      ret = PTR_ERR(reset_gpio);
      goto unlock;
    }
    if (!reset_gpio)
      goto unlock;
    newly_requested = true;
  }

  /* Only the cleanup action is devm-managed.  The GPIO request itself must
   * survive an active owner unbind, otherwise ASIS rebind cannot promise line
   * continuity across the unrequested window. */
  ret = devm_add_action(dev, max9296_release_board_reset_gpio, sensor);
  if (ret) {
    if (newly_requested) {
      if (max9296_power_users > 0)
        max9296_board_reset_gpio = reset_gpio;
      else
        gpiod_put(reset_gpio);
    }
    goto unlock;
  }
  sensor->reset_gpio = reset_gpio;
  max9296_board_reset_gpio = reset_gpio;
  max9296_board_reset_owner = sensor;

unlock:
  mutex_unlock(&max9296_power_lock);
  return ret;
}

/* GPIOD_ASIS deliberately leaves an active peer's reset untouched at probe.
 * Every actual first-on/last-off transition calls this under the epoch lock,
 * making the descriptor an output with logical 1 (physical low/asserted) before
 * the existing reset sequence toggles it. */
static int max9296_prepare_reset_gpio_locked(
    struct max9296_dev *sensor, struct gpio_desc **reset_gpio) {
  int ret;

  lockdep_assert_held(&max9296_power_lock);

  *reset_gpio = max9296_board_reset_gpio;
  if (!*reset_gpio)
    *reset_gpio = sensor->reset_gpio;
  if (!*reset_gpio)
    return 0;

  ret = gpiod_direction_output(*reset_gpio, 1);
  if (ret) {
    dev_err(&sensor->i2c_client->dev,
            "failed to configure shared reset GPIO: %d\n", ret);
    *reset_gpio = NULL;
  }
  return ret;
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

static int max9296_reset(struct max9296_dev *sensor) {
  struct gpio_desc *reset_gpio;
  int ret;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  ret = max9296_prepare_reset_gpio_locked(sensor, &reset_gpio);
  if (ret) {
    max9296_power(sensor, false);
    return ret;
  }

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
  return 0;
}

static int max9296_set_power_on(struct max9296_dev *sensor) {
  int ret;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  usleep_range(10000, 11000);
  ret = max9296_reset(sensor);
  usleep_range(10000, 11000);
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return ret;
}

static int max9296_set_power_off(struct max9296_dev *sensor) {
  struct gpio_desc *reset_gpio;
  int ret;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s start", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  ret = max9296_prepare_reset_gpio_locked(sensor, &reset_gpio);

  if (reset_gpio) {
    if (debug)
      printk(KERN_NOTICE "[%s:%d][%s:%d] %s (reset low)", "RST",
             sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    gpiod_set_value_cansleep(reset_gpio, 1);
  }

  max9296_power(sensor, false);
  memset(sensor->health.hinf_valid, 0, sizeof(sensor->health.hinf_valid));
  sensor->streaming = false;
  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return ret;
}

/* Resolve the DT-declared sibling independently of shared.sensor.  The I2C
 * device reference is owned by sensor->shared.client and released exactly once
 * by the probe-failure or normal-remove cleanup path.  It keeps the device
 * object present; max9296_shared_lock and the ready/dying protocol below, not
 * that reference, protect the peer's devm private storage. */
static struct max9296_dev *max9296_declared_shared_peer_locked(
    struct max9296_dev *sensor) {
  struct v4l2_subdev *sd;
  struct max9296_dev *peer;

  lockdep_assert_held(&max9296_shared_lock);

  if (!sensor->shared.np)
    return NULL;
  if (!sensor->shared.client)
    sensor->shared.client = of_find_i2c_device_by_node(sensor->shared.np);
  if (!sensor->shared.client)
    return NULL;

  sd = i2c_get_clientdata(sensor->shared.client);
  if (!sd)
    return NULL;
  peer = to_max9296_dev(sd);
  return peer == sensor ? NULL : peer;
}

/* Publish a peer only after both probes committed and both DT endpoints name
 * each other.  Both raw links are installed in the same critical section, so
 * ordinary operation cannot create the historical B->A / A->NULL state.
 * Removal still resolves the declaration independently to clean up such a
 * state defensively. */
static struct max9296_dev *max9296_ready_shared_peer_locked(
    struct max9296_dev *sensor) {
  struct max9296_dev *peer;

  lockdep_assert_held(&max9296_shared_lock);

  if (!READ_ONCE(sensor->shared.probe_ready) || READ_ONCE(sensor->dying))
    return NULL;

  peer = max9296_declared_shared_peer_locked(sensor);
  if (!peer || !READ_ONCE(peer->shared.probe_ready) ||
      READ_ONCE(peer->dying) ||
      peer->shared.np != sensor->i2c_client->dev.of_node)
    return NULL;

  sensor->shared.sd = &peer->sd;
  WRITE_ONCE(sensor->shared.sensor, peer);
  peer->shared.sd = &sensor->sd;
  WRITE_ONCE(peer->shared.sensor, sensor);
  max9296_refresh_worker_status_locked(sensor);
  max9296_refresh_worker_status_locked(peer);
  return peer;
}

/* Configure one physical FSYNC domain without ever taking the peer's sensor
 * lock.  Callers hold their own sensor lock and the board FSYNC transaction
 * lock.  The board power lock makes the current epoch and both reservations a
 * single snapshot; the shared lock pins peer admission for the short lockless
 * WRITE_ONCE propagation used by the legacy V4L2 interval semantics. */
static int max9296_configure_shared_fsync_locked(
    struct max9296_dev *sensor, unsigned int fps, bool bind) {
  struct max9296_dev *peer;
  u64 epoch;
  int ret = 0;

  lockdep_assert_held(&sensor->lock);
  lockdep_assert_held(&max9296_fsync_config_lock);

  mutex_lock(&max9296_power_lock);
  if (READ_ONCE(sensor->dying)) {
    ret = -ENODEV;
    goto unlock_power;
  }
  epoch = max9296_hw_epoch;
  mutex_lock(&max9296_shared_lock);
  peer = max9296_ready_shared_peer_locked(sensor);

  if ((sensor->fsync_contract_epoch == epoch &&
       sensor->fsync_contract_fps != fps) ||
      (peer && peer->fsync_contract_epoch == epoch &&
       peer->fsync_contract_fps != fps)) {
    ret = -ESTALE;
    goto unlock;
  }

  /* No live/request field is changed until every current reservation agrees. */
  if (bind) {
    sensor->fsync_contract_epoch = epoch;
    sensor->fsync_contract_fps = fps;
  }
  WRITE_ONCE(sensor->fps, fps);
  WRITE_ONCE(sensor->frame_interval.numerator, 1);
  WRITE_ONCE(sensor->frame_interval.denominator, fps);
  if (peer) {
    WRITE_ONCE(peer->fps, fps);
    WRITE_ONCE(peer->frame_interval.numerator, 1);
    WRITE_ONCE(peer->frame_interval.denominator, fps);
  }

unlock:
  mutex_unlock(&max9296_shared_lock);
unlock_power:
  mutex_unlock(&max9296_power_lock);
  return ret;
}

static int max9296_update_shared_fsync_locked(struct max9296_dev *sensor,
                                               unsigned int fps, bool bind) {
  int ret;

  lockdep_assert_held(&sensor->lock);
  mutex_lock(&max9296_fsync_config_lock);
  ret = max9296_configure_shared_fsync_locked(sensor, fps, bind);
  mutex_unlock(&max9296_fsync_config_lock);
  return ret;
}

static void max9296_drop_fsync_contract_locked(struct max9296_dev *sensor) {
  lockdep_assert_held(&sensor->lock);
  mutex_lock(&max9296_fsync_config_lock);
  mutex_lock(&max9296_power_lock);
  sensor->fsync_contract_epoch = 0;
  sensor->fsync_contract_fps = 0;
  mutex_unlock(&max9296_power_lock);
  mutex_unlock(&max9296_fsync_config_lock);
}

static int max9296_set_power(struct max9296_dev *sensor, bool on) {
  int ret = 0;
  bool run;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  mutex_lock(&max9296_power_lock);

  if (on) {
    run = (max9296_power_users++ == 0);
  } else {
    if (WARN_ON(max9296_power_users == 0)) {
      mutex_unlock(&max9296_power_lock);
      return 0;
    }
    run = (--max9296_power_users == 0);
  }

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (%s users:%d %s)", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         on ? "on" : "off", max9296_power_users, run ? "run" : "skip");

  if (run) {
    /* Invalidate every previously programmed fingerprint before the physical
     * transition starts.  This also closes the legacy close/open hole where
     * restart used to skip firmware after a real board power cycle. */
    max9296_hw_epoch++;
    sensor->state.power = MAX9296_STATE_RUNNING;

    if (on)
      ret = max9296_set_power_on(sensor);
    else
      ret = max9296_set_power_off(sensor);

    /* A failed first-user transition never publishes an owner.  The reset
     * helper has already forced PWDN low, and the advanced epoch remains as the
     * fail-closed record that any partial physical sequence invalidated old
     * fingerprints.  Last-off likewise relinquishes the final logical owner;
     * PWDN was disabled even if reset direction setup failed. */
    if (ret && on) {
      WARN_ON(max9296_power_users != 1);
      max9296_power_users = 0;
      /* Direction setup may have failed before the reset helper could establish
       * a safe line state.  Do not call that helper again; PWDN has already
       * been driven inactive, and the next first-user attempt will retry it. */
      max9296_power(sensor, false);
    }
    sensor->state.power = ret ? MAX9296_STATE_FAILED
                              : (on ? MAX9296_STATE_DONE
                                    : MAX9296_STATE_IDLE);

    if (!on)
      max9296_put_orphaned_reset_gpio_locked();

    if (!on || ret)
      ssleep(5); /* settle before any subsequent global power-on */
  }

  mutex_unlock(&max9296_power_lock);

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s end", "RST",
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return ret;
}

/* --------------- Subdev Operations --------------- */

static int max9296_s_power(struct v4l2_subdev *sd, int on) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  int ret = 0;
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (%d)", "RST",
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__, on);

  mutex_lock(&sensor->lock);

  if (READ_ONCE(sensor->dying)) {
    ret = -ENODEV;
    goto out;
  }

  /*
   * Update the power count. The global refcount inside max9296_set_power()
   * decides whether the hardware sequence actually runs, so this function must
   * not touch state.power - and above all must never write the peer's, which
   * used to erase the marker protecting a still-streaming channel.
  */
  if (on) {
    if (sensor->power_count == 0) {
      if (sensor->prepare_lease_held) {
        /* Transfer the one existing global user; do not acquire a second. */
        sensor->prepare_lease_held = false;
        cancel_delayed_work(&sensor->prepare_lease_timeout);
        sensor->prepare_lease_generation = 0;
        if (sensor->prepare_state != MAX9296_PREP_STALE)
          sensor->prepare_state = MAX9296_PREP_CONSUMED;
      } else {
        ret = max9296_set_power(sensor, true);
        if (ret)
          goto out;
      }
    }
    sensor->power_count++;
  } else {
    if (WARN_ON(sensor->power_count <= 0)) {
      ret = -EINVAL;
      goto out;
    }
    sensor->power_count--;

    if (sensor->power_count == 0)
      ret = max9296_set_power(sensor, false);
  }

out:
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

  fmt->reserved[1] = READ_ONCE(sensor->fps);
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
  struct max9296_hw_fingerprint old_fingerprint, new_fingerprint;
  const struct max9296_mode_info *new_mode;
  struct v4l2_mbus_framefmt *mbus_fmt = &format->format;
  struct v4l2_mbus_framefmt *fmt;
  bool old_valid;
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

  if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
    fmt = v4l2_subdev_get_try_format(sd, cfg, 0);
    *fmt = *mbus_fmt;
    goto out;
  }

  if (READ_ONCE(sensor->fps) > new_mode->max_fps) {
    printk(KERN_WARNING
           "[%s:%d][%s:%d] %s mode=%ux%u current_fps=%u max_fps=%u rejected",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, new_mode->width, new_mode->height,
           READ_ONCE(sensor->fps), new_mode->max_fps);
    ret = -EINVAL;
    goto out;
  }

  old_valid = !max9296_normalize_fingerprint_locked(sensor,
                                                     &old_fingerprint);
  if (new_mode != sensor->current_mode)
    sensor->pending_mode_change = true;
  if (mbus_fmt->code != sensor->fmt.code)
    sensor->pending_fmt_change = true;

  sensor->current_mode = new_mode;
  sensor->fmt = *mbus_fmt;

  if (old_valid &&
      !max9296_normalize_fingerprint_locked(sensor, &new_fingerprint) &&
      !max9296_fingerprint_equal(&old_fingerprint, &new_fingerprint))
    max9296_mark_prepare_stale_locked(sensor);

  __v4l2_ctrl_s_ctrl_int64(sensor->ctrls.pixel_rate,
                           max9296_calc_pixel_rate(sensor));
out:

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
  bool dual = max9296_hw_is_dual(sensor);
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

static int max9296_check_exposure_policy(
    struct max9296_dev *sensor, const char *channel, u32 exposure,
    const struct max9296_mode_info *mode, u32 fps, u32 safe_max_fps) {
  int ret;

  if (!mode || !fps || fps > mode->max_fps)
    ret = -EINVAL;
  else if (!safe_max_fps || fps > safe_max_fps)
    ret = -EBUSY;
  else
    return 0;

  printk(KERN_WARNING
         "[%s:%d][%s:%d] exposure write rejected channel=%s "
         "mode=%ux%u(id=%d) fps=%u exposure=%u safe_max_fps=%u ret=%d",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, channel,
         mode ? mode->width : 0, mode ? mode->height : 0,
         mode ? mode->id : -1, fps, exposure, safe_max_fps, ret);
  return ret;
}

/*
 * All AP1302 EXP_TIME (0x500c) traffic must pass this function.  The normal
 * frame-rate limit and the exposure-write safety limit are intentionally
 * separate mode properties: formats may still negotiate above 30 fps, but a
 * risky exposure write is rejected before the I2C transaction.
 */
static int max9296_write_exposure(struct max9296_dev *sensor, u32 i2c_addr,
                                  const char *channel, u32 exposure) {
  const struct max9296_mode_info *mode = sensor->current_mode;
  u32 fps = READ_ONCE(sensor->fps);
  u32 safe_max_fps = mode ? mode->exposure_safe_max_fps : 0;
  int ret;

  ret = max9296_check_exposure_policy(sensor, channel, exposure, mode, fps,
                                      safe_max_fps);
  if (ret)
    return ret;

  return maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_EXP_TIME, exposure,
                             2, 4);
}

static int max9296_preflight_exposure(struct max9296_dev *sensor,
                                       const char *channel, u32 exposure) {
  const struct max9296_mode_info *mode = sensor->current_mode;
  u32 fps = READ_ONCE(sensor->fps);
  u32 safe_max_fps = mode ? mode->exposure_safe_max_fps : 0;

  return max9296_check_exposure_policy(sensor, channel, exposure, mode, fps,
                                       safe_max_fps);
}

static u32 max9296_cached_exposure_value(
    const struct max9296_dev *sensor,
    const struct max9296_channel_ctrl *channel) {
  if (channel->exposure)
    return channel->exposure;
  if (sensor->ctrl_cache.exposure)
    return sensor->ctrl_cache.exposure;
  return 10000;
}

static int max9296_write_exposure_per_channel(struct max9296_dev *sensor,
                                               u32 exposure) {
  int ret;

  if (!max9296_hw_is_dual(sensor))
    return max9296_write_exposure(sensor, AP1302_I2C_ADDR, "single",
                                  exposure);

  ret = max9296_write_exposure(sensor, AP1302_CH0_I2C_ADDR, "ch0", exposure);
  if (ret)
    return ret;

  return max9296_write_exposure(sensor, AP1302_CH1_I2C_ADDR, "ch1",
                                exposure);
}

static u16 max9296_dz_percent_to_fixed8(u32 percent) {
  return (u16)((percent * 0x100 + 50) / 100);
}

static u16 max9296_dz_center_to_fixed8(u32 position) {
  return (u16)((position * 0x100 + 0x7fff) / 0xffff);
}

static int max9296_write_zoom_channel(
    struct max9296_dev *sensor, u32 i2c_addr, const char *channel,
    const struct max9296_channel_ctrl *ctrl, u32 dz_percent) {
  u16 dz = max9296_dz_percent_to_fixed8(dz_percent);
  u16 dz_x = max9296_dz_center_to_fixed8(ctrl->dz_x);
  u16 dz_y = max9296_dz_center_to_fixed8(ctrl->dz_y);
  int ret;

  printk(KERN_NOTICE
         "[%s:%d][%s:%d] zoom apply channel=%s dz=%u(0x%04x) "
         "dz_x=%d(0x%04x) dz_y=%d(0x%04x)",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, channel,
         dz_percent, dz, ctrl->dz_x, dz_x, ctrl->dz_y, dz_y);

  /* 0x1012 is transition speed, not a center coordinate. 0x8000 asks the
   * AP1302 firmware to apply the following target without a visible ramp. */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_DZ_STEP_FCT,
                            AP1302_DZ_STEP_IMMEDIATE, 2, 2);
  if (ret)
    return ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_DZ_CENTER_X, dz_x, 2,
                            2);
  if (ret)
    return ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_DZ_CENTER_Y, dz_y, 2,
                            2);
  if (ret)
    return ret;

  return maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_DZ_TGT_FCT, dz, 2,
                             2);
}

static int max9296_apply_cached_crop(struct max9296_dev *sensor) {
  bool dual;
  char ch0_name[8], ch1_name[8];
  int ret;

  lockdep_assert_held(&sensor->lock);

  if (!sensor->ctrl_cache.crop_enable)
    return 0;

  dual = max9296_hw_is_dual(sensor);
  printk(KERN_NOTICE
         "[%s:%d][%s:%d] crop apply enable=1 topology=%s "
         "dz=%d ch0=(%d,%d) ch1=(%d,%d)",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         dual ? "dual" : "single", sensor->ctrl_cache.dz,
         sensor->ctrl_cache.ch0.dz_x, sensor->ctrl_cache.ch0.dz_y,
         sensor->ctrl_cache.ch1.dz_x, sensor->ctrl_cache.ch1.dz_y);

  if (!dual) {
    const struct max9296_channel_ctrl *active_ctrl =
        sensor->enable == 0x02 ? &sensor->ctrl_cache.ch1
                               : &sensor->ctrl_cache.ch0;

    max9296_fmt_ch(ch0_name, sizeof(ch0_name), sensor, AP1302_I2C_ADDR);
    ret = max9296_write_zoom_channel(sensor, AP1302_I2C_ADDR, ch0_name,
                                     active_ctrl, sensor->ctrl_cache.dz);
    goto out;
  }

  max9296_fmt_ch(ch0_name, sizeof(ch0_name), sensor, AP1302_CH0_I2C_ADDR);
  max9296_fmt_ch(ch1_name, sizeof(ch1_name), sensor, AP1302_CH1_I2C_ADDR);
  ret = max9296_write_zoom_channel(sensor, AP1302_CH0_I2C_ADDR, ch0_name,
                                   &sensor->ctrl_cache.ch0,
                                   sensor->ctrl_cache.dz);
  if (ret)
    goto out;

  ret = max9296_write_zoom_channel(sensor, AP1302_CH1_I2C_ADDR, ch1_name,
                                   &sensor->ctrl_cache.ch1,
                                   sensor->ctrl_cache.dz);

out:
  if (ret)
    printk(KERN_WARNING
           "[%s:%d][%s:%d] crop apply failed topology=%s "
           "dz=%d ch0=(%d,%d) ch1=(%d,%d) ret=%d",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           dual ? "dual" : "single", sensor->ctrl_cache.dz,
           sensor->ctrl_cache.ch0.dz_x, sensor->ctrl_cache.ch0.dz_y,
           sensor->ctrl_cache.ch1.dz_x, sensor->ctrl_cache.ch1.dz_y, ret);
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

static const char *awb_mode_name(int mode) {
  switch (mode & AP1302_AWB_MODE_MASK) {
    case AP1302_AWB_MODE_OFF:     return "off";
    case AP1302_AWB_MODE_HORIZON: return "horizon";
    case AP1302_AWB_MODE_A:       return "a";
    case AP1302_AWB_MODE_CWF:     return "cwf";
    case AP1302_AWB_MODE_D50:     return "d50";
    case AP1302_AWB_MODE_D65:     return "d65";
    case AP1302_AWB_MODE_D75:     return "d75";
    case AP1302_AWB_MODE_TEMP:    return "temp";
    case AP1302_AWB_MODE_MEASURE: return "measure";
    case AP1302_AWB_MODE_AUTO:    return "auto";
    default:                      return "?";
  }
}

static int max9296_set_ctrl_white_balance(struct max9296_dev *sensor, int awb) {
  u16 awb_val = AP1302_AWB_CTRL_FROM_MODE(awb);

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s awb:%s(0x%04x)", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         awb_mode_name(awb), awb_val);

  return max9296_write_per_channel(sensor, AP1302_REG_AWB_CTRL, awb_val, 2, 2);
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
    ret = max9296_write_exposure_per_channel(sensor, exp_val);
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
  bool dual = max9296_hw_is_dual(sensor);
  u32 ch0_addr = dual ? AP1302_CH0_I2C_ADDR : AP1302_I2C_ADDR;
  u32 ch1_addr = dual ? AP1302_CH1_I2C_ADDR : AP1302_I2C_ADDR;

  switch (ctrl->id) {
  case V4L2_CID_DMA_REG_READ_CH0: {
    u16 reg_addr = sensor->ctrl_cache.dma_read_addr_ch0;
    u16 read_val = 0;
    int ret;

    if (sensor->power_count == 0 || !sensor->ctrl_cache.firmware_ready)
      return 0;

    ret = max9296_dma_read_reg(sensor, ch0_addr, reg_addr, &read_val);
    if (ret)
      return ret;

    ctrl->val = (reg_addr << 16) | read_val;
    break;
  }
  case V4L2_CID_DMA_REG_READ_CH1: {
    u16 reg_addr = sensor->ctrl_cache.dma_read_addr_ch1;
    u16 read_val = 0;
    int ret;

    if (sensor->power_count == 0 || !sensor->ctrl_cache.firmware_ready)
      return 0;

    ret = max9296_dma_read_reg(sensor, ch1_addr, reg_addr, &read_val);
    if (ret)
      return ret;

    ctrl->val = (reg_addr << 16) | read_val;
    break;
  }
  default:
    break;
  }
  return 0;
}

static void max9296_cache_ctrl(struct max9296_dev *sensor,
                               struct v4l2_ctrl *ctrl) {
  switch (ctrl->id) {
  /* Shared controls */
  case V4L2_CID_EXP_TIME:
    /* Shared exposure time: keep both channels in sync by default */
    sensor->ctrl_cache.exposure = ctrl->val;
    sensor->ctrl_cache.ch0.exposure = ctrl->val;
    sensor->ctrl_cache.ch1.exposure = ctrl->val;
    break;
  case V4L2_CID_DZ:
    sensor->ctrl_cache.dz = ctrl->val;
    break;
  case V4L2_CID_DZ_X:
    sensor->ctrl_cache.dz_x = ctrl->val;
    sensor->ctrl_cache.ch0.dz_x = ctrl->val;
    sensor->ctrl_cache.ch1.dz_x = ctrl->val;
    break;
  case V4L2_CID_DZ_Y:
    sensor->ctrl_cache.dz_y = ctrl->val;
    sensor->ctrl_cache.ch0.dz_y = ctrl->val;
    sensor->ctrl_cache.ch1.dz_y = ctrl->val;
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
  case V4L2_CID_LED_FLASH_CH0:
    sensor->ctrl_cache.ch0.led_flash = ctrl->val;
    break;
  case V4L2_CID_DZ_X_CH0:
    sensor->ctrl_cache.ch0.dz_x = ctrl->val;
    break;
  case V4L2_CID_DZ_Y_CH0:
    sensor->ctrl_cache.ch0.dz_y = ctrl->val;
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
  case V4L2_CID_LED_FLASH_CH1:
    sensor->ctrl_cache.ch1.led_flash = ctrl->val;
    break;
  case V4L2_CID_DZ_X_CH1:
    sensor->ctrl_cache.ch1.dz_x = ctrl->val;
    break;
  case V4L2_CID_DZ_Y_CH1:
    sensor->ctrl_cache.ch1.dz_y = ctrl->val;
    break;

  /* MCP4018 digital potentiometer */
  case V4L2_CID_MCP4018_WIPER:
    sensor->ctrl_cache.mcp4018_wiper = ctrl->val;
    break;
  case V4L2_CID_MCP4018_WIPER_CH1:
    sensor->ctrl_cache.mcp4018_wiper_ch1 = ctrl->val;
    break;
  case V4L2_CID_MCP4018_POWER_CH0:
    sensor->ctrl_cache.mcp4018_power = ctrl->val ? 1 : 0;
    break;
  case V4L2_CID_MCP4018_POWER_CH1:
    sensor->ctrl_cache.mcp4018_power_ch1 = ctrl->val ? 1 : 0;
    break;

  /* DMA register access - cache read address */
  case V4L2_CID_DMA_REG_READ_CH0:
    sensor->ctrl_cache.dma_read_addr_ch0 = (u16)(ctrl->val >> 16);
    break;
  case V4L2_CID_DMA_REG_READ_CH1:
    sensor->ctrl_cache.dma_read_addr_ch1 = (u16)(ctrl->val >> 16);
    break;
  case V4L2_CID_DMA_REG_WRITE_CH0:
  case V4L2_CID_DMA_REG_WRITE_CH1:
    /* Write-only: no persistent cache needed */
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
static int max9296_apply_channel_controls(struct max9296_dev *sensor,
                                          u32 i2c_addr,
                                          struct max9296_channel_ctrl *ch_ctrl,
                                          u8 ser_addr, u8 mcp4018_host,
                                          u8 mcp4018_wiper,
                                          const char *ch_name,
                                          const char *mode_name) {
  /* Pre-compute all values so entry log shows the full plan. */
  bool mcp_active  = (ch_ctrl->led_flash & (1 << 8));
  u8   flash_delay = (u8)(ch_ctrl->led_flash & 0xff);
  u16  ae_val      = ch_ctrl->ae_on ? AP1302_AE_CTRL_AUTO
                                    : AP1302_AE_CTRL_MANUAL;
  u16  awb_val     = AP1302_AWB_CTRL_FROM_MODE(ch_ctrl->awb);
  u16  gain_seed   = ch_ctrl->gain ? ch_ctrl->gain : 256;
  u16  rot         = (ch_ctrl->hflip ? 0x01 : 0x00) |
                     (ch_ctrl->vflip ? 0x02 : 0x00);
  u32  exp_seed    = ch_ctrl->exposure
                         ? ch_ctrl->exposure
                         : (sensor->ctrl_cache.exposure
                                ? sensor->ctrl_cache.exposure
                                : 10000);
  u32 fps = READ_ONCE(sensor->fps);
  u32 safe_max_fps = sensor->current_mode
                         ? sensor->current_mode->exposure_safe_max_fps
                         : 0;
  bool skip_exposure_seed = ch_ctrl->ae_on && fps > safe_max_fps;
  int ret;
  int first_err = 0;

  if (!skip_exposure_seed) {
    ret = max9296_preflight_exposure(sensor, ch_name, exp_seed);
    if (ret)
      return ret;
  }

  /* Entry log — what is about to be applied. */
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s %s apply (addr:0x%02x ae:%s "
                     "awb:%s(0x%04x) gain:%d exp:%u rot:0x%02x mcp:%s "
                     "wiper:0x%02x delay:0x%02x)",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         ch_name, mode_name,
         i2c_addr, ch_ctrl->ae_on ? "on" : "off",
         awb_mode_name(ch_ctrl->awb), awb_val, gain_seed, exp_seed, rot,
         mcp_active ? "on" : "off", mcp4018_wiper, flash_delay);

  if (!skip_exposure_seed) {
    /* STEP 1: Initialize AE to manual mode first. */
    ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_CTRL,
                              AP1302_AE_CTRL_MANUAL, 2, 2);
    if (ret && !first_err)
      first_err = ret;
    msleep(100);

    /* Seed exposure time while in manual mode. Some FW revisions need a
     * non-zero seed before switching to AE auto. */
    ret = max9296_write_exposure(sensor, i2c_addr, ch_name, exp_seed);
    if (ret && !first_err)
      first_err = ret;
    msleep(100);
  }

  /* STEP 2: Apply configured AE mode (auto/manual) */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_CTRL, ae_val, 2, 2);
  if (ret && !first_err)
    first_err = ret;
  if (ch_ctrl->ae_on)
    msleep(100);

  /* AWB: ch_ctrl->awb holds AWB_CTRL MODE (0x0~0xf). */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AWB_CTRL, awb_val, 2, 2);
  if (ret && !first_err)
    first_err = ret;

  /* Gain value (always set, used when switching to manual) */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_GAIN, gain_seed, 2, 2);
  if (ret && !first_err)
    first_err = ret;

  /* Rotation (hflip + vflip combined) */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_ROTATION, rot, 2, 2);
  if (ret && !first_err)
    first_err = ret;

  /* Per-channel tuning values */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_LSC_CTRL,
                            ch_ctrl->lsc, 2, 2);
  if (ret && !first_err)
    first_err = ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_BRIGHTNESS,
                            ch_ctrl->brightness, 2, 2);
  if (ret && !first_err)
    first_err = ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_CONTRAST,
                            ch_ctrl->contrast, 2, 2);
  if (ret && !first_err)
    first_err = ret;
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_SATURATION,
                            ch_ctrl->saturation, 2, 2);
  if (ret && !first_err)
    first_err = ret;

  /* LED flash (AR0234 R0x3270 via AP1302 DMA). Firmware routes to the
   * correct physical sensor in both dual and single modes. */
  ret = max9296_dma_write_reg(sensor, i2c_addr, AR0234_REG_LED_FLASH_CONTROL,
                              (u16)ch_ctrl->led_flash);
  if (ret && !first_err)
    first_err = ret;

  /* MCP4018 wiper — atomic open/write/close. Gated on flash enable bit:
   * when the flash is disabled the LED/MCP4018 chain may be unpopulated. */
  if (mcp_active) {
    ret = max9295_mfp4_set(sensor, ser_addr, true);
    if (ret && !first_err)
      first_err = ret;
    if (!ret) {
      ret = mcp4018_write_wiper(sensor, mcp4018_host, mcp4018_wiper,
                                ser_addr);
      if (ret && !first_err)
        first_err = ret;
    }
    ret = max9295_mfp4_set(sensor, ser_addr, false);
    if (ret && !first_err)
      first_err = ret;
  }

  /* Exit log — high-level result. Details are in the per-step logs above. */
  if (first_err)
    printk(KERN_ERR "[%s:%d][%s:%d] %s %s applied fail (ret=%d)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           ch_name, mode_name, first_err);
  else
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s %s applied success",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           ch_name, mode_name);

  /* Exposure-policy failures return above, before the first I2C write. Keep
   * the established best-effort behavior for admissible AE/gain/tuning/LED
   * replay: report operational I2C errors, but do not invalidate prepare. */
  return 0;
}

static int max9296_apply_cached_controls(struct max9296_dev *sensor) {
  bool dual = max9296_hw_is_dual(sensor);
  int i2c_nr = sensor->i2c_client->adapter->nr;
  const char *ch0_name, *ch1_name;
  int first_err = 0;
  int ret;

  lockdep_assert_held(&sensor->lock);
  sensor->ctrl_cache.firmware_ready = false;

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
    /* Dual-channel mode: apply each channel's settings separately.
     * MCP4018 per-port wiper is inlined via max9296_apply_channel_controls. */
    ret = max9296_apply_channel_controls(
        sensor, AP1302_CH0_I2C_ADDR, &sensor->ctrl_cache.ch0,
        MAX9295_SER_ADDR_CH0, MCP4018_HOST_ADDR,
        (u8)sensor->ctrl_cache.mcp4018_wiper, ch0_name, "dual");
    if (ret && !first_err)
      first_err = ret;
    ret = max9296_apply_channel_controls(
        sensor, AP1302_CH1_I2C_ADDR, &sensor->ctrl_cache.ch1,
        MAX9295_SER_ADDR_CH1, MCP4018_HOST_ADDR_CH1,
        (u8)sensor->ctrl_cache.mcp4018_wiper_ch1, ch1_name, "dual");
    if (ret && !first_err)
      first_err = ret;
  } else {
    /* Single-channel mode: apply ch0 cache slot to global address.
     * sensor->enable bitmask identifies the active local channel
     * (0x01 = local ch0 / Port A, 0x02 = local ch1 / Port B);
     * add link_status.ch_shift to get the global channel number.
     * MCP4018 is hardware-direct: pick the port matching the active local ch.
     */
    char single_name[8];
    unsigned int local_ch = (sensor->enable == 0x02) ? 1 : 0;
    unsigned int global_ch = sensor->link_status.ch_shift + local_ch;
    u8 ser   = max9296_ser_addr(sensor, local_ch);
    u8 host  = local_ch ? MCP4018_HOST_ADDR_CH1 : MCP4018_HOST_ADDR;
    u8 wiper = local_ch ? (u8)sensor->ctrl_cache.mcp4018_wiper_ch1
                        : (u8)sensor->ctrl_cache.mcp4018_wiper;
    struct max9296_channel_ctrl *active_ctrl =
        local_ch ? &sensor->ctrl_cache.ch1 : &sensor->ctrl_cache.ch0;
    snprintf(single_name, sizeof(single_name), "ch%u", global_ch);
    ret = max9296_apply_channel_controls(sensor, AP1302_I2C_ADDR, active_ctrl,
                                         ser, host, wiper, single_name,
                                         "single");
    if (ret)
      first_err = ret;
  }

  if (!first_err)
    sensor->ctrl_cache.firmware_ready = true;
  return first_err;
}

static int max9296_s_ctrl(struct v4l2_ctrl *ctrl) {
  struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  int ret;
  bool dual = max9296_hw_is_dual(sensor);
  u32 ch0_addr = dual ? AP1302_CH0_I2C_ADDR : AP1302_I2C_ADDR;
  u32 ch1_addr = dual ? AP1302_CH1_I2C_ADDR : AP1302_I2C_ADDR;
  char ch0_name[8], ch1_name[8];

  max9296_fmt_ch(ch0_name, sizeof(ch0_name), sensor, ch0_addr);
  max9296_fmt_ch(ch1_name, sizeof(ch1_name), sensor, ch1_addr);

  if (debug)
    printk(
        KERN_NOTICE
        "[%s:%d][%s:%d] %s ctrl->id:0x%x ctrl->val:%d fw_ready:%d pw_cnt:%d\n",
        KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
        __FUNCTION__, ctrl->id, ctrl->val, sensor->ctrl_cache.firmware_ready,
        sensor->power_count);
  /* v4l2_ctrl_lock() locks our own mutex */

  if (ctrl->id == V4L2_CID_CROP_ENABLE) {
    bool requested = !!ctrl->val;

    if (sensor->streaming)
      return -EBUSY;
    if (sensor->ctrl_cache.crop_enable != requested) {
      sensor->ctrl_cache.crop_enable = requested;
      max9296_mark_prepare_stale_locked(sensor);
    }
    return 0;
  }

  /* A control that would write EXP_TIME must be rejected before any I2C
   * side effect (including the preceding AE_CTRL manual-mode write).  During
   * probe/power-off there is no I2C transaction, so defaults remain cacheable
   * and the same policy is enforced when firmware restoration reaches HW. */
  switch (ctrl->id) {
  case V4L2_CID_EXP_TIME:
    ret = max9296_preflight_exposure(sensor, "shared", ctrl->val);
    break;
  case V4L2_CID_EXPOSURE_CH0:
    ret = max9296_preflight_exposure(sensor, ch0_name, ctrl->val);
    break;
  case V4L2_CID_EXPOSURE_CH1:
    ret = max9296_preflight_exposure(sensor, ch1_name, ctrl->val);
    break;
  case V4L2_CID_EXPOSURE_AUTO_CH0:
    if (!ctrl->val) {
      u32 exposure = max9296_cached_exposure_value(
          sensor, &sensor->ctrl_cache.ch0);
      ret = max9296_preflight_exposure(sensor, ch0_name, exposure);
    } else {
      ret = 0;
    }
    break;
  case V4L2_CID_EXPOSURE_AUTO_CH1:
    if (!ctrl->val) {
      u32 exposure = max9296_cached_exposure_value(
          sensor, &sensor->ctrl_cache.ch1);
      ret = max9296_preflight_exposure(sensor, ch1_name, exposure);
    } else {
      ret = 0;
    }
    break;
  default:
    ret = 0;
    break;
  }
  if (ret)
    return ret;

  /* A clustered member update is delivered through the master (dz). Snapshot
   * every requested member before one full-tuple hardware apply. */
  if (ctrl == sensor->ctrls.dz) {
    if (sensor->ctrls.dz->is_new)
      sensor->ctrl_cache.dz = sensor->ctrls.dz->val;
    if (sensor->ctrls.dz_x_ch0->is_new)
      sensor->ctrl_cache.ch0.dz_x = sensor->ctrls.dz_x_ch0->val;
    if (sensor->ctrls.dz_y_ch0->is_new)
      sensor->ctrl_cache.ch0.dz_y = sensor->ctrls.dz_y_ch0->val;
    if (sensor->ctrls.dz_x_ch1->is_new)
      sensor->ctrl_cache.ch1.dz_x = sensor->ctrls.dz_x_ch1->val;
    if (sensor->ctrls.dz_y_ch1->is_new)
      sensor->ctrl_cache.ch1.dz_y = sensor->ctrls.dz_y_ch1->val;
  } else {
    /* Always update cache when the request is admissible (even if powered
     * off). Common dz_x/dz_y aliases intentionally update both channels. */
    max9296_cache_ctrl(sensor, ctrl);
  }

  /* S_FMT may describe the next topology while last_mode still describes the
   * AP1302/MAX9295 addresses currently programmed in hardware. Keep the new
   * desired value, but do not send it into the stale topology; prepare will
   * replay the cache after programming the requested mode. */
  if (sensor->pending_mode_change || sensor->pending_fmt_change)
    return 0;

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
    /* Shared exposure time: always write to global 0x3c (applies to both channels) */
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s EXP_TIME ctrl->val:%d cache.exp:%d "
                       "ch0.exp:%d ch1.exp:%d",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, ctrl->val, sensor->ctrl_cache.exposure,
           sensor->ctrl_cache.ch0.exposure, sensor->ctrl_cache.ch1.exposure);
    ret = max9296_write_exposure(sensor, AP1302_I2C_ADDR, "shared",
                                 ctrl->val);
    break;
  case V4L2_CID_DZ:
  case V4L2_CID_DZ_X:
  case V4L2_CID_DZ_Y:
    ret = max9296_apply_cached_crop(sensor);
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
    ret = max9296_write_exposure(sensor, ch0_addr, ch0_name, exp_val);
    }
    break;
  }
  case V4L2_CID_AUTO_WHITE_BALANCE_CH0: {
    u16 awb_val = AP1302_AWB_CTRL_FROM_MODE(ctrl->val);
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
    ret = max9296_write_exposure(sensor, ch0_addr, ch0_name, ctrl->val);
    break;
  case V4L2_CID_DZ_X_CH0:
  case V4L2_CID_DZ_Y_CH0:
    ret = max9296_apply_cached_crop(sensor);
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
  case V4L2_CID_LED_FLASH_CH0:
    ret = max9296_dma_write_reg(sensor, ch0_addr, AR0234_REG_LED_FLASH_CONTROL, (u16)ctrl->val);
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
    ret = max9296_write_exposure(sensor, ch1_addr, ch1_name, exp_val);
    }
    break;
  }
  case V4L2_CID_AUTO_WHITE_BALANCE_CH1: {
    u16 awb_val = AP1302_AWB_CTRL_FROM_MODE(ctrl->val);
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
    ret = max9296_write_exposure(sensor, ch1_addr, ch1_name, ctrl->val);
    break;
  case V4L2_CID_DZ_X_CH1:
  case V4L2_CID_DZ_Y_CH1:
    ret = max9296_apply_cached_crop(sensor);
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
  case V4L2_CID_LED_FLASH_CH1:
    ret = max9296_dma_write_reg(sensor, ch1_addr, AR0234_REG_LED_FLASH_CONTROL, (u16)ctrl->val);
    break;

  /* MCP4018 digital potentiometer (atomic: open I2C gate -> write -> close).
   * Collision-safe vs the shared host addr 0x2F between Port A (local CH0) and
   * Port B (local CH1). Works in both adapters — on adapter 1 the "CH0/CH1"
   * slots physically drive global ch2/ch3.
   */
  case V4L2_CID_MCP4018_WIPER: {
    u8 ser = max9296_ser_addr(sensor, 0);

    max9295_mfp4_set(sensor, ser, true);
    ret = mcp4018_write_wiper(sensor, MCP4018_HOST_ADDR, (u8)ctrl->val, ser);
    max9295_mfp4_set(sensor, ser, false);
    break;
  }
  case V4L2_CID_MCP4018_WIPER_CH1: {
    u8 ser;

    if (!max9296_ch_ctrl_applies(sensor, 1)) {
      ret = 0;
      break;
    }
    ser = max9296_ser_addr(sensor, 1);
    max9295_mfp4_set(sensor, ser, true);
    ret = mcp4018_write_wiper(sensor, MCP4018_HOST_ADDR_CH1, (u8)ctrl->val, ser);
    max9295_mfp4_set(sensor, ser, false);
    break;
  }
  /* Standalone power toggle retained as diagnostic handle (not used by gstApp) */
  case V4L2_CID_MCP4018_POWER_CH0:
    ret = max9295_mfp4_set(sensor, max9296_ser_addr(sensor, 0), !!ctrl->val);
    break;
  case V4L2_CID_MCP4018_POWER_CH1:
    if (!max9296_ch_ctrl_applies(sensor, 1)) {
      ret = 0;
      break;
    }
    ret = max9295_mfp4_set(sensor, max9296_ser_addr(sensor, 1), !!ctrl->val);
    break;

  /* Generic DMA register write: [31:16]=reg_addr, [15:0]=data */
  case V4L2_CID_DMA_REG_WRITE_CH0: {
    u16 reg_addr = (u16)(ctrl->val >> 16);
    u16 reg_data = (u16)(ctrl->val & 0xFFFF);
    ret = max9296_dma_write_reg(sensor, ch0_addr, reg_addr, reg_data);
    break;
  }
  case V4L2_CID_DMA_REG_WRITE_CH1: {
    u16 reg_addr = (u16)(ctrl->val >> 16);
    u16 reg_data = (u16)(ctrl->val & 0xFFFF);
    ret = max9296_dma_write_reg(sensor, ch1_addr, reg_addr, reg_data);
    break;
  }

  /* DMA register read: address cached, actual read in g_volatile_ctrl */
  case V4L2_CID_DMA_REG_READ_CH0:
  case V4L2_CID_DMA_REG_READ_CH1:
    ret = 0;
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
              V4L2_CTRL_TYPE_INTEGER, "AWB Mode", 0, AP1302_AWB_MODE_MASK,
              AP1302_AWB_MODE_AUTO, auto_wb_ch0, auto_wb_ch1),
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
    CTRL_DESC(V4L2_CID_LED_FLASH_CH0, V4L2_CID_LED_FLASH_CH1,
              V4L2_CTRL_TYPE_INTEGER, "LED Flash", 0, 0x1ff, 0, led_flash_ch0,
              led_flash_ch1),
    CTRL_DESC(V4L2_CID_DZ_X_CH0, V4L2_CID_DZ_X_CH1,
              V4L2_CTRL_TYPE_INTEGER, "DZ X", 0, 65535,
              MAX9296_DZ_CENTER_DEFAULT, dz_x_ch0, dz_x_ch1),
    CTRL_DESC(V4L2_CID_DZ_Y_CH0, V4L2_CID_DZ_Y_CH1,
              V4L2_CTRL_TYPE_INTEGER, "DZ Y", 0, 65535,
              MAX9296_DZ_CENTER_DEFAULT, dz_y_ch0, dz_y_ch1),
};

static int max9296_init_controls(struct max9296_dev *sensor) {
  const struct v4l2_ctrl_ops *ops = &max9296_ctrl_ops;
  struct max9296_ctrls *ctrls = &sensor->ctrls;
  struct v4l2_ctrl_handler *hdl = &ctrls->handler;
  int ret;
  //printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  v4l2_ctrl_handler_init(hdl, 65);

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
  {
    static const struct v4l2_ctrl_config cfg_crop_enable = {
        .ops = &max9296_ctrl_ops,
        .id = V4L2_CID_CROP_ENABLE,
        .type = V4L2_CTRL_TYPE_BOOLEAN,
        .name = "crop_enable",
        .min = 0,
        .max = 1,
        .def = 0,
        .step = 1,
    };
    static const struct v4l2_ctrl_config cfg_dz = {
        .ops = &max9296_ctrl_ops,
        .id = V4L2_CID_DZ,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "dz",
        .min = MAX9296_DZ_MIN,
        .max = MAX9296_DZ_MAX,
        .def = MAX9296_DZ_DEFAULT,
        .step = 1,
    };
    static const struct v4l2_ctrl_config cfg_dz_x = {
        .ops = &max9296_ctrl_ops,
        .id = V4L2_CID_DZ_X,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "dz_x",
        .min = 0,
        .max = 65535,
        .def = MAX9296_DZ_CENTER_DEFAULT,
        .step = 1,
    };
    static const struct v4l2_ctrl_config cfg_dz_y = {
        .ops = &max9296_ctrl_ops,
        .id = V4L2_CID_DZ_Y,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "dz_y",
        .min = 0,
        .max = 65535,
        .def = MAX9296_DZ_CENTER_DEFAULT,
        .step = 1,
    };

    ctrls->crop_enable =
        v4l2_ctrl_new_custom(hdl, &cfg_crop_enable, NULL);
    ctrls->dz = v4l2_ctrl_new_custom(hdl, &cfg_dz, NULL);
    ctrls->dz_x = v4l2_ctrl_new_custom(hdl, &cfg_dz_x, NULL);
    ctrls->dz_y = v4l2_ctrl_new_custom(hdl, &cfg_dz_y, NULL);
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

  ctrls->crop_cluster[0] = ctrls->dz;
  ctrls->crop_cluster[1] = ctrls->dz_x_ch0;
  ctrls->crop_cluster[2] = ctrls->dz_y_ch0;
  ctrls->crop_cluster[3] = ctrls->dz_x_ch1;
  ctrls->crop_cluster[4] = ctrls->dz_y_ch1;
  if (ctrls->crop_cluster[0] && ctrls->crop_cluster[1] &&
      ctrls->crop_cluster[2] && ctrls->crop_cluster[3] &&
      ctrls->crop_cluster[4])
    v4l2_ctrl_cluster(ARRAY_SIZE(ctrls->crop_cluster), ctrls->crop_cluster);

  /* MCP4018 digital potentiometer wiper control */
  {
    bool second = (sensor->i2c_client->adapter->nr == 1);
    int ch0_num = second ? 2 : 0;
    int ch1_num = second ? 3 : 1;

    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_MCP4018_WIPER,
          .type = V4L2_CTRL_TYPE_INTEGER,
          .min = 0,
          .max = MCP4018_WIPER_MAX,
          .def = MCP4018_WIPER_DEFAULT,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "MCP4018 Wiper CH%d", ch0_num);
      ctrls->mcp4018_wiper = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_MCP4018_WIPER_CH1,
          .type = V4L2_CTRL_TYPE_INTEGER,
          .min = 0,
          .max = MCP4018_WIPER_MAX,
          .def = MCP4018_WIPER_DEFAULT,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "MCP4018 Wiper CH%d", ch1_num);
      ctrls->mcp4018_wiper_ch1 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
  }

  /* MCP4018 VCC power (MAX9295 MFP4 GPIO) */
  {
    bool second = (sensor->i2c_client->adapter->nr == 1);
    int ch0_num = second ? 2 : 0;
    int ch1_num = second ? 3 : 1;

    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_MCP4018_POWER_CH0,
          .type = V4L2_CTRL_TYPE_BOOLEAN,
          .min = 0,
          .max = 1,
          .def = 0,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "MCP4018 Power CH%d", ch0_num);
      if (!cfg.name)
        return -ENOMEM;
      ctrls->mcp4018_power = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_MCP4018_POWER_CH1,
          .type = V4L2_CTRL_TYPE_BOOLEAN,
          .min = 0,
          .max = 1,
          .def = 0,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "MCP4018 Power CH%d", ch1_num);
      if (!cfg.name)
        return -ENOMEM;
      ctrls->mcp4018_power_ch1 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
  }

  /* Generic AR0234 DMA register access controls */
  {
    bool second = (sensor->i2c_client->adapter->nr == 1);
    int ch0_num = second ? 2 : 0;
    int ch1_num = second ? 3 : 1;

    /* DMA Write CH0 */
    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_DMA_REG_WRITE_CH0,
          .type = V4L2_CTRL_TYPE_INTEGER,
          .min = 0,
          .max = 0x7FFFFFFF,
          .def = 0,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "DMA Reg Write CH%d", ch0_num);
      ctrls->dma_reg_write_ch0 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
    /* DMA Write CH1 */
    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_DMA_REG_WRITE_CH1,
          .type = V4L2_CTRL_TYPE_INTEGER,
          .min = 0,
          .max = 0x7FFFFFFF,
          .def = 0,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "DMA Reg Write CH%d", ch1_num);
      ctrls->dma_reg_write_ch1 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
    /* DMA Read CH0 (volatile - reads HW on get) */
    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_DMA_REG_READ_CH0,
          .type = V4L2_CTRL_TYPE_INTEGER,
          .flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
          .min = 0,
          .max = 0x7FFFFFFF,
          .def = 0,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "DMA Reg Read CH%d", ch0_num);
      ctrls->dma_reg_read_ch0 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
    }
    /* DMA Read CH1 (volatile - reads HW on get) */
    {
      struct v4l2_ctrl_config cfg = {
          .ops = &max9296_ctrl_ops,
          .id = V4L2_CID_DMA_REG_READ_CH1,
          .type = V4L2_CTRL_TYPE_INTEGER,
          .flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
          .min = 0,
          .max = 0x7FFFFFFF,
          .def = 0,
          .step = 1,
      };
      cfg.name = devm_kasprintf(&sensor->i2c_client->dev, GFP_KERNEL,
                                "DMA Reg Read CH%d", ch1_num);
      ctrls->dma_reg_read_ch1 = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
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
  const struct max9296_mode_info *mode;

  if (fie->pad != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s fie->pad:%d return err", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
           fie->pad);
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

  mode = max9296_find_mode(sensor, fie->width, fie->height, false);
  if (!mode || fie->index >= mode->max_fps) {
    if (debug)
      printk(KERN_CRIT
             "[%s:%d][%s:%d] %s mode=%ux%u index=%u max_fps=%u return err",
             KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
             __FUNCTION__, fie->width, fie->height, fie->index,
             mode ? mode->max_fps : 0);
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
  fi->interval.numerator =
      READ_ONCE(sensor->frame_interval.numerator);
  fi->interval.denominator =
      READ_ONCE(sensor->frame_interval.denominator);
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
  unsigned int old_fps;
  unsigned int fps;
  int ret = 0;

  if (fi->pad != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s return err", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
    return -EINVAL;
  }

  mutex_lock(&sensor->lock);

  if (fi->interval.numerator == 0 || fi->interval.denominator == 0) {
    ret = -EINVAL;
    goto out;
  }

  fps = fi->interval.denominator / fi->interval.numerator;

  if (fps < 1 || fps > MAX9296_360P_MAX_FPS) {
    printk(KERN_CRIT "[%s:%d][%s:%d] %s invalid fps %u (valid: 1~120)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, fps);
    ret = -EINVAL;
    goto out;
  }

  if (!sensor->current_mode || fps > sensor->current_mode->max_fps) {
    printk(KERN_WARNING
           "[%s:%d][%s:%d] %s mode=%ux%u fps=%u max_fps=%u rejected",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, sensor->current_mode ? sensor->current_mode->width : 0,
           sensor->current_mode ? sensor->current_mode->height : 0, fps,
           sensor->current_mode ? sensor->current_mode->max_fps : 0);
    ret = -EINVAL;
    goto out;
  }

  old_fps = READ_ONCE(sensor->fps);
  ret = max9296_update_shared_fsync_locked(sensor, fps, false);
  if (ret)
    goto out;
  if (old_fps != fps)
    max9296_mark_prepare_stale_locked(sensor);

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
  if (ret)
    return ret;
  ret = maxim_ops_i2c_write(sensor, 0x3c, 0x6024, 0x00300000, 2, 4);
  if (ret)
    return ret;
  ret = maxim_ops_i2c_write(sensor, 0x3c, 0x6034, 0x012c0000, 2, 4);
  if (ret)
    return ret;

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
  int ret;

  client->addr = 0x3c;
  ret = i2c_master_send(client, data, size);
  client->addr = addr;

  if (ret < 0) {
    v4l_err(client, "firmware load i2c failure: %d\n", ret);
    return ret;
  }
  if (ret != size) {
    v4l_err(client, "firmware load short i2c write: %d/%d\n", ret, size);
    return -EIO;
  }

  return 0;
}

static const char *get_fw_name(struct i2c_client *client) {
  if (firmware[0])
    return firmware;
  else
    return MAX9296_FIRMWARE;
}
static int max9296_loadfw(struct i2c_client *client) {
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
  retval = request_firmware(&fw, fwname, FWDEV(client));
  if (retval) {
    // v4l_err(client, "unable to open firmware %s\n", fwname);
    printk(KERN_CRIT "[%s:%d][%s:%d] unable to open firmware %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, fwname);
    return retval;
  }

  retval = start_fw_load(client);
  if (retval < 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] start firmware load i2c failure", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__);
    release_firmware(fw);
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
    release_firmware(fw);
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
  return 0;
}

MODULE_FIRMWARE(MAX9296_FIRMWARE);

/* Resolve the requested format to the exact register table that will be
 * programmed.  The right-hand single-channel tables share their public mode
 * ids with the left-hand tables, so current_mode alone is not a complete
 * hardware identity. */
static const struct max9296_mode_info *
max9296_resolve_prepare_mode_locked(const struct max9296_dev *sensor) {
  const struct max9296_mode_info *mode = sensor->current_mode;

  if (!mode)
    return NULL;

  switch (mode->id) {
  case MAX9296_MODE_1280x720:
    return sensor->enable == 0x02 ? &max9296_mode_data_HD_R
                                  : &max9296_mode_data[MAX9296_MODE_1280x720];
  case MAX9296_MODE_1920x1080:
    return sensor->enable == 0x02
               ? &max9296_mode_data_FHD_R
               : &max9296_mode_data[MAX9296_MODE_1920x1080];
  case MAX9296_MODE_640x360:
    return sensor->enable == 0x02
               ? &max9296_mode_data_360_R
               : &max9296_mode_data[MAX9296_MODE_640x360];
  case MAX9296_MODE_2560x720:
    return &max9296_mode_data[MAX9296_MODE_2560x720];
  case MAX9296_MODE_3840x1080:
    return &max9296_mode_data[MAX9296_MODE_3840x1080];
  case MAX9296_MODE_1280x360:
    return &max9296_mode_data[MAX9296_MODE_1280x360];
  default:
    return NULL;
  }
}

static int max9296_normalize_fingerprint_locked(
    const struct max9296_dev *sensor, struct max9296_hw_fingerprint *fingerprint) {
  const struct max9296_mode_info *mode;
  u32 fps;

  mode = max9296_resolve_prepare_mode_locked(sensor);
  if (!mode)
    return -EINVAL;

  fps = READ_ONCE(sensor->fps);
  if (!fps || fps > mode->max_fps)
    return -EINVAL;

  fingerprint->mode = mode;
  fingerprint->width = mode->width;
  fingerprint->height = mode->height;
  fingerprint->code = sensor->fmt.code;
  fingerprint->fps = fps;
  fingerprint->enable = sensor->enable;
  fingerprint->crop_enable = sensor->ctrl_cache.crop_enable;

  return 0;
}

static bool max9296_fingerprint_equal(
    const struct max9296_hw_fingerprint *left,
    const struct max9296_hw_fingerprint *right) {
  return left->mode == right->mode && left->width == right->width &&
         left->height == right->height && left->code == right->code &&
         left->fps == right->fps && left->enable == right->enable &&
         left->crop_enable == right->crop_enable;
}

/* Runtime negotiation is allowed after prepare, but it must not silently keep
 * READY/CONSUMED attached to a different hardware tuple.  Hardware validity is
 * intentionally retained so STREAMON reports -ESTALE instead of attempting an
 * unsafe same-power-lifetime dual/single table switch. */
static void max9296_mark_prepare_stale_locked(struct max9296_dev *sensor) {
  lockdep_assert_held(&sensor->lock);

  if (sensor->prepare_state == MAX9296_PREP_READY ||
      sensor->prepare_state == MAX9296_PREP_CONSUMED)
    sensor->prepare_state = MAX9296_PREP_STALE;
}

static bool max9296_prepare_matches_locked(
    const struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint) {
  const struct max9296_hw_fingerprint *initialized =
      &sensor->initialized_fingerprint;

  return max9296_fingerprint_equal(initialized, fingerprint);
}

static int max9296_set_mode(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint) {
  const struct max9296_mode_info *mode = fingerprint->mode;
  int ret;

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s width:%d, height:%d, enable:0x%02x",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, mode->width, mode->height, fingerprint->enable);
  sensor->state.init = MAX9296_STATE_RUNNING;
  sensor->last_mode = mode;

  ret = max9296_load_regs(sensor, mode);

  sensor->state.init = ret ? MAX9296_STATE_FAILED : MAX9296_STATE_DONE;

  return ret;
}

static int max9296_program_preview_context_channel(
    struct max9296_dev *sensor, u32 addr, u16 width, u16 height, u32 fps,
    unsigned int sensor_mode) {
  unsigned int current_value;
  int finish_ret;
  int ret;

  ret = maxim_ops_i2c_write(sensor, addr, AP1302_REG_ATOMIC,
                            AP1302_ATOMIC_BEGIN, 2, 2);
  if (ret)
    return ret;

#define PREVIEW_WRITE(_reg, _val)                                          \
  do {                                                                     \
    ret = maxim_ops_i2c_write(sensor, addr, (_reg), (_val), 2, 2);         \
    if (ret)                                                               \
      goto finish;                                                         \
  } while (0)

  PREVIEW_WRITE(AP1302_REG_PREVIEW_WIDTH, width);
  PREVIEW_WRITE(AP1302_REG_PREVIEW_HEIGHT, height);
  PREVIEW_WRITE(AP1302_REG_PREVIEW_ROI_X0, MAX9296_PREVIEW_ROI_X0);
  PREVIEW_WRITE(AP1302_REG_PREVIEW_ROI_Y0, MAX9296_PREVIEW_ROI_Y0);
  PREVIEW_WRITE(AP1302_REG_PREVIEW_ROI_X1, MAX9296_PREVIEW_ROI_X1);
  PREVIEW_WRITE(AP1302_REG_PREVIEW_ROI_Y1, MAX9296_PREVIEW_ROI_Y1);
  PREVIEW_WRITE(AP1302_REG_PREVIEW_ASPECT, MAX9296_PREVIEW_ASPECT);

  if (sensor_mode != MAX9296_360P_SENSOR_MODE_KEEP) {
    ret = maxim_ops_i2c_read(sensor, addr, AP1302_REG_PREVIEW_SENSOR_MODE, 2,
                             2, &current_value);
    if (ret)
      goto finish;
    PREVIEW_WRITE(AP1302_REG_PREVIEW_SENSOR_MODE,
                  max9296_preview_sensor_mode(current_value, 2U,
                                              sensor_mode));
  }

  if (max9296_preview_output_uses_high_fps(width, height, fps)) {
    PREVIEW_WRITE(AP1302_REG_PREVIEW_MAX_FPS,
                  max9296_preview_max_fps_fixed8(fps));
    PREVIEW_WRITE(AP1302_REG_TRIGGER_MAX_MISMATCH, 0x0000);
  }

finish:
  finish_ret = maxim_ops_i2c_write(sensor, addr, AP1302_REG_ATOMIC,
                                   AP1302_ATOMIC_FINISH, 2, 2);
#undef PREVIEW_WRITE
  return ret ? ret : finish_ret;
}

static bool max9296_fingerprint_mode_is_known(
    const struct max9296_mode_info *mode) {
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE(max9296_mode_data); i++)
    if (mode == &max9296_mode_data[i])
      return true;

  return mode == &max9296_mode_data_HD_R ||
         mode == &max9296_mode_data_FHD_R ||
         mode == &max9296_mode_data_360_R;
}

static int max9296_preflight_prepare_locked(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint) {
  const struct max9296_mode_info *mode = fingerprint->mode;
  struct max9296_channel_ctrl *channel;
  char channel_name[8];
  unsigned int local_channel;
  unsigned int first_channel;
  unsigned int channel_count;
  bool right_mode;
  int ret;

  lockdep_assert_held(&sensor->lock);

  if (!max9296_fingerprint_mode_is_known(mode) ||
      fingerprint->width != mode->width ||
      fingerprint->height != mode->height ||
      fingerprint->code != MEDIA_BUS_FMT_UYVY8_2X8 ||
      fingerprint->fps < 1 || fingerprint->fps > mode->max_fps ||
      fingerprint->crop_enable != sensor->ctrl_cache.crop_enable)
    return -EINVAL;

  right_mode = mode == &max9296_mode_data_HD_R ||
               mode == &max9296_mode_data_FHD_R ||
               mode == &max9296_mode_data_360_R;
  if ((max9296_mode_is_dual(mode) && fingerprint->enable != 0x03) ||
      (!max9296_mode_is_dual(mode) && fingerprint->enable != 0x01 &&
       fingerprint->enable != 0x02) ||
      (right_mode && fingerprint->enable != 0x02) ||
      (!max9296_mode_is_dual(mode) && !right_mode &&
       fingerprint->enable != 0x01))
    return -EINVAL;

  if (sensor->ctrl_cache.dz < MAX9296_DZ_MIN ||
      sensor->ctrl_cache.dz > MAX9296_DZ_MAX ||
      sensor->ctrl_cache.ch0.dz_x < 0 ||
      sensor->ctrl_cache.ch0.dz_x > 65535 ||
      sensor->ctrl_cache.ch0.dz_y < 0 ||
      sensor->ctrl_cache.ch0.dz_y > 65535 ||
      sensor->ctrl_cache.ch1.dz_x < 0 ||
      sensor->ctrl_cache.ch1.dz_x > 65535 ||
      sensor->ctrl_cache.ch1.dz_y < 0 ||
      sensor->ctrl_cache.ch1.dz_y > 65535)
    return -EINVAL;

  first_channel = max9296_mode_is_dual(mode)
                      ? 0
                      : (fingerprint->enable == 0x02 ? 1 : 0);
  channel_count = max9296_mode_is_dual(mode) ? 2 : 1;
  for (local_channel = first_channel;
       local_channel < first_channel + channel_count; local_channel++) {
    channel = local_channel ? &sensor->ctrl_cache.ch1
                            : &sensor->ctrl_cache.ch0;
    if (channel->ae_on)
      continue;

    snprintf(channel_name, sizeof(channel_name), "ch%u",
             sensor->link_status.ch_shift + local_channel);
    ret = max9296_check_exposure_policy(
        sensor, channel_name, max9296_cached_exposure_value(sensor, channel),
        mode, fingerprint->fps, mode->exposure_safe_max_fps);
    if (ret)
      return ret;
  }

  return 0;
}

/* Program the registers that historically followed firmware completion in
 * s_stream().  Keep this separate from stream commit: it must not enable MIPI,
 * start FSYNC, publish stream_on, or mark the subdevice streaming. */
static int max9296_post_firmware_program_locked(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint) {
  const struct max9296_mode_info *mode = fingerprint->mode;
  u32 output_width = max9296_mode_is_dual(mode) ? mode->width / 2
                                                : mode->width;
  u32 output_height = mode->height;
  const u32 dual_addrs[] = {AP1302_CH0_I2C_ADDR, AP1302_CH1_I2C_ADDR};
  unsigned int sensor_mode =
      max9296_preview_sensor_mode_override(output_width, output_height);
  unsigned int channel_count = max9296_mode_is_dual(mode) ? 2 : 1;
  unsigned int channel;
  int ret;

  sensor->state.enable = MAX9296_STATE_RUNNING;

  for (channel = 0; channel < channel_count; channel++) {
    u32 addr = channel_count == 1 ? AP1302_I2C_ADDR : dual_addrs[channel];

    if (sensor_mode == MAX9296_360P_SENSOR_MODE_KEEP)
      printk(KERN_NOTICE
             "[%s:%d][%s:%d] preview addr=0x%02x output=%ux%u fps=%u "
             "sensor_mode=KEEP",
             KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, addr,
             output_width, output_height, fingerprint->fps);
    else
      printk(KERN_NOTICE
             "[%s:%d][%s:%d] preview addr=0x%02x output=%ux%u fps=%u "
             "sensor_mode=%u",
             KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, addr,
             output_width, output_height, fingerprint->fps, sensor_mode);

    ret = max9296_program_preview_context_channel(
        sensor, addr, output_width, output_height, fingerprint->fps,
        sensor_mode);
    if (ret)
      goto failed;
  }

  ret = maxim_ops_i2c_write(sensor, 0x3c, 0x1186, 0x038a, 2, 2);
  if (ret)
    goto failed;

  if (max9296_mode_is_dual(mode)) {
    ret = maxim_ops_i2c_write(sensor, 0x00, 0x0471, 0x83, 2, 1);
    if (ret)
      goto failed;
    msleep(500);
  }

  sensor->state.enable = MAX9296_STATE_DONE;
  return 0;

failed:
  sensor->state.enable = MAX9296_STATE_FAILED;
  return ret;
}

/*
 * Synchronous, truthful initialization shared by the legacy STREAMON path and
 * the forthcoming pre-GStreamer prepare command.  sensor->lock is held by the
 * caller.  A fingerprint is published only after every hardware step succeeds.
 */
static int max9296_prepare_hardware_locked(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint) {
  int ret;

  ret = max9296_preflight_prepare_locked(sensor, fingerprint);
  if (ret)
    goto failed;

  sensor->hardware_valid = false;
  sensor->initialized_epoch = 0;
  sensor->stream_commit_epoch = 0;
  sensor->ctrl_cache.firmware_ready = false;
  sensor->state.firmware = MAX9296_STATE_IDLE;
  sensor->state.enable = MAX9296_STATE_IDLE;

  ret = max9296_set_mode(sensor, fingerprint);
  if (ret)
    goto failed;

  sensor->state.firmware = MAX9296_STATE_RUNNING;
  ret = max9296_loadfw(sensor->i2c_client);
  if (ret) {
    sensor->state.firmware = MAX9296_STATE_FAILED;
    goto failed;
  }
  sensor->state.firmware = MAX9296_STATE_DONE;

  ret = max9296_post_firmware_program_locked(sensor, fingerprint);
  if (ret)
    goto failed;

  ret = max9296_apply_cached_crop(sensor);
  if (ret)
    goto failed;

  ret = max9296_apply_cached_controls(sensor);
  if (ret)
    goto failed;

  sensor->initialized_fingerprint = *fingerprint;
  sensor->initialized_epoch = READ_ONCE(max9296_hw_epoch);
  sensor->hardware_valid = true;

  return 0;

failed:
  sensor->hardware_valid = false;
  sensor->initialized_epoch = 0;
  sensor->stream_commit_epoch = 0;
  sensor->ctrl_cache.firmware_ready = false;
  return ret;
}

static void max9296_apply_prepare_fingerprint_locked(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint) {
  const struct max9296_mode_info *mode = fingerprint->mode;

  lockdep_assert_held(&sensor->lock);

  /* current_mode is the public mode selection.  The normalized fingerprint
   * still retains the right-hand table pointer as the exact hardware identity. */
  if (mode == &max9296_mode_data_HD_R)
    mode = &max9296_mode_data[MAX9296_MODE_1280x720];
  else if (mode == &max9296_mode_data_FHD_R)
    mode = &max9296_mode_data[MAX9296_MODE_1920x1080];
  else if (mode == &max9296_mode_data_360_R)
    mode = &max9296_mode_data[MAX9296_MODE_640x360];

  sensor->current_mode = mode;
  sensor->fmt.width = fingerprint->width;
  sensor->fmt.height = fingerprint->height;
  sensor->fmt.code = fingerprint->code;
  WRITE_ONCE(sensor->fps, fingerprint->fps);
  WRITE_ONCE(sensor->frame_interval.numerator, 1);
  WRITE_ONCE(sensor->frame_interval.denominator, fingerprint->fps);
  sensor->enable = fingerprint->enable;
  sensor->ctrl_cache.crop_enable = fingerprint->crop_enable;
  sensor->pending_mode_change = false;
  sensor->pending_fmt_change = false;

  __v4l2_ctrl_s_ctrl_int64(sensor->ctrls.pixel_rate,
                           max9296_calc_pixel_rate(sensor));
}

/*
 * Reclassify a leaked V4L2 power reference as this prepare's lease.
 *
 * The vendor ISI capture driver takes a subdev power reference when it builds
 * the pipeline and never returns it: imx8-isi-cap.c contains a single
 * s_power call (on), and its release path reaches only s_stream(0).  An idle
 * instance therefore keeps references that no owner will ever hand back, which
 * used to make every later prepare fail with -EBUSY until the i2c device was
 * rebound.  Measured on pim-camera-v016: s_power(1) 14, s_power(0) 0.
 *
 * A reference nobody owns is exactly what a lease is, so adopt it rather than
 * acquiring a second one.  Every outstanding local reference is backed by one
 * global user, so folding them into the lease leaves max9296_power_users
 * unchanged, keeps the board free of a power transition, and preserves the
 * retained hardware fingerprint for warm reuse.  It also maintains the
 * "lease xor power_count" invariant the removal path reconciles.
 *
 * Callers must have rejected an actively streaming owner first.
 *
 * Returns true when a reference was adopted, false when the caller still has to
 * acquire one.
 */
static bool max9296_prepare_adopt_power_locked(struct max9296_dev *sensor) {
  lockdep_assert_held(&sensor->lock);

  if (sensor->power_count <= 0)
    return false;

  sensor->power_count = 0;
  return true;
}

/*
 * Start one driver-owned prepare while sensor->lock is held.  The sysfs ABI is
 * deliberately added by Task 4; keeping ownership here makes that caller a
 * parser/admission layer rather than a second power-accounting implementation.
 */
static int max9296_prepare_request_locked(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint, u64 generation) {
  u64 epoch;
  bool adopted;
  int ret;

  ret = max9296_preflight_prepare_locked(sensor, fingerprint);
  if (ret)
    return ret;

  /* Keep power acquisition and the first FSYNC reservation in one short
   * transaction.  This closes the gap where a legacy frame-interval write
   * could land after reset but before a prepare published its cadence. */
  mutex_lock(&max9296_fsync_config_lock);
  adopted = max9296_prepare_adopt_power_locked(sensor);
  ret = adopted ? 0 : max9296_set_power(sensor, true);
  if (ret) {
    mutex_unlock(&max9296_fsync_config_lock);
    return ret;
  }
  if (READ_ONCE(sensor->dying) ||
      !READ_ONCE(sensor->shared.probe_ready)) {
    ret = READ_ONCE(sensor->dying) ? -ENODEV : -EAGAIN;
    goto release_unpublished_power;
  }
  ret = max9296_configure_shared_fsync_locked(
      sensor, fingerprint->fps, true);
  if (ret)
    goto release_unpublished_power;
  mutex_unlock(&max9296_fsync_config_lock);

  max9296_apply_prepare_fingerprint_locked(sensor, fingerprint);
  sensor->prepare_state = MAX9296_PREP_PREPARING;
  sensor->prepare_generation = generation;
  sensor->prepare_lease_generation = 0;
  sensor->prepare_fingerprint = *fingerprint;
  sensor->prepare_errno = 0;
  sensor->prepare_lease_held = true;

  /* An expired request can leave valid hardware behind while a peer keeps the
   * board powered.  Acquiring this instance's user does not reset in that
   * case, so reuse the exact current fingerprint instead of downloading the
   * firmware again.  A first-user acquisition advances the epoch and falls
   * through to the normal synchronous initialization. */
  epoch = READ_ONCE(max9296_hw_epoch);
  if (!sensor->hardware_valid || sensor->initialized_epoch != epoch ||
      !max9296_prepare_matches_locked(sensor, fingerprint)) {
    ret = max9296_prepare_hardware_locked(sensor, fingerprint);
    if (ret)
      goto release_failed_lease;
  }

  sensor->prepare_state = MAX9296_PREP_READY;
  sensor->prepare_lease_generation = generation;
  return 0;

release_failed_lease:
  max9296_drop_fsync_contract_locked(sensor);
  sensor->prepare_releasing = true;
  sensor->prepare_lease_held = false;
  max9296_set_power(sensor, false);
  sensor->prepare_releasing = false;
  sensor->prepare_errno = ret;
  sensor->prepare_state = MAX9296_PREP_FAILED;
  return ret;

release_unpublished_power:
  max9296_set_power(sensor, false);
  mutex_unlock(&max9296_fsync_config_lock);
  return ret;
}

static bool max9296_prepare_lease_can_arm_locked(
    const struct max9296_dev *sensor, u64 generation) {
  return !READ_ONCE(sensor->dying) && sensor->prepare_lease_held &&
         sensor->power_count == 0 &&
         (sensor->prepare_state == MAX9296_PREP_READY ||
          sensor->prepare_state == MAX9296_PREP_STALE) &&
         sensor->prepare_generation == generation &&
         sensor->prepare_lease_generation == generation;
}

static int max9296_queue_prepare_lease_locked(struct max9296_dev *sensor,
                                               u64 generation) {
  if (!max9296_prepare_lease_can_arm_locked(sensor, generation))
    return -ESTALE;
  if (WARN_ON(!queue_delayed_work(system_wq,
                                  &sensor->prepare_lease_timeout, 60 * HZ)))
    return -EBUSY;
  return 0;
}

static int max9296_prepare_existing_lease_locked(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint, u64 generation) {
  int ret;

  sensor->prepare_state = MAX9296_PREP_PREPARING;
  sensor->prepare_generation = generation;
  sensor->prepare_lease_generation = 0;
  sensor->prepare_fingerprint = *fingerprint;
  sensor->prepare_errno = 0;

  ret = max9296_prepare_hardware_locked(sensor, fingerprint);
  if (ret) {
    max9296_drop_fsync_contract_locked(sensor);
    sensor->prepare_releasing = true;
    sensor->prepare_lease_held = false;
    max9296_set_power(sensor, false);
    sensor->prepare_releasing = false;
    sensor->prepare_errno = ret;
    sensor->prepare_state = MAX9296_PREP_FAILED;
    return ret;
  }

  sensor->prepare_state = MAX9296_PREP_READY;
  sensor->prepare_lease_generation = generation;
  return 0;
}

/*
 * A single delayed_work object cannot carry an immutable per-arm generation:
 * changing fields before an already-running callback has drained lets that old
 * callback observe and release the new lease.  Serialize request/rearm callers,
 * synchronously drain the one work item without sensor->lock, then take the
 * sensor lock, revalidate the current ownership, and queue a fresh invocation.
 */
static int max9296_prepare_request(
    struct max9296_dev *sensor,
    const struct max9296_hw_fingerprint *fingerprint, u64 generation) {
  u64 epoch;
  bool hardware_current;
  int arm_ret;
  int ret;

  if (!mutex_trylock(&sensor->prepare_request_lock))
    return READ_ONCE(sensor->dying) ? -ENODEV : -EBUSY;
  cancel_delayed_work_sync(&sensor->prepare_lease_timeout);

  mutex_lock(&sensor->lock);
  if (READ_ONCE(sensor->dying)) {
    ret = -ENODEV;
    goto unlock;
  }
  if (!READ_ONCE(sensor->shared.probe_ready)) {
    ret = -EAGAIN;
    goto unlock;
  }
  if (sensor->prepare_state == MAX9296_PREP_PREPARING ||
      sensor->prepare_releasing) {
    ret = -EBUSY;
    goto unlock;
  }
  /* power_count is not evidence of a live owner on this BSP.  The vendor
   * capture driver never returns its s_power(1) reference, so an idle instance
   * keeps a count that no close will ever clear.  Only an actively streaming
   * owner may reject a prepare; a leaked idle reference is folded into the
   * lease by max9296_prepare_adopt_power_locked(). */
  if (sensor->streaming) {
    ret = -EBUSY;
    goto unlock;
  }

  ret = max9296_preflight_prepare_locked(sensor, fingerprint);
  if (ret)
    goto preserve_lease;

  /* One orchestration generation cannot be rebound to another command. */
  if (sensor->prepare_generation == generation && generation != 0 &&
      !max9296_fingerprint_equal(&sensor->prepare_fingerprint, fingerprint)) {
    ret = -ESTALE;
    goto preserve_lease;
  }

  epoch = READ_ONCE(max9296_hw_epoch);
  hardware_current = sensor->hardware_valid &&
                     sensor->initialized_epoch == epoch;
  if (hardware_current &&
      !max9296_prepare_matches_locked(sensor, fingerprint)) {
    /* Changing exact dual/left/right programming within one hardware epoch is
     * unsafe because serializer address routing may already have changed. */
    ret = -ESTALE;
    goto preserve_lease;
  }

  if (sensor->prepare_lease_held) {
    ret = max9296_update_shared_fsync_locked(
        sensor, fingerprint->fps, true);
    if (ret)
      goto preserve_lease;
    max9296_apply_prepare_fingerprint_locked(sensor, fingerprint);

    if (hardware_current) {
      /* Same generation+tuple is idempotent.  A new generation with the same
       * hardware tuple renews only request identity and timeout. */
      sensor->prepare_generation = generation;
      sensor->prepare_lease_generation = generation;
      sensor->prepare_fingerprint = *fingerprint;
      sensor->prepare_errno = 0;
      sensor->prepare_state = MAX9296_PREP_READY;
      ret = max9296_queue_prepare_lease_locked(sensor, generation);
    } else {
      /* The lease still owns power but its fingerprint was invalidated by a
       * real external epoch transition.  Reuse the ownership, not the stale
       * initialization result. */
      ret = max9296_prepare_existing_lease_locked(sensor, fingerprint,
                                                  generation);
      if (!ret)
        ret = max9296_queue_prepare_lease_locked(sensor, generation);
    }
    if (ret)
      goto release_unarmed_lease;
    goto unlock;
  }

  /* Fresh ownership acquires board power and reserves shared FSYNC inside the
   * common helper before it publishes any request/live tuple. */
  ret = max9296_prepare_request_locked(sensor, fingerprint, generation);
  if (ret)
    goto unlock;

  if (!max9296_prepare_lease_can_arm_locked(sensor, generation) ||
      WARN_ON(!queue_delayed_work(system_wq,
                                  &sensor->prepare_lease_timeout, 60 * HZ))) {
    ret = READ_ONCE(sensor->dying) ? -ENODEV : -EBUSY;
    goto release_unarmed_lease;
  }
  goto unlock;

preserve_lease:
  sensor->prepare_errno = ret;
  if (sensor->prepare_lease_held) {
    arm_ret = max9296_queue_prepare_lease_locked(
        sensor, sensor->prepare_generation);

    if (arm_ret) {
      ret = arm_ret;
      goto release_unarmed_lease;
    }
  }
  goto unlock;

release_unarmed_lease:
  if (sensor->prepare_lease_held) {
    /* No timeout/V4L2 owner can preserve this request.  Do not leave its
     * per-instance cadence blocking a peer that keeps the epoch powered. */
    max9296_drop_fsync_contract_locked(sensor);
    sensor->prepare_lease_held = false;
    sensor->prepare_lease_generation = 0;
    sensor->prepare_errno = ret;
    sensor->prepare_state = MAX9296_PREP_FAILED;
    sensor->prepare_releasing = true;
    max9296_set_power(sensor, false);
    sensor->prepare_releasing = false;
  }

unlock:
  mutex_unlock(&sensor->lock);
  mutex_unlock(&sensor->prepare_request_lock);
  return ret;
}

static int max9296_cancel_prepare(struct max9296_dev *sensor) {
  int ret = 0;

  if (!mutex_trylock(&sensor->prepare_request_lock))
    return READ_ONCE(sensor->dying) ? -ENODEV : -EBUSY;
  cancel_delayed_work_sync(&sensor->prepare_lease_timeout);

  mutex_lock(&sensor->lock);
  if (READ_ONCE(sensor->dying)) {
    ret = -ENODEV;
    goto unlock;
  }
  if (!READ_ONCE(sensor->shared.probe_ready)) {
    ret = -EAGAIN;
    goto unlock;
  }
  if (sensor->prepare_state == MAX9296_PREP_PREPARING ||
      sensor->prepare_releasing) {
    ret = -EBUSY;
    goto unlock;
  }
  if (sensor->streaming || sensor->power_count > 0) {
    ret = -EBUSY;
    goto unlock;
  }

  if (sensor->prepare_lease_held) {
    sensor->prepare_releasing = true;
    sensor->prepare_lease_held = false;
    sensor->prepare_lease_generation = 0;
    ret = max9296_set_power(sensor, false);
    sensor->prepare_releasing = false;
  }
  sensor->prepare_errno = ret;
  sensor->prepare_state = MAX9296_PREP_IDLE;

unlock:
  mutex_unlock(&sensor->lock);
  mutex_unlock(&sensor->prepare_request_lock);
  return ret;
}

/* An expiry never waits for or cancels initialization.  A worker admitted for
 * an older request also cannot return the ownership of a newer generation. */
static void max9296_prepare_lease_timeout(struct work_struct *work) {
  struct max9296_dev *sensor =
      container_of(to_delayed_work(work), struct max9296_dev,
                   prepare_lease_timeout);
  int ret;

  mutex_lock(&sensor->lock);
  if (max9296_prepare_lease_can_arm_locked(
          sensor, sensor->prepare_lease_generation)) {
    sensor->prepare_releasing = true;
    sensor->prepare_lease_held = false;
    sensor->prepare_lease_generation = 0;
    sensor->prepare_state = MAX9296_PREP_EXPIRED;
    ret = max9296_set_power(sensor, false);
    sensor->prepare_errno = ret;
    sensor->prepare_releasing = false;
  }
  mutex_unlock(&sensor->lock);
}

static bool max9296_stream_epoch_current(const struct max9296_dev *sensor) {
  u64 epoch = READ_ONCE(max9296_hw_epoch);

  return READ_ONCE(sensor->streaming) && READ_ONCE(sensor->hardware_valid) &&
         READ_ONCE(sensor->initialized_epoch) == epoch &&
         READ_ONCE(sensor->stream_commit_epoch) == epoch;
}

/* Final FSYNC authority.  max9296_power_lock is also the epoch mutation lock,
 * so no power transition/removal invalidation can land between the predicate
 * and either edge of a pulse.  Callers pass the stream(s) whose current epoch
 * authorizes this one shared-board pulse. */
static bool max9296_fsync_pulse_current(
    struct max9296_dev *gpio_owner, struct max9296_dev *primary,
    struct max9296_dev *secondary, unsigned int high, unsigned int low) {
  bool pulse;

  mutex_lock(&max9296_power_lock);
  pulse = !READ_ONCE(gpio_owner->dying) &&
          !READ_ONCE(primary->dying) &&
          max9296_stream_epoch_current(primary) &&
          (!secondary ||
           (!READ_ONCE(secondary->dying) &&
            max9296_stream_epoch_current(secondary)));
  if (pulse) {
    gpiod_set_value_cansleep(gpio_owner->fsync_gpio, 1);
    usleep_range(high, high);
    gpiod_set_value_cansleep(gpio_owner->fsync_gpio, 0);
  }
  mutex_unlock(&max9296_power_lock);

  if (pulse)
    usleep_range(low, low);
  return pulse;
}

/* sensor->lock makes STREAMOFF atomic with this output commit; the nested
 * board lock makes epoch invalidation atomic with every executable 0x0313
 * write. */
static bool max9296_enable_output_locked(struct max9296_dev *sensor) {
  bool enabled = false;

  lockdep_assert_held(&sensor->lock);
  mutex_lock(&max9296_power_lock);
  if (!sensor->dying && max9296_stream_epoch_current(sensor) &&
      sensor->state.fsync == MAX9296_STATE_RUNNING) {
    sensor->stream_on = 0;
    if (sensor->current_mode->id == MAX9296_MODE_1280x720 ||
        sensor->current_mode->id == MAX9296_MODE_640x360)
      maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x02, 2, 1);
    else
      maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x82, 2, 1);
    enabled = true;
  }
  mutex_unlock(&max9296_power_lock);

  return enabled;
}

static void max9296_stream_commit_locked(struct max9296_dev *sensor) {
  /* Both prepare-backed and legacy STREAMON reach this point only after the
   * requested fingerprint is programmed or proven to match live hardware. */
  sensor->pending_mode_change = false;
  sensor->pending_fmt_change = false;

  sensor->stream_commit_epoch = READ_ONCE(max9296_hw_epoch);
  sensor->stream_on = 1;
  sensor->restart = 0;
}

static int max9296_s_stream(struct v4l2_subdev *sd, int enable) {
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct max9296_hw_fingerprint fingerprint;
  u64 epoch;
  int worker_errno;
  int ret = 0;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (%d)", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__,
         enable);

  /* PREPARING holds sensor->lock across seconds of firmware I/O.  Do not make
   * STREAMON disappear into that wait: sample the published state, try the
   * lock, then repeat every admission check while locked before acting. */
  if (enable) {
    if (READ_ONCE(sensor->dying))
      return -ENODEV;
    if (READ_ONCE(sensor->prepare_state) == MAX9296_PREP_PREPARING ||
        READ_ONCE(sensor->prepare_releasing))
      return -EBUSY;
    worker_errno = READ_ONCE(sensor->worker_errno);
    if (worker_errno)
      return worker_errno;
    if (!mutex_trylock(&sensor->prepare_request_lock))
      return READ_ONCE(sensor->dying) ? -ENODEV : -EBUSY;
    if (READ_ONCE(sensor->dying)) {
      mutex_unlock(&sensor->prepare_request_lock);
      return -ENODEV;
    }
    if (!mutex_trylock(&sensor->lock)) {
      ret = READ_ONCE(sensor->dying) ? -ENODEV : -EBUSY;
      mutex_unlock(&sensor->prepare_request_lock);
      return ret;
    }
  } else {
    mutex_lock(&sensor->lock);
  }

  if (READ_ONCE(sensor->dying)) {
    ret = -ENODEV;
    goto out;
  }
  if (sensor->prepare_state == MAX9296_PREP_PREPARING ||
      sensor->prepare_releasing) {
    ret = -EBUSY;
    goto out;
  }

  if (enable) {
    worker_errno = READ_ONCE(sensor->worker_errno);
    if (worker_errno) {
      ret = worker_errno;
      goto out;
    }
    ret = max9296_normalize_fingerprint_locked(sensor, &fingerprint);
    if (ret)
      goto out;
    ret = max9296_preflight_prepare_locked(sensor, &fingerprint);
    if (ret)
      goto out;
    ret = max9296_update_shared_fsync_locked(
        sensor, fingerprint.fps, true);
    if (ret)
      goto out;

    epoch = READ_ONCE(max9296_hw_epoch);
    if (sensor->hardware_valid && sensor->initialized_epoch == epoch) {
      if (!max9296_prepare_matches_locked(sensor, &fingerprint)) {
        /* Switching dual/left/right programming without a board reset is not
         * safe: a dual table may have remapped a serializer to 0x60. */
        ret = -ESTALE;
        goto out;
      }
    } else {
      ret = max9296_prepare_hardware_locked(sensor, &fingerprint);
      if (ret) {
        max9296_drop_fsync_contract_locked(sensor);
        goto out;
      }
    }

    /* gstApp configures VideoBin controls before prepare. They remain cached
     * while powered off and prepare deliberately leaves firmware_ready false.
     * Replay that cache here while CSI output is still disabled; the enable
     * worker repeats this under the same mutex to close post-STREAMON updates. */
    ret = max9296_apply_cached_crop(sensor);
    if (ret) {
      sensor->hardware_valid = false;
      sensor->initialized_epoch = 0;
      max9296_drop_fsync_contract_locked(sensor);
      goto out;
    }
    ret = max9296_apply_cached_controls(sensor);
    if (ret) {
      sensor->hardware_valid = false;
      sensor->initialized_epoch = 0;
      max9296_drop_fsync_contract_locked(sensor);
      goto out;
    }

    /* Removal publishes dying under this same epoch/ownership lock.  Recheck
     * after any multi-second fallback initialization and make the final
     * admission plus stream commit one atomic action. */
    mutex_lock(&max9296_power_lock);
    if (READ_ONCE(sensor->dying)) {
      mutex_unlock(&max9296_power_lock);
      ret = -ENODEV;
      goto out;
    }
    if (!sensor->streaming)
      memset(sensor->health.hinf_valid, 0,
             sizeof(sensor->health.hinf_valid));
    max9296_stream_commit_locked(sensor);
    sensor->streaming = true;
    mutex_unlock(&max9296_power_lock);
  } else {
    /* Stop authorizing FSYNC before the physical output-disable write.  The
     * board lock makes this transition atomic with a pulse in progress. */
    mutex_lock(&max9296_power_lock);
    if (sensor->streaming)
      memset(sensor->health.hinf_valid, 0,
             sizeof(sensor->health.hinf_valid));
    sensor->streaming = false;
    sensor->stream_on = 0;
    sensor->stream_commit_epoch = 0;
    sensor->state.fsync = MAX9296_STATE_IDLE;
    if (sensor->shared.sensor != NULL)
      sensor->shared.sensor->state.fsync = MAX9296_STATE_IDLE;
    ret = max9296_disable_stream_mipi(sensor);
    mutex_unlock(&max9296_power_lock);

    sensor->restart = 1;
  }
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
out:
  mutex_unlock(&sensor->lock);
  if (enable)
    mutex_unlock(&sensor->prepare_request_lock);
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
  /*
   * low_fps remembers which fps the current `low` was derived from. Without it
   * the `if (low == 0)` guard computed the period exactly once per thread
   * lifetime - and this thread lives from probe to remove - so a runtime
   * VIDIOC_SUBDEV_S_FRAME_INTERVAL updated sensor->fps while FSYNC kept
   * pulsing at the old rate. max9296_s_frame_interval() writes no hardware
   * register, so this pulse train is the only path fps has to the sensors:
   * freezing it meant runtime fps changes could never take effect.
   */
  unsigned int high = 1000, low = 0, low_fps = 0;
  /*
   * Per-thread, not static: it carries across loop iterations of THIS thread
   * only. As a function-scope static it would be shared by every FSYNC thread
   * in the system - harmless while max9296_0 owns the sole fsync-gpios, but a
   * silent cross-instance coupling the moment a second one is given one.
   */
  unsigned int restart_cnt = 0;
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  while (1) {
    set_current_state(TASK_INTERRUPTIBLE);

    if (kthread_should_stop())
      break;

    if (sensor->restart == 1) {
      restart_cnt = 1;
      usleep_range(300000, 300000);
      continue;
    }

    if (restart_cnt > 0) {
      --restart_cnt;
      usleep_range(1000000, 1000000);
      continue;
    }

    // single mode
    if (sensor->shared.sensor == NULL) {
      if (sensor->state.enable == MAX9296_STATE_DONE &&
          max9296_stream_epoch_current(sensor)) {
        if (sensor->state.fsync != MAX9296_STATE_RUNNING)
          usleep_range(1000000, 1000000);

        if (low_fps != READ_ONCE(sensor->fps)) {
          low_fps = READ_ONCE(sensor->fps);
          low = (1000000 / low_fps) - high;
          printk(KERN_NOTICE
                 "[%s:%d][%s:%d] %s single fps : %u, low : %u, high : %u\n",
                 KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
                 __FUNCTION__, low_fps, low, high);
        }

        if (!max9296_fsync_pulse_current(sensor, sensor, NULL, high, low))
          continue;
        sensor->state.fsync = MAX9296_STATE_RUNNING;
      } else {
        usleep_range(10000, 11000);
      }
    } else /* default dual mode */
    {
      /* dual mode init */
      if ((sensor->state.init == MAX9296_STATE_DONE) &&
          (sensor->shared.sensor->state.init == MAX9296_STATE_DONE) &&
          max9296_stream_epoch_current(sensor) &&
          max9296_stream_epoch_current(sensor->shared.sensor)) {
        if ((sensor->state.enable == MAX9296_STATE_DONE) &&
            (sensor->shared.sensor->state.enable == MAX9296_STATE_DONE)) {
          if (sensor->state.fsync != MAX9296_STATE_RUNNING)
            usleep_range(1000000, 1000000);

          if (sensor->shared.sensor->state.fsync != MAX9296_STATE_RUNNING)
            usleep_range(1000000, 1000000);

          if (low_fps != READ_ONCE(sensor->fps)) {
            low_fps = READ_ONCE(sensor->fps);
            low = (1000000 / low_fps) - high;
            printk(KERN_NOTICE
                   "[%s:%d][%s:%d] %s dual fps : %u, low : %u, high : %u\n",
                   KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
                   __FUNCTION__, low_fps, low, high);
          }

          if (!max9296_fsync_pulse_current(
                  sensor, sensor, sensor->shared.sensor, high, low))
            continue;
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
        /*
         * Only one side has come up (the outer test already ruled out both).
         * Drive FSYNC for whichever side that is.
         *
         * state.setup used to gate these tests as a stand-in for "the other
         * side is idle", but max9296_set_fmt() latches it to DONE on every
         * call - TRY and error paths included - and never clears it. Merely
         * opening the other video node and setting a format was therefore
         * enough to make both tests fail forever: FSYNC never started,
         * state.fsync never reached RUNNING, and max9296_enable() consequently
         * never wrote the CSI output enable (0x0313), so the streaming channel
         * produced no frames. init+enable already say who is actually
         * streaming, so the setup terms are dropped.
         *
         * Keep pulsing while the other side is inside max9296_set_mode(): the
         * side that is already streaming needs FSYNC continuously, and its
         * register table load takes seconds. Nothing races it - max9296_enable()
         * only ever programs its own deserializer now.
         */
        if ((sensor->state.init == MAX9296_STATE_DONE) &&
            (sensor->state.enable == MAX9296_STATE_DONE) &&
            max9296_stream_epoch_current(sensor)) {
          fsync_state = &sensor->state.fsync;
          fps = READ_ONCE(sensor->fps);
          start = 1;
        } else if ((sensor->shared.sensor->state.init == MAX9296_STATE_DONE) &&
                   (sensor->shared.sensor->state.enable ==
                    MAX9296_STATE_DONE) &&
                   max9296_stream_epoch_current(sensor->shared.sensor)) {
          fsync_state = &sensor->shared.sensor->state.fsync;
          fps = READ_ONCE(sensor->shared.sensor->fps);
          start = 1;
        } else {
          usleep_range(10000, 11000);
        }

        if (start) {
          if (*fsync_state != MAX9296_STATE_RUNNING)
            usleep_range(1000000, 1000000);

          if (low_fps != fps) {
            low_fps = fps;
            low = (1000000 / low_fps) - high;
            printk(KERN_NOTICE
                   "[%s:%d][%s:%d] %s side fps : %u, low : %u, high : %u\n",
                   KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
                   __FUNCTION__, low_fps, low, high);
          }

          if (fsync_state == &sensor->state.fsync)
            start = max9296_fsync_pulse_current(
                sensor, sensor, NULL, high, low);
          else
            start = max9296_fsync_pulse_current(
                sensor, sensor->shared.sensor, NULL, high, low);
          if (!start)
            continue;
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
  int crop_ret;

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
        (sensor->state.fsync == MAX9296_STATE_RUNNING) &&
        max9296_stream_epoch_current(sensor)) {
      // pr_emerg("\x1b[34m%s() %d line in %s file : \x1b[0m --- %s\n",
      // __FUNCTION__, __LINE__, __FILE__, dev_name(&sensor->i2c_client->dev));

      msleep_interruptible(300);

      /*
       * Program this deserializer, and only this one.
       *
       * This used to choose between a two-sided write and a self-only write on
       * peer->state.setup, with the two-sided path nested under
       * `if (sensor->fsync_gpio != NULL)` and no else. Two failures came out of
       * that:
       *
       *   - Only max9296_0 owns the board-global FSYNC GPIO, so max9296_1
       *     (i2c3, /dev/video4) entered the two-sided path, failed the inner
       *     test and wrote nothing at all - no CSI output enable, no frames.
       *     max9296_0 could not cover for it: stream_on is a one-shot cleared
       *     on its own first pass, so with /dev/video3 already streaming its
       *     enable thread never ran again.
       *   - When both threads did run, one wrote the other's AP1302 AE/AWB
       *     registers while that instance's own max9296_apply_cached_controls()
       *     wrote the same registers on the same client, unserialized.
       *
       * Every enable thread is already gated on its own stream_on and
       * state.fsync, so having each instance program itself covers every start
       * order with exactly one writer per register. What aligns the two
       * deserializers is the shared FSYNC net, not the pairing of these writes.
       *
       * Done under sensor->lock, which max9296_s_stream() holds for its whole
       * body: the stream can no longer go down between the liveness test and
       * the writes. Without that, a STREAMOFF landing in the sleep above was
       * followed by 0x0313 being switched back on for a channel nobody was
       * streaming - max9296_set_stream_mipi() had already written 0x00 and this
       * pass overwrote it. sensor->streaming is the live flag; state.fsync is
       * cleared by the same stream-off path.
       */
      mutex_lock(&sensor->lock);
      if (sensor->streaming && max9296_stream_epoch_current(sensor) &&
          (sensor->state.fsync == MAX9296_STATE_RUNNING)) {
        /* s_stream() primes the cache before commit, but controls can change
         * after it returns while firmware_ready is still false. Replay the
         * latest enabled crop under the same mutex immediately before 0x0313. A
         * failure leaves stream_on set so the worker retries without emitting
         * a frame from the stale ROI. */
        crop_ret = max9296_apply_cached_crop(sensor);
        if (crop_ret) {
          printk(KERN_ERR
                 "[%s:%d][%s:%d] pre-output crop apply failed: %d; retrying",
                 KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
                 crop_ret);
        } else if (max9296_enable_output_locked(sensor)) {
          printk(KERN_NOTICE "[%s:%d][%s:%d] CSI output enabled",
                 KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__);
        }
      }
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
#define MAX9296_WORKER_FSYNC 0x1U
#define MAX9296_WORKER_ENABLE 0x2U
#define MAX9296_WORKER_ALL (MAX9296_WORKER_FSYNC | MAX9296_WORKER_ENABLE)

/* max9296_shared_lock pins every raw peer inspected here.  The per-instance
 * enable worker is always required.  FSYNC is supplied either by this device
 * or by its published GPIO-owning peer. */
static int max9296_worker_status_locked(struct max9296_dev *sensor) {
  struct max9296_dev *owner;
  int worker_errno;

  lockdep_assert_held(&max9296_shared_lock);

  worker_errno = READ_ONCE(sensor->worker_errno);
  if (!sensor->thread_en || IS_ERR(sensor->thread_en))
    return worker_errno < 0 ? worker_errno : -ENODEV;

  if (sensor->fsync_gpio) {
    owner = sensor;
  } else {
    owner = READ_ONCE(sensor->shared.sensor);
    if (!owner || !READ_ONCE(owner->shared.probe_ready) ||
        READ_ONCE(owner->dying) || !owner->fsync_gpio)
      return worker_errno < 0 && worker_errno != -EAGAIN ? worker_errno
                                                         : -ENODEV;
  }

  if (!owner->thread_fsync || IS_ERR(owner->thread_fsync)) {
    int owner_errno;

    owner_errno = READ_ONCE(owner->worker_errno);

    if (owner_errno < 0 && owner_errno != -EAGAIN)
      return owner_errno;
    return worker_errno < 0 && worker_errno != -EAGAIN ? worker_errno
                                                         : -ENODEV;
  }

  return 0;
}

static void max9296_refresh_worker_status_locked(struct max9296_dev *sensor) {
  int worker_errno;

  lockdep_assert_held(&max9296_shared_lock);
  worker_errno = max9296_worker_status_locked(sensor);
  WRITE_ONCE(sensor->worker_errno, worker_errno);
}

/* Start only the requested missing workers.  A live task pointer is never
 * replaced, so reciprocal relink and retry cannot create duplicates.  On a
 * partial failure the already-started worker remains owned by the sensor and
 * the normal probe/remove cleanup stops it exactly once. */
static int max9296_start_workers(struct max9296_dev *sensor,
                                 unsigned int workers) {
  struct task_struct *task;
  char name[64];
  int ret;

  /* Probe and serialized peer detach are the only callers.  Neither can race
   * another start for this sensor, so a NULL slot remains private until the
   * validated live task is published below. */
  if ((workers & MAX9296_WORKER_FSYNC) && sensor->fsync_gpio &&
      !sensor->thread_fsync) {
    task = kthread_run(max9296_fsync, sensor, "max9296_fsync");
    if (IS_ERR(task)) {
      ret = PTR_ERR(task);
      WRITE_ONCE(sensor->worker_errno, ret);
      return ret;
    }
    WRITE_ONCE(sensor->thread_fsync, task);
  }

  if ((workers & MAX9296_WORKER_ENABLE) && !sensor->thread_en) {
    snprintf(name, sizeof(name), "max9296_enable_%s",
             dev_name(&sensor->i2c_client->dev));
    task = kthread_run(max9296_enable, sensor, name);
    if (IS_ERR(task)) {
      ret = PTR_ERR(task);
      WRITE_ONCE(sensor->worker_errno, ret);
      return ret;
    }
    WRITE_ONCE(sensor->thread_en, task);
  }

  return 0;
}

//-------------------------------------------------------------------------
static int max9296_shared_init(void *data) {
  struct max9296_dev *sensor = (struct max9296_dev *)data;
  bool resolved;

  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  while (1) {
    set_current_state(TASK_INTERRUPTIBLE);

    if (kthread_should_stop())
      break;

    mutex_lock(&max9296_shared_lock);
    resolved = max9296_ready_shared_peer_locked(sensor) != NULL;
    mutex_unlock(&max9296_shared_lock);

    if (resolved)
      break;

    usleep_range(10000, 11000);
  }
  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;
}

/* Release only resources owned by this failed probe.  probe_ready is committed
 * after the final fallible setup step, so no sibling resolver can have
 * published this never-bound devm object. */
static void max9296_release_probe_shared(struct max9296_dev *sensor) {
  if (sensor->shared.thread_shared_init &&
      !IS_ERR(sensor->shared.thread_shared_init)) {
    kthread_stop(sensor->shared.thread_shared_init);
    put_task_struct(sensor->shared.thread_shared_init);
  }
  sensor->shared.thread_shared_init = NULL;

  mutex_lock(&max9296_shared_lock);
  WRITE_ONCE(sensor->shared.probe_ready, false);
  if (i2c_get_clientdata(sensor->i2c_client) == &sensor->sd)
    i2c_set_clientdata(sensor->i2c_client, NULL);
  mutex_unlock(&max9296_shared_lock);

  sensor->shared.sensor = NULL;
  sensor->shared.sd = NULL;
  if (sensor->shared.client) {
    put_device(&sensor->shared.client->dev);
    sensor->shared.client = NULL;
  }
  of_node_put(sensor->shared.np);
  sensor->shared.np = NULL;
}

//-------------------------------------------------------------------------
struct max9296_prepare_command {
  bool cancel;
  u64 generation;
  u32 width;
  u32 height;
  u32 fps;
  u32 enable;
  enum max9296_mode_id mode_id;
};

static int max9296_parse_prepare_command(
    const char *buf, size_t count, struct max9296_prepare_command *command) {
  char *input, *cursor, *token;
  char *fields[7];
  u64 values[5];
  unsigned int nr_fields = 0, i;
  int ret = -EINVAL;

  if (!count || count > 127 || memchr(buf, '\0', count))
    return -EINVAL;

  input = kmemdup_nul(buf, count, GFP_KERNEL);
  if (!input)
    return -ENOMEM;
  cursor = strim(input);

  while ((token = strsep(&cursor, " \t")) != NULL) {
    if (!*token)
      continue;
    if (nr_fields == ARRAY_SIZE(fields))
      goto out;
    fields[nr_fields++] = token;
  }

  memset(command, 0, sizeof(*command));
  if (nr_fields == 1 && !strcmp(fields[0], "0")) {
    command->cancel = true;
    ret = 0;
    goto out;
  }
  if (nr_fields != 6 || strcmp(fields[0], "1"))
    goto out;

  for (i = 1; i < nr_fields; i++) {
    const char *digit;

    if (!fields[i][0])
      goto out;
    for (digit = fields[i]; *digit; digit++)
      if (*digit < '0' || *digit > '9')
        goto out;
    ret = kstrtoull(fields[i], 10, &values[i - 1]);
    if (ret) {
      ret = -EINVAL;
      goto out;
    }
  }

  command->generation = values[0];
  if (command->generation == 0 || values[1] > U32_MAX ||
      values[2] > U32_MAX || values[3] > U32_MAX || values[4] > U32_MAX) {
    ret = -EINVAL;
    goto out;
  }
  command->width = values[1];
  command->height = values[2];
  command->fps = values[3];
  command->enable = values[4];

  if (command->fps < 1 || command->fps > 120) {
    ret = -EINVAL;
    goto out;
  }

  if (command->width == 2560 && command->height == 720) {
    command->mode_id = MAX9296_MODE_2560x720;
    if (command->enable != 3)
      goto invalid;
  } else if (command->width == 3840 && command->height == 1080) {
    command->mode_id = MAX9296_MODE_3840x1080;
    if (command->enable != 3)
      goto invalid;
  } else if (command->width == 1280 && command->height == 720) {
    command->mode_id = MAX9296_MODE_1280x720;
    if (command->enable != 1 && command->enable != 2)
      goto invalid;
  } else if (command->width == 1920 && command->height == 1080) {
    command->mode_id = MAX9296_MODE_1920x1080;
    if (command->enable != 1 && command->enable != 2)
      goto invalid;
  } else if (command->width == 1280 && command->height == 360) {
    command->mode_id = MAX9296_MODE_1280x360;
    if (command->enable != 3)
      goto invalid;
  } else if (command->width == 640 && command->height == 360) {
    command->mode_id = MAX9296_MODE_640x360;
    if (command->enable != 1 && command->enable != 2)
      goto invalid;
  } else {
    goto invalid;
  }

  if (command->fps > max9296_mode_data[command->mode_id].max_fps)
    goto invalid;

  ret = 0;
  goto out;

invalid:
  ret = -EINVAL;
out:
  kfree(input);
  return ret;
}

static const char *max9296_prepare_state_name(
    enum max9296_prepare_request_state state) {
  switch (state) {
  case MAX9296_PREP_IDLE:
    return "IDLE";
  case MAX9296_PREP_PREPARING:
    return "PREPARING";
  case MAX9296_PREP_READY:
    return "READY";
  case MAX9296_PREP_STALE:
    return "STALE";
  case MAX9296_PREP_CONSUMED:
    return "CONSUMED";
  case MAX9296_PREP_FAILED:
    return "FAILED";
  case MAX9296_PREP_EXPIRED:
    return "EXPIRED";
  default:
    return "UNKNOWN";
  }
}

static void max9296_prepare_mode_names(
    const struct max9296_hw_fingerprint *fingerprint, const char **mode,
    const char **table) {
  if (fingerprint->mode == &max9296_mode_data[MAX9296_MODE_2560x720] ||
      fingerprint->mode == &max9296_mode_data[MAX9296_MODE_3840x1080] ||
      fingerprint->mode == &max9296_mode_data[MAX9296_MODE_1280x360]) {
    *mode = "dual-wide";
    *table = "dual";
  } else if (fingerprint->mode == &max9296_mode_data_HD_R ||
             fingerprint->mode == &max9296_mode_data_FHD_R ||
             fingerprint->mode == &max9296_mode_data_360_R) {
    *mode = "single";
    *table = "right";
  } else if (fingerprint->mode ==
                 &max9296_mode_data[MAX9296_MODE_1280x720] ||
             fingerprint->mode ==
                 &max9296_mode_data[MAX9296_MODE_1920x1080] ||
             fingerprint->mode ==
                 &max9296_mode_data[MAX9296_MODE_640x360]) {
    *mode = "single";
    *table = "left";
  } else {
    *mode = "none";
    *table = "none";
  }
}

static ssize_t sysfs_prepare_show(struct device *dev,
                                  struct device_attribute *attr, char *buf) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct max9296_hw_fingerprint prepared, runtime;
  enum max9296_prepare_request_state state;
  const char *state_name, *mode, *table;
  u64 generation, epoch;
  unsigned int lease, match = 0;
  int last_errno, worker_errno;

  if (READ_ONCE(sensor->dying))
    return -ENODEV;
  if (!READ_ONCE(sensor->shared.probe_ready))
    return -EAGAIN;

  mutex_lock(&sensor->lock);
  mutex_lock(&max9296_power_lock);
  if (sensor->dying) {
    mutex_unlock(&max9296_power_lock);
    mutex_unlock(&sensor->lock);
    return -ENODEV;
  }
  if (!READ_ONCE(sensor->shared.probe_ready)) {
    mutex_unlock(&max9296_power_lock);
    mutex_unlock(&sensor->lock);
    return -EAGAIN;
  }

  state = sensor->prepare_state;
  prepared = sensor->prepare_fingerprint;
  generation = sensor->prepare_generation;
  epoch = max9296_hw_epoch;
  lease = sensor->prepare_lease_held;
  last_errno = sensor->prepare_errno;
  worker_errno = READ_ONCE(sensor->worker_errno);
  if (sensor->hardware_valid && sensor->initialized_epoch == epoch &&
      !max9296_normalize_fingerprint_locked(sensor, &runtime) &&
      max9296_fingerprint_equal(&runtime, &prepared) &&
      max9296_prepare_matches_locked(sensor, &runtime))
    match = 1;
  mutex_unlock(&max9296_power_lock);
  mutex_unlock(&sensor->lock);

  state_name = max9296_prepare_state_name(state);
  max9296_prepare_mode_names(&prepared, &mode, &table);
  return scnprintf(buf, PAGE_SIZE, "state=%s generation=%llu epoch=%llu mode=%s table=%s width=%u height=%u fps=%u code=0x%x enable=%u crop_enable=%u errno=%d worker_errno=%d lease=%u match=%u\n",
                   state_name, (unsigned long long)generation,
                   (unsigned long long)epoch, mode, table, prepared.width,
                   prepared.height, prepared.fps, prepared.code,
                   prepared.enable, prepared.crop_enable, last_errno,
                   worker_errno, lease, match);
}

static ssize_t sysfs_prepare_store(struct device *dev,
                                   struct device_attribute *attr,
                                   const char *buf, size_t count) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct max9296_prepare_command command;
  struct max9296_hw_fingerprint fingerprint;
  int ret;

  if (READ_ONCE(sensor->dying))
    return -ENODEV;
  if (!READ_ONCE(sensor->shared.probe_ready))
    return -EAGAIN;

  ret = max9296_parse_prepare_command(buf, count, &command);
  if (ret)
    return ret;
  if (command.cancel) {
    ret = max9296_cancel_prepare(sensor);
    return ret ? ret : count;
  }

  if (command.mode_id == MAX9296_MODE_1280x720 && command.enable == 2)
    fingerprint.mode = &max9296_mode_data_HD_R;
  else if (command.mode_id == MAX9296_MODE_1920x1080 && command.enable == 2)
    fingerprint.mode = &max9296_mode_data_FHD_R;
  else if (command.mode_id == MAX9296_MODE_640x360 && command.enable == 2)
    fingerprint.mode = &max9296_mode_data_360_R;
  else
    fingerprint.mode = &max9296_mode_data[command.mode_id];
  fingerprint.width = command.width;
  fingerprint.height = command.height;
  fingerprint.code = MEDIA_BUS_FMT_UYVY8_2X8;
  fingerprint.fps = command.fps;
  fingerprint.enable = command.enable;
  fingerprint.crop_enable = READ_ONCE(sensor->ctrl_cache.crop_enable);

  ret = max9296_prepare_request(sensor, &fingerprint, command.generation);
  return ret ? ret : count;
}
static DEVICE_ATTR(prepare, 0664, sysfs_prepare_show, sysfs_prepare_store);
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
  unsigned int enable;

  mutex_lock(&sensor->lock);
  enable = sensor->enable;
  mutex_unlock(&sensor->lock);

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] sensor enable : 0x%x", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, enable);
  return snprintf(buf, PAGE_SIZE, "%u\n", enable);
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
  if (val > U32_MAX)
    return -EINVAL;

  mutex_lock(&sensor->lock);
  if (sensor->enable != val) {
    sensor->enable = val;
    max9296_mark_prepare_stale_locked(sensor);
  }
  mutex_unlock(&sensor->lock);

  if (debug)
    printk(KERN_NOTICE "[%s:%d][%s:%d] sensor enable : 0x%x", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           (unsigned int)val);

  return count;
}
static DEVICE_ATTR(enable, 0664, sysfs_enable_show, sysfs_enable_store);
//-------------------------------------------------------------------------
struct max9296_health_channel_sample {
  unsigned int channel;
  bool enabled;
  bool link_up;
  const char *phy;
  const char *link_status;
  const char *control_tunnel;
  const char *serializer_status;
  const char *isp_status;
  const char *sensor_status;
  const char *hinf_progress;
  int serializer_errno;
  int isp_errno;
  unsigned int serializer_id;
  unsigned int hinf_count;
  bool serializer_id_valid;
  bool hinf_valid;
};

struct max9296_health_sample {
  u64 sequence;
  s64 observed_ms;
  bool dual;
  bool streaming;
  unsigned int configured_local_mask;
  unsigned int configured_global_mask;
  int deserializer_errno;
  int ctrl3_errno;
  int rx3_errno;
  unsigned int deserializer_id;
  unsigned int ctrl3;
  unsigned int rx3;
  bool deserializer_id_valid;
  bool ctrl3_valid;
  bool rx3_valid;
  bool link_a_up;
  bool link_b_up;
  struct max9296_health_channel_sample channel[2];
};

static void max9296_health_set_unavailable(
    struct max9296_health_channel_sample *channel, const char *link_status) {
  channel->link_status = link_status;
  channel->control_tunnel = "BLOCKED";
  channel->serializer_status = "BLOCKED";
  channel->isp_status = "BLOCKED";
  channel->sensor_status = "BLOCKED";
  channel->hinf_progress = "NOT_AVAILABLE";
}

/* Collect a shallow, on-demand observation while sensor->lock is held.
 *
 * Probe dependency is intentionally not identical to the video data path:
 * after DES/RX3, MAX9295 management and AP1302 are probed as parallel branches.
 * A MAX9295 ID NAK must not prevent the AP1302 read that distinguishes a
 * serializer-management failure from an unavailable remote control tunnel.
 * AR0234 DMA is excluded here because it can take hundreds of milliseconds;
 * sensor static presence will be a separate explicitly rate-limited deep ABI.
 */
static void max9296_collect_health_locked(struct max9296_dev *sensor,
                                          struct max9296_health_sample *sample) {
  unsigned int des_id = 0, ctrl3 = 0, rx3 = 0;
  unsigned int local_ch;

  memset(sample, 0, sizeof(*sample));
  sample->sequence = ++sensor->health.sequence;
  sample->observed_ms = ktime_to_ms(ktime_get_boottime());
  sample->dual = max9296_hw_is_dual(sensor);
  sample->streaming = sensor->streaming;
  sample->configured_local_mask = sensor->enable & 0x03;
  sample->configured_global_mask =
      sample->configured_local_mask << sensor->link_status.ch_shift;

  sample->deserializer_errno = max9296_health_i2c_read_once(
      sensor, 0, MAX9296_REG_CHIP_ID, 2, 1, &des_id);
  if (!sample->deserializer_errno) {
    sample->deserializer_id_valid = true;
    sample->deserializer_id = des_id;
    if (des_id != MAX9296_CHIP_ID)
      sample->deserializer_errno = -ENODEV;
  }
  if (!sample->deserializer_errno) {
    sample->ctrl3_errno = max9296_health_i2c_read_once(
        sensor, 0, MAX9296_REG_CTRL3, 2, 1, &ctrl3);
    if (!sample->ctrl3_errno) {
      sample->ctrl3_valid = true;
      sample->ctrl3 = ctrl3;
    }
    sample->rx3_errno = max9296_health_i2c_read_once(
        sensor, 0, MAX9296_REG_RX3, 2, 1, &rx3);
    if (!sample->rx3_errno) {
      sample->rx3_valid = true;
      sample->rx3 = rx3;
      sample->link_a_up =
          (rx3 & MAX9296_RX3_LINK_A_UP) == MAX9296_RX3_LINK_A_UP;
      sample->link_b_up =
          (rx3 & MAX9296_RX3_LINK_B_UP) == MAX9296_RX3_LINK_B_UP;
    }
  } else {
    sample->ctrl3_errno = -ENXIO;
    sample->rx3_errno = -ENXIO;
  }

  for (local_ch = 0; local_ch < ARRAY_SIZE(sample->channel); local_ch++) {
    struct max9296_health_channel_sample *channel =
        &sample->channel[local_ch];
    unsigned int serializer_id = 0, frame_count = 0;
    unsigned int serializer_addr, ap1302_addr;
    bool physical_up;

    channel->channel = sensor->link_status.ch_shift + local_ch;
    channel->enabled = sample->configured_local_mask & BIT(local_ch);
    channel->serializer_errno = -ENODATA;
    channel->isp_errno = -ENODATA;
    channel->phy = "NONE";

    if (!channel->enabled) {
      channel->link_status = "N/A";
      channel->control_tunnel = "N/A";
      channel->serializer_status = "N/A";
      channel->isp_status = "N/A";
      channel->sensor_status = "N/A";
      channel->hinf_progress = "NOT_EXPECTED";
      sensor->health.hinf_valid[local_ch] = false;
      continue;
    }
    if (sample->deserializer_errno) {
      max9296_health_set_unavailable(channel, "BLOCKED_BY_DES");
      sensor->health.hinf_valid[local_ch] = false;
      continue;
    }
    if (!sample->rx3_valid) {
      max9296_health_set_unavailable(channel, "UNKNOWN");
      sensor->health.hinf_valid[local_ch] = false;
      continue;
    }

    if (sample->dual) {
      /* Bench evidence: removing odd ch1/ch3 clears RX3 Link A (0x06),
       * leaving 0x60. Therefore local even -> Link B, local odd -> Link A. */
      if (local_ch == 0) {
        channel->phy = "B";
        physical_up = sample->link_b_up;
      } else {
        channel->phy = "A";
        physical_up = sample->link_a_up;
      }
    } else {
      /* Single-channel tables may route either configured channel over Link A;
       * channel identity must come from enable, not from the PHY nibble. */
      physical_up = sample->link_a_up || sample->link_b_up;
      if (sample->link_a_up && sample->link_b_up)
        channel->phy = "AB";
      else if (sample->link_a_up)
        channel->phy = "A";
      else if (sample->link_b_up)
        channel->phy = "B";
    }
    channel->link_up = physical_up;
    if (!physical_up) {
      max9296_health_set_unavailable(channel, "DOWN");
      sensor->health.hinf_valid[local_ch] = false;
      continue;
    }
    channel->link_status = "OK";

    serializer_addr = max9296_ser_addr(sensor, local_ch);
    ap1302_addr = sample->dual
                      ? (local_ch ? AP1302_CH1_I2C_ADDR
                                  : AP1302_CH0_I2C_ADDR)
                      : AP1302_I2C_ADDR;

    /* Parallel branches below: do not gate AP1302 on serializer ID ACK. */
    channel->serializer_errno = max9296_health_i2c_read_once(
        sensor, serializer_addr, MAX9295_REG_CHIP_ID, 2, 1,
        &serializer_id);
    if (!channel->serializer_errno) {
      channel->serializer_id_valid = true;
      channel->serializer_id = serializer_id;
      if (serializer_id != MAX9295_CHIP_ID)
        channel->serializer_errno = -ENODEV;
    }
    channel->isp_errno = max9296_health_i2c_read_once(
        sensor, ap1302_addr, AP1302_REG_FRAME_CNT, 2, 2, &frame_count);
    if (!channel->isp_errno) {
      channel->hinf_valid = true;
      channel->hinf_count = (frame_count >> 8) & 0xff;
    }

    if (!channel->serializer_errno && !channel->isp_errno)
      channel->control_tunnel = "OK";
    else if (!channel->serializer_errno || !channel->isp_errno)
      channel->control_tunnel = "PARTIAL";
    else
      channel->control_tunnel = "AMBIGUOUS";

    if (!channel->serializer_errno)
      channel->serializer_status = "OK";
    else if (!channel->isp_errno)
      channel->serializer_status = "FAIL";
    else
      channel->serializer_status = "UNKNOWN";

    if (!channel->isp_errno) {
      if (!sample->streaming) {
        channel->isp_status = "OK";
        channel->hinf_progress = "NOT_EXPECTED";
        sensor->health.hinf_valid[local_ch] = false;
      } else if (!sensor->health.hinf_valid[local_ch]) {
        channel->isp_status = "STARTING";
        channel->hinf_progress = "STARTING";
        sensor->health.hinf_valid[local_ch] = true;
        sensor->health.hinf_count[local_ch] = channel->hinf_count;
      } else if (sensor->health.hinf_count[local_ch] != channel->hinf_count) {
        channel->isp_status = "OK";
        channel->hinf_progress = "YES";
        sensor->health.hinf_count[local_ch] = channel->hinf_count;
      } else {
        /* HINF stall alone cannot distinguish AR0234 from AP1302. */
        channel->isp_status = "UNKNOWN";
        channel->hinf_progress = "NO";
      }
      channel->sensor_status = "UNKNOWN";
    } else {
      sensor->health.hinf_valid[local_ch] = false;
      channel->hinf_progress = "NOT_AVAILABLE";
      if (!channel->serializer_errno)
        channel->isp_status = "FAIL";
      else
        channel->isp_status = "UNKNOWN";
      channel->sensor_status = "BLOCKED";
    }
  }
}

static ssize_t sysfs_health_raw_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct max9296_health_sample sample;
  s64 now_ms = ktime_to_ms(ktime_get_boottime());
  size_t len = 0;
  unsigned int i;
  char deserializer_id[16], ctrl3[16], rx3[16];

  /* Do not queue behind STREAMON, controls, firmware load, or teardown. The
   * userspace exporter skips busy samples and lets the prior snapshot expire. */
  if (!mutex_trylock(&sensor->lock))
    return scnprintf(buf, PAGE_SIZE,
                     "{\"schema\":1,\"adapter\":%d,\"sequence\":%llu,"
                     "\"observed_monotonic_ms\":%lld,\"busy\":true}\n",
                     sensor->i2c_client->adapter->nr,
                     (unsigned long long)READ_ONCE(sensor->health.sequence),
                     (long long)now_ms);

  max9296_collect_health_locked(sensor, &sample);
  mutex_unlock(&sensor->lock);

  if (sample.deserializer_id_valid)
    scnprintf(deserializer_id, sizeof(deserializer_id), "%u",
              sample.deserializer_id);
  else
    strscpy(deserializer_id, "null", sizeof(deserializer_id));
  if (sample.ctrl3_valid)
    scnprintf(ctrl3, sizeof(ctrl3), "%u", sample.ctrl3);
  else
    strscpy(ctrl3, "null", sizeof(ctrl3));
  if (sample.rx3_valid)
    scnprintf(rx3, sizeof(rx3), "%u", sample.rx3);
  else
    strscpy(rx3, "null", sizeof(rx3));

  len += scnprintf(
      buf + len, PAGE_SIZE - len,
      "{\"schema\":1,\"adapter\":%d,\"sequence\":%llu,"
      "\"observed_monotonic_ms\":%lld,\"busy\":false,"
      "\"mode\":\"%s\",\"streaming\":%s,"
      "\"configured_local_mask\":%u,\"configured_global_mask\":%u,"
      "\"deserializer\":{\"status\":\"%s\",\"errno\":%d,"
      "\"device_id\":%s,\"ctrl3_errno\":%d,\"ctrl3\":%s,"
      "\"rx3_errno\":%d,\"rx3\":%s,\"link_a_up\":%s,"
      "\"link_b_up\":%s},\"channels\":[",
      sensor->i2c_client->adapter->nr,
      (unsigned long long)sample.sequence, (long long)sample.observed_ms,
      sample.dual ? "dual-wide" : "single",
      sample.streaming ? "true" : "false", sample.configured_local_mask,
      sample.configured_global_mask,
      sample.deserializer_errno ? "FAIL" : "OK", sample.deserializer_errno,
      deserializer_id, sample.ctrl3_errno, ctrl3, sample.rx3_errno, rx3,
      sample.link_a_up ? "true" : "false",
      sample.link_b_up ? "true" : "false");

  for (i = 0; i < ARRAY_SIZE(sample.channel); i++) {
    const struct max9296_health_channel_sample *channel = &sample.channel[i];
    char serializer_id[16], hinf_count[16];

    if (channel->serializer_id_valid)
      scnprintf(serializer_id, sizeof(serializer_id), "%u",
                channel->serializer_id);
    else
      strscpy(serializer_id, "null", sizeof(serializer_id));
    if (channel->hinf_valid)
      scnprintf(hinf_count, sizeof(hinf_count), "%u", channel->hinf_count);
    else
      strscpy(hinf_count, "null", sizeof(hinf_count));

    len += scnprintf(
        buf + len, PAGE_SIZE - len,
        "%s{\"channel\":%u,\"enabled\":%s,\"phy\":\"%s\","
        "\"link\":{\"status\":\"%s\",\"up\":%s},"
        "\"control_tunnel\":\"%s\","
        "\"serializer\":{\"status\":\"%s\",\"errno\":%d,"
        "\"device_id\":%s},"
        "\"isp\":{\"status\":\"%s\",\"errno\":%d,"
        "\"hinf_count\":%s,\"hinf_progress\":\"%s\"},"
        "\"sensor\":{\"status\":\"%s\","
        "\"probe\":\"DEEP_NOT_RUN\"}}",
        i ? "," : "", channel->channel,
        channel->enabled ? "true" : "false", channel->phy,
        channel->link_status, channel->link_up ? "true" : "false",
        channel->control_tunnel, channel->serializer_status,
        channel->serializer_errno, serializer_id, channel->isp_status,
        channel->isp_errno, hinf_count, channel->hinf_progress,
        channel->sensor_status);
  }
  len += scnprintf(buf + len, PAGE_SIZE - len, "]}\n");
  return len;
}
static DEVICE_ATTR(health_raw, 0444, sysfs_health_raw_show, NULL);
//-------------------------------------------------------------------------
static ssize_t sysfs_link_status_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf) {
  struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
  struct max9296_dev *sensor = to_max9296_dev(sd);

  return snprintf(buf, PAGE_SIZE, "%d\n", sensor->link_status.disconnect);
}
static DEVICE_ATTR(link_status, 0444, sysfs_link_status_show, NULL);
//-------------------------------------------------------------------------
static int max9296_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct fwnode_handle *endpoint;
  struct max9296_dev *sensor;
  struct v4l2_mbus_framefmt *fmt;
  int ret;

  printk(KERN_NOTICE "[%s:%d][%s:%d] max9296 version : %s", KEYWORD,
         client->adapter->nr, _FILE_, __LINE__, SW_VERSION);

  sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
  if (!sensor)
    return -ENOMEM;

  sensor->i2c_client = client;

  /*
   * Initialised up front, not just before max9296_init_controls(), so that
   * every error path below can reach the common cleanup tail: mutex_destroy()
   * on a still-zeroed mutex trips a CONFIG_DEBUG_MUTEXES magic check.
  */
  mutex_init(&sensor->lock);
  mutex_init(&sensor->prepare_request_lock);
  INIT_DELAYED_WORK(&sensor->prepare_lease_timeout,
                    max9296_prepare_lease_timeout);
  sensor->prepare_state = MAX9296_PREP_IDLE;

  sensor->state.init = MAX9296_STATE_IDLE;
  sensor->state.firmware = MAX9296_STATE_IDLE;
  sensor->state.enable = MAX9296_STATE_IDLE;
  sensor->state.fsync = MAX9296_STATE_IDLE;
  sensor->state.power = MAX9296_STATE_IDLE;
  sensor->stream_on = 0;
  sensor->ctrl_cache.firmware_ready = false;
  sensor->ctrl_cache.mcp4018_wiper = MCP4018_WIPER_DEFAULT;
  sensor->ctrl_cache.mcp4018_wiper_ch1 = MCP4018_WIPER_DEFAULT;
  sensor->ctrl_cache.mcp4018_power = 0;      /* OFF (MFP4 LOW) */
  sensor->ctrl_cache.mcp4018_power_ch1 = 0;  /* OFF (MFP4 LOW) */
  /* Per-channel cache defaults are set after max9296_init_controls() below */

  sensor->link_status.disconnect = -1;  /* not checked yet */
  /* adapter 2 → ch0(bit0)/ch1(bit1), adapter 1 → ch2(bit2)/ch3(bit3) */
  sensor->link_status.ch_shift = (client->adapter->nr == 2) ? 0 : 2;

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

  /* Request the shared reset without disturbing an already-powered peer. */
  ret = max9296_acquire_reset_gpio(sensor);
  if (ret) {
    printk(KERN_CRIT "[%s:%d][%s:%d] reset gpio error(%d)", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, ret);
    return ret;
  }
  if (!sensor->reset_gpio) {
    printk(KERN_WARNING "[%s:%d][%s:%d] warning reset gpio...", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    //dev_info(dev, "warning reset gpio...\n");
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
          kthread_create(max9296_shared_init, sensor, "max9296_shared_init");
      if (IS_ERR(sensor->shared.thread_shared_init)) {
        printk(KERN_CRIT
               "[%s:%d][%s:%d] sensor->shared.thread_shared_init error",
               KEYWORD, client->adapter->nr, _FILE_, __LINE__);
        /*
         * ret still holds the 0 from v4l2_fwnode_endpoint_parse() here, so
         * without this probe would report success on a failed kthread_create.
         */
        ret = PTR_ERR(sensor->shared.thread_shared_init);
        goto entity_cleanup;
      }
      /* The resolver can return immediately when its peer is already ready.
       * Pin the task before waking it so cleanup can always stop/put safely. */
      get_task_struct(sensor->shared.thread_shared_init);
      wake_up_process(sensor->shared.thread_shared_init);
    }
  }

  mutex_lock(&max9296_shared_lock);
  v4l2_i2c_subdev_init(&sensor->sd, client, &max9296_subdev_ops);
  mutex_unlock(&max9296_shared_lock);

  sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
  sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
  sensor->sd.entity.ops = &max9296_sd_media_ops;
  sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
  ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
  if (ret) {
    printk(KERN_CRIT "[%s:%d][%s:%d] media_entity_pads_init error(%d)", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, ret);
    goto entity_cleanup;
  }

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
      sensor->ctrls.auto_wb_ch0 ? sensor->ctrls.auto_wb_ch0->val
                                : AP1302_AWB_MODE_AUTO;
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
  sensor->ctrl_cache.ch0.dz_x = sensor->ctrls.dz_x_ch0
                                    ? sensor->ctrls.dz_x_ch0->val
                                    : MAX9296_DZ_CENTER_DEFAULT;
  sensor->ctrl_cache.ch0.dz_y = sensor->ctrls.dz_y_ch0
                                    ? sensor->ctrls.dz_y_ch0->val
                                    : MAX9296_DZ_CENTER_DEFAULT;

  sensor->ctrl_cache.ch1.ae_on = sensor->ctrls.auto_exp_ch1
                                     ? (sensor->ctrls.auto_exp_ch1->val ? 1 : 0)
                                     : 1;
  sensor->ctrl_cache.ch1.awb =
      sensor->ctrls.auto_wb_ch1 ? sensor->ctrls.auto_wb_ch1->val
                                : AP1302_AWB_MODE_AUTO;
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
  sensor->ctrl_cache.ch1.dz_x = sensor->ctrls.dz_x_ch1
                                    ? sensor->ctrls.dz_x_ch1->val
                                    : MAX9296_DZ_CENTER_DEFAULT;
  sensor->ctrl_cache.ch1.dz_y = sensor->ctrls.dz_y_ch1
                                    ? sensor->ctrls.dz_y_ch1->val
                                    : MAX9296_DZ_CENTER_DEFAULT;

  sensor->ctrl_cache.exposure =
      sensor->ctrls.exp_time ? sensor->ctrls.exp_time->val : 10000;
  sensor->ctrl_cache.crop_enable =
      sensor->ctrls.crop_enable ? !!sensor->ctrls.crop_enable->val : false;
  sensor->ctrl_cache.dz =
      sensor->ctrls.dz ? sensor->ctrls.dz->val : MAX9296_DZ_DEFAULT;
  sensor->ctrl_cache.dz_x = sensor->ctrls.dz_x
                                ? sensor->ctrls.dz_x->val
                                : MAX9296_DZ_CENTER_DEFAULT;
  sensor->ctrl_cache.dz_y = sensor->ctrls.dz_y
                                ? sensor->ctrls.dz_y->val
                                : MAX9296_DZ_CENTER_DEFAULT;

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

  ret = max9296_start_workers(sensor, MAX9296_WORKER_ALL);
  if (ret) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sensor worker start error(%d)", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__, ret);
    goto free_ctrls;
  }
  mutex_lock(&max9296_shared_lock);
  max9296_refresh_worker_status_locked(sensor);
  mutex_unlock(&max9296_shared_lock);
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
    goto remove_rotate_attr;
  }
  if (device_create_file(&client->dev, &dev_attr_link_status) != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sysfs link_status entry failed", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    ret = (-EINVAL);
    goto remove_enable_attr;
  }
  if (device_create_file(&client->dev, &dev_attr_health_raw) != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sysfs health_raw entry failed", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    ret = (-EINVAL);
    goto remove_link_status_attr;
  }
  if (device_create_file(&client->dev, &dev_attr_prepare) != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sysfs prepare entry failed", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    ret = -EINVAL;
    goto remove_health_raw_attr;
  }

  /* Keep async registration as the final fallible step.  Once registered,
   * external V4L2 operations may start custom firmware work that the ordinary
   * probe unwind cannot safely drain. */
  ret = v4l2_async_register_subdev_sensor_common(&sensor->sd);
  if (ret) {
    printk(KERN_CRIT
           "[%s:%d][%s:%d] v4l2_async_register_subdev_sensor_common error(%d)",
           KEYWORD, client->adapter->nr, _FILE_, __LINE__, ret);
    goto remove_prepare_attr;
  }

  /* Final non-failing commit: sibling resolvers may publish us only now. */
  mutex_lock(&max9296_shared_lock);
  WRITE_ONCE(sensor->shared.probe_ready, true);
  max9296_ready_shared_peer_locked(sensor);
  mutex_unlock(&max9296_shared_lock);

  if (debug)
    printk(KERN_INFO "[%s:%d][%s:%d] %s end", KEYWORD,
           sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);
  return 0;

remove_prepare_attr:
  device_remove_file(&client->dev, &dev_attr_prepare);
remove_health_raw_attr:
  device_remove_file(&client->dev, &dev_attr_health_raw);
remove_link_status_attr:
  device_remove_file(&client->dev, &dev_attr_link_status);
remove_enable_attr:
  device_remove_file(&client->dev, &dev_attr_enable);
remove_rotate_attr:
  device_remove_file(&client->dev, &dev_attr_rotate);
free_ctrls:
  /* Stop threads that were already started */
  if (sensor->thread_en && !IS_ERR(sensor->thread_en)) {
    kthread_stop(sensor->thread_en);
    sensor->thread_en = NULL;
  }
  if (sensor->thread_fsync && !IS_ERR(sensor->thread_fsync)) {
    kthread_stop(sensor->thread_fsync);
    sensor->thread_fsync = NULL;
  }

  v4l2_ctrl_handler_free(&sensor->ctrls.handler);
entity_cleanup:
  max9296_release_probe_shared(sensor);

  media_entity_cleanup(&sensor->sd.entity);
  mutex_destroy(&sensor->prepare_request_lock);
  mutex_destroy(&sensor->lock);
  return ret;
}

static int max9296_remove(struct i2c_client *client) {
  struct v4l2_subdev *sd = i2c_get_clientdata(client);
  struct max9296_dev *sensor = to_max9296_dev(sd);
  struct max9296_dev *peer;
  unsigned int peer_workers_stopped = 0;
  bool peer_references_sensor;
  bool accounted;
  int worker_errno;

  printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD,
         sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  /* A device reference does not pin devm allocations after another remove()
   * returns.  Keep sibling remove callbacks serialized for every raw peer use. */
  mutex_lock(&max9296_remove_lock);

  /* Phase 0: withdraw every admission point before waiting on admitted work. */
  /* The board lock is the final FSYNC/output authority too: removal becomes
   * visible atomically before any later pulse or non-zero output write. */
  mutex_lock(&max9296_power_lock);
  WRITE_ONCE(sensor->dying, true);
  mutex_unlock(&max9296_power_lock);

  mutex_lock(&max9296_shared_lock);
  /* shared.sensor may legitimately still be NULL while the reciprocal raw
   * link is already live. Resolve the declared client under the same lock that
   * publishes/withdraws clientdata, then validate the committed reciprocal
   * relationship before retaining the raw devm pointer under remove_lock. */
  peer = max9296_declared_shared_peer_locked(sensor);
  if (peer && (!READ_ONCE(peer->shared.probe_ready) ||
               READ_ONCE(peer->dying) ||
               peer->shared.np != sensor->i2c_client->dev.of_node))
    peer = NULL;
  peer_references_sensor =
      peer && READ_ONCE(peer->shared.sensor) == sensor;
  WRITE_ONCE(sensor->shared.probe_ready, false);
  mutex_unlock(&max9296_shared_lock);

  /* Removing the file waits for already admitted blocking stores to return.
   * Admission is closed above, so no new prepare request can enter. */
  device_remove_file(&client->dev, &dev_attr_prepare);
  v4l2_async_unregister_subdev(&sensor->sd);

  /* Exclude fresh prepare/renewal and wait for any already admitted request.
   * The expiry worker does not take prepare_request_lock, so it remains safe to
   * sync-drain here; sensor->lock is intentionally not held. */
  mutex_lock(&sensor->prepare_request_lock);
  cancel_delayed_work_sync(&sensor->prepare_lease_timeout);

  /* A lease and a positive local power_count are two forms of the same single
   * per-instance global user.  Reconcile it exactly once.  Removal deliberately
   * does not toggle rails: if this was the final accounted user, advance the
   * epoch explicitly so peer fingerprints cannot survive the implicit reset
   * caused by unbind/rebind GPIO setup. */
  mutex_lock(&sensor->lock);
  mutex_lock(&max9296_power_lock);
  WARN_ON(sensor->prepare_lease_held && sensor->power_count > 0);
  accounted = sensor->prepare_lease_held || sensor->power_count > 0;
  sensor->prepare_lease_held = false;
  sensor->prepare_lease_generation = 0;
  sensor->power_count = 0;
  if (accounted) {
    if (!WARN_ON(max9296_power_users == 0)) {
      max9296_power_users--;
      if (max9296_power_users == 0)
        max9296_hw_epoch++;
    }
  }
  max9296_put_orphaned_reset_gpio_locked();
  sensor->hardware_valid = false;
  sensor->initialized_epoch = 0;
  sensor->stream_commit_epoch = 0;
  mutex_unlock(&max9296_power_lock);
  mutex_unlock(&sensor->lock);
  mutex_unlock(&sensor->prepare_request_lock);

  /*
   * Phase 1: Stop peer threads that reference our memory.
   *
   * Stop first, clear the pointer second. The reverse order raced: those
   * threads test peer->shared.sensor and then dereference it in a later
   * statement, sometimes with a 1 s usleep_range() in between (the FSYNC
   * catch-up waits), and kthread_stop() only waits for the current iteration
   * to finish - it cannot interrupt one mid-way. Clearing the pointer first
   * therefore let a thread wake up past its NULL check and store through
   * NULL+offset. While the threads are still running the pointer is valid:
   * nothing of ours has been freed yet at this point in remove().
   */
  if (peer_references_sensor) {
    /* Close nonblocking STREAMON admission before waiting for an already
     * admitted request.  prepare_request_lock is not used by either worker,
     * so joining below cannot deadlock on it. */
    if (!READ_ONCE(peer->worker_errno))
      WRITE_ONCE(peer->worker_errno, -EAGAIN);
    mutex_lock(&peer->prepare_request_lock);

    if (peer->thread_fsync && !IS_ERR(peer->thread_fsync)) {
      kthread_stop(peer->thread_fsync);
      peer->thread_fsync = NULL;
      peer_workers_stopped |= MAX9296_WORKER_FSYNC;
    }
    /* thread_en has no raw-peer access: keep it live instead of creating an
     * unnecessary stop/restart failure window. */
    /* Non-worker peer paths (power/reset and STREAMOFF) dereference the same
     * raw link under max9296_power_lock. Wait for any admitted path, then
     * withdraw under the established power -> shared order. */
    mutex_lock(&max9296_power_lock);
    mutex_lock(&max9296_shared_lock);
    if (READ_ONCE(peer->shared.sensor) == sensor) {
      WRITE_ONCE(peer->shared.sensor, NULL);
      peer->shared.sd = NULL;
    }
    mutex_unlock(&max9296_shared_lock);
    mutex_unlock(&max9296_power_lock);

    /* kthread_run must stay outside power/shared locks: the new FSYNC worker
     * immediately enters paths that take max9296_power_lock.  The stopped mask
     * also makes this a no-op for the non-GPIO peer and prevents duplicates. */
    worker_errno = max9296_start_workers(peer, peer_workers_stopped);
    mutex_lock(&max9296_shared_lock);
    if (!worker_errno) {
      max9296_refresh_worker_status_locked(peer);
      if (READ_ONCE(peer->shared.sensor))
        max9296_refresh_worker_status_locked(
            READ_ONCE(peer->shared.sensor));
    }
    worker_errno = READ_ONCE(peer->worker_errno);
    mutex_unlock(&max9296_shared_lock);

    if (worker_errno) {
      bool was_streaming;

      /* A missing GPIO owner or failed worker restart must not leave status
       * claiming output is live.  Serialize with the enable worker and use the
       * established request -> sensor -> board lock order. */
      mutex_lock(&peer->lock);
      mutex_lock(&max9296_power_lock);
      was_streaming = peer->streaming;
      peer->streaming = false;
      peer->stream_on = 0;
      peer->stream_commit_epoch = 0;
      peer->state.fsync = MAX9296_STATE_IDLE;
      if (was_streaming)
        max9296_disable_stream_mipi(peer);
      mutex_unlock(&max9296_power_lock);
      mutex_unlock(&peer->lock);
      printk(KERN_CRIT
             "[%s:%d][%s:%d] survivor worker/FSYNC unavailable(%d)", KEYWORD,
             peer->i2c_client->adapter->nr, _FILE_, __LINE__, worker_errno);
    }
    mutex_unlock(&peer->prepare_request_lock);
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
  if (sensor->shared.thread_shared_init &&
      !IS_ERR(sensor->shared.thread_shared_init)) {
    kthread_stop(sensor->shared.thread_shared_init);
    put_task_struct(sensor->shared.thread_shared_init);
  }
  sensor->shared.thread_shared_init = NULL;

  /* Phase 3: Clean up shared references */
  mutex_lock(&max9296_shared_lock);
  WRITE_ONCE(sensor->shared.sensor, NULL);
  sensor->shared.sd = NULL;
  mutex_unlock(&max9296_shared_lock);
  if (sensor->shared.client) {
    put_device(&sensor->shared.client->dev);
    sensor->shared.client = NULL;
  }
  /* of_parse_phandle() in probe took a reference on this node */
  of_node_put(sensor->shared.np);
  sensor->shared.np = NULL;

  /* Phase 4: V4L2/media cleanup */
  device_remove_file(&client->dev, &dev_attr_health_raw);
  device_remove_file(&client->dev, &dev_attr_link_status);
  device_remove_file(&client->dev, &dev_attr_enable);
  device_remove_file(&client->dev, &dev_attr_rotate);
  mutex_lock(&max9296_shared_lock);
  if (i2c_get_clientdata(client) == sd)
    i2c_set_clientdata(client, NULL);
  mutex_unlock(&max9296_shared_lock);
  media_entity_cleanup(&sensor->sd.entity);
  v4l2_ctrl_handler_free(&sensor->ctrls.handler);
  mutex_destroy(&sensor->prepare_request_lock);
  mutex_destroy(&sensor->lock);

  mutex_unlock(&max9296_remove_lock);

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
