/* The screen before a ReXGlue port: a moment to change its settings without editing
 * the TOML by hand.
 *
 * A game's icon on the HOME menu is an NSP forwarder that loads this NRO with the path
 * of the port to run. It shows the game's name and waits five seconds: press nothing
 * and the game starts exactly as before, press Plus and its settings open. Whatever is
 * changed is written into the port's own TOML next to its NRO (prototype2.toml for
 * prototype2.nro) and then the run is handed over to the port.
 *
 *   argv[1]  sdmc:/switch/prototype2-nx/prototype2.nro
 *   edits    sdmc:/switch/prototype2-nx/prototype2.toml
 *   loads    sdmc:/switch/prototype2-nx/prototype2.nro
 *
 * The keys offered are chosen by the TOML itself: only keys already in the file are
 * shown (a key the port does not know is a setting that silently does nothing), with
 * their values from a table here. Every other line is copied through untouched.
 *
 * Based on fungalspores/winenx-start (GPL-3.0); drawing with libnx's framebuffer and
 * the console's own font through FreeType (draw.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "draw.h"

#define WAIT_SECONDS 5
#define MAX_SETTINGS 32768

/* ---- the settings this screen offers ---- */
typedef struct
{
    const char *game;                               /* NRO stem it is offered for, NULL = every port */
    const char *key;
    const char *title_ru, *title_en;
    const char *note_ru, *note_en;
    const char *values[10];                          /* NULL-terminated, written as they are */
    const char *shown[10];                           /* how a value is shown */
    const char *recommended;                         /* value marked "рекомендуется", NULL = none */
} option;

static const option options[] =
{
    /* Only the resolution: the one setting that changes what you get on screen. */
    { "prototype2", "p2_render_width", "Разрешение", "Resolution",
      "Внутреннее, растягивается на экран. Меньше — быстрее и холоднее, но мыльнее.",
      "Internal, scaled to the screen. Lower is faster and cooler but softer.",
      { "640", "800", "960", "1200", NULL },
      { "640x352", "800x448", "960x544", "1200x672", NULL }, "800" },
    { "prototype", "proto_render_width", "Разрешение", "Resolution",
      "Внутреннее, растягивается на экран. Меньше — быстрее и холоднее, но мыльнее.",
      "Internal, scaled to the screen. Lower is faster and cooler but softer.",
      { "640", "720", "960", "1280", NULL },
      { "640x352", "720x400", "960x544", "1280x720", NULL }, "720" },
    { "shd", "shd_render_width", "Разрешение", "Resolution",
      "Внутреннее, растягивается на экран. Меньше — быстрее и холоднее, но мыльнее.",
      "Internal, scaled to the screen. Lower is faster and cooler but softer.",
      { "640", "960", "1280", NULL },
      { "640x360", "960x540", "1280x720", NULL }, "640" },
};

#define OPTION_COUNT ((int)(sizeof(options) / sizeof(options[0])))

static char program_path[FS_MAX_PATH];
static char program_stem[64];
static char settings_path[FS_MAX_PATH];
static char game_name[128];
static char *settings_text;
static int chosen[OPTION_COUNT];
static int original[OPTION_COUNT];                 /* value the TOML had (0 if the key was missing) */
static bool written[OPTION_COUNT];                 /* key already rewritten in place */
static int shown_list[OPTION_COUNT];               /* options present in this TOML */
static int shown_count;

/* ---- the TOML ---- */

/* sdmc:/switch/prototype2-nx/prototype2.nro -> ...prototype2.toml */
static bool settings_for( const char *program )
{
    const char *slash = strrchr( program, '/' );
    const char *dot;
    size_t stem;

    if (!slash || strlen( program ) >= sizeof(program_path)) return false;
    strcpy( program_path, program );
    dot = strrchr( slash, '.' );
    if (!dot) dot = slash + strlen( slash );
    snprintf( program_stem, sizeof(program_stem), "%.*s", (int)(dot - slash - 1), slash + 1 );
    stem = (size_t)(dot - program);
    if (stem + sizeof(".toml") >= sizeof(settings_path)) return false;
    memcpy( settings_path, program, stem );
    strcpy( settings_path + stem, ".toml" );
    return true;
}

static void read_settings( void )
{
    FILE *file;
    size_t got = 0;

    settings_text = malloc( MAX_SETTINGS );
    if (!settings_text) return;
    if ((file = fopen( settings_path, "rb" )))
    {
        got = fread( settings_text, 1, MAX_SETTINGS - 1, file );
        fclose( file );
    }
    settings_text[got] = 0;
}

/* "key = value" with any spaces; returns the value of a key, or NULL. */
static const char *value_of( const char *key, char *buffer, size_t size )
{
    const char *line = settings_text;
    size_t key_length = strlen( key );

    while (line && *line)
    {
        const char *end = strchr( line, '\n' );
        const char *p = line;

        while (*p == ' ' || *p == '\t') p++;
        if (*p != '#' && !strncmp( p, key, key_length ) &&
            (p[key_length] == ' ' || p[key_length] == '\t' || p[key_length] == '='))
        {
            const char *eq = strchr( p, '=' );
            const char *stop = end ? end : p + strlen( p );
            size_t length;

            if (eq && eq < stop)
            {
                eq++;
                while (*eq == ' ' || *eq == '\t' || *eq == '"') eq++;
                while (stop > eq && (stop[-1] == '\r' || stop[-1] == ' ' || stop[-1] == '"')) stop--;
                length = (size_t)(stop - eq);
                if (length >= size) length = size - 1;
                memcpy( buffer, eq, length );
                buffer[length] = 0;
                return buffer;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return NULL;
}

static bool line_is_key( const char *line, const char *key )
{
    size_t key_length = strlen( key );

    while (*line == ' ' || *line == '\t') line++;
    return !strncmp( line, key, key_length ) &&
           (line[key_length] == ' ' || line[key_length] == '\t' || line[key_length] == '=');
}

static void read_current( void )
{
    int i, v;

    shown_count = 0;
    for (i = 0; i < OPTION_COUNT; i++)
    {
        char buffer[64];
        const char *now = value_of( options[i].key, buffer, sizeof(buffer) );

        chosen[i] = 0;
        original[i] = 0;
        if (options[i].game && strcmp( options[i].game, program_stem )) continue;  /* another game's */
        /* Silent Hill: only its resolution -- the shared Prototype keys do nothing there. */
        if (!options[i].game && !strcmp( program_stem, "shd" )) continue;
        /* A key missing from the TOML is still offered: value 0 is the port's default,
         * and write_settings appends the key only if the user moves away from it. */
        shown_list[shown_count++] = i;
        if (!now) continue;
        for (v = 0; options[i].values[v]; v++)
            if (!strcmp( now, options[i].values[v] )) { chosen[i] = v; original[i] = v; break; }
    }
}

/* The height that goes with a width (p2_render_height follows p2_render_width, proto_ likewise). */
static const char *height_for( const char *width )
{
    static const char *pairs[][2] = { { "640", "352" }, { "720", "400" }, { "800", "448" }, { "880", "496" }, { "960", "544" },
                                      { "1040", "592" }, { "1120", "624" }, { "1200", "672" },
                                      { "1280", "720" } };
    unsigned int i;

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        if (!strcmp( width, pairs[i][0] )) return pairs[i][1];
    return NULL;
}

/* Silent Hill's own frame is 1280x720, a true 16:9, and its heights are multiples of 16 rather
 * than the Prototypes' grid. Its own 720 is kept exact instead of coming out of the arithmetic. */
static const char *shd_height_for( const char *width )
{
    static const char *pairs[][2] = { { "640", "360" }, { "720", "400" }, { "800", "448" }, { "880", "496" },
                                      { "960", "540" }, { "1040", "585" }, { "1120", "630" }, { "1280", "720" } };
    unsigned int i;

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        if (!strcmp( width, pairs[i][0] )) return pairs[i][1];
    return NULL;
}

/* Writes the file back: each offered key set where it stands, everything else copied
 * through untouched. Line endings of the original are kept (CRLF or LF). */
static void write_settings( void )
{
    FILE *file = fopen( settings_path, "wb" );
    const char *line = settings_text;
    const char *height = NULL, *height_key = NULL;
    bool height_done = false;
    int s;

    if (!file) return;
    memset( written, 0, sizeof(written) );
    for (s = 0; s < shown_count; s++)
    {
        int i = shown_list[s];
        if (!strcmp( options[i].key, "p2_render_width" )) height_key = "p2_render_height";
        else if (!strcmp( options[i].key, "proto_render_width" )) height_key = "proto_render_height";
        else if (!strcmp( options[i].key, "shd_render_width" ))
        {
            height_key = "shd_render_height";
            height = shd_height_for( options[i].values[chosen[i]] );
            continue;
        }
        else continue;
        height = height_for( options[i].values[chosen[i]] );
    }

    while (line && *line)
    {
        const char *end = strchr( line, '\n' );
        size_t length = end ? (size_t)(end - line) + 1 : strlen( line );
        bool crlf = end && end > line && end[-1] == '\r';
        int mine = -1;

        for (s = 0; s < shown_count; s++)
            if (line_is_key( line, options[shown_list[s]].key )) mine = shown_list[s];
        if (mine >= 0)
        {
            written[mine] = true;
            /* a string value keeps its quotes: present_effect = "fsr" */
            const char *eq = strchr( line, '=' );
            bool quoted = eq && (!end || eq < end) && strchr( eq, '"' ) && (!end || strchr( eq, '"' ) < end);
            fprintf( file, quoted ? "%s = \"%s\"%s" : "%s = %s%s", options[mine].key,
                     options[mine].values[chosen[mine]], end ? (crlf ? "\r\n" : "\n") : "" );
        }
        else if (height && line_is_key( line, height_key ))
        {
            height_done = true;
            fprintf( file, "%s = %s%s", height_key, height, end ? (crlf ? "\r\n" : "\n") : "" );
        }
        else
            fwrite( line, 1, length, file );
        line = end ? end + 1 : NULL;
    }
    /* Keys the TOML did not have: appended only if the user changed them from the default. */
    for (s = 0; s < shown_count; s++)
    {
        int i = shown_list[s];
        const char *v = options[i].values[chosen[i]];
        bool quoted = v[0] >= 'a' && v[0] <= 'z' && strcmp( v, "true" ) && strcmp( v, "false" );

        if (written[i] || chosen[i] == original[i]) continue;
        fprintf( file, quoted ? "%s = \"%s\"\n" : "%s = %s\n", options[i].key, v );
        if (height && !height_done && height_key &&
            (!strcmp( options[i].key, "p2_render_width" ) || !strcmp( options[i].key, "proto_render_width" )))
        {
            fprintf( file, "%s = %s\n", height_key, height );
            height_done = true;
        }
    }
    fclose( file );
}

/* ---- the screen ---- */

#define WHITE 0x00ebebeb
#define GREY  0x00969696
#define RED   0x003c3cd2

#define CHANNEL "t.me/fspores"

static void draw_channel( void )
{
    draw_text( 1190 - draw_width( 18, CHANNEL ), 700, 18, 0x00606060, CHANNEL );
}

static void draw_middle( int y, int size, u32 colour, const char *text )
{
    draw_text( (1280 - draw_width( size, text )) / 2, y, size, colour, text );
}

static void draw_waiting( int seconds_left )
{
    char line[160];

    draw_begin();
    draw_bar( 0, 0, 1280, 6, RED );
    draw_middle( 250, 44, WHITE, game_name );
    draw_middle( 292, 22, GREY, "Nintendo Switch · ReXGlue" );
    draw_middle( 400, 32, RED, "Нажмите + — настройки игры" );
    draw_middle( 438, 24, GREY, "Press + for settings" );
    snprintf( line, sizeof(line), "Запуск через %d   ·   starting in %d", seconds_left, seconds_left );
    draw_middle( 530, 24, GREY, line );
    draw_channel();
    draw_end();
}

#define PER_PAGE 4

static void draw_settings( int picked )
{
    int first = picked / PER_PAGE * PER_PAGE;
    int s;
    char page[32];

    draw_begin();
    draw_bar( 0, 0, 1280, 6, RED );
    draw_text( 90, 90, 34, WHITE, game_name );
    draw_text( 90, 124, 20, GREY, "Настройки / Settings" );
    if (shown_count > PER_PAGE)
    {
        snprintf( page, sizeof(page), "%d / %d", picked / PER_PAGE + 1, (shown_count + PER_PAGE - 1) / PER_PAGE );
        draw_text( 1190 - draw_width( 20, page ), 124, 20, GREY, page );
    }
    draw_bar( 90, 146, 1100, 1, 0x00303030 );

    for (s = first; s < shown_count && s < first + PER_PAGE; s++)
    {
        const option *o = &options[shown_list[s]];
        int i = shown_list[s];
        int y = 206 + (s - first) * 112;
        u32 colour = s == picked ? RED : WHITE;
        char value[96];

        if (s == picked)
        {
            draw_bar( 60, y - 40, 1160, 100, 0x00181820 );
            draw_bar( 60, y - 40, 5, 100, RED );
        }
        draw_text( 90, y, 28, colour, o->title_ru );
        draw_text( 90 + draw_width( 28, o->title_ru ) + 14, y, 20, GREY, o->title_en );
        snprintf( value, sizeof(value), "< %s >", o->shown[chosen[i]] );
        draw_text( 1190 - draw_width( 26, value ), y, 26, colour, value );
        /* Mark the value we measured as the best trade-off. */
        if (o->recommended && !strcmp( o->values[chosen[i]], o->recommended ))
        {
            const char *mark = "рекомендуется / recommended";
            draw_text( 1190 - draw_width( 26, value ) - draw_width( 18, mark ) - 16, y + 4, 18,
                       GREY, mark );
        }
        draw_text( 90, y + 30, 18, GREY, o->note_ru );
        draw_text( 90, y + 54, 18, GREY, o->note_en );
    }

    draw_bar( 90, 636, 1100, 1, 0x00303030 );
    draw_text( 90, 672, 20, GREY, "Вверх/вниз — выбрать   ·   влево/вправо — изменить   ·   A — играть" );
    draw_text( 90, 700, 20, GREY, "Up/Down choose   ·   Left/Right change   ·   A start the game" );
    draw_channel();
    draw_end();
}

/* ---- ---- */

int main( int argc, char **argv )
{
    const char *program = argc > 1 ? argv[1] : NULL;
    bool settings_open = false;
    int picked = 0;
    PadState pad;
    u64 started;

    if (!program || !settings_for( program )) return 0;   /* nothing to start */

    /* The game's name: the second argument, else the NRO's file name. */
    if (argc > 2) snprintf( game_name, sizeof(game_name), "%s", argv[2] );
    else
    {
        const char *slash = strrchr( program, '/' );
        snprintf( game_name, sizeof(game_name), "%s", slash ? slash + 1 : program );
        char *dot = strrchr( game_name, '.' );
        if (dot) *dot = 0;
    }

    read_settings();
    read_current();

    if (!draw_start() || shown_count == 0)
    {
        envSetNextLoad( program_path, program_path );   /* no screen or nothing to set: just play */
        return 0;
    }

    padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
    padInitializeDefault( &pad );
    started = armGetSystemTick();

    while (appletMainLoop())
    {
        int seconds_left = WAIT_SECONDS - (int)(armTicksToNs( armGetSystemTick() - started ) / 1000000000ull);
        u64 pressed;

        padUpdate( &pad );
        pressed = padGetButtonsDown( &pad );

        if (!settings_open)
        {
            if (pressed & HidNpadButton_Plus) settings_open = true;
            else if ((pressed & HidNpadButton_A) || seconds_left <= 0) break;
            else draw_waiting( seconds_left );
            continue;
        }

        if (pressed & (HidNpadButton_A | HidNpadButton_Plus)) break;
        if (pressed & (HidNpadButton_Up | HidNpadButton_AnyUp)) picked = (picked + shown_count - 1) % shown_count;
        if (pressed & (HidNpadButton_Down | HidNpadButton_AnyDown)) picked = (picked + 1) % shown_count;
        if (pressed & (HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_AnyLeft | HidNpadButton_AnyRight))
        {
            int i = shown_list[picked], count = 0;

            while (options[i].values[count]) count++;
            chosen[i] = (pressed & (HidNpadButton_Left | HidNpadButton_AnyLeft))
                      ? (chosen[i] + count - 1) % count
                      : (chosen[i] + 1) % count;
        }
        draw_settings( picked );
    }

    if (settings_open) write_settings();
    free( settings_text );
    draw_stop();
    envSetNextLoad( program_path, program_path );
    return 0;
}
