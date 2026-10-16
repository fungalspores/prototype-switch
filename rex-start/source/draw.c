/* Text on the screen without a graphics stack.
 *
 * The first version of this program drew through SDL, which brings in Mesa and the
 * Nvidia driver — eight megabytes of graphics stack, started inside the cut-down
 * 32-bit address space the games need. This draws into the console's own
 * framebuffer instead and rasterises the system font with FreeType, so the whole
 * thing is a megabyte and depends on nothing but libnx.
 *
 * The font is the one the console itself uses (plGetSharedFontByType), which has
 * Cyrillic in it: the screen can be in Russian as well as English.
 */
#include <string.h>
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "draw.h"

#define WIDTH 1280
#define HEIGHT 720

static Framebuffer framebuffer;
static FT_Library library;
static FT_Face face;
static u32 *pixels;
static u32 stride_words;

bool draw_start( void )
{
    PlFontData font;

    if (R_FAILED(plInitialize( PlServiceType_User ))) return false;
    if (R_FAILED(plGetSharedFontByType( &font, PlSharedFontType_Standard ))) return false;
    if (FT_Init_FreeType( &library )) return false;
    if (FT_New_Memory_Face( library, font.address, font.size, 0, &face )) return false;
    framebufferCreate( &framebuffer, nwindowGetDefault(), WIDTH, HEIGHT, PIXEL_FORMAT_RGBA_8888, 2 );
    framebufferMakeLinear( &framebuffer );
    return true;
}

void draw_stop( void )
{
    framebufferClose( &framebuffer );
    FT_Done_Face( face );
    FT_Done_FreeType( library );
    plExit();
}

void draw_begin( void )
{
    u32 stride;

    pixels = (u32 *)framebufferBegin( &framebuffer, &stride );
    stride_words = stride / sizeof(u32);
    memset( pixels, 0, (size_t)stride_words * HEIGHT * sizeof(u32) );
}

void draw_end( void )
{
    framebufferEnd( &framebuffer );
    pixels = NULL;
}

/* One glyph, blended onto what is already there. FreeType hands over an 8-bit
 * coverage map; the colour is the same everywhere, only the strength changes. */
static void put_glyph( const FT_Bitmap *map, int left, int top, u32 colour )
{
    unsigned int x, y;
    u32 r = colour & 0xff, g = (colour >> 8) & 0xff, b = (colour >> 16) & 0xff;

    for (y = 0; y < map->rows; y++)
    {
        int py = top + (int)y;

        if (py < 0 || py >= HEIGHT) continue;
        for (x = 0; x < map->width; x++)
        {
            int px = left + (int)x;
            u32 cover = map->buffer[y * (unsigned int)map->pitch + x];
            u32 *at, old;
            u32 orr, og, ob;

            if (px < 0 || px >= WIDTH || !cover) continue;
            at = &pixels[(size_t)py * stride_words + (size_t)px];
            old = *at;
            orr = old & 0xff; og = (old >> 8) & 0xff; ob = (old >> 16) & 0xff;
            orr = (r * cover + orr * (255 - cover)) / 255;
            og  = (g * cover + og  * (255 - cover)) / 255;
            ob  = (b * cover + ob  * (255 - cover)) / 255;
            *at = orr | (og << 8) | (ob << 16) | 0xff000000;
        }
    }
}

/* UTF-8 in, one code point out. */
static const char *next_code_point( const char *text, u32 *out )
{
    unsigned char first = (unsigned char)*text++;

    if (first < 0x80) { *out = first; return text; }
    if ((first & 0xe0) == 0xc0)
    {
        *out = ((u32)(first & 0x1f) << 6) | (*text & 0x3f);
        return text + 1;
    }
    if ((first & 0xf0) == 0xe0)
    {
        *out = ((u32)(first & 0x0f) << 12) | ((u32)(text[0] & 0x3f) << 6) | (text[1] & 0x3f);
        return text + 2;
    }
    *out = '?';
    return text + 1;
}

void draw_text( int x, int y, int size, u32 colour, const char *text )
{
    if (!pixels || !text) return;
    FT_Set_Pixel_Sizes( face, 0, (FT_UInt)size );
    while (*text)
    {
        u32 code;

        text = next_code_point( text, &code );
        if (FT_Load_Char( face, code, FT_LOAD_RENDER )) continue;
        put_glyph( &face->glyph->bitmap, x + face->glyph->bitmap_left,
                   y - face->glyph->bitmap_top, colour );
        x += (int)(face->glyph->advance.x >> 6);
    }
}

/* How wide a line will be, for putting things to the right or in the middle. */
int draw_width( int size, const char *text )
{
    int width = 0;

    if (!text) return 0;
    FT_Set_Pixel_Sizes( face, 0, (FT_UInt)size );
    while (*text)
    {
        u32 code;

        text = next_code_point( text, &code );
        if (FT_Load_Char( face, code, FT_LOAD_DEFAULT )) continue;
        width += (int)(face->glyph->advance.x >> 6);
    }
    return width;
}

void draw_bar( int x, int y, int w, int h, u32 colour )
{
    int px, py;

    if (!pixels) return;
    for (py = y; py < y + h; py++)
    {
        if (py < 0 || py >= HEIGHT) continue;
        for (px = x; px < x + w; px++)
        {
            if (px < 0 || px >= WIDTH) continue;
            pixels[(size_t)py * stride_words + (size_t)px] = colour | 0xff000000;
        }
    }
}
