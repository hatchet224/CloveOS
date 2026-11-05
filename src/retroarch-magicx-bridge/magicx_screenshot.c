/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "magicx_screenshot.h"
#include "vendor/lodepng.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool fail(char error[MAGICX_ERROR_MAX], const char *reason)
{
    (void)snprintf(error, MAGICX_ERROR_MAX, "%s", reason);
    return false;
}
bool magicx_screenshot_rotated(const void *pixels, unsigned int width, unsigned int height,
                       size_t pitch, enum magicx_pixel_format format, unsigned int clockwise_turns,
                       struct magicx_blob *output, char error[MAGICX_ERROR_MAX])
{
    unsigned char *rgb, *png = NULL;
    size_t bytes_per_pixel, png_size = 0U;
    unsigned int x, y, result, output_width, output_height;
    if (output == NULL || output->data != NULL || output->size != 0U || pixels == NULL ||
        (uintptr_t)pixels == UINTPTR_MAX || width == 0U || height == 0U ||
        width > 1024U || height > 1024U || clockwise_turns > 3U ||
        (format != MAGICX_RGB1555 && format != MAGICX_RGB565 && format != MAGICX_XRGB8888 &&
         format != MAGICX_BGR24_BOTTOM_UP)) {
        return fail(error, "no qualified frame available for coherent screenshot");
    }
    bytes_per_pixel = format == MAGICX_XRGB8888 ? 4U : (format == MAGICX_BGR24_BOTTOM_UP ? 3U : 2U);
    if (pitch < (size_t)width * bytes_per_pixel || pitch > SIZE_MAX / height) {
        return fail(error, "invalid software framebuffer pitch");
    }
    output_width = clockwise_turns % 2U != 0U ? height : width;
    output_height = clockwise_turns % 2U != 0U ? width : height;
    rgb = malloc((size_t)width * height * 3U);
    if (rgb == NULL) return fail(error, "screenshot allocation failed");
    for (y = 0U; y < height; ++y) {
        size_t source_y = format == MAGICX_BGR24_BOTTOM_UP ? (size_t)height - 1U - y : y;
        const unsigned char *row = (const unsigned char *)pixels + source_y * pitch;
        for (x = 0U; x < width; ++x) {
            unsigned int red, green, blue;
            unsigned int dx = x, dy = y;
            if (clockwise_turns == 1U) { dx = height - 1U - y; dy = x; }
            else if (clockwise_turns == 2U) { dx = width - 1U - x; dy = height - 1U - y; }
            else if (clockwise_turns == 3U) { dx = y; dy = width - 1U - x; }
            size_t at = ((size_t)dy * output_width + dx) * 3U;
            if (format == MAGICX_BGR24_BOTTOM_UP) {
                red = row[(size_t)x * 3U + 2U]; green = row[(size_t)x * 3U + 1U]; blue = row[(size_t)x * 3U];
            } else if (format == MAGICX_XRGB8888) {
                uint32_t p;
                memcpy(&p, row + (size_t)x * 4U, sizeof(p));
                red = (p >> 16U) & 255U; green = (p >> 8U) & 255U; blue = p & 255U;
            } else {
                uint16_t p;
                memcpy(&p, row + (size_t)x * 2U, sizeof(p));
                red = (p >> (format == MAGICX_RGB565 ? 11U : 10U)) & 31U;
                green = (p >> 5U) & (format == MAGICX_RGB565 ? 63U : 31U);
                blue = p & 31U;
                red = (red << 3U) | (red >> 2U);
                green = format == MAGICX_RGB565 ? (green << 2U) | (green >> 4U)
                                                : (green << 3U) | (green >> 2U);
                blue = (blue << 3U) | (blue >> 2U);
            }
            rgb[at] = (unsigned char)red;
            rgb[at + 1U] = (unsigned char)green;
            rgb[at + 2U] = (unsigned char)blue;
        }
    }
    result = lodepng_encode24(&png, &png_size, rgb, output_width, output_height);
    free(rgb);
    if (result != 0U || png_size == 0U || png_size > 4U * 1024U * 1024U) {
        free(png);
        return fail(error, "coherent PNG encoding failed or exceeded 4 MiB");
    }
    output->data = png;
    output->size = png_size;
    return true;
}

bool magicx_screenshot(const void *pixels, unsigned int width, unsigned int height,
                       size_t pitch, enum magicx_pixel_format format,
                       struct magicx_blob *output, char error[MAGICX_ERROR_MAX])
{
    return magicx_screenshot_rotated(pixels,width,height,pitch,format,0U,output,error);
}
