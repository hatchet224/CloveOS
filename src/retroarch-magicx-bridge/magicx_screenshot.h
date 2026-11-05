/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MAGICX_SCREENSHOT_H
#define MAGICX_SCREENSHOT_H
#include "magicx_bridge.h"
/* Frame at the paused runloop boundary. Hardware readback is BGR24 bottom-up;
 * the caller must read it on the render thread. Never pass a hardware sentinel. */
enum magicx_pixel_format { MAGICX_RGB1555, MAGICX_XRGB8888, MAGICX_RGB565, MAGICX_BGR24_BOTTOM_UP };
bool magicx_screenshot(const void *pixels, unsigned int width, unsigned int height,
                       size_t pitch, enum magicx_pixel_format format,
                       struct magicx_blob *output, char error[MAGICX_ERROR_MAX]);
/* Rotate normalized top-down pixels by clockwise quarter turns. Hardware GL
 * viewport capture uses this to undo the panel's counter-clockwise GL matrix;
 * software cached frames already precede that matrix and use the wrapper. */
bool magicx_screenshot_rotated(const void *pixels, unsigned int width, unsigned int height,
                       size_t pitch, enum magicx_pixel_format format, unsigned int clockwise_turns,
                       struct magicx_blob *output, char error[MAGICX_ERROR_MAX]);
#endif
