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

#define SW_VERSION "2.3"
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
  struct v4l2_ctrl *led_flash_ch0;
  struct v4l2_ctrl *led_flash_ch1;

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
};

struct max9296_ctrl_cache {
  bool firmware_ready;
  bool pending;

  /* Channel-specific settings */
  struct max9296_channel_ctrl ch0;
  struct max9296_channel_ctrl ch1;

  /* Shared setting value, applied to both channels when set */
  int exposure; /* V4L2_CID_EXP_TIME - exp_time (u32) */

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
  struct task_struct *thread_fw_init;

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
/* True when the hardware is currently programmed for both cameras.
 *
 * Deliberately reads last_mode, not current_mode. current_mode is the requested
 * format and max9296_set_fmt() moves it with no gate, while the register tables
 * are loaded at most once per probe lifetime (max9296_load_regs is gated on
 * sensor->restart). last_mode is assigned in max9296_set_mode() right before
 * load_regs, so it alone says which table the hardware actually received - and
 * the serializer address is a property of that, not of the pending format.
 * A dual stream followed by stream-off and an S_FMT to a single mode leaves a
 * serializer physically at 0x60 while current_mode already reads single.
 */
static bool max9296_hw_is_dual(const struct max9296_dev *sensor) {
  const struct max9296_mode_info *mode = sensor->last_mode;

  return mode && (mode->id == MAX9296_MODE_2560x720 ||
                  mode->id == MAX9296_MODE_3840x1080);
}

/* MAX9295 I2C address for a local channel (0 or 1).
 *
 * 0x60 is NOT a hardware property of ch1 - it exists only as a side effect of
 * {0x40, 0x0000, 2, 0xC0} inside max9296_init_setting_1080p_crop_720p_2ch_30fps,
 * the only serializer self-address write in this driver. The single-channel
 * tables (max9296_init_setting_720p_30fps_L/_R) carry no such remap and address
 * the serializer exclusively at 0x40 - including MAX9295_REG_MFP4_CTRL itself.
 * max9296_load_regs() also runs at most once per probe lifetime (gated on
 * sensor->restart), so the dual remap and a single-channel table can never both
 * execute. In single-channel mode the one serializer therefore always answers at
 * its power-on default 0x40, whichever local channel is active.
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
  int local_err = 0;
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
      } else {
        local_err = ret;
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

  /* MAX9296 local register failure is fatal; serializer failure is not */
  return local_err;
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
  if (1)
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s (%s)", KEYWORD,
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
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s (rate:%llu)", KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__, rate);
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
 * max9296_power_lock also serializes the sequence itself, so a concurrent open
 * on the other channel waits for it instead of racing a second reset onto the
 * same pins.
 */
static DEFINE_MUTEX(max9296_power_lock);
static int max9296_power_users;

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
    sensor->state.power = MAX9296_STATE_RUNNING;

    if (on)
      ret = max9296_set_power_on(sensor);
    else
      max9296_set_power_off(sensor);

    sensor->state.power = on ? MAX9296_STATE_DONE : MAX9296_STATE_IDLE;

    if (!on)
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

  /*
   * Update the power count. The global refcount inside max9296_set_power()
   * decides whether the hardware sequence actually runs, so this function must
   * not touch state.power - and above all must never write the peer's, which
   * used to erase the marker protecting a still-streaming channel.
   */
  if (on) {
    if (sensor->power_count == 0)
      ret = max9296_set_power(sensor, 1);

    sensor->power_count++;
  } else {
    sensor->power_count--;

    WARN_ON(sensor->power_count < 0);

    if (sensor->power_count == 0)
      ret = max9296_set_power(sensor, 0);
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
  bool dual = (sensor->current_mode->id == MAX9296_MODE_2560x720 ||
               sensor->current_mode->id == MAX9296_MODE_3840x1080);
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
  case V4L2_CID_LED_FLASH_CH0:
    sensor->ctrl_cache.ch0.led_flash = ctrl->val;
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
static void max9296_apply_channel_controls(struct max9296_dev *sensor,
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
  int  ret, err = 0;

  /* Entry log — what is about to be applied. */
  printk(KERN_NOTICE "[%s:%d][%s:%d] %s %s apply (addr:0x%02x ae:%s "
                     "awb:%s(0x%04x) gain:%d exp:%u rot:0x%02x mcp:%s "
                     "wiper:0x%02x delay:0x%02x)",
         KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
         ch_name, mode_name,
         i2c_addr, ch_ctrl->ae_on ? "on" : "off",
         awb_mode_name(ch_ctrl->awb), awb_val, gain_seed, exp_seed, rot,
         mcp_active ? "on" : "off", mcp4018_wiper, flash_delay);

  /* STEP 1: Initialize AE to manual mode first */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_CTRL,
                            AP1302_AE_CTRL_MANUAL, 2, 2);
  if (ret)
    err = ret;
  msleep(100);

  /* Seed exposure time while in manual mode. Some FW revisions need a
   * non-zero seed before switching to AE auto. */
  ret = maxim_ops_i2c_write(sensor, AP1302_I2C_ADDR, AP1302_REG_EXP_TIME,
                            exp_seed, 2, 4);
  if (ret)
    err = ret;
  msleep(100);

  /* STEP 2: Apply configured AE mode (auto/manual) */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_CTRL, ae_val, 2, 2);
  if (ret)
    err = ret;
  if (ch_ctrl->ae_on)
    msleep(100);

  /* AWB: ch_ctrl->awb holds AWB_CTRL MODE (0x0~0xf). */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AWB_CTRL, awb_val, 2, 2);
  if (ret)
    err = ret;

  /* Gain value (always set, used when switching to manual) */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_AE_GAIN, gain_seed, 2, 2);
  if (ret)
    err = ret;

  /* Rotation (hflip + vflip combined) */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_ROTATION, rot, 2, 2);
  if (ret)
    err = ret;

  /* Per-channel tuning values */
  ret = maxim_ops_i2c_write(sensor, i2c_addr, AP1302_REG_LSC_CTRL,
                            ch_ctrl->lsc, 2, 2);
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

  /* LED flash (AR0234 R0x3270 via AP1302 DMA). Firmware routes to the
   * correct physical sensor in both dual and single modes. */
  ret = max9296_dma_write_reg(sensor, i2c_addr, AR0234_REG_LED_FLASH_CONTROL,
                              (u16)ch_ctrl->led_flash);
  if (ret)
    err = ret;

  /* MCP4018 wiper — atomic open/write/close. Gated on flash enable bit:
   * when the flash is disabled the LED/MCP4018 chain may be unpopulated. */
  if (mcp_active) {
    max9295_mfp4_set(sensor, ser_addr, true);
    ret = mcp4018_write_wiper(sensor, mcp4018_host, mcp4018_wiper, ser_addr);
    max9295_mfp4_set(sensor, ser_addr, false);
    if (ret)
      err = ret;
  }

  /* Exit log — high-level result. Details are in the per-step logs above. */
  if (err)
    printk(KERN_ERR "[%s:%d][%s:%d] %s %s applied fail (ret=%d)",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           ch_name, mode_name, err);
  else
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s %s applied success",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           ch_name, mode_name);
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
    /* Dual-channel mode: apply each channel's settings separately.
     * MCP4018 per-port wiper is inlined via max9296_apply_channel_controls. */
    max9296_apply_channel_controls(sensor, AP1302_CH0_I2C_ADDR,
                                   &sensor->ctrl_cache.ch0,
                                   MAX9295_SER_ADDR_CH0, MCP4018_HOST_ADDR,
                                   (u8)sensor->ctrl_cache.mcp4018_wiper,
                                   ch0_name, "dual");
    max9296_apply_channel_controls(sensor, AP1302_CH1_I2C_ADDR,
                                   &sensor->ctrl_cache.ch1,
                                   MAX9295_SER_ADDR_CH1, MCP4018_HOST_ADDR_CH1,
                                   (u8)sensor->ctrl_cache.mcp4018_wiper_ch1,
                                   ch1_name, "dual");
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
    snprintf(single_name, sizeof(single_name), "ch%u", global_ch);
    max9296_apply_channel_controls(sensor, AP1302_I2C_ADDR,
                                   &sensor->ctrl_cache.ch0,
                                   ser, host, wiper, single_name, "single");
  }

  sensor->ctrl_cache.pending = false;
  sensor->ctrl_cache.firmware_ready = true;
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
    /* Shared exposure time: always write to global 0x3c (applies to both channels) */
    printk(KERN_NOTICE "[%s:%d][%s:%d] %s EXP_TIME ctrl->val:%d cache.exp:%d "
                       "ch0.exp:%d ch1.exp:%d",
           KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
           __FUNCTION__, ctrl->val, sensor->ctrl_cache.exposure,
           sensor->ctrl_cache.ch0.exposure, sensor->ctrl_cache.ch1.exposure);
    ret = maxim_ops_i2c_write(sensor, AP1302_I2C_ADDR, AP1302_REG_EXP_TIME,
                              ctrl->val, 2, 4);
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
    ret = maxim_ops_i2c_write(sensor, ch0_addr, AP1302_REG_EXP_TIME,
                              ctrl->val, 2, 4);
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
      ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_EXP_TIME, exp_val,
                                2, 4);
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
    ret = maxim_ops_i2c_write(sensor, ch1_addr, AP1302_REG_EXP_TIME,
                              ctrl->val, 2, 4);
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
};

static int max9296_init_controls(struct max9296_dev *sensor) {
  const struct v4l2_ctrl_ops *ops = &max9296_ctrl_ops;
  struct max9296_ctrls *ctrls = &sensor->ctrls;
  struct v4l2_ctrl_handler *hdl = &ctrls->handler;
  int ret;
  //printk(KERN_NOTICE "[%s:%d][%s:%d] %s", KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__, __FUNCTION__);

  v4l2_ctrl_handler_init(hdl, 56);

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

    /*
     * stream_on is a one-shot request to max9296_enable(), and only that thread
     * cleared it - at best a few seconds after STREAMON, once FSYNC is running.
     * A stream shorter than that (failed negotiation, single-frame probe, quick
     * source switch) used to leave it set, and the thread would later act on the
     * stale request and turn the CSI output back on for a deserializer nobody is
     * streaming. Cancel the request here instead.
     */
    sensor->stream_on = 0;

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
      if (sensor->state.enable == MAX9296_STATE_DONE) {
        if (sensor->state.fsync != MAX9296_STATE_RUNNING)
          usleep_range(1000000, 1000000);

        if (low_fps != sensor->fps) {
          low_fps = sensor->fps;
          low = (1000000 / low_fps) - high;
          printk(KERN_NOTICE
                 "[%s:%d][%s:%d] %s single fps : %u, low : %u, high : %u\n",
                 KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
                 __FUNCTION__, low_fps, low, high);
        }

        gpiod_set_value_cansleep(sensor->fsync_gpio, 1);
        usleep_range(high, high);
        gpiod_set_value_cansleep(sensor->fsync_gpio, 0);
        usleep_range(low, low);
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
            usleep_range(1000000, 1000000);

          if (sensor->shared.sensor->state.fsync != MAX9296_STATE_RUNNING)
            usleep_range(1000000, 1000000);

          if (low_fps != sensor->fps) {
            low_fps = sensor->fps;
            low = (1000000 / low_fps) - high;
            printk(KERN_NOTICE
                   "[%s:%d][%s:%d] %s dual fps : %u, low : %u, high : %u\n",
                   KEYWORD, sensor->i2c_client->adapter->nr, _FILE_, __LINE__,
                   __FUNCTION__, low_fps, low, high);
          }

          gpiod_set_value_cansleep(sensor->fsync_gpio, 1);
          usleep_range(high, high);
          gpiod_set_value_cansleep(sensor->fsync_gpio, 0);
          usleep_range(low, low);
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
            (sensor->state.enable == MAX9296_STATE_DONE)) {
          fsync_state = &sensor->state.fsync;
          fps = sensor->fps;
          start = 1;
        } else if ((sensor->shared.sensor->state.init == MAX9296_STATE_DONE) &&
                   (sensor->shared.sensor->state.enable ==
                    MAX9296_STATE_DONE)) {
          fsync_state = &sensor->shared.sensor->state.fsync;
          fps = sensor->shared.sensor->fps;
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

          gpiod_set_value_cansleep(sensor->fsync_gpio, 1);
          usleep_range(high, high);
          gpiod_set_value_cansleep(sensor->fsync_gpio, 0);
          usleep_range(low, low);
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
      if (sensor->streaming &&
          (sensor->state.fsync == MAX9296_STATE_RUNNING)) {
        /*
         * Consume the request here - only on a pass that actually serves it.
         *
         * The clear used to sit at the very end of the pass, where it could
         * erase a STREAMON that arrived while the pass was running: the app
         * restarting its pipeline inside that ~600 ms window lost its request
         * and the deserializer never got its CSI output enable. Clearing it
         * before the liveness test is no better - a test that then fails would
         * throw the request away with nothing left to re-raise it, wedging the
         * channel at no frames. Inside the test both cases are safe: a rejected
         * pass leaves the flag up for the next iteration, and a STREAMON
         * landing mid-pass cannot be swallowed because the clear and the writes
         * are atomic under sensor->lock, which max9296_s_stream() holds for its
         * whole body. Such a STREAMON either completes before we take the lock,
         * in which case this pass is the one that serves it, or lands after we
         * drop it and leaves the flag raised for the next iteration.
         */
        sensor->stream_on = 0;

        if (sensor->current_mode->id == MAX9296_MODE_1280x720) {
          maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x02, 2, 1);
          usleep_range(10000, 11000);
        } else {
          maxim_ops_i2c_write(sensor, 0x00, 0x0313, 0x82, 2, 1);
          usleep_range(10000, 11000);
        }

        // ae
        maxim_ops_i2c_write(sensor, 0x3c, 0x5002, 0x0290, 2, 2);
        usleep_range(100000, 101000);
        maxim_ops_i2c_write(sensor, 0x3c, 0x5002, 0x0299, 2, 2);
        // awb
        usleep_range(100000, 101000);
        maxim_ops_i2c_write(sensor, 0x3c, 0x5100, 0x115f, 2, 2);
        // lsc
        usleep_range(100000, 101000);
        maxim_ops_i2c_write(sensor, 0x3c, 0x54a0, 0x3fff, 2, 2);

        /* Override hardcoded AE/AWB init with V4L2 cached controls */
        max9296_apply_cached_controls(sensor);
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
  char str[128] = {
      0,
  };
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

  sensor->state.init = MAX9296_STATE_IDLE;
  sensor->state.firmware = MAX9296_STATE_IDLE;
  sensor->state.enable = MAX9296_STATE_IDLE;
  sensor->state.fsync = MAX9296_STATE_IDLE;
  sensor->state.power = MAX9296_STATE_IDLE;
  sensor->stream_on = 0;
  sensor->ctrl_cache.firmware_ready = false;
  sensor->ctrl_cache.pending = false;
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

  /* request optional reset pin */
  sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
  if (IS_ERR(sensor->reset_gpio)) {
    printk(KERN_CRIT "[%s:%d][%s:%d] reset gpio error", KEYWORD,
           client->adapter->nr, _FILE_, __LINE__);
    return PTR_ERR(sensor->reset_gpio);
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
          kthread_run(max9296_shared_init, sensor, "max9296_shared_init");
      if (IS_ERR(sensor->shared.thread_shared_init)) {
        printk(KERN_CRIT
               "[%s:%d][%s:%d] sensor->shared.thread_shared_init error",
               KEYWORD, client->adapter->nr, _FILE_, __LINE__);
        /*
         * ret still holds the 0 from v4l2_fwnode_endpoint_parse() here, so
         * without this probe would report success on a failed kthread_run.
         */
        ret = PTR_ERR(sensor->shared.thread_shared_init);
        goto entity_cleanup;
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
  if (device_create_file(&client->dev, &dev_attr_link_status) != 0) {
    printk(KERN_CRIT "[%s:%d][%s:%d] sysfs link_status entry failed", KEYWORD,
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

  v4l2_ctrl_handler_free(&sensor->ctrls.handler);
entity_cleanup:
  /*
   * The peer-resolver kthread has to be stopped on EVERY error path reachable
   * after its kthread_run(), which is why it lives in the common tail rather
   * than under free_ctrls. sensor is devm-allocated, so returning an error
   * frees it while that thread is still dereferencing sensor->shared.* every
   * 10 ms - and when the peer never resolves it never breaks out on its own,
   * leaving a zombie thread on freed memory. The IS_ERR guard covers the
   * kthread_run failure path, which jumps here with an ERR_PTR in hand.
   */
  if (sensor->shared.thread_shared_init &&
      !IS_ERR(sensor->shared.thread_shared_init)) {
    kthread_stop(sensor->shared.thread_shared_init);
    put_task_struct(sensor->shared.thread_shared_init);
  }
  sensor->shared.thread_shared_init = NULL;
  of_node_put(sensor->shared.np);
  sensor->shared.np = NULL;

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

  /*
   * Phase 0: stop new s_power() callers, then hand back this instance's share
   * of the board-global power count. remove() never goes through
   * max9296_s_power(), so without this an unbind with the node still open would
   * leave max9296_power_users stuck above zero: a later rebind would see a
   * non-zero count and skip the power-on sequence entirely, and the count could
   * never fall back to zero. The per-device state this replaced was
   * re-initialised at probe, so the leak is specific to module-global state.
   *
   * This must run BEFORE the kthreads are stopped. max9296_s_stream() holds
   * sensor->lock while it waits for the firmware kthread, and Phase 2 kills
   * that kthread - taking sensor->lock afterwards would block here forever,
   * wedging the unbind in D state with the driver-core device_lock held.
   *
   * The rails are deliberately left up rather than calling
   * max9296_set_power_off() here. A teardown-time power-down buys nothing: the
   * next probe requests every GPIO as GPIOD_OUT_HIGH, which with the DT's
   * GPIO_ACTIVE_LOW flags drives all three rails off again, and the first
   * s_power(1) after that runs the full sequence. It would also be unreliable:
   * in a two-device teardown the peer's Phase 1 has already NULLed our
   * shared.sensor, so max9296_1 - which has no reset-gpios of its own - could
   * not reach the board-wide camera-module rail on GPIO1_IO01 and would drop
   * only its own deserializer. Leaving everything energised matches pre-patch
   * behaviour in every ordering.
   */
  v4l2_async_unregister_subdev(&sensor->sd);

  mutex_lock(&sensor->lock);
  mutex_lock(&max9296_power_lock);
  if (sensor->power_count > 0) {
    sensor->power_count = 0;
    if (!WARN_ON(max9296_power_users == 0))
      max9296_power_users--;
  }
  mutex_unlock(&max9296_power_lock);
  mutex_unlock(&sensor->lock);

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
  if (peer && peer->shared.sensor == sensor) {
    if (peer->thread_fsync && !IS_ERR(peer->thread_fsync)) {
      kthread_stop(peer->thread_fsync);
      peer->thread_fsync = NULL;
    }
    if (peer->thread_en && !IS_ERR(peer->thread_en)) {
      kthread_stop(peer->thread_en);
      peer->thread_en = NULL;
    }
    WRITE_ONCE(peer->shared.sensor, NULL);
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
  /* of_parse_phandle() in probe took a reference on this node */
  of_node_put(sensor->shared.np);
  sensor->shared.np = NULL;

  /* Phase 4: V4L2/media cleanup */
  device_remove_file(&client->dev, &dev_attr_rotate);
  device_remove_file(&client->dev, &dev_attr_enable);
  device_remove_file(&client->dev, &dev_attr_link_status);
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
