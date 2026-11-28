/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Settings (~/.config/millerx/millerx.conf), sort order and the column dividers. */
#include "millerx.h"
#include <string.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>

Look LOOK = { NULL, 2, FALSE, PREVIEW_DEFAULT, TRUE, FALSE, .column_width = COL_DEFAULT,
              .use_ffmpeg = TRUE, .use_pdftoppm = TRUE, .use_pkexec = TRUE, .use_blender = TRUE, .use_git = FALSE, .git_colours = FALSE, .automount = FALSE, .clicks = CLICKS_CLASSIC,
              .thumb_size = TSIZE_MEDIUM,
              .thumbs = THUMBS_LOCAL, .detect = DETECT_NAME, .ask_programs = TRUE, .trust_system = TRUE };

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

/* millerX Unleashed is a setting now (0.9.0): someone who only had Unleashed keeps its settings, sort
 * orders and trusted programs, once, and gets Unleashed's clicks. Who has millerX's own settings
 * already keeps those (the setting is one click away). */
static void unleashed_take_over(const char *path)
{
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    char *old = g_build_filename(g_get_user_config_dir(), "millerx-unleashed", NULL);
    char *old_conf = g_build_filename(old, "millerx.conf", NULL);
    if (g_file_test(old_conf, G_FILE_TEST_IS_REGULAR)) {
        char *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0700);
        static const char *const FILES[] = { "millerx.conf", "sorting.conf", "trusted" };
        for (guint i = 0; i < G_N_ELEMENTS(FILES); i++) {
            char *from = g_build_filename(old, FILES[i], NULL), *to = g_build_filename(dir, FILES[i], NULL);
            char *data = NULL;
            gsize len = 0;
            if (!g_file_test(to, G_FILE_TEST_EXISTS) && g_file_test(from, G_FILE_TEST_IS_REGULAR) &&
                g_file_get_contents(from, &data, &len, NULL)) {
                int fd = g_open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);   /* the trusted list: yours only */
                if (fd >= 0) {
                    gboolean ok = write(fd, data, len) == (gssize) len;
                    close(fd);
                    if (!ok)
                        g_unlink(to);
                }
            }
            g_free(data);
            g_free(from);
            g_free(to);
        }
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL)) {
            g_key_file_set_string(kf, "browse", "clicks", "unleashed");
            g_key_file_save_to_file(kf, path, NULL);
        }
        g_key_file_free(kf);
        g_free(dir);
    }
    g_free(old_conf);
    g_free(old);
}

/* every value checked, defaults for anything odd */
void look_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_file();
    unleashed_take_over(path);
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
    LOOK.use_git = get_bool(kf, "helpers", "git", FALSE);
    LOOK.git_colours = get_bool(kf, "helpers", "git_colours", FALSE);
    LOOK.automount = get_bool(kf, "devices", "automount", FALSE);
    char *th = get_str(kf, "thumbnails", "show");
    LOOK.thumbs = !g_strcmp0(th, "always") ? THUMBS_ALWAYS : !g_strcmp0(th, "never") ? THUMBS_NEVER : THUMBS_LOCAL;
    g_free(th);
    char *dt = get_str(kf, "look", "recognise");      /* by name unless asked for */
    LOOK.detect = !g_strcmp0(dt, "content") ? DETECT_CONTENT : DETECT_NAME;
    g_free(dt);
    LOOK.ask_programs = get_bool(kf, "browse", "ask_programs", TRUE);
    LOOK.trust_system = get_bool(kf, "browse", "trust_system", TRUE);
    char *ts = get_str(kf, "thumbnails", "size");
    LOOK.thumb_size = !g_strcmp0(ts, "tiny") ? TSIZE_TINY : !g_strcmp0(ts, "small") ? TSIZE_SMALL
                    : !g_strcmp0(ts, "large") ? TSIZE_LARGE : TSIZE_MEDIUM;
    g_free(ts);
    LOOK.gallery_names = get_bool(kf, "look", "gallery_names", TRUE);
    g_free(LOOK.language);
    LOOK.language = get_str(kf, "look", "language");
    g_free(LOOK.compare);
    LOOK.compare = get_str(kf, "helpers", "compare");
    if (!LOOK.compare || !*LOOK.compare) {
        g_free(LOOK.compare);
        LOOK.compare = g_strdup("auto");
    }
    if (!LOOK.language || (strcmp(LOOK.language, "en") && strcmp(LOOK.language, "wizard")
                              && strcmp(LOOK.language, "wienerisch") && !g_str_has_prefix(LOOK.language, "own:"))) {
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
    char *clicks = get_str(kf, "browse", "clicks");
    LOOK.clicks = !g_strcmp0(clicks, "unleashed") ? CLICKS_UNLEASHED : CLICKS_CLASSIC;
    g_free(clicks);
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
    g_key_file_set_string(kf, "browse", "clicks", LOOK.clicks == CLICKS_UNLEASHED ? "unleashed" : "classic");
    g_key_file_set_boolean(kf, "browse", "ask_programs", LOOK.ask_programs);
    g_key_file_set_boolean(kf, "browse", "trust_system", LOOK.trust_system);
    g_key_file_set_boolean(kf, "helpers", "ffmpeg", LOOK.use_ffmpeg);
    g_key_file_set_boolean(kf, "helpers", "pdftoppm", LOOK.use_pdftoppm);
    g_key_file_set_boolean(kf, "helpers", "pkexec", LOOK.use_pkexec);
    g_key_file_set_boolean(kf, "helpers", "blender", LOOK.use_blender);
    g_key_file_set_boolean(kf, "helpers", "git", LOOK.use_git);
    g_key_file_set_boolean(kf, "helpers", "git_colours", LOOK.git_colours);
    g_key_file_set_boolean(kf, "devices", "automount", LOOK.automount);
    g_key_file_set_string(kf, "helpers", "compare", LOOK.compare ? LOOK.compare : "auto");
    g_key_file_set_string(kf, "thumbnails", "show",
                          LOOK.thumbs == THUMBS_LOCAL ? "local" : LOOK.thumbs == THUMBS_NEVER ? "never" : "always");
    g_key_file_set_string(kf, "look", "recognise", LOOK.detect == DETECT_CONTENT ? "content" : "name");
    g_key_file_set_string(kf, "thumbnails", "size",
                          LOOK.thumb_size == TSIZE_TINY ? "tiny" : LOOK.thumb_size == TSIZE_SMALL ? "small"
                          : LOOK.thumb_size == TSIZE_LARGE ? "large" : "medium");
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

/* ---- own languages -----------------------------------------------------------------
 * Anyone can write millerX's texts their own way: copy millerx.pot, fill in the msgstr lines and
 * put it as NAME.po (or compiled, NAME.mo) into ~/.local/share/millerx/languages. It shows up in
 * Settings → Language as NAME. */
char *own_languages_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "millerx", "languages", NULL);
}

static gboolean own_name_ok(const char *name)
{
    return name && *name && name[0] != '.' && !strchr(name, '/') && g_utf8_validate(name, -1, NULL);
}

static int name_cmp(gconstpointer a, gconstpointer b)    /* g_ptr_array_sort hands pointers to the entries */
{
    return g_utf8_collate(*(char *const *) a, *(char *const *) b);
}

GPtrArray *own_languages_list(void)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    char *dir = own_languages_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    char *msgfmt = g_find_program_in_path("msgfmt");    /* a .po needs it to be turned into a .mo */
    const char *e;
    while (d && (e = g_dir_read_name(d))) {
        gboolean po = g_str_has_suffix(e, ".po"), mo = g_str_has_suffix(e, ".mo");
        if (!mo && !(po && msgfmt))
            continue;
        char *name = g_strndup(e, strlen(e) - 3);
        gboolean dup = FALSE;
        for (guint i = 0; i < names->len && !dup; i++)
            dup = !strcmp(names->pdata[i], name);
        if (own_name_ok(name) && !dup)
            g_ptr_array_add(names, name);
        else
            g_free(name);
    }
    if (d)
        g_dir_close(d);
    g_free(msgfmt);
    g_free(dir);
    g_ptr_array_sort(names, name_cmp);
    return names;
}

/* Settings → Language → "Your own…": a picked file is checked (a .po: its %s and the like must
 * match, or millerX could crash) and copied into the folder, made if needed. Returns its NAME;
 * on failure NULL with *err saying why. */
char *own_language_add(const char *path, char **err)
{
    char *base = g_path_get_basename(path);
    gboolean po = g_str_has_suffix(base, ".po"), mo = g_str_has_suffix(base, ".mo");
    char *name = po || mo ? g_strndup(base, strlen(base) - 3) : NULL;
    char *dir = NULL, *other = NULL;
    GFile *src = NULL, *dst = NULL;
    if (!own_name_ok(name)) {
        *err = g_strdup(_("Pick a language file: a .po or .mo file."));
        goto fail;
    }
    if (po) {
        char *msgfmt = g_find_program_in_path("msgfmt"), *out = NULL;
        int status = 0;
        if (!msgfmt) {
            *err = g_strdup(_("A .po file needs msgfmt (gettext) to be checked, and it isn't installed."));
            goto fail;
        }
        /* compiled to a temporary .mo, which is then checked like a .mo: --check-format alone
         * skips every entry without a c-format flag */
        char *tmp = NULL;
        int fd = g_file_open_tmp("millerx-lang-XXXXXX.mo", &tmp, NULL);
        if (fd < 0) {
            g_free(msgfmt);
            *err = g_strdup(_("Couldn't make a temporary file."));
            goto fail;
        }
        close(fd);
        char *argv[] = { msgfmt, (char *) "--check-format", (char *) "-o", tmp, (char *) path, NULL };
        gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &out, &status, NULL)
                      && g_spawn_check_wait_status(status, NULL);
        g_free(msgfmt);
        if (!ok) {
            g_unlink(tmp);
            g_free(tmp);
            *err = out ? out : g_strdup("msgfmt");          /* msgfmt says which line is wrong */
            goto fail;
        }
        g_free(out);
        ok = mo_check_file(tmp, err);
        g_unlink(tmp);
        g_free(tmp);
        if (!ok)
            goto fail;
    } else if (!mo_check_file(path, err)) {
        goto fail;
    }
    dir = own_languages_dir();
    g_mkdir_with_parents(dir, 0755);
    src = g_file_new_for_path(path);
    dst = g_file_new_build_filename(dir, base, NULL);
    GError *e = NULL;
    if (!g_file_equal(src, dst) && !g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &e)) {
        *err = g_strdup(e->message);
        g_error_free(e);
        goto fail;
    }
    other = g_strconcat(dir, G_DIR_SEPARATOR_S, name, po ? ".mo" : ".po", NULL);
    g_unlink(other);                 /* the same NAME as the other kind: the new file is the one used */
    g_free(other);
    g_free(dir);
    g_free(base);
    g_object_unref(src);
    g_object_unref(dst);
    return name;
fail:
    g_free(name);
    g_free(dir);
    g_free(base);
    if (src)
        g_object_unref(src);
    if (dst)
        g_object_unref(dst);
    return NULL;
}

/* gettext looks for LANG/LC_MESSAGES/millerx.mo under the folder it is bound to: put the chosen
 * file there as "en@own" (a .po is checked and compiled by msgfmt first; it wins over a .mo) */
char *own_language_prepare(const char *name)
{
    if (!own_name_ok(name))
        return NULL;
    char *dir = own_languages_dir();
    char *po = g_strconcat(dir, G_DIR_SEPARATOR_S, name, ".po", NULL);
    char *mo = g_strconcat(dir, G_DIR_SEPARATOR_S, name, ".mo", NULL);
    char *base = g_build_filename(g_get_user_cache_dir(), "millerx", "language", NULL);
    char *lc = g_build_filename(base, "en@own", "LC_MESSAGES", NULL);
    char *out = g_build_filename(lc, "millerx.mo", NULL);
    gboolean ok = FALSE;
    g_mkdir_with_parents(lc, 0700);
    g_unlink(out);
    if (g_file_test(po, G_FILE_TEST_IS_REGULAR)) {
        char *msgfmt = g_find_program_in_path("msgfmt"), *err = NULL;
        int status = 0;
        if (msgfmt) {
            char *argv[] = { msgfmt, (char *) "--check-format", (char *) "-o", out, po, NULL };   /* a wrong %s would crash */
            ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &err, &status, NULL)
                 && g_spawn_check_wait_status(status, NULL);
            if (!ok)
                g_printerr("millerX: %s has mistakes, using English:\n%s", po, err ? err : "");
        } else {
            g_printerr("millerX: %s needs msgfmt (gettext), using English\n", po);
        }
        g_free(err);
        g_free(msgfmt);
    } else {
        char *data;
        gsize len;
        ok = g_file_get_contents(mo, &data, &len, NULL) && g_file_set_contents(out, data, (gssize) len, NULL);
        if (ok)
            g_free(data);
    }
    char *why = NULL;
    if (ok && !mo_check_file(out, &why)) {     /* also files put there by hand: checked before every use */
        g_printerr("millerX: own language %s can't be used, using English: %s\n", name, why);
        g_unlink(out);
        ok = FALSE;
    }
    g_free(why);
    g_free(dir);
    g_free(po);
    g_free(mo);
    g_free(lc);
    g_free(out);
    if (!ok) {
        g_free(base);
        return NULL;
    }
    return base;
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
