/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#error "Screenshot tests require active assertions"
#endif
#include "magicx_screenshot.h"
#include "vendor/lodepng.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(const void *pixels, size_t pitch, enum magicx_pixel_format format)
{
    const unsigned char expected[] = {255,0,0,0,255,0,0,0,255,255,255,255};
    struct magicx_blob png = {0};
    char error[MAGICX_ERROR_MAX];
    unsigned char *decoded = NULL;
    unsigned int width = 0, height = 0;
    assert(magicx_screenshot(pixels, 2U, 2U, pitch, format, &png, error));
    assert(lodepng_decode24(&decoded, &width, &height, png.data, png.size) == 0U);
    assert(width == 2U && height == 2U && memcmp(decoded, expected, sizeof(expected)) == 0);
    free(decoded);
    free(png.data);
}
static void check_rotations(void)
{
    /* Distinct non-square corners, BGR bottom-up rows and padding expose both
     * rotation direction and vertical-flip/pitch errors. */
    const unsigned char pixels[]={0,0,128,0,0,160,0,0,192,99,99,99,
                                  0,0,32,0,0,64,0,0,96,99,99,99};
    const unsigned int expected[4][6]={{1,2,3,4,5,6},{4,1,5,2,6,3},
                                      {6,5,4,3,2,1},{3,6,2,5,1,4}};
    for(unsigned int turn=0U;turn<4U;++turn) {
        struct magicx_blob png={0};char error[MAGICX_ERROR_MAX];
        unsigned char *decoded=NULL;unsigned int width=0U,height=0U;
        assert(magicx_screenshot_rotated(pixels,3U,2U,12U,MAGICX_BGR24_BOTTOM_UP,turn,&png,error));
        assert(lodepng_decode24(&decoded,&width,&height,png.data,png.size)==0U);
        assert(width==(turn%2U?2U:3U) && height==(turn%2U?3U:2U));
        for(unsigned int i=0U;i<6U;++i) {
            assert(decoded[i*3U]==expected[turn][i]*32U);
            assert(decoded[i*3U+1U]==0U && decoded[i*3U+2U]==0U);
        }
        free(decoded);free(png.data);
    }
    struct magicx_blob png={0};char error[MAGICX_ERROR_MAX];
    assert(!magicx_screenshot_rotated(pixels,3U,2U,12U,MAGICX_BGR24_BOTTOM_UP,4U,&png,error));
    assert(png.data==NULL && png.size==0U);
    puts("SCREENSHOT_ORIENTATION_PASS four_quarter_turns=1 exact_decoded_pixels=1 nonsquare=1 padded_bottom_up=1 software_wrapper_unchanged=1");
}
int main(void)
{
    check_rotations();
    const uint16_t rgb565[] = {0xf800,0x07e0,0x1234,0x001f,0xffff,0x1234};
    const uint16_t rgb1555[] = {0x7c00,0x03e0,0x001f,0x7fff};
    const uint32_t xrgb[] = {0x00ff0000,0x0000ff00,0x000000ff,0x00ffffff};
    const unsigned char gpu[] = {255,0,0,255,255,255,99,99,0,0,255,0,255,0,99,99};
    struct magicx_blob png = {0};
    char error[MAGICX_ERROR_MAX];
    check(rgb565, 6U, MAGICX_RGB565);
    check(rgb1555, 4U, MAGICX_RGB1555);
    check(xrgb, 8U, MAGICX_XRGB8888);
    check(gpu, 8U, MAGICX_BGR24_BOTTOM_UP);
    assert(!magicx_screenshot(NULL,2U,2U,4U,MAGICX_RGB565,&png,error));
    assert(!magicx_screenshot((void *)UINTPTR_MAX,2U,2U,4U,MAGICX_RGB565,&png,error));
    assert(!magicx_screenshot(rgb565,0U,2U,4U,MAGICX_RGB565,&png,error));
    assert(!magicx_screenshot(rgb565,1025U,2U,4U,MAGICX_RGB565,&png,error));
    assert(!magicx_screenshot(rgb565,2U,2U,3U,MAGICX_RGB565,&png,error));
    assert(!magicx_screenshot(rgb565,2U,2U,SIZE_MAX,MAGICX_RGB565,&png,error));
    assert(!magicx_screenshot(rgb565,2U,2U,4U,(enum magicx_pixel_format)99,&png,error));
    assert(png.data == NULL && png.size == 0U);
    png.data = (void *)rgb565; png.size = 3U;
    assert(!magicx_screenshot(rgb565,2U,2U,4U,MAGICX_RGB565,&png,error));
    assert(png.data == rgb565 && png.size == 3U);
    puts("SCREENSHOT_PASS: decoded exact RGB565/RGB1555/XRGB8888 pixels, padded pitch, rejected hardware/null/invalid bounds, output atomicity");
    return 0;
}
