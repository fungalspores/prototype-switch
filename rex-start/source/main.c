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
    const char *values[8];                          /* NULL-terminated, written as they are */
    const char *shown[8];                           /* how a value is shown */
} option;

static const option options[] =
{
    /* Prototype 2 */
    { "prototype2", "p2_render_width", "Разрешение", "Resolution",
      "Внутреннее, растягивается FSR. Больше — чётче, но тяжелее для видеокарты.",
      "Internal, scaled with FSR. Higher is sharper but costs the GPU more.",
      { "640", "800", "960", "1040", "1200", NULL },
      { "640x352", "800x448", "960x544", "1040x592", "1200x672", NULL } },
    { "prototype2", "p2_native_texture_lod_bias", "Резкость текстур", "Texture sharpness",
      "Меньше — резче текстуры вдали.", "Lower is sharper far textures.",
      { "1.0", "0.5", "0.0", NULL }, { "мягко / soft", "средне / medium", "резко / sharp", NULL } },
    { "prototype2", "p2_native_max_anisotropy", "Анизотропия", "Anisotropic filtering",
      "Чёткость полов и дорог под углом.", "Floors and roads at an angle.",
      { "1", "2", "4", NULL }, { "выкл / off", "2x", "4x", NULL } },
    { "prototype2", "p2_geo_lod_bias_scale", "Детализация моделей", "Model detail",
      "Меньше — детальнее модели вблизи. Грузит процессор.", "Lower is more detail nearby. Costs the CPU.",
      { "4.0", "2.0", "1.0", NULL }, { "низкая / low", "средняя / medium", "высокая / high", NULL } },
    { "prototype2", "p2_shadow_range_scale", "Дальность теней", "Shadow distance",
      "Тени дальних объектов.", "Shadows of far objects.",
      { "0.3", "0.5", "0.7", "1.0", NULL }, { "30%", "50%", "70%", "100%", NULL } },
    { "prototype2", "p2_ped_max_scale", "Пешеходы", "Pedestrians",
      "Сильнее всего грузит процессор.", "Costs the CPU the most.",
      { "0.15", "0.3", "0.5", "1.0", NULL }, { "15%", "30%", "50%", "100%", NULL } },
    { "prototype2", "p2_vehicle_max_scale", "Машины", "Vehicles",
      "Тоже грузит процессор.", "Costs the CPU too.",
      { "0.1", "0.15", "0.3", "1.0", NULL }, { "10%", "15%", "30%", "100%", NULL } },
    /* Prototype */
    { "prototype", "proto_render_width", "Разрешение", "Resolution",
      "Внутреннее, растягивается FSR. Меньше — быстрее, но мыльнее.",
      "Internal, scaled with FSR. Lower is faster but softer.",
      { "1280", "1120", "960", "800", NULL },
      { "1280x720", "1120x624", "960x544", "800x448", NULL } },
    { "prototype", "p2_native_texture_lod_bias", "Резкость текстур", "Texture sharpness",
      "Меньше — резче текстуры вдали.", "Lower is sharper far textures.",
      { "1.0", "0.5", "0.0", NULL }, { "мягко / soft", "средне / medium", "резко / sharp", NULL } },
    { "prototype", "p2_native_max_anisotropy", "Анизотропия", "Anisotropic filtering",
      "Чёткость полов и дорог под углом.", "Floors and roads at an angle.",
      { "1", "2", "4", NULL }, { "выкл / off", "2x", "4x", NULL } },
    /* both games */
    { NULL, "present_effect", "Масштабирование", "Upscaling",
      "Как картинка растягивается на экран.", "How the picture is scaled to the screen.",
      { "fsr", "bilinear", NULL }, { "FSR 1", "обычное / bilinear", NULL } },
    { NULL, "user_language", "Язык", "Language",
      "Если в вашей копии игры есть этот язык.", "If your copy of the game has it.",
      { "12", "1", NULL }, { "Русский", "English", NULL } },
};

#define OPTION_COUNT ((int)(sizeof(options) / sizeof(options[0])))

static char program_path[FS_MAX_PATH];
static char program_stem[64];
static char settings_path[FS_MAX_PATH];
static char game_name[128];
static char *settings_text;
static int chosen[OPTION_COUNT];
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
        if (options[i].game && strcmp( options[i].game, program_stem )) continue;  /* another game's */
        if (!now) continue;                         /* this port does not have it */
        shown_list[shown_count++] = i;
        for (v = 0; options[i].values[v]; v++)
            if (!strcmp( now, options[i].values[v] )) { chosen[i] = v; break; }
    }
}

/* The height that goes with a width (p2_render_height follows p2_render_width, proto_ likewise). */
static const char *height_for( const char *width )
{
    static const char *pairs[][2] = { { "640", "352" }, { "800", "448" }, { "960", "544" },
                                      { "1040", "592" }, { "1120", "624" }, { "1200", "672" },
                                      { "1280", "720" } };
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
    int s;

    if (!file) return;
    for (s = 0; s < shown_count; s++)
    {
        int i = shown_list[s];
        if (!strcmp( options[i].key, "p2_render_width" )) height_key = "p2_render_height";
        else if (!strcmp( options[i].key, "proto_render_width" )) height_key = "proto_render_height";
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
            /* a string value keeps its quotes: present_effect = "fsr" */
            const char *eq = strchr( line, '=' );
            bool quoted = eq && (!end || eq < end) && strchr( eq, '"' ) && (!end || strchr( eq, '"' ) < end);
            fprintf( file, quoted ? "%s = \"%s\"%s" : "%s = %s%s", options[mine].key,
                     options[mine].values[chosen[mine]], end ? (crlf ? "\r\n" : "\n") : "" );
        }
        else if (height && line_is_key( line, height_key ))
            fprintf( file, "%s = %s%s", height_key, height, end ? (crlf ? "\r\n" : "\n") : "" );
        else
            fwrite( line, 1, length, file );
        line = end ? end + 1 : NULL;
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
