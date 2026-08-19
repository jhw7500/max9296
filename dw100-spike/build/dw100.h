/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* Copyright 2022 NXP */
/*
 * SPIKE-LOCAL backport of include/uapi/linux/dw100.h (mainline v6.1).
 *
 * In a real backport this file goes to include/uapi/linux/dw100.h and
 * V4L2_CID_USER_DW100_BASE is added to include/uapi/linux/v4l2-controls.h.
 * Here we define the BASE self-contained (with #ifndef guard) so the
 * out-of-tree module compiles without patching the BSP kernel headers.
 *
 * BASE value taken verbatim from mainline v6.1 v4l2-controls.h:
 *   #define V4L2_CID_USER_DW100_BASE (V4L2_CID_USER_BASE + 0x1190)
 * Confirmed no collision in BSP 5.10.35 (USER bases stop at 0x10c0).
 */
#ifndef __UAPI_DW100_H__
#define __UAPI_DW100_H__

#include <linux/v4l2-controls.h>

#ifndef V4L2_CID_USER_DW100_BASE
#define V4L2_CID_USER_DW100_BASE		(V4L2_CID_USER_BASE + 0x1190)
#endif

#define V4L2_CID_DW100_DEWARPING_16x16_VERTEX_MAP (V4L2_CID_USER_DW100_BASE + 1)

#endif
