/*
 * Driver for ELAN press-type (touch) fingerprint sensors
 *
 * Copyright (C) 2026 Filip Spanne (original driver, 04f3:0c6e)
 * Copyright (C) 2026 Mohammad Nauman (04f3:0c3d, full-frame capture,
 *                                    multi-frame touches, rotation matcher)
 *
 * These sensors stream raw frames like the swipe sensors handled by the
 * elan driver, but the finger rests on the sensor instead of being swiped
 * across it. The imaged area is far too small for reliable minutiae
 * matching, so enrollment stores the images themselves and matching is
 * done by normalized cross-correlation, like the Windows driver does.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#pragma once

#include "fpi-device.h"
#include "fpi-usb-transfer.h"

#define ELANPRESS_VEND_ID 0x04f3

#define ELANPRESS_EP_CMD_OUT (0x1 | FPI_USB_ENDPOINT_OUT)
#define ELANPRESS_EP_CMD_IN (0x3 | FPI_USB_ENDPOINT_IN)
#define ELANPRESS_EP_IMG_IN (0x2 | FPI_USB_ENDPOINT_IN)

#define ELANPRESS_CMD_LEN 2
#define ELANPRESS_CMD_TIMEOUT 5000
#define ELANPRESS_FRAME_TIMEOUT 2000

/* the pre-scan read blocks in the device until a finger arrives; after this
 * long the wait is restarted (stop, LED, fresh background) */
#define ELANPRESS_WAIT_FINGER_TIMEOUT 10000
/* once frames are flowing, a pre-scan that does not answer within this
 * time means the finger lifted */
#define ELANPRESS_FINGER_GONE_TIMEOUT 300

/* interval between finger presence polls when the device answers at once */
#define ELANPRESS_POLL_INTERVAL_MS 30

/* pre_scan response when a finger is on the sensor */
#define ELANPRESS_FINGER_PRESENT 0x55

/* frames captured per touch */
#define ELANPRESS_MIN_FRAMES 3
#define ELANPRESS_MAX_FRAMES 14

/* distinct images of a touch used as probes (verify); how many an enroll
 * touch keeps is per device, see elanpress_devices[] */
#define ELANPRESS_MAX_PROBES 3

/* touches stored during enrollment, unless the device sets its own */
#define ELANPRESS_ENROLL_STAGES 20
#define ELANPRESS_MAX_ENROLL_STAGES 40

/* a background frame is rejected (finger probably still on the pad) when
 * its mean is this far from the device's own calibration mean; this is the
 * elan driver's ELAN_CALIBRATION_MAX_DELTA */
#define ELANPRESS_BG_MAX_DELTA 500
#define ELANPRESS_BG_ATTEMPTS 8
/* used while there is no clean background to fall back on. Subtracting a
 * dirty one ruins the touch, so wait up to ~10 s for the finger to lift. */
#define ELANPRESS_BG_ATTEMPTS_FIRST 60
/* first firmware that answers the calibration-mean command */
#define ELANPRESS_MIN_CALIBRATION_FW 0x0138

G_DECLARE_FINAL_TYPE (FpiDeviceElanPress, fpi_device_elanpress, FPI,
                      DEVICE_ELANPRESS, FpDevice);

/* Per-device defaults. Each set was measured on one physical pad and is
 * provisional; FP_ELANPRESS_THRESHOLD, FP_ELANPRESS_ENROLL_STAGES,
 * FP_ELANPRESS_IMAGES_PER_TOUCH and FP_ELANPRESS_DUP_SIMILARITY override
 * them at runtime. driver_data indexes elanpress_devices[]. */
typedef struct
{
  double threshold;        /* accept when the best score reaches this */
  int    enroll_stages;    /* touches stored during enrollment */
  int    images_per_touch; /* distinct images an enroll touch keeps */
  double dup_similarity;   /* zero-shift similarity above which two frames
                            * of a touch are duplicates */
} ElanpressDeviceConfig;

enum {
  ELANPRESS_0C3D,
  ELANPRESS_0C4B,
  ELANPRESS_0C63,
  ELANPRESS_0C6E,
};

static const ElanpressDeviceConfig elanpress_devices[] = {
  /* 64x88. 140-image pool, one subject: 0/91 impostor images accepted, ~60%
   * of genuine touches. Re-processing that pool's raw frames, one image per
   * enroll touch instead of three accepted a fifth fewer genuine touches,
   * and a 0.95 duplicate cutoff changed nothing. */
  [ELANPRESS_0C3D] = { 0.76, 20, 3, 0.85 },
  /* 80x80, one subject. With 40 enroll touches 15/16 genuine touches were
   * accepted (0.894 to 0.970) and 0/12 impostor touches (at most 0.758);
   * with 20, 2/4 genuine. */
  [ELANPRESS_0C4B] = { 0.85, 40, 3, 0.85 },
  /* 80x80, two subjects on one pad: 0.76 admitted an unenrolled finger
   * there, 0.85 did not (see the merge request discussion). */
  [ELANPRESS_0C63] = { 0.85, 20, 3, 0.85 },
  /* 150x52, two people. With these values, 15 held-out touches of the
   * enrolled finger scored at least 0.908 and 47 touches of four other
   * fingers and a second person at most 0.766. Most frames of one press
   * are more than 0.85 alike, so only the higher cutoff leaves several
   * probes per touch, and more enroll images per touch raised the best
   * impostor score more than the genuine ones. */
  [ELANPRESS_0C6E] = { 0.80, 20, 1, 0.95 },
};

static const FpIdEntry elanpress_id_table[] = {
  {.vid = ELANPRESS_VEND_ID, .pid = 0x0c3d, .driver_data = ELANPRESS_0C3D, },
  {.vid = ELANPRESS_VEND_ID, .pid = 0x0c4b, .driver_data = ELANPRESS_0C4B, },
  {.vid = ELANPRESS_VEND_ID, .pid = 0x0c63, .driver_data = ELANPRESS_0C63, },
  {.vid = ELANPRESS_VEND_ID, .pid = 0x0c6e, .driver_data = ELANPRESS_0C6E, },
  {.vid = 0, .pid = 0, },
};
