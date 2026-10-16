/* The little bit of drawing this program needs; see draw.c. */
#ifndef WINE_NX_START_DRAW_H
#define WINE_NX_START_DRAW_H

#include <stdbool.h>
#include <switch.h>

bool draw_start( void );
void draw_stop( void );
void draw_begin( void );
void draw_end( void );
void draw_text( int x, int y, int size, u32 colour, const char *text );
int draw_width( int size, const char *text );
void draw_bar( int x, int y, int w, int h, u32 colour );

#endif
