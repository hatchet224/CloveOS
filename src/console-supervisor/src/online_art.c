#include "supervisor/online.h"
#include "stb_image.h"
#include "lodepng.h"
#include <limits.h>
#include <stdlib.h>
bool sc_online_normalize_image(const unsigned char *bytes, size_t size,
                                 unsigned char **png, size_t *png_size)
{
    unsigned char *image, *scaled; int w, h, channels, actual_w, actual_h;
    unsigned int width, height; bool success;
    *png = NULL; *png_size = 0U;
    if (bytes == NULL || size > 8U * 1024U * 1024U || size > INT_MAX ||
        !stbi_info_from_memory(bytes, (int)size, &w, &h, &channels) || w < 1 || h < 1 ||
        w > 2048 || h > 2048 || (uint64_t)w * (uint64_t)h > 1048576U) return false;
    image = stbi_load_from_memory(bytes, (int)size, &actual_w, &actual_h, &channels, 4);
    if (image == NULL) return false;
    if (actual_w != w || actual_h != h) { stbi_image_free(image); return false; }
    width = (unsigned int)w; height = (unsigned int)h;
    if (width > 320U || height > 360U) {
        if ((uint64_t)width * 360U > (uint64_t)height * 320U) {
            height = height * 320U / width; width = 320U;
        } else { width = width * 360U / height; height = 360U; }
        if (width == 0U) width = 1U; if (height == 0U) height = 1U;
    }
    scaled = malloc((size_t)width * height * 4U);
    if (scaled == NULL) { stbi_image_free(image); return false; }
    for (unsigned int y = 0U; y < height; ++y)
        for (unsigned int x = 0U; x < width; ++x)
            for (unsigned int c = 0U; c < 4U; ++c)
                scaled[((size_t)y * width + x) * 4U + c] =
                    image[((size_t)(y * (unsigned int)h / height) * (unsigned int)w +
                           x * (unsigned int)w / width) * 4U + c];
    success = lodepng_encode32(png, png_size, scaled, width, height) == 0U &&
              *png_size > 0U && *png_size <= 8U * 1024U * 1024U;
    free(scaled); stbi_image_free(image);
    if (!success) { free(*png); *png = NULL; *png_size = 0U; }
    return success;
}
