// SPDX-License-Identifier: GPL-2.0

/*
 * A v4l2-mem2mem device that wraps the video codec MMAL component.
 *
 * Copyright 2018 Raspberry Pi (Trading) Ltd.
 * Author: Dave Stevenson (dave.stevenson@raspberrypi.com)
 *
 * Loosely based on the vim2m virtual driver by Pawel Osciak
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 *
 * Whilst this driver uses the v4l2_mem2mem framework, it does not need the
 * scheduling aspects, so will always take the buffers, pass them to the VPU,
 * and then signal the job as complete.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/syscalls.h>

#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-contig.h>

#include "vchiq-mmal/mmal-encodings.h"
#include "vchiq-mmal/mmal-msg.h"
#include "vchiq-mmal/mmal-parameters.h"
#include "vchiq-mmal/mmal-vchiq.h"

MODULE_IMPORT_NS(DMA_BUF);

/*
 * Default /dev/videoN node numbers for decode and encode.
 * Deliberately avoid the very low numbers as these are often taken by webcams
 * etc, and simple apps tend to only go for /dev/video0.
 */
static int decode_video_nr = 10;
module_param(decode_video_nr, int, 0644);
MODULE_PARM_DESC(decode_video_nr, "decoder video device number");

static int encode_video_nr = 11;
module_param(encode_video_nr, int, 0644);
MODULE_PARM_DESC(encode_video_nr, "encoder video device number");

static int isp_video_nr = 12;
module_param(isp_video_nr, int, 0644);
MODULE_PARM_DESC(isp_video_nr, "isp video device number");

static int deinterlace_video_nr = 18;
module_param(deinterlace_video_nr, int, 0644);
MODULE_PARM_DESC(deinterlace_video_nr, "deinterlace video device number");

static int encode_image_nr = 31;
module_param(encode_image_nr, int, 0644);
MODULE_PARM_DESC(encode_image_nr, "encoder image device number");

/*
 * Workaround for GStreamer v4l2convert component not considering Bayer formats
 * as raw, and therefore not considering a V4L2 device that supports them as
 * a suitable candidate.
 */
static bool disable_bayer;
module_param(disable_bayer, bool, 0644);
MODULE_PARM_DESC(disable_bayer, "Disable support for Bayer formats");

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "activates debug info (0-3)");

static bool advanced_deinterlace = true;
module_param(advanced_deinterlace, bool, 0644);
MODULE_PARM_DESC(advanced_deinterlace, "Use advanced deinterlace");

static int field_override;
module_param(field_override, int, 0644);
MODULE_PARM_DESC(field_override, "force TB(8)/BT(9) field");

enum bcm2835_codec_role {
	DECODE,
	ENCODE,
	ISP,
	DEINTERLACE,
	ENCODE_IMAGE,
	NUM_ROLES
};

static const char * const roles[] = {
	"decode",
	"encode",
	"isp",
	"image_fx",
	"encode_image",
};

static const char * const components[] = {
	"ril.video_decode",
	"ril.video_encode",
	"ril.isp",
	"ril.image_fx",
	"ril.image_encode",
};

/* Timeout for stop_streaming to allow all buffers to return */
#define COMPLETE_TIMEOUT (2 * HZ)

#define MIN_W		32
#define MIN_H		32
#define MAX_W_CODEC	1920
#define MAX_H_CODEC	1920
#define MAX_W_ISP	16384
#define MAX_H_ISP	16384
#define BPL_ALIGN	32
/*
 * The decoder spec supports the V4L2_EVENT_SOURCE_CHANGE event, but the docs
 * seem to want it to always be generated on startup, which prevents the client
 * from configuring the CAPTURE queue based on any parsing it has already done
 * which may save time and allow allocation of CAPTURE buffers early. Surely
 * SOURCE_CHANGE means something has changed, not just "always notify".
 *
 * For those clients that don't set the CAPTURE resolution, adopt a default
 * resolution that is seriously unlikely to be correct, therefore almost
 * guaranteed to get the SOURCE_CHANGE event.
 */
#define DEFAULT_WIDTH	32
#define DEFAULT_HEIGHT	32
/*
 * The unanswered question - what is the maximum size of a compressed frame?
 * V4L2 mandates that the encoded frame must fit in a single buffer. Sizing
 * that buffer is a compromise between wasting memory and risking not fitting.
 * The 1080P version of Big Buck Bunny has some frames that exceed 512kB.
 * Adopt a moderately arbitrary split at 720P for switching between 512 and
 * 768kB buffers.
 */
#define DEF_COMP_BUF_SIZE_GREATER_720P	(768 << 10)
#define DEF_COMP_BUF_SIZE_720P_OR_LESS	(512 << 10)
/* JPEG image can be very large. For paranoid reasons 4MB is used */
#define DEF_COMP_BUF_SIZE_JPEG (4096 << 10)

/* Flags that indicate a format can be used for capture/output */
#define MEM2MEM_CAPTURE		BIT(0)
#define MEM2MEM_OUTPUT		BIT(1)

#define MEM2MEM_NAME		"bcm2835-codec"

struct bcm2835_codec_fmt {
	u32	fourcc;
	int	depth;
	u8	bytesperline_align[NUM_ROLES];
	u32	flags;
	u32	mmal_fmt;
	int	size_multiplier_x2;
	bool	is_bayer;
};

static const struct bcm2835_codec_fmt supported_formats[] = {
	{
		/* YUV formats */
		.fourcc			= V4L2_PIX_FMT_YUV420,
		.depth			= 8,
		.bytesperline_align	= { 32, 64, 64, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_I420,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_YVU420,
		.depth			= 8,
		.bytesperline_align	= { 32, 64, 64, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_YV12,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_NV12,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_NV12,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_NV21,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_NV21,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_RGB565,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_RGB16,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_YUYV,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_YUYV,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_UYVY,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_UYVY,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_YVYU,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_YVYU,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_VYUY,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_VYUY,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_NV12_COL128,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_YUVUV128,
		.size_multiplier_x2	= 3,
	}, {
		/* RGB formats */
		.fourcc			= V4L2_PIX_FMT_RGB24,
		.depth			= 24,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_RGB24,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_BGR24,
		.depth			= 24,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BGR24,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_BGR32,
		.depth			= 32,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BGRA,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_RGBA32,
		.depth			= 32,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_RGBA,
		.size_multiplier_x2	= 2,
	}, {
		/* Bayer formats */
		/* 8 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB8,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB8,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR8,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR8,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG8,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG8,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG8,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG8,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* 10 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB10P,
		.depth			= 10,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB10P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR10P,
		.depth			= 10,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR10P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG10P,
		.depth			= 10,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG10P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG10P,
		.depth			= 10,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG10P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* 12 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB12P,
		.depth			= 12,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB12P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR12P,
		.depth			= 12,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR12P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG12P,
		.depth			= 12,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG12P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG12P,
		.depth			= 12,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG12P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* 14 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB14P,
		.depth			= 14,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB14P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR14P,
		.depth			= 14,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR14P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,

	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG14P,
		.depth			= 14,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG14P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG14P,
		.depth			= 14,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG14P,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* 16 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB16,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB16,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR16,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR16,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG16,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG16,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG16,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG16,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* Bayer formats unpacked to 16bpp */
		/* 10 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB10,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB10,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR10,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR10,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG10,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG10,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG10,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG10,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* 12 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB12,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB12,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR12,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR12,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG12,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG12,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG12,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG12,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* 14 bit */
		.fourcc			= V4L2_PIX_FMT_SRGGB14,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SRGGB14,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SBGGR14,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SBGGR14,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGRBG14,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGRBG14,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_SGBRG14,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BAYER_SGBRG14,
		.size_multiplier_x2	= 2,
		.is_bayer		= true,
	}, {
		/* Monochrome MIPI formats */
		/* 8 bit */
		.fourcc			= V4L2_PIX_FMT_GREY,
		.depth			= 8,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_GREY,
		.size_multiplier_x2	= 2,
	}, {
		/* 10 bit */
		.fourcc			= V4L2_PIX_FMT_Y10P,
		.depth			= 10,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_Y10P,
		.size_multiplier_x2	= 2,
	}, {
		/* 12 bit */
		.fourcc			= V4L2_PIX_FMT_Y12P,
		.depth			= 12,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_Y12P,
		.size_multiplier_x2	= 2,
	}, {
		/* 14 bit */
		.fourcc			= V4L2_PIX_FMT_Y14P,
		.depth			= 14,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_Y14P,
		.size_multiplier_x2	= 2,
	}, {
		/* 16 bit */
		.fourcc			= V4L2_PIX_FMT_Y16,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_Y16,
		.size_multiplier_x2	= 2,
	}, {
		/* 10 bit as 16bpp */
		.fourcc			= V4L2_PIX_FMT_Y10,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_Y10,
		.size_multiplier_x2	= 2,
	}, {
		/* 12 bit as 16bpp */
		.fourcc			= V4L2_PIX_FMT_Y12,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_Y12,
		.size_multiplier_x2	= 2,
	}, {
		/* 14 bit as 16bpp */
		.fourcc			= V4L2_PIX_FMT_Y14,
		.depth			= 16,
		.bytesperline_align	= { 32, 32, 32, 32, 32 },
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_Y14,
		.size_multiplier_x2	= 2,
	}, {
		/* Compressed formats */
		.fourcc			= V4L2_PIX_FMT_H264,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_H264,
	}, {
		.fourcc			= V4L2_PIX_FMT_JPEG,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_JPEG,
	}, {
		.fourcc			= V4L2_PIX_FMT_MJPEG,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_MJPEG,
	}, {
		.fourcc			= V4L2_PIX_FMT_MPEG4,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_MP4V,
	}, {
		.fourcc			= V4L2_PIX_FMT_H263,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_H263,
	}, {
		.fourcc			= V4L2_PIX_FMT_MPEG2,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_MP2V,
	}, {
		.fourcc			= V4L2_PIX_FMT_VC1_ANNEX_G,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_WVC1,
	}
};

struct bcm2835_codec_fmt_list {
	struct bcm2835_codec_fmt *list;
	unsigned int num_entries;
};

struct m2m_mmal_buffer {
	struct v4l2_m2m_buffer	m2m;
	struct mmal_buffer	mmal;
};

/* Per-queue, driver-specific private data */
struct bcm2835_codec_q_data {
	/*
	 * These parameters should be treated as gospel, with everything else
	 * being determined from them.
	 */
	/* Buffer width/height */
	unsigned int		bytesperline;
	unsigned int		height;
	/* Crop size used for selection handling */
	unsigned int		crop_width;
	unsigned int		crop_height;
	bool			selection_set;
	struct v4l2_fract	aspect_ratio;
	enum v4l2_field		field;

	unsigned int		sizeimage;
	unsigned int		sequence;
	struct bcm2835_codec_fmt	*fmt;

	/* One extra buffer header so we can send an EOS. */
	struct m2m_mmal_buffer	eos_buffer;
	bool			eos_buffer_in_use;	/* debug only */
};

struct bcm2835_codec_dev {
	struct platform_device *pdev;

	/* v4l2 devices */
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	/* mutex for the v4l2 device */
	struct mutex		dev_mutex;
	atomic_t		num_inst;

	/* allocated mmal instance and components */
	enum bcm2835_codec_role	role;
	/* The list of formats supported on input and output queues. */
	struct bcm2835_codec_fmt_list	supported_fmts[2];

	/*
	 * Max size supported varies based on role. Store during
	 * bcm2835_codec_create for use later.
	 */
	unsigned int max_w;
	unsigned int max_h;

	struct vchiq_mmal_instance	*instance;

	struct v4l2_m2m_dev	*m2m_dev;
};

struct bcm2835_codec_ctx {
	struct v4l2_fh		fh;
	struct bcm2835_codec_dev	*dev;

	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *gop_size;

	struct vchiq_mmal_component  *component;
	bool component_enabled;

	enum v4l2_colorspace	colorspace;
	enum v4l2_ycbcr_encoding ycbcr_enc;
	enum v4l2_xfer_func	xfer_func;
	enum v4l2_quantization	quant;

	int hflip;
	int vflip;

	/* Source and destination queue data */
	struct bcm2835_codec_q_data   q_data[2];
	s32  bitrate;
	unsigned int	framerate_num;
	unsigned int	framerate_denom;

	bool aborting;
	int num_ip_buffers;
	int num_op_buffers;
	struct completion frame_cmplt;
};

struct bcm2835_codec_driver {
	struct platform_device *pdev;
	struct media_device	mdev;

	struct bcm2835_codec_dev *encode;
	struct bcm2835_codec_dev *decode;
	struct bcm2835_codec_dev *isp;
	struct bcm2835_codec_dev *deinterlace;
	struct bcm2835_codec_dev *encode_image;
};

enum {
	V4L2_M2M_SRC = 0,
	V4L2_M2M_DST = 1,
};

static const struct bcm2835_codec_fmt *get_fmt(u32 mmal_fmt)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_formats); i++) {
		if (supported_formats[i].mmal_fmt == mmal_fmt &&
		    (!disable_bayer || !supported_formats[i].is_bayer))
			return &supported_formats[i];
	}
	return NULL;
}

static inline
struct bcm2835_codec_fmt_list *get_format_list(struct bcm2835_codec_dev *dev,
					       bool capture)
{
	return &dev->supported_fmts[capture ? 1 : 0];
}

static
struct bcm2835_codec_fmt *get_default_format(struct bcm2835_codec_dev *dev,
					     bool capture)
{
	return &dev->supported_fmts[capture ? 1 : 0].list[0];
}

static
struct bcm2835_codec_fmt *find_format_pix_fmt(u32 pix_fmt,
					      struct bcm2835_codec_dev *dev,
					      bool capture)
{
	struct bcm2835_codec_fmt *fmt;
	unsigned int k;
	struct bcm2835_codec_fmt_list *fmts =
					&dev->supported_fmts[capture ? 1 : 0];

	for (k = 0; k < fmts->num_entries; k++) {
		fmt = &fmts->list[k];
		if (fmt->fourcc == pix_fmt)
			break;
	}
	if (k == fmts->num_entries)
		return NULL;

	return &fmts->list[k];
}

static inline
struct bcm2835_codec_fmt *find_format(struct v4l2_format *f,
				      struct bcm2835_codec_dev *dev,
				      bool capture)
{
	return find_format_pix_fmt(f->fmt.pix_mp.pixelformat, dev, capture);
}

static inline struct bcm2835_codec_ctx *file2ctx(struct file *file)
{
	return container_of(file->private_data, struct bcm2835_codec_ctx, fh);
}

static struct bcm2835_codec_q_data *get_q_data(struct bcm2835_codec_ctx *ctx,
					       enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return &ctx->q_data[V4L2_M2M_SRC];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return &ctx->q_data[V4L2_M2M_DST];
	default:
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Invalid queue type %u\n",
			 __func__, type);
		break;
	}
	return NULL;
}

static struct vchiq_mmal_port *get_port_data(struct bcm2835_codec_ctx *ctx,
					     enum v4l2_buf_type type)
{
	if (!ctx->component)
		return NULL;

	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return &ctx->component->input[0];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return &ctx->component->output[0];
	default:
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Invalid queue type %u\n",
			 __func__, type);
		break;
	}
	return NULL;
}

/*
 * mem2mem callbacks
 */

/*
 * job_ready() - check whether an instance is ready to be scheduled to run
 */
static int job_ready(void *priv)
{
	struct bcm2835_codec_ctx *ctx = priv;

	if (!v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) &&
	    !v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx))
		return 0;

	return 1;
}

static void job_abort(void *priv)
{
	struct bcm2835_codec_ctx *ctx = priv;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s\n", __func__);
	/* Will cancel the transaction in the next interrupt handler */
	ctx->aborting = 1;
}

static inline unsigned int get_sizeimage(int bpl, int width, int height,
					 struct bcm2835_codec_fmt *fmt)
{
	if (fmt->flags & V4L2_FMT_FLAG_COMPRESSED) {
		if (fmt->fourcc == V4L2_PIX_FMT_JPEG)
			return DEF_COMP_BUF_SIZE_JPEG;

		if (width * height > 1280 * 720)
			return DEF_COMP_BUF_SIZE_GREATER_720P;
		else
			return DEF_COMP_BUF_SIZE_720P_OR_LESS;
	}

	if (fmt->fourcc != V4L2_PIX_FMT_NV12_COL128)
		return (bpl * height * fmt->size_multiplier_x2) >> 1;

	/*
	 * V4L2_PIX_FMT_NV12_COL128 is 128 pixel wide columns.
	 * bytesperline is the column stride in lines, so multiply by
	 * the number of columns and 128.
	 */
	return (ALIGN(width, 128) * bpl);
}

static inline unsigned int get_bytesperline(int width, int height,
					    struct bcm2835_codec_fmt *fmt,
					    enum bcm2835_codec_role role)
{
	if (fmt->fourcc != V4L2_PIX_FMT_NV12_COL128)
		return ALIGN((width * fmt->depth) >> 3,
			     fmt->bytesperline_align[role]);

	/*
	 * V4L2_PIX_FMT_NV12_COL128 passes the column stride in lines via
	 * bytesperline.
	 * The minimum value for this is sufficient for the base luma and chroma
	 * with no padding.
	 */
	return (height * 3) >> 1;
}

static void setup_mmal_port_format(struct bcm2835_codec_ctx *ctx,
				   struct bcm2835_codec_q_data *q_data,
				   struct vchiq_mmal_port *port)
{
	port->format.encoding = q_data->fmt->mmal_fmt;
	port->format.flags = 0;

	if (!(q_data->fmt->flags & V4L2_FMT_FLAG_COMPRESSED)) {
		if (q_data->fmt->mmal_fmt != MMAL_ENCODING_YUVUV128) {
			/* Raw image format - set width/height */
			port->es.video.width = (q_data->bytesperline << 3) /
							q_data->fmt->depth;
			port->es.video.height = q_data->height;
			port->es.video.crop.width = q_data->crop_width;
			port->es.video.crop.height = q_data->crop_height;
		} else {
			/* NV12_COL128 / YUVUV128 column format */
			/* Column stride in lines */
			port->es.video.width = q_data->bytesperline;
			port->es.video.height = q_data->height;
			port->es.video.crop.width = q_data->crop_width;
			port->es.video.crop.height = q_data->crop_height;
			port->format.flags = MMAL_ES_FORMAT_FLAG_COL_FMTS_WIDTH_IS_COL_STRIDE;
		}
		port->es.video.frame_rate.numerator = ctx->framerate_num;
		port->es.video.frame_rate.denominator = ctx->framerate_denom;
	} else {
		/* Compressed format - leave resolution as 0 for decode */
		if (ctx->dev->role == DECODE) {
			port->es.video.width = 0;
			port->es.video.height = 0;
			port->es.video.crop.width = 0;
			port->es.video.crop.height = 0;
		} else {
			port->es.video.width = q_data->crop_width;
			port->es.video.height = q_data->height;
			port->es.video.crop.width = q_data->crop_width;
			port->es.video.crop.height = q_data->crop_height;
			port->format.bitrate = ctx->bitrate;
			port->es.video.frame_rate.numerator = ctx->framerate_num;
			port->es.video.frame_rate.denominator = ctx->framerate_denom;
		}
	}
	port->es.video.crop.x = 0;
	port->es.video.crop.y = 0;

	port->current_buffer.size = q_data->sizeimage;
};

static void ip_buffer_cb(struct vchiq_mmal_instance *instance,
			 struct vchiq_mmal_port *port, int status,
			 struct mmal_buffer *mmal_buf)
{
	struct bcm2835_codec_ctx *ctx = port->cb_ctx/*, *curr_ctx*/;
	struct m2m_mmal_buffer *buf =
			container_of(mmal_buf, struct m2m_mmal_buffer, mmal);

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: port %p buf %p length %lu, flags %x\n",
		 __func__, port, mmal_buf, mmal_buf->length,
		 mmal_buf->mmal_flags);

	if (buf == &ctx->q_data[V4L2_M2M_SRC].eos_buffer) {
		/* Do we need to add lcoking to prevent multiple submission of
		 * the EOS, and therefore handle mutliple return here?
		 */
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: eos buffer returned.\n",
			 __func__);
		ctx->q_data[V4L2_M2M_SRC].eos_buffer_in_use = false;
		return;
	}

	if (status) {
		/* error in transfer */
		if (buf)
			/* there was a buffer with the error so return it */
			vb2_buffer_done(&buf->m2m.vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
		return;
	}
	if (mmal_buf->cmd) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Not expecting cmd msgs on ip callback - %08x\n",
			 __func__, mmal_buf->cmd);
		/*
		 * CHECKME: Should we return here. The buffer shouldn't have a
		 * message context or vb2 buf associated.
		 */
	}

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: no error. Return buffer %p\n",
		 __func__, &buf->m2m.vb.vb2_buf);
	vb2_buffer_done(&buf->m2m.vb.vb2_buf,
			port->enabled ? VB2_BUF_STATE_DONE :
					VB2_BUF_STATE_QUEUED);

	ctx->num_ip_buffers++;
	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: done %d input buffers\n",
		 __func__, ctx->num_ip_buffers);

	if (!port->enabled && atomic_read(&port->buffers_with_vpu))
		complete(&ctx->frame_cmplt);
}

static void queue_res_chg_event(struct bcm2835_codec_ctx *ctx)
{
	static const struct v4l2_event ev_src_ch = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes =
		V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	v4l2_event_queue_fh(&ctx->fh, &ev_src_ch);
}

static void send_eos_event(struct bcm2835_codec_ctx *ctx)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_EOS,
	};

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "Sending EOS event\n");

	v4l2_event_queue_fh(&ctx->fh, &ev);
}

static void color_mmal2v4l(struct bcm2835_codec_ctx *ctx, u32 encoding,
			   u32 color_space)
{
	int is_rgb;

	switch (encoding) {
	case MMAL_ENCODING_I420:
	case MMAL_ENCODING_YV12:
	case MMAL_ENCODING_NV12:
	case MMAL_ENCODING_NV21:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
		/* YUV based colourspaces */
		switch (color_space) {
		case MMAL_COLOR_SPACE_ITUR_BT601:
			ctx->colorspace = V4L2_COLORSPACE_SMPTE170M;
			break;

		case MMAL_COLOR_SPACE_ITUR_BT709:
			ctx->colorspace = V4L2_COLORSPACE_REC709;
			break;
		default:
			break;
		}
		break;
	default:
		/* RGB based colourspaces */
		ctx->colorspace = V4L2_COLORSPACE_SRGB;
		break;
	}
	ctx->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(ctx->colorspace);
	ctx->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(ctx->colorspace);
	is_rgb = ctx->colorspace == V4L2_COLORSPACE_SRGB;
	ctx->quant = V4L2_MAP_QUANTIZATION_DEFAULT(is_rgb, ctx->colorspace,
						   ctx->ycbcr_enc);
}

static void handle_fmt_changed(struct bcm2835_codec_ctx *ctx,
			       struct mmal_buffer *mmal_buf)
{
	struct bcm2835_codec_q_data *q_data;
	struct mmal_msg_event_format_changed *format =
		(struct mmal_msg_event_format_changed *)mmal_buf->buffer;
	struct mmal_parameter_video_interlace_type interlace;
	int interlace_size = sizeof(interlace);
	struct vb2_queue *vq;
	int ret;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Format changed: buff size min %u, rec %u, buff num min %u, rec %u\n",
		 __func__,
		 format->buffer_size_min,
		 format->buffer_size_recommended,
		 format->buffer_num_min,
		 format->buffer_num_recommended
		);
	if (format->format.type != MMAL_ES_TYPE_VIDEO) {
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Format changed but not video %u\n",
			 __func__, format->format.type);
		return;
	}
	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Format changed to %ux%u, crop %ux%u, colourspace %08X\n",
		 __func__, format->es.video.width, format->es.video.height,
		 format->es.video.crop.width, format->es.video.crop.height,
		 format->es.video.color_space);

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Format was %ux%u, crop %ux%u\n",
		 __func__, q_data->bytesperline, q_data->height,
		 q_data->crop_width, q_data->crop_height);

	q_data->crop_width = format->es.video.crop.width;
	q_data->crop_height = format->es.video.crop.height;
	/*
	 * Stop S_FMT updating crop_height should it be unaligned.
	 * Client can still update the crop region via S_SELECTION should it
	 * really want to, but the decoder is likely to complain that the
	 * format then doesn't match.
	 */
	q_data->selection_set = true;
	q_data->bytesperline = get_bytesperline(format->es.video.width,
						format->es.video.height,
						q_data->fmt, ctx->dev->role);

	q_data->height = format->es.video.height;
	q_data->sizeimage = format->buffer_size_min;
	if (format->es.video.color_space)
		color_mmal2v4l(ctx, format->format.encoding,
			       format->es.video.color_space);

	q_data->aspect_ratio.numerator = format->es.video.par.numerator;
	q_data->aspect_ratio.denominator = format->es.video.par.denominator;

	ret = vchiq_mmal_port_parameter_get(ctx->dev->instance,
					    &ctx->component->output[0],
					    MMAL_PARAMETER_VIDEO_INTERLACE_TYPE,
					    &interlace,
					    &interlace_size);
	if (!ret) {
		switch (interlace.mode) {
		case MMAL_INTERLACE_PROGRESSIVE:
		default:
			q_data->field = V4L2_FIELD_NONE;
			break;
		case MMAL_INTERLACE_FIELDS_INTERLEAVED_UPPER_FIRST:
			q_data->field = V4L2_FIELD_INTERLACED_TB;
			break;
		case MMAL_INTERLACE_FIELDS_INTERLEAVED_LOWER_FIRST:
			q_data->field = V4L2_FIELD_INTERLACED_BT;
			break;
		}
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: interlace mode %u, v4l2 field %u\n",
			 __func__, interlace.mode, q_data->field);
	} else {
		q_data->field = V4L2_FIELD_NONE;
	}

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vq->streaming)
		vq->last_buffer_dequeued = true;

	queue_res_chg_event(ctx);
}

static void op_buffer_cb(struct vchiq_mmal_instance *instance,
			 struct vchiq_mmal_port *port, int status,
			 struct mmal_buffer *mmal_buf)
{
	struct bcm2835_codec_ctx *ctx = port->cb_ctx;
	enum vb2_buffer_state buf_state = VB2_BUF_STATE_DONE;
	struct m2m_mmal_buffer *buf;
	struct vb2_v4l2_buffer *vb2;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev,
		 "%s: status:%d, buf:%p, length:%lu, flags %04x, pts %lld\n",
		 __func__, status, mmal_buf, mmal_buf->length,
		 mmal_buf->mmal_flags, mmal_buf->pts);

	buf = container_of(mmal_buf, struct m2m_mmal_buffer, mmal);
	vb2 = &buf->m2m.vb;

	if (status) {
		/* error in transfer */
		if (vb2) {
			/* there was a buffer with the error so return it */
			vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_ERROR);
		}
		return;
	}

	if (mmal_buf->cmd) {
		switch (mmal_buf->cmd) {
		case MMAL_EVENT_FORMAT_CHANGED:
		{
			handle_fmt_changed(ctx, mmal_buf);
			break;
		}
		default:
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Unexpected event on output callback - %08x\n",
				 __func__, mmal_buf->cmd);
			break;
		}
		return;
	}

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: length %lu, flags %x, idx %u\n",
		 __func__, mmal_buf->length, mmal_buf->mmal_flags,
		 vb2->vb2_buf.index);

	if (mmal_buf->length == 0) {
		/* stream ended, or buffer being returned during disable. */
		v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: Empty buffer - flags %04x",
			 __func__, mmal_buf->mmal_flags);
		if (!(mmal_buf->mmal_flags & MMAL_BUFFER_HEADER_FLAG_EOS)) {
			if (!port->enabled) {
				vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_QUEUED);
				if (atomic_read(&port->buffers_with_vpu))
					complete(&ctx->frame_cmplt);
			} else {
				vchiq_mmal_submit_buffer(ctx->dev->instance,
							 &ctx->component->output[0],
							 mmal_buf);
			}
			return;
		}
	}
	if (mmal_buf->mmal_flags & MMAL_BUFFER_HEADER_FLAG_EOS) {
		/* EOS packet from the VPU */
		send_eos_event(ctx);
		vb2->flags |= V4L2_BUF_FLAG_LAST;
	}

	if (mmal_buf->mmal_flags & MMAL_BUFFER_HEADER_FLAG_CORRUPTED)
		buf_state = VB2_BUF_STATE_ERROR;

	/* vb2 timestamps in nsecs, mmal in usecs */
	vb2->vb2_buf.timestamp = mmal_buf->pts * 1000;

	vb2_set_plane_payload(&vb2->vb2_buf, 0, mmal_buf->length);
	switch (mmal_buf->mmal_flags &
				(MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED |
				 MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST)) {
	case 0:
	case MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST: /* Bogus */
		vb2->field = V4L2_FIELD_NONE;
		break;
	case MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED:
		vb2->field = V4L2_FIELD_INTERLACED_BT;
		break;
	case (MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED |
	      MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST):
		vb2->field = V4L2_FIELD_INTERLACED_TB;
		break;
	}

	if (mmal_buf->mmal_flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
		vb2->flags |= V4L2_BUF_FLAG_KEYFRAME;

	vb2_buffer_done(&vb2->vb2_buf, buf_state);
	ctx->num_op_buffers++;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: done %d output buffers\n",
		 __func__, ctx->num_op_buffers);

	if (!port->enabled && atomic_read(&port->buffers_with_vpu))
		complete(&ctx->frame_cmplt);
}

/* vb2_to_mmal_buffer() - converts vb2 buffer header to MMAL
 *
 * Copies all the required fields from a VB2 buffer to the MMAL buffer header,
 * ready for sending to the VPU.
 */
static void vb2_to_mmal_buffer(struct m2m_mmal_buffer *buf,
			       struct vb2_v4l2_buffer *vb2)
{
	u64 pts;

	buf->mmal.mmal_flags = 0;
	if (vb2->flags & V4L2_BUF_FLAG_KEYFRAME)
		buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_FLAG_KEYFRAME;

	/*
	 * Adding this means that the data must be framed correctly as one frame
	 * per buffer. The underlying decoder has no such requirement, but it
	 * will reduce latency as the bistream parser will be kicked immediately
	 * to parse the frame, rather than relying on its own heuristics for
	 * when to wake up.
	 */
	buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

	buf->mmal.length = vb2->vb2_buf.planes[0].bytesused;
	/*
	 * Minor ambiguity in the V4L2 spec as to whether passing in a 0 length
	 * buffer, or one with V4L2_BUF_FLAG_LAST set denotes end of stream.
	 * Handle either.
	 */
	if (!buf->mmal.length || vb2->flags & V4L2_BUF_FLAG_LAST)
		buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_FLAG_EOS;

	/* vb2 timestamps in nsecs, mmal in usecs */
	pts = vb2->vb2_buf.timestamp;
	do_div(pts, 1000);
	buf->mmal.pts = pts;
	buf->mmal.dts = MMAL_TIME_UNKNOWN;

	switch (field_override ? field_override : vb2->field) {
	default:
	case V4L2_FIELD_NONE:
		break;
	case V4L2_FIELD_INTERLACED_BT:
		buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED;
		break;
	case V4L2_FIELD_INTERLACED_TB:
		buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED |
					MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
		break;
	}
}

/* device_run() - prepares and starts the device
 *
 * This simulates all the immediate preparations required before starting
 * a device. This will be called by the framework when it decides to schedule
 * a particular instance.
 */
static void device_run(void *priv)
{
	struct bcm2835_codec_ctx *ctx = priv;
	struct bcm2835_codec_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct m2m_mmal_buffer *src_m2m_buf = NULL, *dst_m2m_buf = NULL;
	struct v4l2_m2m_buffer *m2m;
	int ret;

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: off we go\n", __func__);

	if (ctx->fh.m2m_ctx->out_q_ctx.q.streaming) {
		src_buf = v4l2_m2m_buf_remove(&ctx->fh.m2m_ctx->out_q_ctx);
		if (src_buf) {
			m2m = container_of(src_buf, struct v4l2_m2m_buffer, vb);
			src_m2m_buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);
			vb2_to_mmal_buffer(src_m2m_buf, src_buf);

			ret = vchiq_mmal_submit_buffer(dev->instance,
						       &ctx->component->input[0],
						       &src_m2m_buf->mmal);
			v4l2_dbg(3, debug, &ctx->dev->v4l2_dev,
				 "%s: Submitted ip buffer len %lu, pts %llu, flags %04x\n",
				 __func__, src_m2m_buf->mmal.length,
				 src_m2m_buf->mmal.pts,
				 src_m2m_buf->mmal.mmal_flags);
			if (ret)
				v4l2_err(&ctx->dev->v4l2_dev,
					 "%s: Failed submitting ip buffer\n",
					 __func__);
		}
	}

	if (ctx->fh.m2m_ctx->cap_q_ctx.q.streaming) {
		dst_buf = v4l2_m2m_buf_remove(&ctx->fh.m2m_ctx->cap_q_ctx);
		if (dst_buf) {
			m2m = container_of(dst_buf, struct v4l2_m2m_buffer, vb);
			dst_m2m_buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);
			vb2_to_mmal_buffer(dst_m2m_buf, dst_buf);

			v4l2_dbg(3, debug, &ctx->dev->v4l2_dev,
				 "%s: Submitted op buffer\n", __func__);
			ret = vchiq_mmal_submit_buffer(dev->instance,
						       &ctx->component->output[0],
						       &dst_m2m_buf->mmal);
			if (ret)
				v4l2_err(&ctx->dev->v4l2_dev,
					 "%s: Failed submitting op buffer\n",
					 __func__);
		}
	}

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: Submitted src %p, dst %p\n",
		 __func__, src_m2m_buf, dst_m2m_buf);

	/* Complete the job here. */
	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

/*
 * video ioctls
 */
static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct bcm2835_codec_dev *dev = video_drvdata(file);

	strncpy(cap->driver, MEM2MEM_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, dev->vfd.name, sizeof(cap->card) - 1);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 MEM2MEM_NAME);
	return 0;
}

static int enum_fmt(struct v4l2_fmtdesc *f, struct bcm2835_codec_ctx *ctx,
		    bool capture)
{
	struct bcm2835_codec_fmt *fmt;
	struct bcm2835_codec_fmt_list *fmts =
					get_format_list(ctx->dev, capture);

	if (f->index < fmts->num_entries) {
		/* Format found */
		fmt = &fmts->list[f->index];
		f->pixelformat = fmt->fourcc;
		f->flags = fmt->flags;
		return 0;
	}

	/* Format not found */
	return -EINVAL;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	return enum_fmt(f, ctx, true);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	return enum_fmt(f, ctx, false);
}

static int vidioc_g_fmt(struct bcm2835_codec_ctx *ctx, struct v4l2_format *f)
{
	struct vb2_queue *vq;
	struct bcm2835_codec_q_data *q_data;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);

	f->fmt.pix_mp.width			= q_data->crop_width;
	f->fmt.pix_mp.height			= q_data->height;
	f->fmt.pix_mp.pixelformat		= q_data->fmt->fourcc;
	f->fmt.pix_mp.field			= q_data->field;
	f->fmt.pix_mp.colorspace		= ctx->colorspace;
	f->fmt.pix_mp.plane_fmt[0].sizeimage	= q_data->sizeimage;
	f->fmt.pix_mp.plane_fmt[0].bytesperline	= q_data->bytesperline;
	f->fmt.pix_mp.num_planes		= 1;
	f->fmt.pix_mp.ycbcr_enc			= ctx->ycbcr_enc;
	f->fmt.pix_mp.quantization		= ctx->quant;
	f->fmt.pix_mp.xfer_func			= ctx->xfer_func;

	memset(f->fmt.pix_mp.plane_fmt[0].reserved, 0,
	       sizeof(f->fmt.pix_mp.plane_fmt[0].reserved));

	return 0;
}

static int vidioc_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_try_fmt(struct bcm2835_codec_ctx *ctx, struct v4l2_format *f,
			  struct bcm2835_codec_fmt *fmt)
{
	unsigned int sizeimage, min_bytesperline;

	/*
	 * The V4L2 specification requires the driver to correct the format
	 * struct if any of the dimensions is unsupported
	 */
	if (f->fmt.pix_mp.width > ctx->dev->max_w)
		f->fmt.pix_mp.width = ctx->dev->max_w;
	if (f->fmt.pix_mp.height > ctx->dev->max_h)
		f->fmt.pix_mp.height = ctx->dev->max_h;

	if (!(fmt->flags & V4L2_FMT_FLAG_COMPRESSED)) {
		/* Only clip min w/h on capture. Treat 0x0 as unknown. */
		if (f->fmt.pix_mp.width < MIN_W)
			f->fmt.pix_mp.width = MIN_W;
		if (f->fmt.pix_mp.height < MIN_H)
			f->fmt.pix_mp.height = MIN_H;

		/*
		 * For decoders and image encoders the buffer must have
		 * a vertical alignment of 16 lines.
		 * The selection will reflect any cropping rectangle when only
		 * some of the pixels are active.
		 */
		if (ctx->dev->role == DECODE || ctx->dev->role == ENCODE_IMAGE)
			f->fmt.pix_mp.height = ALIGN(f->fmt.pix_mp.height, 16);
	}
	f->fmt.pix_mp.num_planes = 1;
	min_bytesperline = get_bytesperline(f->fmt.pix_mp.width,
					    f->fmt.pix_mp.height,
					    fmt, ctx->dev->role);
	if (f->fmt.pix_mp.plane_fmt[0].bytesperline < min_bytesperline)
		f->fmt.pix_mp.plane_fmt[0].bytesperline = min_bytesperline;
	f->fmt.pix_mp.plane_fmt[0].bytesperline =
		ALIGN(f->fmt.pix_mp.plane_fmt[0].bytesperline,
		      fmt->bytesperline_align[ctx->dev->role]);

	sizeimage = get_sizeimage(f->fmt.pix_mp.plane_fmt[0].bytesperline,
				  f->fmt.pix_mp.width, f->fmt.pix_mp.height,
				  fmt);
	/*
	 * Drivers must set sizeimage for uncompressed formats
	 * Compressed formats allow the client to request an alternate
	 * size for the buffer.
	 */
	if (!(fmt->flags & V4L2_FMT_FLAG_COMPRESSED) ||
	    f->fmt.pix_mp.plane_fmt[0].sizeimage < sizeimage)
		f->fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;

	memset(f->fmt.pix_mp.plane_fmt[0].reserved, 0,
	       sizeof(f->fmt.pix_mp.plane_fmt[0].reserved));

	if (ctx->dev->role == DECODE || ctx->dev->role == DEINTERLACE) {
		switch (f->fmt.pix_mp.field) {
		/*
		 * All of this is pretty much guesswork as we'll set the
		 * interlace format correctly come format changed, and signal
		 * it appropriately on each buffer.
		 */
		default:
		case V4L2_FIELD_NONE:
		case V4L2_FIELD_ANY:
			f->fmt.pix_mp.field = V4L2_FIELD_NONE;
			break;
		case V4L2_FIELD_INTERLACED:
			f->fmt.pix_mp.field = V4L2_FIELD_INTERLACED;
			break;
		case V4L2_FIELD_TOP:
		case V4L2_FIELD_BOTTOM:
		case V4L2_FIELD_INTERLACED_TB:
			f->fmt.pix_mp.field = V4L2_FIELD_INTERLACED_TB;
			break;
		case V4L2_FIELD_INTERLACED_BT:
			f->fmt.pix_mp.field = V4L2_FIELD_INTERLACED_BT;
			break;
		}
	} else {
		f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	}

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct bcm2835_codec_fmt *fmt;
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	fmt = find_format(f, ctx->dev, true);
	if (!fmt) {
		f->fmt.pix_mp.pixelformat = get_default_format(ctx->dev,
							       true)->fourcc;
		fmt = find_format(f, ctx->dev, true);
	}

	return vidioc_try_fmt(ctx, f, fmt);
}

static int vidioc_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct bcm2835_codec_fmt *fmt;
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	fmt = find_format(f, ctx->dev, false);
	if (!fmt) {
		f->fmt.pix_mp.pixelformat = get_default_format(ctx->dev,
							       false)->fourcc;
		fmt = find_format(f, ctx->dev, false);
	}

	if (!f->fmt.pix_mp.colorspace)
		f->fmt.pix_mp.colorspace = ctx->colorspace;

	return vidioc_try_fmt(ctx, f, fmt);
}

static int vidioc_s_fmt(struct bcm2835_codec_ctx *ctx, struct v4l2_format *f,
			unsigned int requested_height)
{
	struct bcm2835_codec_q_data *q_data;
	struct vb2_queue *vq;
	struct vchiq_mmal_port *port;
	bool update_capture_port = false;
	bool reenable_port = false;
	int ret;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev,	"Setting format for type %d, wxh: %dx%d, fmt: %08x, size %u\n",
		 f->type, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		 f->fmt.pix_mp.pixelformat,
		 f->fmt.pix_mp.plane_fmt[0].sizeimage);

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	if (vb2_is_busy(vq)) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	q_data->fmt = find_format(f, ctx->dev,
				  f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	q_data->crop_width = f->fmt.pix_mp.width;
	q_data->height = f->fmt.pix_mp.height;
	if (!q_data->selection_set ||
	    (q_data->fmt->flags & V4L2_FMT_FLAG_COMPRESSED))
		q_data->crop_height = requested_height;

	/*
	 * Copying the behaviour of vicodec which retains a single set of
	 * colorspace parameters for both input and output.
	 */
	ctx->colorspace = f->fmt.pix_mp.colorspace;
	ctx->xfer_func = f->fmt.pix_mp.xfer_func;
	ctx->ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
	ctx->quant = f->fmt.pix_mp.quantization;

	q_data->field = f->fmt.pix_mp.field;

	/* All parameters should have been set correctly by try_fmt */
	q_data->bytesperline = f->fmt.pix_mp.plane_fmt[0].bytesperline;
	q_data->sizeimage = f->fmt.pix_mp.plane_fmt[0].sizeimage;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev,	"Calculated bpl as %u, size %u\n",
		 q_data->bytesperline, q_data->sizeimage);

	if ((ctx->dev->role == DECODE || ctx->dev->role == ENCODE_IMAGE) &&
	    q_data->fmt->flags & V4L2_FMT_FLAG_COMPRESSED &&
	    q_data->crop_width && q_data->height) {
		/*
		 * On the decoder or image encoder, if provided with
		 * a resolution on the input side, then replicate that
		 * to the output side.
		 * GStreamer appears not to support V4L2_EVENT_SOURCE_CHANGE,
		 * nor set up a resolution on the output side, therefore
		 * we can't decode anything at a resolution other than the
		 * default one.
		 */
		struct bcm2835_codec_q_data *q_data_dst =
						&ctx->q_data[V4L2_M2M_DST];

		q_data_dst->crop_width = q_data->crop_width;
		q_data_dst->crop_height = q_data->crop_height;
		q_data_dst->height = ALIGN(q_data->crop_height, 16);

		q_data_dst->bytesperline =
			get_bytesperline(f->fmt.pix_mp.width,
					 f->fmt.pix_mp.height,
					 q_data_dst->fmt, ctx->dev->role);
		q_data_dst->sizeimage = get_sizeimage(q_data_dst->bytesperline,
						      q_data_dst->crop_width,
						      q_data_dst->height,
						      q_data_dst->fmt);
		update_capture_port = true;
	}

	/* If we have a component then setup the port as well */
	port = get_port_data(ctx, vq->type);
	if (!port)
		return 0;

	if (port->enabled) {
		unsigned int num_buffers;

		/*
		 * This should only ever happen with DECODE and the MMAL output
		 * port that has been enabled for resolution changed events.
		 * In this case no buffers have been allocated or sent to the
		 * component, so warn on that.
		 */
		WARN_ON(ctx->dev->role != DECODE ||
			f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
			atomic_read(&port->buffers_with_vpu));

		/*
		 * Disable will reread the port format, so retain buffer count.
		 */
		num_buffers = port->current_buffer.num;

		ret = vchiq_mmal_port_disable(ctx->dev->instance, port);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Error disabling port update buffer count, ret %d\n",
				 __func__, ret);

		port->current_buffer.num = num_buffers;

		reenable_port = true;
	}

	setup_mmal_port_format(ctx, q_data, port);
	ret = vchiq_mmal_port_set_format(ctx->dev->instance, port);
	if (ret) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed vchiq_mmal_port_set_format on port, ret %d\n",
			 __func__, ret);
		ret = -EINVAL;
	}

	if (q_data->sizeimage < port->minimum_buffer.size) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Current buffer size of %u < min buf size %u - driver mismatch to MMAL\n",
			 __func__, q_data->sizeimage,
			 port->minimum_buffer.size);
	}

	if (reenable_port) {
		ret = vchiq_mmal_port_enable(ctx->dev->instance,
					     port,
					     op_buffer_cb);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling o/p port, ret %d\n",
				 __func__, ret);
	}
	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev,	"Set format for type %d, wxh: %dx%d, fmt: %08x, size %u\n",
		 f->type, q_data->crop_width, q_data->height,
		 q_data->fmt->fourcc, q_data->sizeimage);

	if (update_capture_port) {
		struct vchiq_mmal_port *port_dst = &ctx->component->output[0];
		struct bcm2835_codec_q_data *q_data_dst =
						&ctx->q_data[V4L2_M2M_DST];

		setup_mmal_port_format(ctx, q_data_dst, port_dst);
		ret = vchiq_mmal_port_set_format(ctx->dev->instance, port_dst);
		if (ret) {
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed vchiq_mmal_port_set_format on output port, ret %d\n",
				 __func__, ret);
			ret = -EINVAL;
		}
	}
	return ret;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	unsigned int height = f->fmt.pix_mp.height;
	int ret;

	ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	return vidioc_s_fmt(file2ctx(file), f, height);
}

static int vidioc_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	unsigned int height = f->fmt.pix_mp.height;
	int ret;

	ret = vidioc_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	ret = vidioc_s_fmt(file2ctx(file), f, height);
	return ret;
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data;

	/*
	 * The selection API takes V4L2_BUF_TYPE_VIDEO_CAPTURE and
	 * V4L2_BUF_TYPE_VIDEO_OUTPUT, even if the device implements the MPLANE
	 * API. The V4L2 core will have converted the MPLANE variants to
	 * non-MPLANE.
	 * Open code this instead of using get_q_data in this case.
	 */
	switch (s->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		/* CAPTURE on encoder is not valid. */
		if (ctx->dev->role == ENCODE || ctx->dev->role == ENCODE_IMAGE)
			return -EINVAL;
		q_data = &ctx->q_data[V4L2_M2M_DST];
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		/* OUTPUT on deoder is not valid. */
		if (ctx->dev->role == DECODE)
			return -EINVAL;
		q_data = &ctx->q_data[V4L2_M2M_SRC];
		break;
	default:
		return -EINVAL;
	}

	switch (ctx->dev->role) {
	case DECODE:
		switch (s->target) {
		case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		case V4L2_SEL_TGT_COMPOSE:
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = q_data->crop_width;
			s->r.height = q_data->crop_height;
			break;
		case V4L2_SEL_TGT_COMPOSE_BOUNDS:
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = q_data->crop_width;
			s->r.height = q_data->crop_height;
			break;
		case V4L2_SEL_TGT_CROP_BOUNDS:
		case V4L2_SEL_TGT_CROP_DEFAULT:
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = (q_data->bytesperline << 3) /
						q_data->fmt->depth;
			s->r.height = q_data->height;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ENCODE:
	case ENCODE_IMAGE:
		switch (s->target) {
		case V4L2_SEL_TGT_CROP_DEFAULT:
		case V4L2_SEL_TGT_CROP_BOUNDS:
			s->r.top = 0;
			s->r.left = 0;
			s->r.width = q_data->bytesperline;
			s->r.height = q_data->height;
			break;
		case V4L2_SEL_TGT_CROP:
			s->r.top = 0;
			s->r.left = 0;
			s->r.width = q_data->crop_width;
			s->r.height = q_data->crop_height;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ISP:
	case DEINTERLACE:
		if (s->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			switch (s->target) {
			case V4L2_SEL_TGT_COMPOSE_DEFAULT:
			case V4L2_SEL_TGT_COMPOSE:
				s->r.left = 0;
				s->r.top = 0;
				s->r.width = q_data->crop_width;
				s->r.height = q_data->crop_height;
				break;
			case V4L2_SEL_TGT_COMPOSE_BOUNDS:
				s->r.left = 0;
				s->r.top = 0;
				s->r.width = q_data->crop_width;
				s->r.height = q_data->crop_height;
				break;
			default:
				return -EINVAL;
			}
		} else {
			/* must be V4L2_BUF_TYPE_VIDEO_OUTPUT */
			switch (s->target) {
			case V4L2_SEL_TGT_CROP_DEFAULT:
			case V4L2_SEL_TGT_CROP_BOUNDS:
				s->r.top = 0;
				s->r.left = 0;
				s->r.width = q_data->bytesperline;
				s->r.height = q_data->height;
				break;
			case V4L2_SEL_TGT_CROP:
				s->r.top = 0;
				s->r.left = 0;
				s->r.width = q_data->crop_width;
				s->r.height = q_data->crop_height;
				break;
			default:
				return -EINVAL;
			}
		}
		break;
	case NUM_ROLES:
		break;
	}

	return 0;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data = NULL;
	struct vchiq_mmal_port *port = NULL;
	int ret;

	/*
	 * The selection API takes V4L2_BUF_TYPE_VIDEO_CAPTURE and
	 * V4L2_BUF_TYPE_VIDEO_OUTPUT, even if the device implements the MPLANE
	 * API. The V4L2 core will have converted the MPLANE variants to
	 * non-MPLANE.
	 *
	 * Open code this instead of using get_q_data in this case.
	 */
	switch (s->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		/* CAPTURE on encoder is not valid. */
		if (ctx->dev->role == ENCODE || ctx->dev->role == ENCODE_IMAGE)
			return -EINVAL;
		q_data = &ctx->q_data[V4L2_M2M_DST];
		if (ctx->component)
			port = &ctx->component->output[0];
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		/* OUTPUT on deoder is not valid. */
		if (ctx->dev->role == DECODE)
			return -EINVAL;
		q_data = &ctx->q_data[V4L2_M2M_SRC];
		if (ctx->component)
			port = &ctx->component->input[0];
		break;
	default:
		return -EINVAL;
	}

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: ctx %p, type %d, q_data %p, target %d, rect x/y %d/%d, w/h %ux%u\n",
		 __func__, ctx, s->type, q_data, s->target, s->r.left, s->r.top,
		 s->r.width, s->r.height);

	switch (ctx->dev->role) {
	case DECODE:
		switch (s->target) {
		case V4L2_SEL_TGT_COMPOSE:
			/* Accept cropped image */
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = min(s->r.width, q_data->crop_width);
			s->r.height = min(s->r.height, q_data->height);
			q_data->crop_width = s->r.width;
			q_data->crop_height = s->r.height;
			q_data->selection_set = true;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ENCODE:
	case ENCODE_IMAGE:
		switch (s->target) {
		case V4L2_SEL_TGT_CROP:
			/* Only support crop from (0,0) */
			s->r.top = 0;
			s->r.left = 0;
			s->r.width = min(s->r.width, q_data->crop_width);
			s->r.height = min(s->r.height, q_data->height);
			q_data->crop_width = s->r.width;
			q_data->crop_height = s->r.height;
			q_data->selection_set = true;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ISP:
	case DEINTERLACE:
		if (s->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			switch (s->target) {
			case V4L2_SEL_TGT_COMPOSE:
				/* Accept cropped image */
				s->r.left = 0;
				s->r.top = 0;
				s->r.width = min(s->r.width, q_data->crop_width);
				s->r.height = min(s->r.height, q_data->height);
				q_data->crop_width = s->r.width;
				q_data->crop_height = s->r.height;
				q_data->selection_set = true;
				break;
			default:
				return -EINVAL;
			}
			break;
		} else {
			/* must be V4L2_BUF_TYPE_VIDEO_OUTPUT */
			switch (s->target) {
			case V4L2_SEL_TGT_CROP:
				/* Only support crop from (0,0) */
				s->r.top = 0;
				s->r.left = 0;
				s->r.width = min(s->r.width, q_data->crop_width);
				s->r.height = min(s->r.height, q_data->height);
				q_data->crop_width = s->r.width;
				q_data->crop_height = s->r.height;
				q_data->selection_set = true;
				break;
			default:
				return -EINVAL;
			}
			break;
		}
	case NUM_ROLES:
		break;
	}

	if (!port)
		return 0;

	setup_mmal_port_format(ctx, q_data, port);
	ret = vchiq_mmal_port_set_format(ctx->dev->instance, port);
	if (ret) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed vchiq_mmal_port_set_format on port, ret %d\n",
			 __func__, ret);
		return -EINVAL;
	}

	return 0;
}

static int vidioc_s_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *parm)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	if (!parm->parm.output.timeperframe.denominator ||
	    !parm->parm.output.timeperframe.numerator)
		return -EINVAL;

	ctx->framerate_num =
			parm->parm.output.timeperframe.denominator;
	ctx->framerate_denom =
			parm->parm.output.timeperframe.numerator;

	parm->parm.output.capability = V4L2_CAP_TIMEPERFRAME;

	return 0;
}

static int vidioc_g_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *parm)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	parm->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.output.timeperframe.denominator =
			ctx->framerate_num;
	parm->parm.output.timeperframe.numerator =
			ctx->framerate_denom;

	return 0;
}

static int vidioc_g_pixelaspect(struct file *file, void *fh, int type,
				struct v4l2_fract *f)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	/*
	 * The selection API takes V4L2_BUF_TYPE_VIDEO_CAPTURE and
	 * V4L2_BUF_TYPE_VIDEO_OUTPUT, even if the device implements the MPLANE
	 * API. The V4L2 core will have converted the MPLANE variants to
	 * non-MPLANE.
	 * Open code this instead of using get_q_data in this case.
	 */
	if (ctx->dev->role != DECODE)
		return -ENOIOCTLCMD;

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	*f = ctx->q_data[V4L2_M2M_DST].aspect_ratio;

	return 0;
}

static int vidioc_subscribe_evt(struct v4l2_fh *fh,
				const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static int bcm2835_codec_set_level_profile(struct bcm2835_codec_ctx *ctx,
					   struct v4l2_ctrl *ctrl)
{
	struct mmal_parameter_video_profile param;
	int param_size = sizeof(param);
	int ret;

	/*
	 * Level and Profile are set via the same MMAL parameter.
	 * Retrieve the current settings and amend the one that has changed.
	 */
	ret = vchiq_mmal_port_parameter_get(ctx->dev->instance,
					    &ctx->component->output[0],
					    MMAL_PARAMETER_PROFILE,
					    &param,
					    &param_size);
	if (ret)
		return ret;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
			param.profile = MMAL_VIDEO_PROFILE_H264_BASELINE;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
			param.profile =
				MMAL_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
			param.profile = MMAL_VIDEO_PROFILE_H264_MAIN;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
			param.profile = MMAL_VIDEO_PROFILE_H264_HIGH;
			break;
		default:
			/* Should never get here */
			break;
		}
		break;

	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
			param.level = MMAL_VIDEO_LEVEL_H264_1;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1B:
			param.level = MMAL_VIDEO_LEVEL_H264_1b;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
			param.level = MMAL_VIDEO_LEVEL_H264_11;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
			param.level = MMAL_VIDEO_LEVEL_H264_12;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
			param.level = MMAL_VIDEO_LEVEL_H264_13;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
			param.level = MMAL_VIDEO_LEVEL_H264_2;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
			param.level = MMAL_VIDEO_LEVEL_H264_21;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
			param.level = MMAL_VIDEO_LEVEL_H264_22;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
			param.level = MMAL_VIDEO_LEVEL_H264_3;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
			param.level = MMAL_VIDEO_LEVEL_H264_31;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
			param.level = MMAL_VIDEO_LEVEL_H264_32;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
			param.level = MMAL_VIDEO_LEVEL_H264_4;
			break;
		/*
		 * Note that the hardware spec is level 4.0. Levels above that
		 * are there for correctly encoding the headers and may not
		 * be able to keep up with real-time.
		 */
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
			param.level = MMAL_VIDEO_LEVEL_H264_41;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
			param.level = MMAL_VIDEO_LEVEL_H264_42;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
			param.level = MMAL_VIDEO_LEVEL_H264_5;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
			param.level = MMAL_VIDEO_LEVEL_H264_51;
			break;
		default:
			/* Should never get here */
			break;
		}
	}
	ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
					    &ctx->component->output[0],
					    MMAL_PARAMETER_PROFILE,
					    &param,
					    param_size);

	return ret;
}

/**
 * Returns the n of consecutive macro blocks to be encoded as intra such that a whole frame
 * is refreshed after the specified intra refresh period (accounting for rounding errors)
 */
static int helper_calculate_macroblocks(struct bcm2835_codec_ctx *ctx,int width,int height,int intra_refresh_period){
	u32 mbs=0;
	mbs = ALIGN(width, 16) * ALIGN(height, 16);
	mbs /= 16 * 16;
	if (mbs % intra_refresh_period)
		mbs++;
	mbs /= intra_refresh_period;
	v4l2_err(&ctx->dev->v4l2_dev, "helper_calculate_macroblocks: %dx%d@%d->%d\n",width,height,intra_refresh_period,mbs);
	return mbs;
}

static void helper_print_mmal_parameter_intra_refresh(struct bcm2835_codec_ctx *ctx,const char* TAG,struct mmal_parameter_intra_refresh* param){
	v4l2_err(&ctx->dev->v4l2_dev, "%s mmal_parameter_intra_refresh:{refresh_mode:%d air_mbs:%d air_ref:%d cir_mbs:%d pir_mbs:%d}\n",
		 TAG,(int)param->refresh_mode,param->air_mbs,param->air_ref,param->cir_mbs,param->pir_mbs);
}

static int helper_set_h264_intra(struct bcm2835_codec_ctx *ctx,int intra_value){
	struct mmal_parameter_intra_refresh param;
	u32 param_size;
	int get_status;
	int ret;
	// To calculate cir_mbs param, we need to know width and height
	int width_px=ctx->q_data[0].crop_width;
	int height_px=ctx->q_data[0].crop_height;
	v4l2_err(&ctx->dev->v4l2_dev, "helper_set_h264_intra %d\n",intra_value);
	if(intra_value<=0){
	    // No need to change anything in mmal.
	    return 0;
	}
	// Get first so we don't overwrite anything unexpectedly
	param_size = sizeof(struct mmal_parameter_intra_refresh);
	get_status = vchiq_mmal_port_parameter_get(ctx->dev->instance,
						   &ctx->component->output[0],
						   MMAL_PARAMETER_VIDEO_INTRA_REFRESH,
						   &param,
						   &param_size);
	if (get_status != 0)
	{
		v4l2_err(&ctx->dev->v4l2_dev, "Unable to get existing H264 intra-refresh values. Please update your firmware %d\n",
			 get_status);
		// Set some defaults, don't just pass random stack data
		param.air_mbs = param.air_ref = param.cir_mbs = param.pir_mbs = 0;
	}else{
		helper_print_mmal_parameter_intra_refresh(ctx,"Get from mmal first",&param);
		helper_calculate_macroblocks(ctx,width_px,height_px,10);
	}
	// TODO map types
	param.refresh_mode=MMAL_VIDEO_INTRA_REFRESH_CYCLIC_MROWS;
	param.cir_mbs=intra_value;
	ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
					    &ctx->component->output[0],
					    MMAL_PARAMETER_VIDEO_INTRA_REFRESH,
					    &param,
					    sizeof(struct mmal_parameter_intra_refresh));
	// After setting, get the stuff again and print it out for debugging
	param_size = sizeof(struct mmal_parameter_intra_refresh);
	get_status = vchiq_mmal_port_parameter_get(ctx->dev->instance,
						   &ctx->component->output[0],
						   MMAL_PARAMETER_VIDEO_INTRA_REFRESH,
						   &param,
						   &param_size);
	if(get_status!=0){
		v4l2_err(&ctx->dev->v4l2_dev, "After setting mmal get fails ?\n");
	}else{
		helper_print_mmal_parameter_intra_refresh(ctx,"Get from mmal second",&param);
	}
	return ret;
}

#define VCOS_ALIGN_DOWN(p,n) (((ptrdiff_t)(p)) & ~((n)-1))
#define VCOS_ALIGN_UP(p,n) VCOS_ALIGN_DOWN((ptrdiff_t)(p)+(n)-1,(n))

static int helper_set_h264_slice(struct bcm2835_codec_ctx *ctx,int slice_value){
	u32 mmal_param;
	int ret;
	v4l2_err(&ctx->dev->v4l2_dev, "helper_set_h264_slice %d\n",slice_value);
	if(slice_value<=0)return 0; // Nothing to do
	mmal_param=slice_value;
	ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
					    &ctx->component->output[0],
					    MMAL_PARAMETER_MB_ROWS_PER_SLICE,
					    &mmal_param,
					    sizeof(u32));
	if (ret != 0){
		v4l2_err(&ctx->dev->v4l2_dev, "helper_set_h264_slice %d failed\n",mmal_param);
	}else{
		v4l2_err(&ctx->dev->v4l2_dev, "helper_set_h264_slice %d success\n",mmal_param);
	}
	return ret;
}

static int bcm2835_codec_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bcm2835_codec_ctx *ctx =
		container_of(ctrl->handler, struct bcm2835_codec_ctx, hdl);
	int ret = 0;

	if (ctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		ctx->bitrate = ctrl->val;
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_BIT_RATE,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE: {
		u32 bitrate_mode;

		if (!ctx->component)
			break;

		switch (ctrl->val) {
		default:
		case V4L2_MPEG_VIDEO_BITRATE_MODE_VBR:
			bitrate_mode = MMAL_VIDEO_RATECONTROL_VARIABLE;
			break;
		case V4L2_MPEG_VIDEO_BITRATE_MODE_CBR:
			bitrate_mode = MMAL_VIDEO_RATECONTROL_CONSTANT;
			break;
		}

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_RATECONTROL,
						    &bitrate_mode,
						    sizeof(bitrate_mode));
		break;
	}
	case V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_HEADER_MODE:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_ENCODE_HEADERS_WITH_FRAME,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_H264_I_PERIOD:
		/*
		 * Incorrect initial implementation meant that H264_I_PERIOD
		 * was implemented to control intra-I period. As the MMAL
		 * encoder never produces I-frames that aren't IDR frames, it
		 * should actually have been GOP_SIZE.
		 * Support both controls, but writing to H264_I_PERIOD will
		 * update GOP_SIZE.
		 */
		__v4l2_ctrl_s_ctrl(ctx->gop_size, ctrl->val);
	fallthrough;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_INTRAPERIOD,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		if (!ctx->component)
			break;

		ret = bcm2835_codec_set_level_profile(ctx, ctrl);
		break;

	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_ENCODE_MIN_QUANT,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_ENCODE_MAX_QUANT,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME: {
		u32 mmal_bool = 1;

		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_REQUEST_I_FRAME,
						    &mmal_bool,
						    sizeof(mmal_bool));
		break;
	}
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP: {
		u32 u32_value;

		if (ctrl->id == V4L2_CID_HFLIP)
			ctx->hflip = ctrl->val;
		else
			ctx->vflip = ctrl->val;

		if (!ctx->component)
			break;

		if (ctx->hflip && ctx->vflip)
			u32_value = MMAL_PARAM_MIRROR_BOTH;
		else if (ctx->hflip)
			u32_value = MMAL_PARAM_MIRROR_HORIZONTAL;
		else if (ctx->vflip)
			u32_value = MMAL_PARAM_MIRROR_VERTICAL;
		else
			u32_value = MMAL_PARAM_MIRROR_NONE;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->input[0],
						    MMAL_PARAMETER_MIRROR,
						    &u32_value,
						    sizeof(u32_value));
		break;
	}
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		ret = 0;
		break;
	// Consti10 intra hack
	case V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD:
		if (!ctx->component)
			break;
		{
			int value=ctrl->val;
			ret = helper_set_h264_intra(ctx,value);
		}
		break;
	// Consti10 add AUD
	case V4L2_CID_MPEG_VIDEO_AU_DELIMITER:
		if (!ctx->component)
			break;
		{
			u32 mmal_bool = ctrl->val ? 1 : 0;
			ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
							    &ctx->component->output[0],
							    MMAL_PARAMETER_VIDEO_ENCODE_H264_AU_DELIMITERS,
							    &mmal_bool,
							    sizeof(mmal_bool));
		}
		break;
	// Consti10 add slicing
	case V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB:
		if (!ctx->component)
			break;
		{
			int value=ctrl->val;
			ret = helper_set_h264_slice(ctx,value);
		}
		break;
	case V4L2_CID_JPEG_COMPRESSION_QUALITY:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_JPEG_Q_FACTOR,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	default:
		v4l2_err(&ctx->dev->v4l2_dev, "Invalid control %08x\n", ctrl->id);
		return -EINVAL;
	}

	if (ret)
		v4l2_err(&ctx->dev->v4l2_dev, "Failed setting ctrl %08x, ret %d\n",
			 ctrl->id, ret);
	return ret ? -EINVAL : 0;
}

static const struct v4l2_ctrl_ops bcm2835_codec_ctrl_ops = {
	.s_ctrl = bcm2835_codec_s_ctrl,
};

static int vidioc_try_decoder_cmd(struct file *file, void *priv,
				  struct v4l2_decoder_cmd *cmd)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	if (ctx->dev->role != DECODE)
		return -EINVAL;

	switch (cmd->cmd) {
	case V4L2_DEC_CMD_STOP:
		if (cmd->flags & V4L2_DEC_CMD_STOP_TO_BLACK) {
			v4l2_err(&ctx->dev->v4l2_dev, "%s: DEC cmd->flags=%u stop to black not supported",
				 __func__, cmd->flags);
			return -EINVAL;
		}
		break;
	case V4L2_DEC_CMD_START:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int vidioc_decoder_cmd(struct file *file, void *priv,
			      struct v4l2_decoder_cmd *cmd)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data = &ctx->q_data[V4L2_M2M_SRC];
	struct vb2_queue *dst_vq;
	int ret;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s, cmd %u", __func__,
		 cmd->cmd);
	ret = vidioc_try_decoder_cmd(file, priv, cmd);
	if (ret)
		return ret;

	switch (cmd->cmd) {
	case V4L2_DEC_CMD_STOP:
		if (q_data->eos_buffer_in_use)
			v4l2_err(&ctx->dev->v4l2_dev, "EOS buffers already in use\n");
		q_data->eos_buffer_in_use = true;

		q_data->eos_buffer.mmal.buffer_size = 0;
		q_data->eos_buffer.mmal.length = 0;
		q_data->eos_buffer.mmal.mmal_flags =
						MMAL_BUFFER_HEADER_FLAG_EOS;
		q_data->eos_buffer.mmal.pts = 0;
		q_data->eos_buffer.mmal.dts = 0;

		if (!ctx->component)
			break;

		ret = vchiq_mmal_submit_buffer(ctx->dev->instance,
					       &ctx->component->input[0],
					       &q_data->eos_buffer.mmal);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev,
				 "%s: EOS buffer submit failed %d\n",
				 __func__, ret);

		break;

	case V4L2_DEC_CMD_START:
		dst_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		vb2_clear_last_buffer_dequeued(dst_vq);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int vidioc_try_encoder_cmd(struct file *file, void *priv,
				  struct v4l2_encoder_cmd *cmd)
{
	switch (cmd->cmd) {
	case V4L2_ENC_CMD_STOP:
		break;

	case V4L2_ENC_CMD_START:
		/* Do we need to do anything here? */
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int vidioc_encoder_cmd(struct file *file, void *priv,
			      struct v4l2_encoder_cmd *cmd)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data = &ctx->q_data[V4L2_M2M_SRC];
	int ret;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s, cmd %u", __func__,
		 cmd->cmd);
	ret = vidioc_try_encoder_cmd(file, priv, cmd);
	if (ret)
		return ret;

	switch (cmd->cmd) {
	case V4L2_ENC_CMD_STOP:
		if (q_data->eos_buffer_in_use)
			v4l2_err(&ctx->dev->v4l2_dev, "EOS buffers already in use\n");
		q_data->eos_buffer_in_use = true;

		q_data->eos_buffer.mmal.buffer_size = 0;
		q_data->eos_buffer.mmal.length = 0;
		q_data->eos_buffer.mmal.mmal_flags =
						MMAL_BUFFER_HEADER_FLAG_EOS;
		q_data->eos_buffer.mmal.pts = 0;
		q_data->eos_buffer.mmal.dts = 0;

		if (!ctx->component)
			break;

		ret = vchiq_mmal_submit_buffer(ctx->dev->instance,
					       &ctx->component->input[0],
					       &q_data->eos_buffer.mmal);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev,
				 "%s: EOS buffer submit failed %d\n",
				 __func__, ret);

		break;
	case V4L2_ENC_CMD_START:
		/* Do we need to do anything here? */
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_fmt *fmt;

	fmt = find_format_pix_fmt(fsize->pixel_format, file2ctx(file)->dev,
				  true);
	if (!fmt)
		fmt = find_format_pix_fmt(fsize->pixel_format,
					  file2ctx(file)->dev,
					  false);

	if (!fmt)
		return -EINVAL;

	if (fsize->index)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;

	fsize->stepwise.min_width = MIN_W;
	fsize->stepwise.max_width = ctx->dev->max_w;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.min_height = MIN_H;
	fsize->stepwise.max_height = ctx->dev->max_h;
	fsize->stepwise.step_height = 2;

	return 0;
}

static const struct v4l2_ioctl_ops bcm2835_codec_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap_mplane	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap_mplane	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap_mplane	= vidioc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out_mplane	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out_mplane	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out_mplane	= vidioc_s_fmt_vid_out,

	.vidioc_reqbufs		= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf	= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf		= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf		= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf	= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs	= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf		= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon	= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff	= v4l2_m2m_ioctl_streamoff,

	.vidioc_g_selection	= vidioc_g_selection,
	.vidioc_s_selection	= vidioc_s_selection,

	.vidioc_g_parm		= vidioc_g_parm,
	.vidioc_s_parm		= vidioc_s_parm,

	.vidioc_g_pixelaspect	= vidioc_g_pixelaspect,

	.vidioc_subscribe_event = vidioc_subscribe_evt,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_decoder_cmd = vidioc_decoder_cmd,
	.vidioc_try_decoder_cmd = vidioc_try_decoder_cmd,
	.vidioc_encoder_cmd = vidioc_encoder_cmd,
	.vidioc_try_encoder_cmd = vidioc_try_encoder_cmd,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,
};

static int bcm2835_codec_create_component(struct bcm2835_codec_ctx *ctx)
{
	struct bcm2835_codec_dev *dev = ctx->dev;
	unsigned int enable = 1;
	int ret;

	ret = vchiq_mmal_component_init(dev->instance, components[dev->role],
					&ctx->component);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev, "%s: failed to create component %s\n",
			 __func__, components[dev->role]);
		return -ENOMEM;
	}

	vchiq_mmal_port_parameter_set(dev->instance, &ctx->component->input[0],
				      MMAL_PARAMETER_ZERO_COPY, &enable,
				      sizeof(enable));
	vchiq_mmal_port_parameter_set(dev->instance, &ctx->component->output[0],
				      MMAL_PARAMETER_ZERO_COPY, &enable,
				      sizeof(enable));

	if (dev->role == DECODE) {
		/*
		 * Disable firmware option that ensures decoded timestamps
		 * always increase.
		 */
		enable = 0;
		vchiq_mmal_port_parameter_set(dev->instance,
					      &ctx->component->output[0],
					      MMAL_PARAMETER_VIDEO_VALIDATE_TIMESTAMPS,
					      &enable,
					      sizeof(enable));
		/*
		 * Enable firmware option to stop on colourspace and pixel
		 * aspect ratio changed
		 */
		enable = 1;
		vchiq_mmal_port_parameter_set(dev->instance,
					      &ctx->component->control,
					      MMAL_PARAMETER_VIDEO_STOP_ON_PAR_COLOUR_CHANGE,
					      &enable,
					      sizeof(enable));
	} else if (dev->role == DEINTERLACE) {
		/* Select the default deinterlace algorithm. */
		int half_framerate = 0;
		int default_frame_interval = -1; /* don't interpolate */
		int frame_type = 5; /* 0=progressive, 3=TFF, 4=BFF, 5=see frame */
		int use_qpus = 0;
		enum mmal_parameter_imagefx effect =
			advanced_deinterlace && ctx->q_data[V4L2_M2M_SRC].crop_width <= 800 ?
			MMAL_PARAM_IMAGEFX_DEINTERLACE_ADV :
			MMAL_PARAM_IMAGEFX_DEINTERLACE_FAST;
		struct mmal_parameter_imagefx_parameters params = {
			.effect = effect,
			.num_effect_params = 4,
			.effect_parameter = { frame_type,
					      default_frame_interval,
					      half_framerate,
					      use_qpus },
		};

		vchiq_mmal_port_parameter_set(dev->instance,
					      &ctx->component->output[0],
					      MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS,
					      &params,
					      sizeof(params));

	} else if (dev->role == ENCODE_IMAGE) {
		enable = 0;
		vchiq_mmal_port_parameter_set(dev->instance,
					      &ctx->component->control,
					      MMAL_PARAMETER_EXIF_DISABLE,
					      &enable,
					      sizeof(enable));
		enable = 1;
		vchiq_mmal_port_parameter_set(dev->instance,
					      &ctx->component->output[0],
						  MMAL_PARAMETER_JPEG_IJG_SCALING,
					      &enable,
					      sizeof(enable));
	}

	setup_mmal_port_format(ctx, &ctx->q_data[V4L2_M2M_SRC],
			       &ctx->component->input[0]);
	ctx->component->input[0].cb_ctx = ctx;

	setup_mmal_port_format(ctx, &ctx->q_data[V4L2_M2M_DST],
			       &ctx->component->output[0]);
	ctx->component->output[0].cb_ctx = ctx;

	ret = vchiq_mmal_port_set_format(dev->instance,
					 &ctx->component->input[0]);
	if (ret < 0) {
		v4l2_dbg(1, debug, &dev->v4l2_dev,
			 "%s: vchiq_mmal_port_set_format ip port failed\n",
			 __func__);
		goto destroy_component;
	}

	ret = vchiq_mmal_port_set_format(dev->instance,
					 &ctx->component->output[0]);
	if (ret < 0) {
		v4l2_dbg(1, debug, &dev->v4l2_dev,
			 "%s: vchiq_mmal_port_set_format op port failed\n",
			 __func__);
		goto destroy_component;
	}

	if (dev->role == ENCODE || dev->role == ENCODE_IMAGE) {
		u32 param = 1;

		if (ctx->q_data[V4L2_M2M_SRC].sizeimage <
			ctx->component->output[0].minimum_buffer.size)
			v4l2_err(&dev->v4l2_dev, "buffer size mismatch sizeimage %u < min size %u\n",
				 ctx->q_data[V4L2_M2M_SRC].sizeimage,
				 ctx->component->output[0].minimum_buffer.size);

		if (dev->role == ENCODE) {
			/* Enable SPS Timing header so framerate information is encoded
			 * in the H264 header.
			 */
			vchiq_mmal_port_parameter_set(ctx->dev->instance,
						      &ctx->component->output[0],
						      MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING,
						      &param, sizeof(param));

			/* Enable inserting headers into the first frame */
			vchiq_mmal_port_parameter_set(ctx->dev->instance,
						      &ctx->component->control,
						      MMAL_PARAMETER_VIDEO_ENCODE_HEADERS_WITH_FRAME,
						      &param, sizeof(param));
			/*
			 * Avoid fragmenting the buffers over multiple frames (unless
			 * the frame is bigger than the whole buffer)
			 */
			vchiq_mmal_port_parameter_set(ctx->dev->instance,
						      &ctx->component->control,
						      MMAL_PARAMETER_MINIMISE_FRAGMENTATION,
						      &param, sizeof(param));
			/*
			 * It is better to give SEI to the user (he can drop them if he wants to) instead of not
			 * providing SEI NALUs.
			 */
			ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
							    &ctx->component->output[0],
							    MMAL_PARAMETER_VIDEO_ENCODE_SEI_ENABLE,
							    &param, sizeof(param));
		}
	} else {
		if (ctx->q_data[V4L2_M2M_DST].sizeimage <
			ctx->component->output[0].minimum_buffer.size)
			v4l2_err(&dev->v4l2_dev, "buffer size mismatch sizeimage %u < min size %u\n",
				 ctx->q_data[V4L2_M2M_DST].sizeimage,
				 ctx->component->output[0].minimum_buffer.size);
	}

	/* Now we have a component we can set all the ctrls */
	ret = v4l2_ctrl_handler_setup(&ctx->hdl);

	v4l2_dbg(2, debug, &dev->v4l2_dev, "%s: component created as %s\n",
		 __func__, components[dev->role]);

	return 0;

destroy_component:
	vchiq_mmal_component_finalise(ctx->dev->instance, ctx->component);
	ctx->component = NULL;

	return ret;
}

/*
 * Queue operations
 */

static int bcm2835_codec_queue_setup(struct vb2_queue *vq,
				     unsigned int *nbuffers,
				     unsigned int *nplanes,
				     unsigned int sizes[],
				     struct device *alloc_devs[])
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vq);
	struct bcm2835_codec_q_data *q_data;
	struct vchiq_mmal_port *port;
	unsigned int size;

	q_data = get_q_data(ctx, vq->type);
	if (!q_data)
		return -EINVAL;

	if (!ctx->component)
		if (bcm2835_codec_create_component(ctx))
			return -EINVAL;

	port = get_port_data(ctx, vq->type);

	size = q_data->sizeimage;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;

	sizes[0] = size;
	port->current_buffer.size = size;

	if (*nbuffers < port->minimum_buffer.num)
		*nbuffers = port->minimum_buffer.num;
	/* Add one buffer to take an EOS */
	port->current_buffer.num = *nbuffers + 1;

	return 0;
}

static int bcm2835_codec_mmal_buf_cleanup(struct mmal_buffer *mmal_buf)
{
	mmal_vchi_buffer_cleanup(mmal_buf);

	if (mmal_buf->dma_buf) {
		dma_buf_put(mmal_buf->dma_buf);
		mmal_buf->dma_buf = NULL;
	}

	return 0;
}

static int bcm2835_codec_buf_init(struct vb2_buffer *vb)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct v4l2_m2m_buffer *m2m = container_of(vb2, struct v4l2_m2m_buffer,
						   vb);
	struct m2m_mmal_buffer *buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: ctx:%p, vb %p\n",
		 __func__, ctx, vb);
	buf->mmal.buffer = vb2_plane_vaddr(&buf->m2m.vb.vb2_buf, 0);
	buf->mmal.buffer_size = vb2_plane_size(&buf->m2m.vb.vb2_buf, 0);

	mmal_vchi_buffer_init(ctx->dev->instance, &buf->mmal);

	return 0;
}

static int bcm2835_codec_buf_prepare(struct vb2_buffer *vb)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct bcm2835_codec_q_data *q_data;
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct v4l2_m2m_buffer *m2m = container_of(vbuf, struct v4l2_m2m_buffer,
						   vb);
	struct m2m_mmal_buffer *buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);
	struct dma_buf *dma_buf;
	int ret;

	v4l2_dbg(4, debug, &ctx->dev->v4l2_dev, "%s: type: %d ptr %p\n",
		 __func__, vb->vb2_queue->type, vb);

	q_data = get_q_data(ctx, vb->vb2_queue->type);
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
	}

	if (vb2_plane_size(vb, 0) < q_data->sizeimage) {
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s data will not fit into plane (%lu < %lu)\n",
			 __func__, vb2_plane_size(vb, 0),
			 (long)q_data->sizeimage);
		return -EINVAL;
	}

	if (!V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		vb2_set_plane_payload(vb, 0, q_data->sizeimage);

	switch (vb->memory) {
	case VB2_MEMORY_DMABUF:
		dma_buf = dma_buf_get(vb->planes[0].m.fd);

		if (dma_buf != buf->mmal.dma_buf) {
			/* dmabuf either hasn't already been mapped, or it has
			 * changed.
			 */
			if (buf->mmal.dma_buf) {
				v4l2_err(&ctx->dev->v4l2_dev,
					 "%s Buffer changed - why did the core not call cleanup?\n",
					 __func__);
				bcm2835_codec_mmal_buf_cleanup(&buf->mmal);
			}

			buf->mmal.dma_buf = dma_buf;
		} else {
			/* We already have a reference count on the dmabuf, so
			 * release the one we acquired above.
			 */
			dma_buf_put(dma_buf);
		}
		ret = 0;
		break;
	case VB2_MEMORY_MMAP:
		/*
		 * We want to do this at init, but vb2_core_expbuf checks that
		 * the index < q->num_buffers, and q->num_buffers only gets
		 * updated once all the buffers are allocated.
		 */
		if (!buf->mmal.dma_buf) {
			ret = vb2_core_expbuf_dmabuf(vb->vb2_queue,
						     vb->vb2_queue->type,
						     vb->index, 0,
						     O_CLOEXEC,
						     &buf->mmal.dma_buf);
			if (ret)
				v4l2_err(&ctx->dev->v4l2_dev,
					 "%s: Failed to expbuf idx %d, ret %d\n",
					 __func__, vb->index, ret);
		} else {
			ret = 0;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void bcm2835_codec_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_dbg(4, debug, &ctx->dev->v4l2_dev, "%s: type: %d ptr %p vbuf->flags %u, seq %u, bytesused %u\n",
		 __func__, vb->vb2_queue->type, vb, vbuf->flags, vbuf->sequence,
		 vb->planes[0].bytesused);
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void bcm2835_codec_buffer_cleanup(struct vb2_buffer *vb)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct v4l2_m2m_buffer *m2m = container_of(vb2, struct v4l2_m2m_buffer,
						   vb);
	struct m2m_mmal_buffer *buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: ctx:%p, vb %p\n",
		 __func__, ctx, vb);

	bcm2835_codec_mmal_buf_cleanup(&buf->mmal);
}

static void bcm2835_codec_flush_buffers(struct bcm2835_codec_ctx *ctx,
					struct vchiq_mmal_port *port)
{
	int ret;

	if (atomic_read(&port->buffers_with_vpu)) {
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Waiting for buffers to be returned - %d outstanding\n",
			 __func__, atomic_read(&port->buffers_with_vpu));
		ret = wait_for_completion_timeout(&ctx->frame_cmplt,
						  COMPLETE_TIMEOUT);
		if (ret <= 0) {
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Timeout waiting for buffers to be returned - %d outstanding\n",
				 __func__,
				 atomic_read(&port->buffers_with_vpu));
		}
	}
}
static int bcm2835_codec_start_streaming(struct vb2_queue *q,
					 unsigned int count)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(q);
	struct bcm2835_codec_dev *dev = ctx->dev;
	struct bcm2835_codec_q_data *q_data = get_q_data(ctx, q->type);
	struct vchiq_mmal_port *port = get_port_data(ctx, q->type);
	int ret = 0;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: type: %d count %d\n",
		 __func__, q->type, count);
	q_data->sequence = 0;

	if (!ctx->component_enabled) {
		ret = vchiq_mmal_component_enable(dev->instance,
						  ctx->component);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling component, ret %d\n",
				 __func__, ret);
		ctx->component_enabled = true;
	}

	if (port->enabled) {
		unsigned int num_buffers;

		init_completion(&ctx->frame_cmplt);

		/*
		 * This should only ever happen with DECODE and the MMAL output
		 * port that has been enabled for resolution changed events.
		 * In this case no buffers have been allocated or sent to the
		 * component, so warn on that.
		 */
		WARN_ON(ctx->dev->role != DECODE);
		WARN_ON(q->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		WARN_ON(atomic_read(&port->buffers_with_vpu));

		/*
		 * Disable will reread the port format, so retain buffer count.
		 */
		num_buffers = port->current_buffer.num;

		ret = vchiq_mmal_port_disable(dev->instance, port);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Error disabling port update buffer count, ret %d\n",
				 __func__, ret);
		bcm2835_codec_flush_buffers(ctx, port);
		port->current_buffer.num = num_buffers;
	}

	if (count < port->minimum_buffer.num)
		count = port->minimum_buffer.num;

	if (port->current_buffer.num < count + 1) {
		v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: ctx:%p, buffer count changed %u to %u\n",
			 __func__, ctx, port->current_buffer.num, count + 1);

		port->current_buffer.num = count + 1;
		ret = vchiq_mmal_port_set_format(dev->instance, port);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Error updating buffer count, ret %d\n",
				 __func__, ret);
	}

	if (dev->role == DECODE &&
	    q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
	    !ctx->component->output[0].enabled) {
		/*
		 * Decode needs to enable the MMAL output/V4L2 CAPTURE
		 * port at this point too so that we have everything
		 * set up for dynamic resolution changes.
		 */
		ret = vchiq_mmal_port_enable(dev->instance,
					     &ctx->component->output[0],
					     op_buffer_cb);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling o/p port, ret %d\n",
				 __func__, ret);
	}

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		/*
		 * Create the EOS buffer.
		 * We only need the MMAL part, and want to NOT attach a memory
		 * buffer to it as it should only take flags.
		 */
		memset(&q_data->eos_buffer, 0, sizeof(q_data->eos_buffer));
		mmal_vchi_buffer_init(dev->instance,
				      &q_data->eos_buffer.mmal);
		q_data->eos_buffer_in_use = false;

		ret = vchiq_mmal_port_enable(dev->instance,
					     port,
					     ip_buffer_cb);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling i/p port, ret %d\n",
				 __func__, ret);
	} else {
		if (!port->enabled) {
			ret = vchiq_mmal_port_enable(dev->instance,
						     port,
						     op_buffer_cb);
			if (ret)
				v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling o/p port, ret %d\n",
					 __func__, ret);
		}
	}
	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Done, ret %d\n",
		 __func__, ret);
	return ret;
}

static void bcm2835_codec_stop_streaming(struct vb2_queue *q)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(q);
	struct bcm2835_codec_dev *dev = ctx->dev;
	struct bcm2835_codec_q_data *q_data = get_q_data(ctx, q->type);
	struct vchiq_mmal_port *port = get_port_data(ctx, q->type);
	struct vb2_v4l2_buffer *vbuf;
	int ret;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: type: %d - return buffers\n",
		 __func__, q->type);

	init_completion(&ctx->frame_cmplt);

	/* Clear out all buffers held by m2m framework */
	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: return buffer %p\n",
			 __func__, vbuf);

		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);
	}

	/* Disable MMAL port - this will flush buffers back */
	ret = vchiq_mmal_port_disable(dev->instance, port);
	if (ret)
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed disabling %s port, ret %d\n",
			 __func__, V4L2_TYPE_IS_OUTPUT(q->type) ? "i/p" : "o/p",
			 ret);

	bcm2835_codec_flush_buffers(ctx, port);

	if (dev->role == DECODE &&
	    q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    ctx->component->input[0].enabled) {
		/*
		 * For decode we need to keep the MMAL output port enabled for
		 * resolution changed events whenever the input is enabled.
		 */
		ret = vchiq_mmal_port_enable(dev->instance,
					     &ctx->component->output[0],
					     op_buffer_cb);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling o/p port, ret %d\n",
				 __func__, ret);
	}

	/* If both ports disabled, then disable the component */
	if (ctx->component_enabled &&
	    !ctx->component->input[0].enabled &&
	    !ctx->component->output[0].enabled) {
		ret = vchiq_mmal_component_disable(dev->instance,
						   ctx->component);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling component, ret %d\n",
				 __func__, ret);
		ctx->component_enabled = false;
	}

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		mmal_vchi_buffer_cleanup(&q_data->eos_buffer.mmal);

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: done\n", __func__);
}

static const struct vb2_ops bcm2835_codec_qops = {
	.queue_setup	 = bcm2835_codec_queue_setup,
	.buf_init	 = bcm2835_codec_buf_init,
	.buf_prepare	 = bcm2835_codec_buf_prepare,
	.buf_queue	 = bcm2835_codec_buf_queue,
	.buf_cleanup	 = bcm2835_codec_buffer_cleanup,
	.start_streaming = bcm2835_codec_start_streaming,
	.stop_streaming  = bcm2835_codec_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct bcm2835_codec_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct m2m_mmal_buffer);
	src_vq->ops = &bcm2835_codec_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->dev = &ctx->dev->pdev->dev;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct m2m_mmal_buffer);
	dst_vq->ops = &bcm2835_codec_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dev = &ctx->dev->pdev->dev;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;

	return vb2_queue_init(dst_vq);
}

static void dec_add_profile_ctrls(struct bcm2835_codec_dev *const dev,
				  struct v4l2_ctrl_handler *const hdl)
{
	struct v4l2_ctrl *ctrl;
	unsigned int i;
	const struct bcm2835_codec_fmt_list *const list = &dev->supported_fmts[0];

	for (i = 0; i < list->num_entries; ++i) {
		switch (list->list[i].fourcc) {
		case V4L2_PIX_FMT_H264:
			ctrl = v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
						      V4L2_CID_MPEG_VIDEO_H264_LEVEL,
						      V4L2_MPEG_VIDEO_H264_LEVEL_4_2,
						      ~(BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_0) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1B) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_1) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_2) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_3) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_0) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_1) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_2) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_0) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_1) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_2) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_0) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_1) |
							BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_2)),
						       V4L2_MPEG_VIDEO_H264_LEVEL_4_0);
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
			ctrl = v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
						      V4L2_CID_MPEG_VIDEO_H264_PROFILE,
						      V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
						      ~(BIT(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
							BIT(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
							BIT(V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
							BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)),
						       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
			break;
		case V4L2_PIX_FMT_MPEG2:
			ctrl = v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
						      V4L2_CID_MPEG_VIDEO_MPEG2_LEVEL,
						      V4L2_MPEG_VIDEO_MPEG2_LEVEL_HIGH,
						      ~(BIT(V4L2_MPEG_VIDEO_MPEG2_LEVEL_LOW) |
							BIT(V4L2_MPEG_VIDEO_MPEG2_LEVEL_MAIN) |
							BIT(V4L2_MPEG_VIDEO_MPEG2_LEVEL_HIGH_1440) |
							BIT(V4L2_MPEG_VIDEO_MPEG2_LEVEL_HIGH)),
						      V4L2_MPEG_VIDEO_MPEG2_LEVEL_MAIN);
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
			ctrl = v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
						      V4L2_CID_MPEG_VIDEO_MPEG2_PROFILE,
						      V4L2_MPEG_VIDEO_MPEG2_PROFILE_MAIN,
						      ~(BIT(V4L2_MPEG_VIDEO_MPEG2_PROFILE_SIMPLE) |
							BIT(V4L2_MPEG_VIDEO_MPEG2_PROFILE_MAIN)),
						      V4L2_MPEG_VIDEO_MPEG2_PROFILE_MAIN);
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
			break;
		case V4L2_PIX_FMT_MPEG4:
			ctrl = v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
						      V4L2_CID_MPEG_VIDEO_MPEG4_LEVEL,
						      V4L2_MPEG_VIDEO_MPEG4_LEVEL_5,
						      ~(BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_0) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_0B) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_1) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_2) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_3) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_3B) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_4) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_LEVEL_5)),
						      V4L2_MPEG_VIDEO_MPEG4_LEVEL_4);
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
			ctrl = v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
						      V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE,
						      V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_SIMPLE,
						      ~(BIT(V4L2_MPEG_VIDEO_MPEG4_PROFILE_SIMPLE) |
							BIT(V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_SIMPLE)),
						      V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_SIMPLE);
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
			break;
		/* No profiles defined by V4L2 */
		case V4L2_PIX_FMT_H263:
		case V4L2_PIX_FMT_JPEG:
		case V4L2_PIX_FMT_MJPEG:
		case V4L2_PIX_FMT_VC1_ANNEX_G:
		default:
			break;
		}
	}
}

/*
 * File operations
 */
static int bcm2835_codec_open(struct file *file)
{
	struct bcm2835_codec_dev *dev = video_drvdata(file);
	struct bcm2835_codec_ctx *ctx = NULL;
	struct v4l2_ctrl_handler *hdl;
	int rc = 0;

	if (mutex_lock_interruptible(&dev->dev_mutex)) {
		v4l2_err(&dev->v4l2_dev, "Mutex fail\n");
		return -ERESTARTSYS;
	}
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto open_unlock;
	}

	ctx->q_data[V4L2_M2M_SRC].fmt = get_default_format(dev, false);
	ctx->q_data[V4L2_M2M_DST].fmt = get_default_format(dev, true);

	ctx->q_data[V4L2_M2M_SRC].crop_width = DEFAULT_WIDTH;
	ctx->q_data[V4L2_M2M_SRC].crop_height = DEFAULT_HEIGHT;
	ctx->q_data[V4L2_M2M_SRC].height = DEFAULT_HEIGHT;
	ctx->q_data[V4L2_M2M_SRC].bytesperline =
			get_bytesperline(DEFAULT_WIDTH, DEFAULT_HEIGHT,
					 ctx->q_data[V4L2_M2M_SRC].fmt,
					 dev->role);
	ctx->q_data[V4L2_M2M_SRC].sizeimage =
		get_sizeimage(ctx->q_data[V4L2_M2M_SRC].bytesperline,
			      ctx->q_data[V4L2_M2M_SRC].crop_width,
			      ctx->q_data[V4L2_M2M_SRC].height,
			      ctx->q_data[V4L2_M2M_SRC].fmt);
	ctx->q_data[V4L2_M2M_SRC].field = V4L2_FIELD_NONE;

	ctx->q_data[V4L2_M2M_DST].crop_width = DEFAULT_WIDTH;
	ctx->q_data[V4L2_M2M_DST].crop_height = DEFAULT_HEIGHT;
	ctx->q_data[V4L2_M2M_DST].height = DEFAULT_HEIGHT;
	ctx->q_data[V4L2_M2M_DST].bytesperline =
			get_bytesperline(DEFAULT_WIDTH, DEFAULT_HEIGHT,
					 ctx->q_data[V4L2_M2M_DST].fmt,
					 dev->role);
	ctx->q_data[V4L2_M2M_DST].sizeimage =
		get_sizeimage(ctx->q_data[V4L2_M2M_DST].bytesperline,
			      ctx->q_data[V4L2_M2M_DST].crop_width,
			      ctx->q_data[V4L2_M2M_DST].height,
			      ctx->q_data[V4L2_M2M_DST].fmt);
	ctx->q_data[V4L2_M2M_DST].aspect_ratio.numerator = 1;
	ctx->q_data[V4L2_M2M_DST].aspect_ratio.denominator = 1;
	ctx->q_data[V4L2_M2M_DST].field = V4L2_FIELD_NONE;

	ctx->colorspace = V4L2_COLORSPACE_REC709;
	ctx->bitrate = 10 * 1000 * 1000;

	ctx->framerate_num = 30;
	ctx->framerate_denom = 1;

	/* Initialise V4L2 contexts */
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;
	hdl = &ctx->hdl;
	switch (dev->role) {
	case ENCODE:
	{
		/* Encode controls */
		v4l2_ctrl_handler_init(hdl, 15);

		v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
				       V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
				       V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, 0,
				       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_BITRATE,
				  25 * 1000, 25 * 1000 * 1000,
				  25 * 1000, 10 * 1000 * 1000);
		v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
				       V4L2_CID_MPEG_VIDEO_HEADER_MODE,
				       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
				       0, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER,
				  0, 1,
				  1, 0);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
				  0, 0x7FFFFFFF,
				  1, 60);
		v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
				       V4L2_CID_MPEG_VIDEO_H264_LEVEL,
				       V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
				       ~(BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1B) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_2) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_3) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_2) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_2) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_2) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_5_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_5_1)),
				       V4L2_MPEG_VIDEO_H264_LEVEL_4_0);
		v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
				       V4L2_CID_MPEG_VIDEO_H264_PROFILE,
				       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
				       ~(BIT(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
					 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
					 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
					 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)),
					V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
				  0, 51,
				  1, 20);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
				  0, 51,
				  1, 51);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
				  0, 0, 0, 0);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_B_FRAMES,
				  0, 0,
				  1, 0);
		// Consti10 HACK add intra
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD,
				  -1, 30000,
				  1, -1);
		// Consti10 add AUD
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_AU_DELIMITER,
				  0, 1,
				  1, 0);
		// Consti10 add SLICE
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB,
				  -1, 30000,
				  1, -1);
		ctx->gop_size = v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
						  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
						  0, 0x7FFFFFFF, 1, 60);
		if (hdl->error) {
			rc = hdl->error;
			goto free_ctrl_handler;
		}
		ctx->fh.ctrl_handler = hdl;
		v4l2_ctrl_handler_setup(hdl);
	}
	break;
	case DECODE:
	{
		v4l2_ctrl_handler_init(hdl, 1 + dev->supported_fmts[0].num_entries * 2);

		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MIN_BUFFERS_FOR_CAPTURE,
				  1, 1, 1, 1);
		dec_add_profile_ctrls(dev, hdl);
		if (hdl->error) {
			rc = hdl->error;
			goto free_ctrl_handler;
		}
		ctx->fh.ctrl_handler = hdl;
		v4l2_ctrl_handler_setup(hdl);
	}
	break;
	case ISP:
	{
		v4l2_ctrl_handler_init(hdl, 2);

		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_HFLIP,
				  1, 0, 1, 0);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_VFLIP,
				  1, 0, 1, 0);
		if (hdl->error) {
			rc = hdl->error;
			goto free_ctrl_handler;
		}
		ctx->fh.ctrl_handler = hdl;
		v4l2_ctrl_handler_setup(hdl);
	}
	break;
	case DEINTERLACE:
	{
		v4l2_ctrl_handler_init(hdl, 0);
	}
	break;
	case ENCODE_IMAGE:
	{
		/* Encode image controls */
		v4l2_ctrl_handler_init(hdl, 1);

		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_JPEG_COMPRESSION_QUALITY,
				  1, 100,
				  1, 80);
		if (hdl->error) {
			rc = hdl->error;
			goto free_ctrl_handler;
		}
		ctx->fh.ctrl_handler = hdl;
		v4l2_ctrl_handler_setup(hdl);
	}
	break;
	case NUM_ROLES:
	break;
	}

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, &queue_init);

	if (IS_ERR(ctx->fh.m2m_ctx)) {
		rc = PTR_ERR(ctx->fh.m2m_ctx);

		goto free_ctrl_handler;
	}

	/* Set both queues as buffered as we have buffering in the VPU. That
	 * means that we will be scheduled whenever either an input or output
	 * buffer is available (otherwise one of each are required).
	 */
	v4l2_m2m_set_src_buffered(ctx->fh.m2m_ctx, true);
	v4l2_m2m_set_dst_buffered(ctx->fh.m2m_ctx, true);

	v4l2_fh_add(&ctx->fh);
	atomic_inc(&dev->num_inst);

	mutex_unlock(&dev->dev_mutex);
	return 0;

free_ctrl_handler:
	v4l2_ctrl_handler_free(hdl);
	kfree(ctx);
open_unlock:
	mutex_unlock(&dev->dev_mutex);
	return rc;
}

static int bcm2835_codec_release(struct file *file)
{
	struct bcm2835_codec_dev *dev = video_drvdata(file);
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	v4l2_dbg(1, debug, &dev->v4l2_dev, "%s: Releasing instance %p\n",
		 __func__, ctx);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->hdl);
	mutex_lock(&dev->dev_mutex);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	if (ctx->component)
		vchiq_mmal_component_finalise(dev->instance, ctx->component);

	mutex_unlock(&dev->dev_mutex);
	kfree(ctx);

	atomic_dec(&dev->num_inst);

	return 0;
}

static const struct v4l2_file_operations bcm2835_codec_fops = {
	.owner		= THIS_MODULE,
	.open		= bcm2835_codec_open,
	.release	= bcm2835_codec_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device bcm2835_codec_videodev = {
	.name		= MEM2MEM_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &bcm2835_codec_fops,
	.ioctl_ops	= &bcm2835_codec_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
};

static const struct v4l2_m2m_ops m2m_ops = {
	.device_run	= device_run,
	.job_ready	= job_ready,
	.job_abort	= job_abort,
};

/* Size of the array to provide to the VPU when asking for the list of supported
 * formats.
 * The ISP component currently advertises 62 input formats, so add a small
 * overhead on that.
 */
#define MAX_SUPPORTED_ENCODINGS 70

/* Populate dev->supported_fmts with the formats supported by those ports. */
static int bcm2835_codec_get_supported_fmts(struct bcm2835_codec_dev *dev)
{
	struct bcm2835_codec_fmt *list;
	struct vchiq_mmal_component *component;
	u32 fourccs[MAX_SUPPORTED_ENCODINGS];
	u32 param_size = sizeof(fourccs);
	unsigned int i, j, num_encodings;
	int ret;

	ret = vchiq_mmal_component_init(dev->instance, components[dev->role],
					&component);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev, "%s: failed to create component %s\n",
			 __func__, components[dev->role]);
		return -ENOMEM;
	}

	ret = vchiq_mmal_port_parameter_get(dev->instance,
					    &component->input[0],
					    MMAL_PARAMETER_SUPPORTED_ENCODINGS,
					    &fourccs,
					    &param_size);

	if (ret) {
		if (ret == MMAL_MSG_STATUS_ENOSPC) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: port has more encodings than we provided space for. Some are dropped (%zu vs %u).\n",
				 __func__, param_size / sizeof(u32),
				 MAX_SUPPORTED_ENCODINGS);
			num_encodings = MAX_SUPPORTED_ENCODINGS;
		} else {
			v4l2_err(&dev->v4l2_dev, "%s: get_param ret %u.\n",
				 __func__, ret);
			ret = -EINVAL;
			goto destroy_component;
		}
	} else {
		num_encodings = param_size / sizeof(u32);
	}

	/* Assume at this stage that all encodings will be supported in V4L2.
	 * Any that aren't supported will waste a very small amount of memory.
	 */
	list = devm_kzalloc(&dev->pdev->dev,
			    sizeof(struct bcm2835_codec_fmt) * num_encodings,
			    GFP_KERNEL);
	if (!list) {
		ret = -ENOMEM;
		goto destroy_component;
	}
	dev->supported_fmts[0].list = list;

	for (i = 0, j = 0; i < num_encodings; i++) {
		const struct bcm2835_codec_fmt *fmt = get_fmt(fourccs[i]);

		if (fmt) {
			list[j] = *fmt;
			j++;
		}
	}
	dev->supported_fmts[0].num_entries = j;

	param_size = sizeof(fourccs);
	ret = vchiq_mmal_port_parameter_get(dev->instance,
					    &component->output[0],
					    MMAL_PARAMETER_SUPPORTED_ENCODINGS,
					    &fourccs,
					    &param_size);

	if (ret) {
		if (ret == MMAL_MSG_STATUS_ENOSPC) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: port has more encodings than we provided space for. Some are dropped (%zu vs %u).\n",
				 __func__, param_size / sizeof(u32),
				 MAX_SUPPORTED_ENCODINGS);
			num_encodings = MAX_SUPPORTED_ENCODINGS;
		} else {
			ret = -EINVAL;
			goto destroy_component;
		}
	} else {
		num_encodings = param_size / sizeof(u32);
	}
	/* Assume at this stage that all encodings will be supported in V4L2. */
	list = devm_kzalloc(&dev->pdev->dev,
			    sizeof(struct bcm2835_codec_fmt) * num_encodings,
			    GFP_KERNEL);
	if (!list) {
		ret = -ENOMEM;
		goto destroy_component;
	}
	dev->supported_fmts[1].list = list;

	for (i = 0, j = 0; i < num_encodings; i++) {
		const struct bcm2835_codec_fmt *fmt = get_fmt(fourccs[i]);

		if (fmt) {
			list[j] = *fmt;
			j++;
		}
	}
	dev->supported_fmts[1].num_entries = j;

	ret = 0;

destroy_component:
	vchiq_mmal_component_finalise(dev->instance, component);

	return ret;
}

static int bcm2835_codec_create(struct bcm2835_codec_driver *drv,
				struct bcm2835_codec_dev **new_dev,
				enum bcm2835_codec_role role)
{
	struct platform_device *pdev = drv->pdev;
	struct bcm2835_codec_dev *dev;
	struct video_device *vfd;
	int function;
	int video_nr;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;

	dev->role = role;

	ret = vchiq_mmal_init(&dev->instance);
	if (ret)
		return ret;

	ret = bcm2835_codec_get_supported_fmts(dev);
	if (ret)
		goto vchiq_finalise;

	atomic_set(&dev->num_inst, 0);
	mutex_init(&dev->dev_mutex);

	/* Initialise the video device */
	dev->vfd = bcm2835_codec_videodev;

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	vfd->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	vfd->v4l2_dev->mdev = &drv->mdev;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		goto vchiq_finalise;

	dev->max_w = MAX_W_CODEC;
	dev->max_h = MAX_H_CODEC;

	switch (role) {
	case DECODE:
		v4l2_disable_ioctl(vfd, VIDIOC_ENCODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_ENCODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_S_PARM);
		v4l2_disable_ioctl(vfd, VIDIOC_G_PARM);
		function = MEDIA_ENT_F_PROC_VIDEO_DECODER;
		video_nr = decode_video_nr;
		break;
	case ENCODE:
		v4l2_disable_ioctl(vfd, VIDIOC_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_DECODER_CMD);
		function = MEDIA_ENT_F_PROC_VIDEO_ENCODER;
		video_nr = encode_video_nr;
		break;
	case ISP:
		v4l2_disable_ioctl(vfd, VIDIOC_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_S_PARM);
		v4l2_disable_ioctl(vfd, VIDIOC_G_PARM);
		function = MEDIA_ENT_F_PROC_VIDEO_SCALER;
		video_nr = isp_video_nr;
		dev->max_w = MAX_W_ISP;
		dev->max_h = MAX_H_ISP;
		break;
	case DEINTERLACE:
		v4l2_disable_ioctl(vfd, VIDIOC_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_S_PARM);
		v4l2_disable_ioctl(vfd, VIDIOC_G_PARM);
		function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
		video_nr = deinterlace_video_nr;
		break;
	case ENCODE_IMAGE:
		v4l2_disable_ioctl(vfd, VIDIOC_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_DECODER_CMD);
		function = MEDIA_ENT_F_PROC_VIDEO_ENCODER;
		video_nr = encode_image_nr;
		break;
	default:
		ret = -EINVAL;
		goto unreg_dev;
	}

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, video_nr);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto unreg_dev;
	}

	video_set_drvdata(vfd, dev);
	snprintf(vfd->name, sizeof(vfd->name), "%s-%s",
		 bcm2835_codec_videodev.name, roles[role]);
	v4l2_info(&dev->v4l2_dev, "Device registered as /dev/video%d\n",
		  vfd->num);

	*new_dev = dev;

	dev->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(dev->m2m_dev);
		goto err_m2m;
	}

	ret = v4l2_m2m_register_media_controller(dev->m2m_dev, vfd, function);
	if (ret)
		goto err_m2m;

	v4l2_info(&dev->v4l2_dev, "Loaded V4L2 %s\n",
		  roles[role]);
	return 0;

err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
unreg_dev:
	v4l2_device_unregister(&dev->v4l2_dev);
vchiq_finalise:
	vchiq_mmal_finalise(dev->instance);
	return ret;
}

static int bcm2835_codec_destroy(struct bcm2835_codec_dev *dev)
{
	if (!dev)
		return -ENODEV;

	v4l2_info(&dev->v4l2_dev, "Removing " MEM2MEM_NAME ", %s\n",
		  roles[dev->role]);
	v4l2_m2m_unregister_media_controller(dev->m2m_dev);
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	vchiq_mmal_finalise(dev->instance);

	return 0;
}

static int bcm2835_codec_probe(struct platform_device *pdev)
{
	struct bcm2835_codec_driver *drv;
	struct media_device *mdev;
	int ret = 0;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->pdev = pdev;
	mdev = &drv->mdev;
	mdev->dev = &pdev->dev;

	strscpy(mdev->model, bcm2835_codec_videodev.name, sizeof(mdev->model));
	strscpy(mdev->serial, "0000", sizeof(mdev->serial));
	snprintf(mdev->bus_info, sizeof(mdev->bus_info), "platform:%s",
		 pdev->name);

	/* This should return the vgencmd version information or such .. */
	mdev->hw_revision = 1;
	media_device_init(mdev);

	ret = bcm2835_codec_create(drv, &drv->decode, DECODE);
	if (ret)
		goto out;

	ret = bcm2835_codec_create(drv, &drv->encode, ENCODE);
	if (ret)
		goto out;

	ret = bcm2835_codec_create(drv, &drv->isp, ISP);
	if (ret)
		goto out;

	ret = bcm2835_codec_create(drv, &drv->deinterlace, DEINTERLACE);
	if (ret)
		goto out;

	ret = bcm2835_codec_create(drv, &drv->encode_image, ENCODE_IMAGE);
	if (ret)
		goto out;

	/* Register the media device node */
	if (media_device_register(mdev) < 0)
		goto out;

	platform_set_drvdata(pdev, drv);

	return 0;

out:
	if (drv->encode_image) {
		bcm2835_codec_destroy(drv->encode_image);
		drv->encode_image = NULL;
	}
	if (drv->deinterlace) {
		bcm2835_codec_destroy(drv->deinterlace);
		drv->deinterlace = NULL;
	}
	if (drv->isp) {
		bcm2835_codec_destroy(drv->isp);
		drv->isp = NULL;
	}
	if (drv->encode) {
		bcm2835_codec_destroy(drv->encode);
		drv->encode = NULL;
	}
	if (drv->decode) {
		bcm2835_codec_destroy(drv->decode);
		drv->decode = NULL;
	}
	return ret;
}

static int bcm2835_codec_remove(struct platform_device *pdev)
{
	struct bcm2835_codec_driver *drv = platform_get_drvdata(pdev);

	media_device_unregister(&drv->mdev);

	bcm2835_codec_destroy(drv->encode_image);

	bcm2835_codec_destroy(drv->deinterlace);

	bcm2835_codec_destroy(drv->isp);

	bcm2835_codec_destroy(drv->encode);

	bcm2835_codec_destroy(drv->decode);

	media_device_cleanup(&drv->mdev);

	return 0;
}

static struct platform_driver bcm2835_v4l2_codec_driver = {
	.probe = bcm2835_codec_probe,
	.remove = bcm2835_codec_remove,
	.driver = {
		   .name = "bcm2835-codec",
		   .owner = THIS_MODULE,
		   },
};

module_platform_driver(bcm2835_v4l2_codec_driver);

MODULE_DESCRIPTION("BCM2835 codec V4L2 driver");
MODULE_AUTHOR("Dave Stevenson, <dave.stevenson@raspberrypi.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.0.1");
MODULE_ALIAS("platform:bcm2835-codec");
