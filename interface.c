/*
 * Copyright (C) 2018 Mark Hills <mark@xwax.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#define _GNU_SOURCE /* strdupa() */

#include <assert.h>
#include <errno.h>
#include <iconv.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <SDL.h>
#include <SDL_ttf.h>

#include "interface.h"
#include "layout.h"
#include "player.h"
#include "rig.h"
#include "selector.h"
#include "status.h"
#include "timecoder.h"
#include "xwax.h"

#define REFRESH 30

#define FONT "DejaVuSans.ttf"
#define FONT_SIZE 10
#define FONT_SPACE 15

#define EM_FONT "DejaVuSans-Oblique.ttf"

#define BIG_FONT "DejaVuSans-Bold.ttf"
#define BIG_FONT_SIZE 14
#define BIG_FONT_SPACE 19

#define CLOCK_FONT FONT
#define CLOCK_FONT_SIZE 32

#define DECI_FONT FONT
#define DECI_FONT_SIZE 20

#define DETAIL_FONT "DejaVuSansMono-Bold.ttf"
#define DETAIL_FONT_SIZE 9
#define DETAIL_FONT_SPACE 12

#define DEFAULT_WIDTH 960
#define DEFAULT_HEIGHT 720

#define DEFAULT_SCALE 1.0

#define BORDER 12
#define SPACER 8
#define HALF_SPACER 4

#define CURSOR_WIDTH 4

#define PLAYER_HEIGHT 213
#define OVERVIEW_HEIGHT 16

#define LIBRARY_MIN_WIDTH 64
#define LIBRARY_MIN_HEIGHT 64

#define DEFAULT_METER_SCALE 8
#define MAX_METER_SCALE 11

#define SEARCH_HEIGHT (FONT_SPACE)
#define STATUS_HEIGHT (DETAIL_FONT_SPACE)

#define BPM_WIDTH 32
#define SORT_WIDTH 21
#define RESULTS_ARTIST_WIDTH 200

#define TOKEN_SPACE 2

#define CLOCKS_WIDTH 160

#define SPINNER_SIZE (CLOCK_FONT_SIZE * 2 - 6)
#define SCOPE_SIZE (CLOCK_FONT_SIZE * 2 - 6)

#define SCROLLBAR_SIZE 10

#define METER_WARNING_TIME 20

#define FUNC_LOAD 0
#define FUNC_RECUE 1
#define FUNC_TIMECODE 2

#define EVENT_TICKER (SDL_USEREVENT)
#define EVENT_QUIT   (SDL_USEREVENT + 1)
#define EVENT_STATUS (SDL_USEREVENT + 2)
#define EVENT_SELECTOR (SDL_USEREVENT + 3)

#define MIN(x,y) ((x)<(y)?(x):(y))
#define SQ(x) ((x)*(x))

static const char *font_dirs[] = {
    "/usr/X11R6/lib/X11/fonts/TTF",
    "/usr/share/fonts/truetype/ttf-dejavu/",
    "/usr/share/fonts/ttf-dejavu",
    "/usr/share/fonts/dejavu",
    "/usr/share/fonts/TTF",
    "/usr/share/fonts/truetype/dejavu",
    "/usr/share/fonts/truetype/ttf-dejavu",
    NULL
};

static TTF_Font *clock_font, *deci_font, *detail_font,
    *font, *em_font, *big_font;

static SDL_Color background_col = {18, 18, 30, 255},
    text_col = {220, 225, 235, 255},
    alert_col = {255, 80, 50, 255},
    ok_col = {0, 210, 120, 255},
    elapsed_col = {0, 180, 255, 255},
    cursor_col = {255, 60, 60, 255},
    selected_col = {35, 50, 80, 255},
    detail_col = {120, 130, 150, 255},
    needle_col = {255, 255, 255, 255},
    artist_col = {0, 90, 60, 255},
    bpm_col = {90, 40, 10, 255},
    accent_col = {0, 180, 255, 255},
    panel_border_col = {45, 45, 65, 255};

static unsigned short *spinner_angle;
static int spinner_size;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *spinner_tex;
static SDL_Texture *scope_tex;
static int scope_tex_size;

static int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT,
    meter_scale = DEFAULT_METER_SCALE;
static Uint32 window_flags = SDL_WINDOW_RESIZABLE;
static float scale = DEFAULT_SCALE;
static int win_x = SDL_WINDOWPOS_UNDEFINED, win_y = SDL_WINDOWPOS_UNDEFINED;
static iconv_t utf;
static pthread_t ph;
static struct selector selector;
static struct observer on_status, on_selector;

#define TEXT_CACHE_SLOTS 48

struct text_cache_entry {
    char text[256];
    TTF_Font *font;
    Uint32 fg_key;
    Uint32 bg_key;
    SDL_Texture *texture;
    int w, h;
    bool in_use;
};

static struct text_cache_entry text_cache[TEXT_CACHE_SLOTS];
static int text_cache_next;

static Uint32 color_key(SDL_Color c)
{
    return ((Uint32)c.r << 24) | ((Uint32)c.g << 16)
        | ((Uint32)c.b << 8) | (Uint32)c.a;
}

static void text_cache_clear(void)
{
    int i;
    for (i = 0; i < TEXT_CACHE_SLOTS; i++) {
        if (text_cache[i].in_use) {
            SDL_DestroyTexture(text_cache[i].texture);
            text_cache[i].in_use = false;
        }
    }
    text_cache_next = 0;
}

static SDL_Texture* text_cache_lookup(const char *text, TTF_Font *f,
                                       SDL_Color fg, SDL_Color bg,
                                       int *out_w, int *out_h)
{
    int i;
    Uint32 fk = color_key(fg), bk = color_key(bg);

    for (i = 0; i < TEXT_CACHE_SLOTS; i++) {
        struct text_cache_entry *e = &text_cache[i];
        if (e->in_use && e->font == f && e->fg_key == fk
            && e->bg_key == bk && strcmp(e->text, text) == 0)
        {
            *out_w = e->w;
            *out_h = e->h;
            return e->texture;
        }
    }
    return NULL;
}

static void text_cache_store(const char *text, TTF_Font *f,
                              SDL_Color fg, SDL_Color bg,
                              SDL_Texture *tex, int w, int h)
{
    struct text_cache_entry *e = &text_cache[text_cache_next];

    if (e->in_use)
        SDL_DestroyTexture(e->texture);

    strncpy(e->text, text, sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = '\0';
    e->font = f;
    e->fg_key = color_key(fg);
    e->bg_key = color_key(bg);
    e->texture = tex;
    e->w = w;
    e->h = h;
    e->in_use = true;

    text_cache_next = (text_cache_next + 1) % TEXT_CACHE_SLOTS;
}

static int zoom(int d)
{
    return d * scale;
}

static void time_to_clock(char *buf, char *deci, int t)
{
    int minutes, seconds, frac;
    bool neg;

    if (t < 0) {
        t = abs(t);
        neg = true;
    } else
        neg = false;

    minutes = (t / 60 / 1000) % (60 * 60);
    seconds = (t / 1000) % 60;
    frac = t % 1000;

    if (neg)
        *buf++ = '-';

    sprintf(buf, "%02d:%02d.", minutes, seconds);
    sprintf(deci, "%03d", frac);
}

static void calculate_angle_lut(unsigned short *lut, int size)
{
    int r, c, nr, nc;
    float theta, rat;

    for (r = 0; r < size; r++) {
        nr = r - size / 2;

        for (c = 0; c < size; c++) {
            nc = c - size / 2;

            if (nr == 0)
                theta = M_PI_2;

            else if (nc == 0) {
                theta = 0;

                if (nr < 0)
                    theta = M_PI;

            } else {
                rat = (float)(nc) / -nr;
                theta = atanf(rat);

                if (rat < 0)
                    theta += M_PI;
            }

            if (nc <= 0)
                theta += M_PI;

            lut[r * size + c]
                = ((int)(theta * 1024 / (M_PI * 2)) + 1024) % 1024;
        }
    }
}

static int init_spinner(int size)
{
    spinner_angle = malloc(size * size * (sizeof *spinner_angle));
    if (spinner_angle == NULL) {
        perror("malloc");
        return -1;
    }

    calculate_angle_lut(spinner_angle, size);
    spinner_size = size;
    spinner_tex = NULL;
    return 0;
}

static void clear_spinner(void)
{
    if (spinner_tex) {
        SDL_DestroyTexture(spinner_tex);
        spinner_tex = NULL;
    }
    free(spinner_angle);
}

static TTF_Font* open_font(const char *name, int size) {
    int r, pt;
    char buf[256];
    const char **dir;
    struct stat st;
    TTF_Font *f;

    pt = zoom(size);

    dir = &font_dirs[0];

    while (*dir) {

        sprintf(buf, "%s/%s", *dir, name);

        r = stat(buf, &st);

        if (r != -1) {
            fprintf(stderr, "Loading font '%s', %dpt...\n", buf, pt);

            f = TTF_OpenFont(buf, pt);
            if (!f)
                fprintf(stderr, "Font error: %s\n", TTF_GetError());
            return f;
        }

        if (errno != ENOENT) {
            perror("stat");
            return NULL;
        }

        dir++;
        continue;
    }

    fprintf(stderr, "Font '%s' cannot be found in", name);

    dir = &font_dirs[0];
    while (*dir) {
        fputc(' ', stderr);
        fputs(*dir, stderr);
        dir++;
    }
    fputc('.', stderr);
    fputc('\n', stderr);

    return NULL;
}

static int load_fonts(void)
{
    clock_font = open_font(CLOCK_FONT, CLOCK_FONT_SIZE);
    if (!clock_font)
        return -1;

    deci_font = open_font(DECI_FONT, DECI_FONT_SIZE);
    if (!deci_font)
        return -1;

    font = open_font(FONT, FONT_SIZE);
    if (!font)
        return -1;

    em_font = open_font(EM_FONT, FONT_SIZE);
    if (!em_font)
        return -1;

    big_font = open_font(BIG_FONT, BIG_FONT_SIZE);
    if (!big_font)
        return -1;

    detail_font = open_font(DETAIL_FONT, DETAIL_FONT_SIZE);
    if (!detail_font)
        return -1;

    return 0;
}

static void clear_fonts(void)
{
    TTF_CloseFont(clock_font);
    TTF_CloseFont(deci_font);
    TTF_CloseFont(font);
    TTF_CloseFont(em_font);
    TTF_CloseFont(big_font);
    TTF_CloseFont(detail_font);
}

static int do_draw_text(const struct rect *r, const char *buf,
                        TTF_Font *f, SDL_Color fg, SDL_Color bg,
                        bool locale)
{
    SDL_Surface *rendered;
    SDL_Texture *tex;
    SDL_Rect src, dst, fill;
    int text_w = 0, text_h = 0;
    bool cached = false;

    if (buf != NULL && buf[0] != '\0') {

        tex = text_cache_lookup(buf, f, fg, bg, &text_w, &text_h);
        if (tex) {
            cached = true;
            text_w = MIN(r->w, text_w);
            text_h = MIN(r->h, text_h);
        } else {
            if (!locale) {
                rendered = TTF_RenderText_Shaded(f, buf, fg, bg);
            } else {
                char ubuf[256], *in, *out;
                size_t len, space;

                out = ubuf;
                space = sizeof(ubuf) - 1;

                if (iconv(utf, NULL, NULL, &out, &space) == -1)
                    abort();

                in = strdupa(buf);
                len = strlen(in);

                (void)iconv(utf, &in, &len, &out, &space);
                *out = '\0';

                rendered = TTF_RenderUTF8_Shaded(f, ubuf, fg, bg);
            }

            if (rendered) {
                tex = SDL_CreateTextureFromSurface(renderer, rendered);

                text_cache_store(buf, f, fg, bg, tex,
                                 rendered->w, rendered->h);

                text_w = MIN(r->w, rendered->w);
                text_h = MIN(r->h, rendered->h);

                SDL_FreeSurface(rendered);
            }
        }

        if (tex) {
            src.x = 0;
            src.y = 0;
            src.w = text_w;
            src.h = text_h;

            dst.x = r->x;
            dst.y = r->y;
            dst.w = text_w;
            dst.h = text_h;

            SDL_RenderCopy(renderer, tex, &src, &dst);
        }
    }

    if (text_w < r->w) {
        fill.x = r->x + text_w;
        fill.y = r->y;
        fill.w = r->w - text_w;
        fill.h = r->h;
        SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
        SDL_RenderFillRect(renderer, &fill);
    }

    if (text_h < r->h) {
        fill.x = r->x;
        fill.y = r->y + text_h;
        fill.w = text_w;
        fill.h = r->h - text_h;
        SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
        SDL_RenderFillRect(renderer, &fill);
    }

    return text_w;
}

static int draw_text(const struct rect *r, const char *buf,
                     TTF_Font *f, SDL_Color fg, SDL_Color bg)
{
    return do_draw_text(r, buf, f, fg, bg, false);
}

static int draw_text_in_locale(const struct rect *r, const char *buf,
                               TTF_Font *f, SDL_Color fg, SDL_Color bg)
{
    return do_draw_text(r, buf, f, fg, bg, true);
}

static void track_baseline(const struct rect *r, const TTF_Font *a,
                           struct rect *aligned, const TTF_Font *b)
{
    split(*r, pixels(from_top(TTF_FontAscent(a) - TTF_FontAscent(b), 0)),
          NULL, aligned);
}

static void draw_rect(const struct rect *r, SDL_Color col)
{
    SDL_Rect b;

    b.x = r->x;
    b.y = r->y;
    b.w = r->w;
    b.h = r->h;
    SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, col.a);
    SDL_RenderFillRect(renderer, &b);
}

static void draw_token(const struct rect *r, const char *buf,
                       SDL_Color tcol, SDL_Color col, SDL_Color bg_col)
{
    struct rect b;

    draw_rect(r, bg_col);
    b = shrink(*r, TOKEN_SPACE);
    draw_text(&b, buf, detail_font, tcol, col);
}

static SDL_Color dim(const SDL_Color x, int n)
{
    SDL_Color c;

    c.r = x.r >> n;
    c.g = x.g >> n;
    c.b = x.b >> n;
    c.a = x.a;

    return c;
}

static SDL_Color rgb(double r, double g, double b)
{
    SDL_Color c;

    c.r = r * 255;
    c.g = g * 255;
    c.b = b * 255;
    c.a = 255;

    return c;
}

static SDL_Color hsv(double h, double s, double v)
{
    int i;
    double f, p, q, t;

    if (s == 0.0)
        return rgb(v, v, v);

    h /= 60;
    i = floor(h);
    f = h - i;
    p = v * (1 - s);
    q = v * (1 - s * f);
    t = v * (1 - s * (1 - f));

    switch (i) {
    case 0:
        return rgb(v, t, p);
    case 1:
        return rgb(q, v, p);
    case 2:
        return rgb(p, v, t);
    case 3:
        return rgb(p, q, v);
    case 4:
        return rgb(t, p, v);
    case 5:
    case 6:
        return rgb(v, p, q);
    default:
        abort();
    }
}

static bool show_bpm(double bpm)
{
    return (bpm > 20.0 && bpm < 400.0);
}

static void draw_bpm(const struct rect *r, double bpm, SDL_Color bg_col)
{
    static const double min = 60.0, max = 240.0;
    char buf[32];
    double f, h;

    sprintf(buf, "%5.1f", bpm);

    if (bpm < min || bpm > max) {
        draw_token(r, buf, detail_col, bg_col, bg_col);
        return;
    }

    f = log2(bpm);
    f -= floor(f);
    h = f * 360.0;

    draw_token(r, buf, text_col, hsv(h, 0.8, 0.35), bg_col);
}

static void draw_bpm_field(const struct rect *r, double bpm,
                           SDL_Color bg_col)
{
    if (show_bpm(bpm))
        draw_bpm(r, bpm, bg_col);
    else
        draw_rect(r, bg_col);
}

static void draw_record(const struct rect *r,
                        const struct record *record)
{
    struct rect artist, title, left, right;

    split(*r, from_top(BIG_FONT_SPACE, 0), &artist, &title);
    draw_text_in_locale(&artist, record->artist,
                        big_font, text_col, background_col);

    if (show_bpm(record->bpm)) {
        split(title, from_left(BPM_WIDTH, 0), &left, &right);
        draw_bpm(&left, record->bpm, background_col);

        split(right, from_left(HALF_SPACER, 0), &left, &title);
        draw_rect(&left, background_col);
    }

    draw_text_in_locale(&title, record->title,
                        font, text_col, background_col);
}

static void draw_clock(const struct rect *r, int t, SDL_Color col)
{
    char hms[8], deci[8];
    short int v;
    struct rect sr;

    time_to_clock(hms, deci, t);

    v = draw_text(r, hms, clock_font, col, background_col);

    split(*r, pixels(from_left(v, 0)), NULL, &sr);
    track_baseline(&sr, clock_font, &sr, deci_font);

    draw_text(&sr, deci, deci_font, col, background_col);
}

static void draw_scope(const struct rect *r, struct timecoder *tc)
{
    int row, c, v, mid;
    Uint32 *pixels;
    int pitch;

    if (scope_tex == NULL || scope_tex_size != tc->mon_size) {
        if (scope_tex)
            SDL_DestroyTexture(scope_tex);
        scope_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, tc->mon_size, tc->mon_size);
        scope_tex_size = tc->mon_size;
        if (!scope_tex)
            return;
    }

    mid = tc->mon_size / 2;

    if (SDL_LockTexture(scope_tex, NULL, (void**)&pixels, &pitch) != 0)
        return;

    for (row = 0; row < tc->mon_size; row++) {
        Uint32 *rowpx = (Uint32*)((Uint8*)pixels + row * pitch);
        for (c = 0; c < tc->mon_size; c++) {
            v = tc->mon[row * tc->mon_size + c];

            if ((row == mid || c == mid) && v < 64)
                v = 64;

            rowpx[c] = 0xFF000000
                | ((Uint32)v << 16)
                | ((Uint32)v << 8)
                | (Uint32)v;
        }
    }

    SDL_UnlockTexture(scope_tex);

    SDL_Rect dst = { r->x, r->y, r->w, r->h };
    SDL_RenderCopy(renderer, scope_tex, NULL, &dst);
}

static void draw_spinner(const struct rect *r, struct player *pl)
{
    int row, c, rangle, pangle;
    double elapsed, remain, rps, angle_rad;
    Uint32 *pixels;
    int pitch;
    SDL_Color col;
    int cx, cy, lx, ly;

    if (!spinner_tex) {
        spinner_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, spinner_size, spinner_size);
        if (!spinner_tex)
            return;
    }

    elapsed = player_get_elapsed(pl);
    remain = player_get_remain(pl);

    rps = timecoder_revs_per_sec(pl->timecoder);
    rangle = (int)(player_get_position(pl) * 1024 * rps) % 1024;

    if (elapsed < 0 || remain < 0)
        col = alert_col;
    else
        col = ok_col;

    if (SDL_LockTexture(spinner_tex, NULL, (void**)&pixels, &pitch) != 0)
        return;

    for (row = 0; row < spinner_size; row++) {
        Uint32 *rowpx = (Uint32*)((Uint8*)pixels + row * pitch);
        for (c = 0; c < spinner_size; c++) {
            pangle = spinner_angle[row * spinner_size + c];

            if ((rangle - pangle + 1024) % 1024 < 512) {
                rowpx[c] = 0xFF000000
                    | ((Uint32)(col.r >> 2) << 16)
                    | ((Uint32)(col.g >> 2) << 8)
                    | (Uint32)(col.b >> 2);
            } else {
                rowpx[c] = 0xFF000000
                    | ((Uint32)col.r << 16)
                    | ((Uint32)col.g << 8)
                    | (Uint32)col.b;
            }
        }
    }

    SDL_UnlockTexture(spinner_tex);

    SDL_Rect dst = { r->x, r->y, spinner_size, spinner_size };
    SDL_RenderCopy(renderer, spinner_tex, NULL, &dst);

    angle_rad = (rangle / 1024.0) * 2.0 * M_PI;
    cx = r->x + spinner_size / 2;
    cy = r->y + spinner_size / 2;
    lx = cx + (int)(sin(angle_rad) * spinner_size * 0.45);
    ly = cy - (int)(cos(angle_rad) * spinner_size * 0.45);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
    SDL_RenderDrawLine(renderer, cx, cy, lx, ly);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

static void draw_deck_clocks(const struct rect *r,
                             struct player *pl, struct track *track)
{
    int elapse, remain;
    struct rect upper, lower;
    SDL_Color col;

    split(*r, from_top(CLOCK_FONT_SIZE, 0), &upper, &lower);

    elapse = player_get_elapsed(pl) * 1000;
    remain = player_get_remain(pl) * 1000;

    if (elapse < 0)
        col = alert_col;
    else if (remain > 0)
        col = ok_col;
    else
        col = text_col;

    draw_clock(&upper, elapse, col);

    if (remain <= 0)
        col = alert_col;
    else
        col = text_col;

    if (track_is_importing(track))
        col = dim(col, 2);

    draw_clock(&lower, -remain, col);
}

static void draw_overview(const struct rect *r,
                          struct track *tr, int position)
{
    int x, y, w, h, c, sp, ht, current_position, fade;
    SDL_Color col;

    x = r->x;
    y = r->y;
    w = r->w;
    h = r->h;

    if (tr->length)
        current_position = (long long)position * w / tr->length;
    else
        current_position = 0;

    for (c = 0; c < w; c++) {

        sp = (long long)tr->length * c / w;

        if (sp < tr->length)
            ht = track_get_overview(tr, sp) * h / 256;
        else
            ht = 0;

        if (!tr->length) {
            col = background_col;
            fade = 0;
        } else if (c >= current_position - 1 && c <= current_position + 1) {
            col = needle_col;
            fade = 1;
        } else if (position > tr->length - tr->rate * METER_WARNING_TIME) {
            col = alert_col;
            fade = 3;
        } else {
            col = elapsed_col;
            fade = 3;
        }

        if (track_is_importing(tr))
            col = dim(col, 1);

        if (c < current_position)
            col = dim(col, 1);

        if (h - ht > 0) {
            SDL_SetRenderDrawColor(renderer,
                col.r >> fade, col.g >> fade, col.b >> fade, 255);
            SDL_RenderDrawLine(renderer, x + c, y, x + c, y + h - ht - 1);
        }

        if (ht > 0) {
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_RenderDrawLine(renderer,
                x + c, y + h - ht, x + c, y + h - 1);
        }

        if (ht > 2) {
            int hr = MIN(255, col.r + 50);
            int hg = MIN(255, col.g + 50);
            int hb = MIN(255, col.b + 50);
            SDL_SetRenderDrawColor(renderer, hr, hg, hb, 255);
            SDL_RenderDrawPoint(renderer, x + c, y + h - ht);
        }
    }
}

static void draw_closeup(const struct rect *r,
                         struct track *tr, int position, int sc)
{
    int x, y, w, h, c;

    x = r->x;
    y = r->y;
    w = r->w;
    h = r->h;

    for (c = 0; c < w; c++) {
        int sp, ht, fade;
        SDL_Color col;

        sp = position - (position % (1 << sc))
            + ((c - w / 2) << sc);

        if (sp < tr->length && sp > 0)
            ht = track_get_ppm(tr, sp) * h / 256;
        else
            ht = 0;

        if (c == w / 2) {
            col = needle_col;
            fade = 1;
        } else {
            col = elapsed_col;
            fade = 3;
        }

        if (h - ht > 0) {
            SDL_SetRenderDrawColor(renderer,
                col.r >> fade, col.g >> fade, col.b >> fade, 255);
            SDL_RenderDrawLine(renderer, x + c, y, x + c, y + h - ht - 1);
        }

        if (ht > 0) {
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_RenderDrawLine(renderer,
                x + c, y + h - ht, x + c, y + h - 1);
        }

        if (ht > 2) {
            int hr = MIN(255, col.r + 60);
            int hg = MIN(255, col.g + 60);
            int hb = MIN(255, col.b + 60);
            SDL_SetRenderDrawColor(renderer, hr, hg, hb, 255);
            SDL_RenderDrawPoint(renderer, x + c, y + h - ht);
            if (ht > 4)
                SDL_RenderDrawPoint(renderer, x + c, y + h - ht + 1);
        }
    }
}

static void draw_meters(const struct rect *r,
                        struct track *tr, int position, int sc)
{
    struct rect overview, closeup;

    split(*r, from_top(OVERVIEW_HEIGHT, SPACER), &overview, &closeup);

    if (closeup.h > OVERVIEW_HEIGHT)
        draw_overview(&overview, tr, position);
    else
        closeup = *r;

    draw_closeup(&closeup, tr, position, sc);
}

static void draw_deck_top(const struct rect *r,
                          struct player *pl, struct track *track)
{
    struct rect clocks, left, right, spinner, scope;

    split(*r, from_left(CLOCKS_WIDTH, SPACER), &clocks, &right);

    if (!pl->timecode_control || right.w < 0) {
        draw_deck_clocks(r, pl, track);
        return;
    }

    draw_deck_clocks(&clocks, pl, track);

    split(right, from_right(SPINNER_SIZE, SPACER), &left, &spinner);
    if (left.w < 0)
        return;
    split(spinner, from_bottom(SPINNER_SIZE, 0), NULL, &spinner);
    draw_spinner(&spinner, pl);

    split(left, from_right(SCOPE_SIZE, SPACER), &clocks, &scope);
    if (clocks.w < 0)
        return;
    split(scope, from_bottom(SCOPE_SIZE, 0), NULL, &scope);
    draw_scope(&scope, pl->timecoder);
}

static void draw_deck_status(const struct rect *r,
                             const struct deck *dk)
{
    char buf[128], *c;
    int tc;
    const struct player *pl = &dk->player;

    c = buf;

    c += sprintf(c, "%s: ", pl->timecoder->def->name);

    tc = timecoder_get_position(pl->timecoder, NULL);
    if (pl->timecode_control && tc != -1) {
        c += sprintf(c, "%7d ", tc);
    } else {
        c += sprintf(c, "        ");
    }

    sprintf(c, "pitch:%+0.2f (sync %0.2f %+.5fs = %+0.2f)  %s%s",
            pl->pitch,
            pl->sync_pitch,
            pl->last_difference,
            pl->pitch * pl->sync_pitch,
            pl->recalibrate ? "RCAL  " : "",
            deck_is_locked(dk) ? "LOCK  " : "");

    draw_text(r, buf, detail_font, detail_col, background_col);
}

static void draw_panel_separator(int x, int y, int w)
{
    SDL_SetRenderDrawColor(renderer,
        panel_border_col.r, panel_border_col.g,
        panel_border_col.b, panel_border_col.a);
    SDL_RenderDrawLine(renderer, x, y, x + w - 1, y);
}

static void draw_deck(const struct rect *r,
                      struct deck *dk, int mscale)
{
    int position;
    struct rect trk, top, meters, status, rest, lower;
    struct player *pl;
    struct track *t;

    pl = &dk->player;
    t = pl->track;

    position = player_get_elapsed(pl) * t->rate;

    split(*r, from_top(FONT_SPACE + BIG_FONT_SPACE, 0), &trk, &rest);
    if (rest.h < 160)
        rest = *r;
    else {
        draw_record(&trk, dk->record);
        draw_panel_separator(rest.x, rest.y - 1, rest.w);
    }

    split(rest, from_top(CLOCK_FONT_SIZE * 2, SPACER), &top, &lower);
    if (lower.h < 64)
        lower = rest;
    else {
        draw_deck_top(&top, pl, t);
        draw_panel_separator(lower.x, lower.y - 1, lower.w);
    }

    split(lower, from_bottom(FONT_SPACE, SPACER), &meters, &status);
    if (meters.h < 64)
        meters = lower;
    else
        draw_deck_status(&status, dk);

    draw_meters(&meters, t, position, mscale);
}

static void draw_decks(const struct rect *r,
                       struct deck dk[], size_t ndecks, int mscale)
{
    int d;
    struct rect left, right;

    right = *r;

    for (d = 0; d < ndecks; d++) {
        split(right, columns(d, ndecks, BORDER), &left, &right);
        draw_deck(&left, &dk[d], mscale);
    }
}

static void draw_status(const struct rect *r)
{
    SDL_Color fg, bg, indicator_col;
    struct rect indicator, text_area;
    SDL_Rect dot;

    switch (status_level()) {
    case STATUS_ALERT:
    case STATUS_WARN:
        fg = text_col;
        bg = dim(alert_col, 2);
        indicator_col = alert_col;
        break;
    default:
        fg = detail_col;
        bg = background_col;
        indicator_col = ok_col;
    }

    split(*r, from_left(4, HALF_SPACER), &indicator, &text_area);

    dot.x = indicator.x;
    dot.y = indicator.y + indicator.h / 2 - 2;
    dot.w = 4;
    dot.h = 4;
    SDL_SetRenderDrawColor(renderer,
        indicator_col.r, indicator_col.g, indicator_col.b, 255);
    SDL_RenderFillRect(renderer, &dot);

    draw_text_in_locale(&text_area, status(), detail_font, fg, bg);
}

static void draw_search(const struct rect *r, struct selector *sel)
{
    int s;
    const char *buf;
    char cm[32];
    SDL_Rect cur, border;
    struct rect rtext;

    split(*r, from_left(SCROLLBAR_SIZE, SPACER), NULL, &rtext);

    border.x = rtext.x - 1;
    border.y = rtext.y - 1;
    border.w = rtext.w + 2;
    border.h = rtext.h + 2;
    SDL_SetRenderDrawColor(renderer,
        panel_border_col.r, panel_border_col.g,
        panel_border_col.b, panel_border_col.a);
    SDL_RenderDrawRect(renderer, &border);

    if (sel->search[0] != '\0')
        buf = sel->search;
    else
        buf = NULL;

    s = draw_text(&rtext, buf, font, text_col, background_col);

    cur.x = rtext.x + s;
    cur.y = rtext.y;
    cur.w = CURSOR_WIDTH * r->scale;
    cur.h = rtext.h;

    SDL_SetRenderDrawColor(renderer,
        cursor_col.r, cursor_col.g, cursor_col.b, 255);
    SDL_RenderFillRect(renderer, &cur);

    if (sel->view_index->entries > 1)
        sprintf(cm, "%zd matches", sel->view_index->entries);
    else if (sel->view_index->entries > 0)
        sprintf(cm, "1 match");
    else
        sprintf(cm, "no matches");

    rtext.x += s + CURSOR_WIDTH + SPACER;
    rtext.w -= s + CURSOR_WIDTH + SPACER;

    draw_text(&rtext, cm, em_font, detail_col, background_col);
}

static void draw_scroll_bar(const struct rect *r,
                            const struct listbox *scroll)
{
    SDL_Rect box;
    SDL_Color bg;

    bg = dim(selected_col, 1);

    box.x = r->x;
    box.y = r->y;
    box.w = r->w;
    box.h = r->h;
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(renderer, &box);

    if (scroll->entries > 0) {
        box.x = r->x;
        box.y = r->y + r->h * scroll->offset / scroll->entries;
        box.w = r->w;
        box.h = r->h * MIN(scroll->lines, scroll->entries)
            / scroll->entries;
        SDL_SetRenderDrawColor(renderer,
            accent_col.r, accent_col.g, accent_col.b, 200);
        SDL_RenderFillRect(renderer, &box);
    }
}

typedef void (*draw_row_t)(const void *context, const struct rect rc,
                           unsigned int entry, bool selected);

static void draw_listbox(const struct listbox *lb, const struct rect rc,
                         const void *context, draw_row_t draw)
{
    struct rect left, remain;
    unsigned int row;

    split(rc, from_left(SCROLLBAR_SIZE, SPACER), &left, &remain);
    draw_scroll_bar(&left, lb);

    for (row = 0;; row++) {
        int entry;
        bool sel;
        struct rect line;

        entry = listbox_map(lb, row);
        if (entry == -1)
            break;

        if (entry == listbox_current(lb))
            sel = true;
        else
            sel = false;

        split(remain, from_top(FONT_SPACE, 0), &line, &remain);
        draw(context, line, entry, sel);
    }

    draw_rect(&remain, background_col);
}

static void draw_crate_row(const void *context, const struct rect rc,
                           unsigned int entry, bool selected)
{
    const struct selector *sel = context;
    const struct crate *crate;
    struct rect left, right, content, accent_bar;
    SDL_Color col;

    crate = sel->library->crate[entry];

    if (crate->is_fixed)
        col = detail_col;
    else
        col = text_col;

    if (!selected) {
        draw_text_in_locale(&rc, crate->name,
                            font, col, background_col);
        return;
    }

    accent_bar = rc;
    accent_bar.w = 3;
    draw_rect(&accent_bar, accent_col);

    content = rc;
    content.x += 5;
    content.w -= 5;

    split(content, from_right(SORT_WIDTH, 0), &left, &right);

    switch (sel->sort) {
    case SORT_ARTIST:
        draw_token(&right, "ART", text_col, artist_col, selected_col);
        break;

    case SORT_BPM:
        draw_token(&right, "BPM", text_col, bpm_col, selected_col);
        break;

    case SORT_PLAYLIST:
        draw_token(&right, "PLS", text_col, selected_col, selected_col);
        break;

    default:
        abort();
    }

    if (crate->is_busy) {
        split(left, from_right(25, 0), &left, &right);
        draw_token(&right, "BUSY", text_col,
                   dim(alert_col, 2), selected_col);
    }

    draw_text_in_locale(&left, crate->name, font, col, selected_col);
}

static void draw_crates(const struct rect rc,
                        const struct selector *x)
{
    draw_listbox(&x->crates, rc, x, draw_crate_row);
}

static void draw_record_row(const void *context, const struct rect rc,
                            unsigned int entry, bool selected)
{
    int w;
    struct record *record;
    const struct index *index = context;
    struct rect left, right;
    SDL_Color col;

    if (selected)
        col = selected_col;
    else
        col = background_col;

    if (selected) {
        struct rect accent_bar = rc;
        accent_bar.w = 3;
        draw_rect(&accent_bar, accent_col);
    }

    w = rc.w / 2;
    if (w > RESULTS_ARTIST_WIDTH)
        w = RESULTS_ARTIST_WIDTH;

    record = index->record[entry];

    split(rc, from_left(BPM_WIDTH, 0), &left, &right);
    draw_bpm_field(&left, record->bpm, col);

    split(right, from_left(SPACER, 0), &left, &right);
    draw_rect(&left, col);

    split(right, from_left(w, 0), &left, &right);
    draw_text_in_locale(&left, record->artist, font, text_col, col);

    split(right, from_left(SPACER, 0), &left, &right);
    draw_rect(&left, col);
    draw_text_in_locale(&right, record->title, font, text_col, col);
}

static void draw_index(const struct rect rc,
                       const struct selector *x)
{
    draw_listbox(&x->records, rc, x->view_index, draw_record_row);
}

static void draw_library(const struct rect *r, struct selector *sel)
{
    struct rect rsearch, rlists, rcrates, rrecords;
    unsigned int rows;

    split(*r, from_top(SEARCH_HEIGHT, SPACER), &rsearch, &rlists);

    rows = count_rows(rlists, FONT_SPACE);
    if (rows == 0) {

        draw_search(r, sel);
        selector_set_lines(sel, 1);

        return;
    }

    draw_search(&rsearch, sel);
    selector_set_lines(sel, rows);

    split(rlists, columns(0, 4, SPACER), &rcrates, &rrecords);
    if (rcrates.w > LIBRARY_MIN_WIDTH) {
        draw_index(rrecords, sel);
        draw_crates(rcrates, sel);
    } else {
        draw_index(*r, sel);
    }
}

static bool handle_key(SDL_Keycode key, int mod)
{
    struct selector *sel = &selector;

    if (key >= SDLK_a && key <= SDLK_z) {
        selector_search_refine(sel, (key - SDLK_a) + 'a');
        return true;

    } else if (key >= SDLK_0 && key <= SDLK_9) {
        selector_search_refine(sel, (key - SDLK_0) + '0');
        return true;

    } else if (key == SDLK_SPACE) {
        selector_search_refine(sel, ' ');
        return true;

    } else if (key == SDLK_BACKSPACE) {
        selector_search_expand(sel);
        return true;

    } else if (key == SDLK_PERIOD) {
        selector_search_refine(sel, '.');
        return true;

    } else if (key == SDLK_HOME) {
        selector_top(sel);
        return true;

    } else if (key == SDLK_END) {
        selector_bottom(sel);
        return true;

    } else if (key == SDLK_UP) {
        selector_up(sel);
        return true;

    } else if (key == SDLK_DOWN) {
        selector_down(sel);
        return true;

    } else if (key == SDLK_PAGEUP) {
        selector_page_up(sel);
        return true;

    } else if (key == SDLK_PAGEDOWN) {
        selector_page_down(sel);
        return true;

    } else if (key == SDLK_LEFT) {
        selector_prev(sel);
        return true;

    } else if (key == SDLK_RIGHT) {
        selector_next(sel);
        return true;

    } else if (key == SDLK_TAB) {
        if (mod & KMOD_CTRL) {
            if (mod & KMOD_SHIFT)
                selector_rescan(sel);
            else
                selector_toggle_order(sel);
        } else {
            selector_toggle(sel);
        }
        return true;

    } else if ((key == SDLK_EQUALS) || (key == SDLK_PLUS)) {
        meter_scale--;

        if (meter_scale < 0)
            meter_scale = 0;

        fprintf(stderr, "Meter scale decreased to %d\n", meter_scale);

    } else if (key == SDLK_MINUS) {
        meter_scale++;

        if (meter_scale > MAX_METER_SCALE)
            meter_scale = MAX_METER_SCALE;

        fprintf(stderr, "Meter scale increased to %d\n", meter_scale);

    } else if (key >= SDLK_F1 && key <= SDLK_F12) {
        size_t d;

        d = (key - SDLK_F1) / 4;

        if (d < ndeck) {
            int func;
            struct deck *de;
            struct player *pl;
            struct record *re;
            struct timecoder *tc;

            func = (key - SDLK_F1) % 4;

            de = &deck[d];
            pl = &de->player;
            tc = &de->timecoder;

            if (mod & KMOD_SHIFT && !(mod & KMOD_CTRL)) {
                if (func < ndeck)
                    deck_clone(de, &deck[func]);

            } else switch(func) {
            case FUNC_LOAD:
                re = selector_current(sel);
                if (re != NULL)
                    deck_load(de, re);
                break;

            case FUNC_RECUE:
                deck_recue(de);
                break;

            case FUNC_TIMECODE:
                if (mod & KMOD_CTRL) {
                    if (mod & KMOD_SHIFT)
                        player_set_internal_playback(pl);
                    else
                        timecoder_cycle_definition(tc);
                } else {
                    (void)player_toggle_timecode_control(pl);
                }
                break;
            }
        }
    }

    return false;
}

static int set_size(int w, int h, struct rect *r)
{
    if (window == NULL) {
        window = SDL_CreateWindow(banner, win_x, win_y, w, h, window_flags);
        if (window == NULL) {
            fprintf(stderr, "%s\n", SDL_GetError());
            return -1;
        }

        renderer = SDL_CreateRenderer(window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer == NULL) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            if (renderer == NULL) {
                fprintf(stderr, "%s\n", SDL_GetError());
                return -1;
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    *r = shrink(rect(0, 0, w, h, scale), BORDER);

    fprintf(stderr, "New interface size is %dx%d.\n", w, h);

    return 0;
}

static void push_event(int t)
{
    SDL_Event e;

    if (!SDL_PeepEvents(&e, 1, SDL_PEEKEVENT, t, t)) {
        e.type = t;
        if (SDL_PushEvent(&e) == -1)
            abort();
    }
}

static Uint32 ticker(Uint32 interval, void *p)
{
    push_event(EVENT_TICKER);
    return interval;
}

static void defer_status_redraw(struct observer *o, void *x)
{
    push_event(EVENT_STATUS);
}

static void defer_selector_redraw(struct observer *o, void *x)
{
    push_event(EVENT_SELECTOR);
}

static int interface_main(void)
{
    bool library_update, decks_update, status_update;
    bool draw_status_area, draw_library_area, draw_decks_area;

    SDL_Event event;
    SDL_TimerID timer;

    struct rect rworkspace, rplayers, rlibrary, rstatus, rtmp;

    if (set_size(width, height, &rworkspace) == -1)
        return -1;

    decks_update = true;
    status_update = true;
    library_update = true;

    timer = SDL_AddTimer(REFRESH, ticker, NULL);

    rig_lock();

    for (;;) {

        rig_unlock();

        if (SDL_WaitEvent(&event) < 0)
            break;

        rig_lock();

        switch(event.type) {
        case SDL_QUIT:
            if (rig_quit() == -1)
                return -1;
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            {
                width = event.window.data1;
                height = event.window.data2;
                rworkspace = shrink(rect(0, 0, width, height, scale), BORDER);

                library_update = true;
                decks_update = true;
                status_update = true;
            }
            break;

        case EVENT_TICKER:
            decks_update = true;
            break;

        case EVENT_QUIT:
            goto finish;

        case EVENT_STATUS:
            status_update = true;
            break;

        case EVENT_SELECTOR:
            library_update = true;
            break;

        case SDL_KEYDOWN:
            if (handle_key(event.key.keysym.sym, event.key.keysym.mod))
            {
                struct record *r;

                r = selector_current(&selector);
                if (r != NULL) {
                    status_set(STATUS_VERBOSE, r->pathname);
                } else {
                    status_set(STATUS_VERBOSE, "No search results found");
                }
            }

        } /* switch(event.type) */

        draw_status_area = true;
        draw_library_area = true;
        draw_decks_area = true;

        split(rworkspace, from_bottom(STATUS_HEIGHT, SPACER),
              &rtmp, &rstatus);
        if (rtmp.h < 128 || rtmp.w < 0) {
            rtmp = rworkspace;
            draw_status_area = false;
        }

        split(rtmp, from_top(PLAYER_HEIGHT, SPACER), &rplayers, &rlibrary);
        if (rlibrary.h < LIBRARY_MIN_HEIGHT
            || rlibrary.w < LIBRARY_MIN_WIDTH)
        {
            rplayers = rtmp;
            draw_library_area = false;
        }

        if (rplayers.h < 0 || rplayers.w < 0)
            draw_decks_area = false;

        if (!library_update && !decks_update && !status_update)
            continue;

        SDL_SetRenderDrawColor(renderer,
            background_col.r, background_col.g,
            background_col.b, background_col.a);
        SDL_RenderClear(renderer);

        if (draw_decks_area)
            draw_decks(&rplayers, deck, ndeck, meter_scale);

        if (draw_library_area)
            draw_library(&rlibrary, &selector);

        if (draw_status_area)
            draw_status(&rstatus);

        SDL_RenderPresent(renderer);

        library_update = false;
        status_update = false;
        decks_update = false;

    } /* main loop */

 finish:
    rig_unlock();

    SDL_RemoveTimer(timer);

    return 0;
}

static void* launch(void *p)
{
    interface_main();
    return NULL;
}

static int parse_geometry(const char *s)
{
    int n, x, y, len;
    char buf[128];

    n = sscanf(s, "%[0-9]x%d%n", buf, &height, &len);
    switch (n) {
    case EOF:
        return 0;
    case 0:
        break;
    case 2:
        width = atoi(buf);
        s += len;
        break;
    default:
        return -1;
    }

    n = sscanf(s, "+%d+%d%n", &x, &y, &len);
    switch (n) {
    case EOF:
        return 0;
    case 0:
        break;
    case 2:
        win_x = x;
        win_y = y;
        s += len;
        break;
    default:
        return -1;
    }

    n = sscanf(s, "/%f%n", &scale, &len);
    switch (n) {
    case EOF:
        return 0;
    case 0:
        break;
    case 1:
        if (scale <= 0.0)
            return -1;
        s += len;
        break;
    default:
        return -1;
    }

    if (*s != '\0')
        return -1;

    return 0;
}

int interface_start(struct library *lib, const char *geo, bool decor)
{
    size_t n;

    if (parse_geometry(geo) == -1) {
        fprintf(stderr, "Window geometry ('%s') is not valid.\n", geo);
        return -1;
    }

    if (!decor)
        window_flags |= SDL_WINDOW_BORDERLESS;

    for (n = 0; n < ndeck; n++) {
        if (timecoder_monitor_init(&deck[n].timecoder, zoom(SCOPE_SIZE)) == -1)
            return -1;
    }

    if (init_spinner(zoom(SPINNER_SIZE)) == -1)
        return -1;

    selector_init(&selector, lib);
    watch(&on_status, &status_changed, defer_status_redraw);
    watch(&on_selector, &selector.changed, defer_selector_redraw);
    status_set(STATUS_VERBOSE, banner);

    fprintf(stderr, "Initialising SDL...\n");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == -1) {
        fprintf(stderr, "%s\n", SDL_GetError());
        return -1;
    }

    if (TTF_Init() == -1) {
        fprintf(stderr, "%s\n", TTF_GetError());
        return -1;
    }

    if (load_fonts() == -1)
        return -1;

    utf = iconv_open("UTF8", "");
    if (utf == (iconv_t)-1) {
        perror("iconv_open");
        return -1;
    }

    fprintf(stderr, "Launching interface thread...\n");

    if (pthread_create(&ph, NULL, launch, NULL)) {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void interface_stop(void)
{
    size_t n;

    push_event(EVENT_QUIT);

    if (pthread_join(ph, NULL) != 0)
        abort();

    for (n = 0; n < ndeck; n++)
        timecoder_monitor_clear(&deck[n].timecoder);

    clear_spinner();
    text_cache_clear();

    if (scope_tex) {
        SDL_DestroyTexture(scope_tex);
        scope_tex = NULL;
    }

    ignore(&on_status);
    ignore(&on_selector);
    selector_clear(&selector);
    clear_fonts();

    if (iconv_close(utf) == -1)
        abort();

    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);

    TTF_Quit();
    SDL_Quit();
}
