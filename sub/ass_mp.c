/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>

#include <ass/ass.h>
#include <ass/ass_types.h>

#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "options/path.h"
#include "ass_mp.h"
#include "osd.h"
#include "stream/stream.h"
#include "options/options.h"

// res_y should be track->PlayResY
// It determines scaling of font sizes and more.
void mp_ass_set_style(ASS_Style *style, double res_y,
                      const struct osd_style_opts *opts)
{
    if (!style)
        return;

    if (opts->font) {
        if (!style->FontName || strcmp(style->FontName, opts->font) != 0) {
            free(style->FontName);
            style->FontName = strdup(opts->font);
        }
    }

    // libass_font_size = FontSize * (window_height / res_y)
    // scale translates parameters from PlayResY=720 to res_y
    double scale = res_y / 720.0;

    style->FontSize = opts->font_size * scale;
    style->PrimaryColour = MP_ASS_COLOR(opts->color);
    style->SecondaryColour = style->PrimaryColour;
    style->OutlineColour = MP_ASS_COLOR(opts->border_color);
    if (opts->back_color.a) {
        style->BackColour = MP_ASS_COLOR(opts->back_color);
        style->BorderStyle = 4; // opaque box
    } else {
        style->BackColour = MP_ASS_COLOR(opts->shadow_color);
        style->BorderStyle = 1; // outline
    }
    style->Outline = opts->border_size * scale;
    style->Shadow = opts->shadow_offset * scale;
    style->Spacing = opts->spacing * scale;
    style->MarginL = opts->margin_x * scale;
    style->MarginR = style->MarginL;
    style->MarginV = opts->margin_y * scale;
    style->ScaleX = 1.;
    style->ScaleY = 1.;
    style->Alignment = 1 + (opts->align_x + 1) + (opts->align_y + 2) % 3 * 4;
    style->Blur = opts->blur;
    style->Bold = opts->bold;
    style->Italic = opts->italic;
}

void mp_ass_configure_fonts(ASS_Renderer *priv, struct osd_style_opts *opts,
                            struct mpv_global *global, struct mp_log *log)
{
    void *tmp = talloc_new(NULL);
    char *default_font = mp_find_config_file(tmp, global, "subfont.ttf");
    char *config       = mp_find_config_file(tmp, global, "fonts.conf");

    if (default_font && !mp_path_exists(default_font))
        default_font = NULL;

    mp_verbose(log, "Setting up fonts...\n");
    ass_set_fonts(priv, default_font, opts->font, 1, config, 1);
    mp_verbose(log, "Done.\n");

    talloc_free(tmp);
}

void mp_ass_render_frame(ASS_Renderer *renderer, ASS_Track *track, double time,
                         struct sub_bitmaps *res)
{
    int changed;
    ASS_Image *imgs = ass_render_frame(renderer, track, time, &changed);
    if (changed)
        res->change_id++;
    assert(res->format == 0 || res->format == SUBBITMAP_LIBASS);
    res->format = SUBBITMAP_LIBASS;

    int num_parts_alloc = MP_TALLOC_AVAIL(res->parts);
    for (struct ass_image *img = imgs; img; img = img->next) {
        if (img->w == 0 || img->h == 0)
            continue;
        if (res->num_parts >= num_parts_alloc) {
            num_parts_alloc = MPMAX(num_parts_alloc * 2, 32);
            res->parts = talloc_realloc(NULL, res->parts, struct sub_bitmap,
                                        num_parts_alloc);
        }
        struct sub_bitmap *p = &res->parts[res->num_parts];
        p->bitmap = img->bitmap;
        p->stride = img->stride;
        p->libass.color = img->color;
        p->dw = p->w = img->w;
        p->dh = p->h = img->h;
        p->x = img->dst_x;
        p->y = img->dst_y;
        res->num_parts++;
    }
}

static const int map_ass_level[] = {
    MSGL_ERR,           // 0 "FATAL errors"
    MSGL_WARN,
    MSGL_INFO,
    MSGL_V,
    MSGL_V,
    MSGL_V,             // 5 application recommended level
    MSGL_DEBUG,
    MSGL_TRACE,         // 7 "verbose DEBUG"
};

static void message_callback(int level, const char *format, va_list va, void *ctx)
{
    struct mp_log *log = ctx;
    if (!log)
        return;
    level = map_ass_level[level];
    mp_msg_va(log, level, format, va);
    // libass messages lack trailing \n
    mp_msg(log, level, "\n");
}

ASS_Library *mp_ass_init(struct mpv_global *global, struct mp_log *log)
{
    char *path = mp_find_config_file(NULL, global, "fonts");
    ASS_Library *priv = ass_library_init();
    if (!priv)
        abort();
    ass_set_message_cb(priv, message_callback, log);
    if (path)
        ass_set_fonts_dir(priv, path);
    ass_set_extract_fonts(priv, global->opts->use_embedded_fonts);
    talloc_free(path);
    return priv;
}

void mp_ass_flush_old_events(ASS_Track *track, long long ts)
{
    int n = 0;
    for (; n < track->n_events; n++) {
        if ((track->events[n].Start + track->events[n].Duration) >= ts)
            break;
        ass_free_event(track, n);
        track->n_events--;
    }
    for (int i = 0; n > 0 && i < track->n_events; i++) {
        track->events[i] = track->events[i+n];
    }
}
