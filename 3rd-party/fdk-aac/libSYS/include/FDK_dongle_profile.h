/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FDK_DONGLE_PROFILE_H
#define FDK_DONGLE_PROFILE_H

/* The dongle encodes stereo AAC and decodes only its fixed mono microphone. */
#ifdef FDK_AIRPODS_PROFILE
#define FDK_ENCODER_WORK_CHANNELS 2
#define FDK_DECODER_TRANSPORT_BYTES 512
/* Fixed mono ELD still uses 1024 spectral slots and two 480-frame time regions. */
#define FDK_DECODER_SPECTRAL_SAMPLES 1024
#define FDK_DECODER_TIME_SAMPLES (480 * 2)
#define FDK_DECODER_LIMITER_CHANNELS 1
#define FDK_DECODER_LIMITER_RATE 48000
#define FDK_MIC_ACCESS_UNIT_BYTES 255
#else
#define FDK_ENCODER_WORK_CHANNELS 8
#define FDK_DECODER_TRANSPORT_BYTES (8192 * 4)
#define FDK_DECODER_SPECTRAL_SAMPLES (1024 * 2)
#define FDK_DECODER_TIME_SAMPLES ((2) * (1024 * 4) * 2)
#define FDK_DECODER_LIMITER_CHANNELS 8
#define FDK_DECODER_LIMITER_RATE 96000
#endif

#endif
