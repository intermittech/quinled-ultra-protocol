/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: what the library's modules share (not an API).
 */
#ifndef ULTRA_SENDER_INTERNAL_H
#define ULTRA_SENDER_INTERNAL_H

#include "ultra_sender.h"

/* what the last frame was (retime) */
enum { ULTRA_KIND_NONE = 0, ULTRA_KIND_ULTRA, ULTRA_KIND_WS };

/* TIME = start + the frame's bits on the wire, the start offsets stamped against it, the header with l->hdr_flags */
int64_t ultra_sender_finish_(ultra_sender_t *l, int64_t start_us, uint8_t *out);
/* an RW frame in buf: no frame before its response window's end + the hold-off */
int64_t ultra_sender_rw_hold_(const ultra_sender_t *l, const uint8_t *buf);

#endif
