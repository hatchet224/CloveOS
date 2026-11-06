#include "launcher/display.h"
#include <ctype.h>
#include <string.h>

/* Original compact 5x7 monochrome glyph descriptions, rows encoded as 5 bits.
 * Lowercase renders uppercase, including Spanish Latin-1 letters.
 * Unsupported UTF-8 codepoints consume one '?' glyph.
 * No SDL_ttf, external assets, image decoder, or per-frame allocation is needed. */
struct glyph {
    Uint32 c;
    Uint8 rows[7];
};
static const struct glyph glyphs[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {31, 4, 4, 4, 4, 4, 31}},      {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'?', {14, 17, 1, 2, 4, 0, 4}},      {'!', {4, 4, 4, 4, 4, 0, 4}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {':', {0, 12, 12, 0, 12, 12, 0}},
    {'/', {1, 2, 2, 4, 8, 8, 16}},       {'[', {14, 8, 8, 8, 8, 8, 14}},
    {']', {14, 2, 2, 2, 2, 2, 14}},      {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},        {'%', {17, 2, 4, 4, 8, 16, 17}},
    {'=', {0, 31, 0, 31, 0, 0, 0}},      {'>', {16, 8, 4, 2, 4, 8, 16}},
    {'<', {1, 2, 4, 8, 4, 2, 1}},        {'_', {0, 0, 0, 0, 0, 0, 31}},
    {0xc1U, {4, 8, 14, 17, 31, 17, 17}}, /* Á */
    {0xc9U, {2, 4, 31, 16, 30, 16, 31}}, /* É */
    {0xcdU, {2, 4, 14, 4, 4, 4, 14}},   /* Í */
    {0xd3U, {4, 8, 14, 17, 17, 17, 14}}, /* Ó */
    {0xdaU, {4, 8, 17, 17, 17, 17, 14}}, /* Ú */
    {0xdcU, {10, 0, 17, 17, 17, 17, 14}}, /* Ü */
    {0xd1U, {10, 21, 17, 25, 21, 19, 17}}, /* Ñ */
    {0xbfU, {4, 0, 4, 8, 16, 17, 14}},   /* ¿ */
    {0xa1U, {4, 0, 4, 4, 4, 4, 4}},     /* ¡ */
    {' ', {0, 0, 0, 0, 0, 0, 0}},        {'\'', {4, 4, 8, 0, 0, 0, 0}}};
void cl_text(SDL_Surface *surface, int x, int y, const char *text, unsigned int scale, Uint32 color,
             size_t max_glyphs)
{
    size_t count = 0U;
    if (surface == NULL || text == NULL || scale == 0U || scale > 4U || x < -32 || x > surface->w ||
        y < -32 || y > surface->h) {
        return;
    }
    while (*text != '\0' && count++ < max_glyphs && x < surface->w) {
        Uint32 c = (unsigned char)*text++;
        const Uint8 *rows = NULL;
        size_t i;
        unsigned int row, col;
        if (c == 0xc2U || c == 0xc3U) {
            Uint32 second = (unsigned char)*text;
            if ((second & 0xc0U) == 0x80U) {
                ++text;
                c = ((c & 0x1fU) << 6U) | (second & 0x3fU);
                if (c >= 0xe0U && c <= 0xfeU && c != 0xf7U) c -= 0x20U;
            } else c = '?';
        } else if (c >= 128U) {
            while (((unsigned char)*text & 0xc0U) == 0x80U) ++text;
            c = '?';
        } else c = (Uint32)toupper((int)c);
        for (i = 0U; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
            if (glyphs[i].c == c) {
                rows = glyphs[i].rows;
                break;
            }
        }
        if (rows == NULL) {
            rows = glyphs[36].rows; /* '?' */
        }
        for (row = 0U; row < 7U; ++row) {
            for (col = 0U; col < 5U; ++col) {
                if ((rows[row] & (1U << (4U - col))) != 0U) {
                    SDL_Rect pixel = {x + (int)(col * scale), y + (int)(row * scale), (int)scale,
                                      (int)scale};
                    (void)SDL_FillRect(surface, &pixel, color);
                }
            }
        }
        x += (int)(6U * scale);
    }
}
