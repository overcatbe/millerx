/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Settings (~/.config/millerx/millerx.conf), sort order and the column dividers. */
#include "millerx.h"
#include <string.h>

Look LOOK = { NULL, 2, FALSE, PREVIEW_DEFAULT, TRUE, FALSE, .column_width = COL_DEFAULT,
              .use_ffmpeg = TRUE, .use_pdftoppm = TRUE, .use_pkexec = TRUE, .use_blender = TRUE,
              .thumb_size = TSIZE_MEDIUM,
              .thumbs = THUMBS_LOCAL };

static char *config_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "millerx", "millerx.conf", NULL);
}

static char *get_str(GKeyFile *kf, const char *sec, const char *key)
{
    char *v = g_key_file_get_string(kf, sec, key, NULL);
    if (v) {
        /* a comment after the value ("true   # once a day"), as in the README; "#rrggbb" stays */
        for (char *c = v + 1; *c; c++)
            if (*c == '#' && (c[-1] == ' ' || c[-1] == '\t')) {
                *c = '\0';
                break;
            }
        g_strstrip(v);
    }
    return v;
}

static int get_int(GKeyFile *kf, const char *sec, const char *key, int def, int lo, int hi)
{
    char *v = get_str(kf, sec, key), *end = NULL;
    int r = def;
    if (v && *v) {
        long n = strtol(v, &end, 10);
        if (end && *end == '\0')
            r = (int) CLAMP(n, lo, hi);
    }
    g_free(v);
    return r;
}

static gboolean get_bool(GKeyFile *kf, const char *sec, const char *key, gboolean def)
{
    char *v = get_str(kf, sec, key);
    gboolean r = def;
    if (v) {
        char *l = g_ascii_strdown(v, -1);
        r = !strcmp(l, "true") || !strcmp(l, "1") || !strcmp(l, "yes") || !strcmp(l, "on");
        g_free(l);
    }
    g_free(v);
    return r;
}

/* every value checked, defaults for anything odd */
void look_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_file();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);   /* missing file = defaults */

    char *col = get_str(kf, "look", "divider_color");
    GdkRGBA rgba;
    g_free(LOOK.divider_color);
    if (col && g_ascii_strcasecmp(col, "auto") && gdk_rgba_parse(&rgba, col))
        LOOK.divider_color = col;
    else {
        LOOK.divider_color = g_strdup("auto");
        g_free(col);
    }
    LOOK.divider_width = get_int(kf, "look", "divider_width", 2, 1, 12);
    char *style = get_str(kf, "look", "divider_style");
    LOOK.fade = style && !strcmp(style, "fade");
    g_free(style);
    LOOK.preview_width = get_int(kf, "look", "preview_width", PREVIEW_DEFAULT, 200, 900);
    LOOK.column_width = get_int(kf, "look", "column_width", COL_DEFAULT, COL_MIN, 600);
    LOOK.use_ffmpeg = get_bool(kf, "helpers", "ffmpeg", TRUE);
    LOOK.use_pdftoppm = get_bool(kf, "helpers", "pdftoppm", TRUE);
    LOOK.use_pkexec = get_bool(kf, "helpers", "pkexec", TRUE);
    LOOK.use_blender = get_bool(kf, "helpers", "blender", TRUE);
    char *th = get_str(kf, "thumbnails", "show");
    LOOK.thumbs = !g_strcmp0(th, "always") ? THUMBS_ALWAYS : !g_strcmp0(th, "never") ? THUMBS_NEVER : THUMBS_LOCAL;
    g_free(th);
    char *ts = get_str(kf, "thumbnails", "size");
    LOOK.thumb_size = !g_strcmp0(ts, "small") ? TSIZE_SMALL : !g_strcmp0(ts, "large") ? TSIZE_LARGE : TSIZE_MEDIUM;
    g_free(ts);
    LOOK.gallery_names = get_bool(kf, "look", "gallery_names", TRUE);
    g_free(LOOK.language);
    LOOK.language = get_str(kf, "look", "language");
    if (!LOOK.language || (strcmp(LOOK.language, "en") && strcmp(LOOK.language, "wizard")
                              && strcmp(LOOK.language, "wienerisch"))) {
        g_free(LOOK.language);
        LOOK.language = g_strdup("system");
    }
    char *hl = get_str(kf, "look", "highlight_color");
    GdkRGBA hc;
    g_free(LOOK.highlight);
    if (hl && g_ascii_strcasecmp(hl, "auto") && gdk_rgba_parse(&hc, hl)) {
        LOOK.highlight = hl;
    } else {
        LOOK.highlight = g_strdup("auto");
        g_free(hl);
    }
    LOOK.folders_first = get_bool(kf, "browse", "folders_first", TRUE);
    LOOK.show_hidden = get_bool(kf, "browse", "show_hidden", FALSE);
    LOOK.cache_max_mb = get_int(kf, "thumbnails", "max_mb", 1024, 50, 100000);
    LOOK.cache_days = get_int(kf, "thumbnails", "max_days", 180, 7, 3650);
    LOOK.cache_auto = get_bool(kf, "thumbnails", "auto_clean", TRUE);
    g_free(path);
    g_key_file_unref(kf);
}

/* write the settings back, keeping the comments already in the file */
gboolean look_save(GError **err)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_file();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_string(kf, "look", "divider_color", LOOK.divider_color);
    g_key_file_set_integer(kf, "look", "divider_width", LOOK.divider_width);
    g_key_file_set_string(kf, "look", "divider_style", LOOK.fade ? "fade" : "solid");
    g_key_file_set_integer(kf, "look", "preview_width", LOOK.preview_width);
    g_key_file_set_integer(kf, "look", "column_width", LOOK.column_width);
    g_key_file_set_boolean(kf, "look", "gallery_names", LOOK.gallery_names);
    g_key_file_set_string(kf, "look", "language", LOOK.language ? LOOK.language : "system");
    g_key_file_set_string(kf, "look", "highlight_color", LOOK.highlight);
    g_key_file_set_boolean(kf, "browse", "folders_first", LOOK.folders_first);
    g_key_file_set_boolean(kf, "browse", "show_hidden", LOOK.show_hidden);
    g_key_file_set_boolean(kf, "helpers", "ffmpeg", LOOK.use_ffmpeg);
    g_key_file_set_boolean(kf, "helpers", "pdftoppm", LOOK.use_pdftoppm);
    g_key_file_set_boolean(kf, "helpers", "pkexec", LOOK.use_pkexec);
    g_key_file_set_boolean(kf, "helpers", "blender", LOOK.use_blender);
    g_key_file_set_string(kf, "thumbnails", "show",
                          LOOK.thumbs == THUMBS_LOCAL ? "local" : LOOK.thumbs == THUMBS_NEVER ? "never" : "always");
    g_key_file_set_string(kf, "thumbnails", "size",
                          LOOK.thumb_size == TSIZE_SMALL ? "small" : LOOK.thumb_size == TSIZE_LARGE ? "large" : "medium");
    g_key_file_set_integer(kf, "thumbnails", "max_mb", LOOK.cache_max_mb);
    g_key_file_set_integer(kf, "thumbnails", "max_days", LOOK.cache_days);
    g_key_file_set_boolean(kf, "thumbnails", "auto_clean", LOOK.cache_auto);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    gboolean ok = g_key_file_save_to_file(kf, path, err);
    g_free(dir);
    g_free(path);
    g_key_file_unref(kf);
    return ok;
}

/* main() asks before anything else runs: the language has to be chosen before GTK starts */
char *look_language(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_file();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    char *l = get_str(kf, "look", "language");
    g_free(path);
    g_key_file_unref(kf);
    return l ? l : g_strdup("system");
}

void geom_load(Geom *g)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_file();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    memset(g, 0, sizeof *g);
    g->w = get_int(kf, "window", "width", 0, 0, 20000);
    g->h = get_int(kf, "window", "height", 0, 0, 20000);
    g->has_size = g->w >= 300 && g->h >= 200;
    g->has_pos = g_key_file_has_key(kf, "window", "x", NULL) && g_key_file_has_key(kf, "window", "y", NULL);
    g->x = get_int(kf, "window", "x", 0, -20000, 20000);
    g->y = get_int(kf, "window", "y", 0, -20000, 20000);
    g->max = get_bool(kf, "window", "maximized", FALSE);
    g_free(path);
    g_key_file_unref(kf);
}

/* only the [window] lines change; the rest of the file stays as it is */
void geom_save(const Geom *g)
{
    if (!g->has_size)
        return;
    GKeyFile *kf = g_key_file_new();
    char *path = config_file();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_integer(kf, "window", "width", g->w);
    g_key_file_set_integer(kf, "window", "height", g->h);
    if (g->has_pos) {
        g_key_file_set_integer(kf, "window", "x", g->x);
        g_key_file_set_integer(kf, "window", "y", g->y);
    }
    g_key_file_set_boolean(kf, "window", "maximized", g->max);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
    g_key_file_unref(kf);
}

/* By name, natural order (file2 before file10), case-insensitive whatever the locale
 * (apple, Banana, cherry); the exact name breaks ties. Folders first unless switched off. */
char *sort_key(const char *name, gboolean isdir)
{
    char *fold = g_utf8_casefold(name, -1);
    char *key = g_utf8_collate_key_for_filename(fold, -1);
    char *r = g_strconcat(isdir || !LOOK.folders_first ? "0" : "1", key, "\001", name, NULL);
    g_free(key);
    g_free(fold);
    return r;
}

/* The configured colour, or (auto) the theme background made 20 % brighter on dark
 * themes and 20 % darker on light ones. Grey if the theme won't tell us its colour. */
void divider_rgba(GtkWidget *w, GdkRGBA *out)
{
    if (strcmp(LOOK.divider_color, "auto") && gdk_rgba_parse(out, LOOK.divider_color))
        return;
    GdkRGBA bg;
    if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(w), "theme_bg_color", &bg)) {
        gdk_rgba_parse(out, "#8c8c8c");
        return;
    }
    double lum = 0.2126 * bg.red + 0.7152 * bg.green + 0.0722 * bg.blue;
    if (lum < 0.5) {          /* dark theme: towards white */
        out->red = bg.red + (1 - bg.red) * 0.2;
        out->green = bg.green + (1 - bg.green) * 0.2;
        out->blue = bg.blue + (1 - bg.blue) * 0.2;
    } else {                  /* light theme: towards black */
        out->red = bg.red * 0.8;
        out->green = bg.green * 0.8;
        out->blue = bg.blue * 0.8;
    }
    out->alpha = 1.0;
}

/* the line plus air on both sides; never thinner than 8 px so it's easy to grab */
int divider_hit_width(void)
{
    return MAX(LOOK.divider_width + 4, 8);
}

typedef struct {
    DragFn drag;
    PlainFn reset, done;
    gpointer data;
    gboolean line;
    gboolean pressed;
    double x0;
} Div;

static gboolean div_draw(GtkWidget *w, cairo_t *cr, Div *d)
{
    if (!d->line)
        return FALSE;
    GdkRGBA c;
    divider_rgba(w, &c);
    int lw = LOOK.divider_width;
    int h = gtk_widget_get_allocated_height(w);
    double x = (gtk_widget_get_allocated_width(w) - lw) / 2.0;
    if (LOOK.fade) {
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, h);
        cairo_pattern_add_color_stop_rgba(g, 0.0, c.red, c.green, c.blue, 0);
        cairo_pattern_add_color_stop_rgba(g, 0.12, c.red, c.green, c.blue, c.alpha);
        cairo_pattern_add_color_stop_rgba(g, 0.88, c.red, c.green, c.blue, c.alpha);
        cairo_pattern_add_color_stop_rgba(g, 1.0, c.red, c.green, c.blue, 0);
        cairo_set_source(cr, g);
        cairo_pattern_destroy(g);
    } else {
        cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
    }
    cairo_rectangle(cr, x, 0, lw, h);
    cairo_fill(cr);
    return FALSE;
}

static void div_realize(GtkWidget *w, Div *d)
{
    (void) d;
    GdkCursor *cur = gdk_cursor_new_from_name(gtk_widget_get_display(w), "col-resize");
    gdk_window_set_cursor(gtk_widget_get_window(w), cur);
    if (cur)
        g_object_unref(cur);
}

static gboolean div_press(GtkWidget *w, GdkEventButton *ev, Div *d)
{
    (void) w;
    if (ev->button != 1)
        return FALSE;
    if (ev->type == GDK_2BUTTON_PRESS) {
        d->pressed = FALSE;
        if (d->reset)
            d->reset(d->data);
    } else if (ev->type == GDK_BUTTON_PRESS) {
        d->pressed = TRUE;
        d->x0 = ev->x_root;
    }
    return TRUE;
}

static gboolean div_motion(GtkWidget *w, GdkEventMotion *ev, Div *d)
{
    (void) w;
    if (d->pressed)
        d->drag(ev->x_root - d->x0, d->data);
    return TRUE;
}

static gboolean div_release(GtkWidget *w, GdkEventButton *ev, Div *d)
{
    (void) w; (void) ev;
    if (d->pressed) {
        d->pressed = FALSE;
        if (d->done)
            d->done(d->data);
    }
    return TRUE;
}

/* A thin vertical divider drawn with cairo (themes ignore separator widths).
 * Reads the settings on every draw, so changes show up at once.
 * drag(dx) is called while it's dragged (dx = pixels since the press), done() on release,
 * reset() on double-click. line = FALSE: an invisible grab handle (preview edge). */
GtkWidget *divider_new(DragFn drag, PlainFn reset, PlainFn done, gpointer data, gboolean line)
{
    GtkWidget *area = gtk_drawing_area_new();
    Div *d = g_new0(Div, 1);
    d->drag = drag;
    d->reset = reset;
    d->done = done;
    d->data = data;
    d->line = line;
    g_object_set_data_full(G_OBJECT(area), "div", d, g_free);
    gtk_widget_set_size_request(area, divider_hit_width(), -1);
    g_signal_connect(area, "draw", G_CALLBACK(div_draw), d);
    g_signal_connect(area, "style-updated", G_CALLBACK(gtk_widget_queue_draw), NULL);
    if (drag) {
        gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                    | GDK_POINTER_MOTION_MASK);
        gtk_widget_set_tooltip_text(area, _("Drag to resize, double-click for the default width"));
        g_signal_connect(area, "realize", G_CALLBACK(div_realize), d);
        g_signal_connect(area, "button-press-event", G_CALLBACK(div_press), d);
        g_signal_connect(area, "motion-notify-event", G_CALLBACK(div_motion), d);
        g_signal_connect(area, "button-release-event", G_CALLBACK(div_release), d);
    }
    return area;
}
