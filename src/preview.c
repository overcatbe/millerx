/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Right-hand pane: picture, big type icon, text or archive contents, plus details. */
#include "millerx.h"
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    Preview *p;
    Win *win;              /* keeps the window's memory alive while we work */
    guint token;
    char *path;
    int width, max_height;
    GtkWidget *size_label, *count_label;   /* refs */
    GPtrArray *paths;
    gboolean single_dir;
    /* results */
    GdkPixbuf *pixbuf;
    char *text;
    guint64 total, files, items;
    gboolean running;
} Bg;

static Bg *bg_new(Preview *p, const char *path)
{
    Bg *b = g_new0(Bg, 1);
    b->p = p;
    b->win = win_ref(p->win);
    b->token = p->token;
    b->path = g_strdup(path);
    b->width = p->width;
    /* the picture may use the pane's height, minus room for the name and the details below it */
    int h = gtk_widget_get_allocated_height(p->box);
    b->max_height = MAX(240, h - 260);
    return b;
}

static void bg_free(Bg *b)
{
    g_free(b->path);
    g_clear_object(&b->size_label);
    g_clear_object(&b->count_label);
    if (b->paths)
        g_ptr_array_unref(b->paths);
    g_clear_object(&b->pixbuf);
    g_free(b->text);
    win_unref(b->win);
    g_free(b);
}

static gboolean bg_current(Bg *b)
{
    return !b->win->dead && (guint) g_atomic_int_get((gint *) &b->p->token) == b->token;
}

static void spawn_bg(Bg *b, GThreadFunc fn)
{
    GThread *t = g_thread_new("millerx-preview", fn, b);
    g_thread_unref(t);
}

void preview_clear(Preview *p)
{
    g_atomic_int_inc((gint *) &p->token);
    g_clear_object(&p->current);
    gtk_image_clear(GTK_IMAGE(p->image));
    gtk_widget_hide(p->image);
    gtk_image_clear(GTK_IMAGE(p->icon));
    gtk_widget_hide(p->icon);
    gtk_widget_hide(p->text_scroll);
    if (p->grid_timer) {
        g_source_remove(p->grid_timer);
        p->grid_timer = 0;
    }
    gtk_widget_hide(p->grid_scroll);
    GList *thumbs = gtk_container_get_children(GTK_CONTAINER(p->grid));
    for (GList *l = thumbs; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(thumbs);
    gtk_label_set_text(GTK_LABEL(p->title), "");
    GList *kids = gtk_container_get_children(GTK_CONTAINER(p->details));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
}

/* max_width_chars = 1 + wrapping: long names wrap inside the fixed width
 * instead of making the preview wider */
static GtkWidget *wrap_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    return l;
}

static GtkWidget *row(Preview *p, int n, const char *key, const char *value)
{
    GtkWidget *k = gtk_label_new(key);
    gtk_label_set_xalign(GTK_LABEL(k), 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
    GtkWidget *v = wrap_label(value);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(p->details), k, 0, n, 1, 1);
    gtk_grid_attach(GTK_GRID(p->details), v, 1, n, 1, 1);
    return v;
}

static void show_icon(Preview *p, GIcon *gicon)
{
    if (gicon) {
        gtk_image_set_from_gicon(GTK_IMAGE(p->icon), gicon, GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(p->icon), 128);
        gtk_widget_show(p->icon);
    }
}

/* ---- background work ----------------------------------------------------------- */
static gboolean set_pixbuf(gpointer data)
{
    Bg *b = data;
    if (bg_current(b) && b->pixbuf) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(b->p->image), b->pixbuf);
        gtk_widget_show(b->p->image);
        gtk_widget_hide(b->p->icon);
    }
    bg_free(b);
    return G_SOURCE_REMOVE;
}

/* A picture no bigger than max_w x max_h; small pictures keep their own size. */
static GdkPixbuf *load_fitted(const char *path, int max_w, int max_h)
{
    int w = 0, h = 0;
    GdkPixbuf *pb = NULL;
    if (!gdk_pixbuf_get_file_info(path, &w, &h))
        return NULL;
    if (w > max_w || h > max_h)
        pb = gdk_pixbuf_new_from_file_at_scale(path, max_w, max_h, TRUE, NULL);
    else
        pb = gdk_pixbuf_new_from_file(path, NULL);
    if (!pb)
        return NULL;
    GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation(pb);   /* may swap width and height */
    g_object_unref(pb);
    w = gdk_pixbuf_get_width(o);
    h = gdk_pixbuf_get_height(o);
    if (w > max_w || h > max_h) {
        double sc = MIN((double) max_w / w, (double) max_h / h);
        GdkPixbuf *s = gdk_pixbuf_scale_simple(o, MAX(1, (int) (w * sc)), MAX(1, (int) (h * sc)),
                                               GDK_INTERP_BILINEAR);
        g_object_unref(o);
        o = s;
    }
    return o;
}

static gpointer image_thread(gpointer data)
{
    Bg *b = data;
    b->pixbuf = load_fitted(b->path, b->width - 24, b->max_height);
    g_idle_add(set_pixbuf, b);
    return NULL;
}

/* ---- folder contents as a thumbnail grid ----------------------------------------- */
#define THUMB_COLS 3
#define THUMB_MAX 1000          /* a folder with more shows the first ones */

typedef struct {
    Preview *p;
    Win *win;
    guint token;
    char *path;                 /* the file */
    char *name;
    GdkPixbuf *pixbuf;          /* a picture's thumbnail, or */
    GIcon *icon;                /* the type icon */
    int size;
} Thumb;

static void thumb_free(Thumb *t)
{
    g_free(t->path);
    g_free(t->name);
    g_clear_object(&t->pixbuf);
    g_clear_object(&t->icon);
    win_unref(t->win);
    g_free(t);
}

static gboolean add_thumb(gpointer data)
{
    Thumb *t = data;
    Preview *p = t->p;
    if (!t->win->dead && p->token == t->token) {
        GtkWidget *img = t->pixbuf ? gtk_image_new_from_pixbuf(t->pixbuf)
                                   : gtk_image_new_from_gicon(t->icon, GTK_ICON_SIZE_DIALOG);
        if (!t->pixbuf)
            gtk_image_set_pixel_size(GTK_IMAGE(img), t->size / 2);
        gtk_widget_hide(p->icon);     /* the folder has contents: the grid replaces the big icon */
        gtk_widget_show(p->grid_scroll);
        gtk_widget_set_size_request(img, t->size, t->size);
        gtk_widget_set_tooltip_text(img, t->name);
        GtkWidget *child = gtk_flow_box_child_new();
        gtk_container_add(GTK_CONTAINER(child), img);
        g_object_set_data_full(G_OBJECT(child), "path", g_strdup(t->path), g_free);
        gtk_container_add(GTK_CONTAINER(p->grid), child);
        gtk_widget_show_all(child);
    }
    thumb_free(t);
    return G_SOURCE_REMOVE;
}

typedef struct {
    Preview *p;
    Win *win;
    guint token;
    char *dir;
    int size;
    gboolean hidden;
} GridJob;

/* Same order as the column: folders first (if set), then names, case-insensitive. */
typedef struct {
    char *name, *key;
    gboolean dir;
} Entry;

static void entry_free(Entry *e)
{
    g_free(e->name);
    g_free(e->key);
    g_free(e);
}

static int by_name(gconstpointer a, gconstpointer b)
{
    const Entry *x = *(Entry **) a, *y = *(Entry **) b;
    if (LOOK.folders_first && x->dir != y->dir)
        return x->dir ? -1 : 1;
    return g_strcmp0(x->key, y->key);
}

static gpointer grid_thread(gpointer data)
{
    GridJob *j = data;
    GPtrArray *names = g_ptr_array_new_with_free_func((GDestroyNotify) entry_free);
    GDir *d = g_dir_open(j->dir, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name(d)) && names->len < THUMB_MAX)
            if (j->hidden || n[0] != '.') {
                Entry *e = g_new0(Entry, 1);
                e->name = g_strdup(n);
                char *full = g_build_filename(j->dir, n, NULL);
                e->dir = g_file_test(full, G_FILE_TEST_IS_DIR);
                g_free(full);
                char *disp = g_filename_display_name(n), *fold = g_utf8_casefold(disp, -1);
                e->key = g_utf8_collate_key_for_filename(fold, -1);
                g_free(disp);
                g_free(fold);
                g_ptr_array_add(names, e);
            }
        g_dir_close(d);
    }
    g_ptr_array_sort(names, by_name);
    for (guint i = 0; i < names->len; i++) {
        if (j->win->dead || (guint) g_atomic_int_get((gint *) &j->p->token) != j->token)
            break;      /* the preview shows something else now */
        const char *n = ((Entry *) g_ptr_array_index(names, i))->name;
        Thumb *t = g_new0(Thumb, 1);
        t->p = j->p;
        t->win = win_ref(j->win);
        t->token = j->token;
        t->path = g_build_filename(j->dir, n, NULL);
        t->name = g_filename_display_name(n);
        t->size = j->size;
        gboolean isdir = g_file_test(t->path, G_FILE_TEST_IS_DIR);
        char *ct = isdir ? g_strdup("inode/directory") : g_content_type_guess(n, NULL, 0, NULL);
        if (!isdir && g_str_has_prefix(ct, "image/")) {
            /* a thumbnail Thunar or a file dialog already made is far cheaper than the picture */
            GFile *gf = g_file_new_for_path(t->path);
            GFileInfo *ti = g_file_query_info(gf, G_FILE_ATTRIBUTE_THUMBNAIL_PATH, G_FILE_QUERY_INFO_NONE,
                                              NULL, NULL);
            const char *tp = ti ? g_file_info_get_attribute_byte_string(ti, G_FILE_ATTRIBUTE_THUMBNAIL_PATH)
                                : NULL;
            if (tp)
                t->pixbuf = load_fitted(tp, j->size, j->size);
            if (!t->pixbuf)
                t->pixbuf = load_fitted(t->path, j->size, j->size);
            g_clear_object(&ti);
            g_object_unref(gf);
        }
        if (!t->pixbuf)
            t->icon = g_content_type_get_icon(ct);
        g_free(ct);
        g_idle_add(add_thumb, t);
    }
    g_ptr_array_unref(names);
    win_unref(j->win);
    g_free(j->dir);
    g_free(j);
    return NULL;
}

/* Started a moment after a folder is shown, so stepping through folders with the keys loads nothing. */
static gboolean start_grid(gpointer data)
{
    Preview *p = data;
    p->grid_timer = 0;
    char *dir = p->current ? g_file_get_path(p->current) : NULL;
    if (!dir)
        return G_SOURCE_REMOVE;
    GridJob *j = g_new0(GridJob, 1);
    j->p = p;
    j->win = win_ref(p->win);
    j->token = p->token;
    j->dir = dir;
    j->size = (p->width - 24 - (THUMB_COLS - 1) * 6) / THUMB_COLS - 4;
    j->hidden = p->win->show_hidden;
    g_thread_unref(g_thread_new("millerx-grid", grid_thread, j));
    return G_SOURCE_REMOVE;
}

/* double-click (or Enter) on a thumbnail: go to that file */
static void on_thumb_activated(GtkFlowBox *box, GtkFlowBoxChild *child, Preview *p)
{
    (void) box;
    GFile *f = g_file_new_for_path(g_object_get_data(G_OBJECT(child), "path"));
    win_open_path(p->win, f);
    g_object_unref(f);
}

static gpointer pdf_thread(gpointer data)
{
    Bg *b = data;
    char *dir = g_dir_make_tmp("millerx-XXXXXX", NULL);
    if (dir) {
        char *out = g_build_filename(dir, "p", NULL);
        char *png = g_strconcat(out, ".png", NULL);
        char *w = g_strdup_printf("%d", b->width - 24);
        char *argv[] = { (char *) "pdftoppm", (char *) "-f", (char *) "1", (char *) "-l", (char *) "1",
                         (char *) "-png", (char *) "-singlefile", (char *) "-scale-to", w, b->path, out, NULL };
        GPid pid;
        if (g_spawn_async(NULL, argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL
                          | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &pid, NULL)) {
            int status = -1;
            for (int i = 0; i < 300; i++) {
                if (waitpid(pid, &status, WNOHANG) == pid)
                    break;
                status = -1;
                g_usleep(50000);
            }
            if (status == -1) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                status = -1;
            }
            g_spawn_close_pid(pid);
            if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                b->pixbuf = gdk_pixbuf_new_from_file(png, NULL);
        }
        g_unlink(png);
        g_rmdir(dir);
        g_free(w);
        g_free(png);
        g_free(out);
        g_free(dir);
    }
    g_idle_add(set_pixbuf, b);
    return NULL;
}

static gboolean set_text(gpointer data)
{
    Bg *b = data;
    if (bg_current(b) && b->text && *b->text) {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(b->p->text)), b->text, -1);
        gtk_widget_show_all(b->p->text_scroll);
    }
    bg_free(b);
    return G_SOURCE_REMOVE;
}

/* list of entries in zip / tar(.gz/.bz2/.xz/.zst) / jar / cbz / epub via libarchive */
static gpointer archive_thread(gpointer data)
{
    Bg *b = data;
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_zip(a);
    archive_read_support_format_tar(a);
    archive_read_support_format_gnutar(a);
    archive_read_support_format_cpio(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_iso9660(a);
    if (archive_read_open_filename(a, b->path, 65536) == ARCHIVE_OK) {
        GString *s = g_string_new(NULL);
        guint64 total = 0, shown = 0;
        struct archive_entry *e;
        while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
            total++;
            if (shown < ARCHIVE_LIST_MAX) {
                const char *n = archive_entry_pathname_utf8(e);
                if (!n)
                    n = archive_entry_pathname(e);
                g_string_append(s, n ? n : "?");
                if (archive_entry_filetype(e) == AE_IFDIR && !g_str_has_suffix(n ? n : "", "/"))
                    g_string_append_c(s, '/');
                g_string_append_c(s, '\n');
                shown++;
            }
            archive_read_data_skip(a);
            if (!bg_current(b))
                break;
        }
        if (total) {
            char *body = g_utf8_make_valid(s->str, s->len);
            if (total > shown)
                b->text = g_strdup_printf("%" G_GUINT64_FORMAT " entries:\n%s… and %" G_GUINT64_FORMAT " more",
                                          total, body, total - shown);
            else
                b->text = g_strdup_printf("%" G_GUINT64_FORMAT " entries:\n%s", total, body);
            /* drop the trailing newline before "… and" looks fine; keep it simple */
            g_free(body);
        }
        g_string_free(s, TRUE);
    }
    archive_read_free(a);
    g_idle_add(set_text, b);
    return NULL;
}

static gboolean set_sizes(gpointer data)
{
    Bg *b = data;
    if (bg_current(b)) {
        char *size = format_size_full(b->total);
        char *txt = g_strconcat(size, b->running ? " …" : "", NULL);
        gtk_label_set_text(GTK_LABEL(b->size_label), txt);
        g_free(txt);
        g_free(size);
        char *cnt = b->single_dir
            ? g_strdup_printf("%" G_GUINT64_FORMAT " here, %" G_GUINT64_FORMAT " files in total%s",
                              b->items, b->files, b->running ? " …" : "")
            : g_strdup_printf("%" G_GUINT64_FORMAT " files in total%s", b->files, b->running ? " …" : "");
        gtk_label_set_text(GTK_LABEL(b->count_label), cnt);
        g_free(cnt);
    }
    if (!b->running)
        bg_free(b);
    return G_SOURCE_REMOVE;
}

/* a copy of the counters for one progress update */
static void post_sizes(Bg *b, gboolean running)
{
    if (!running) {
        b->running = FALSE;
        g_idle_add(set_sizes, b);
        return;
    }
    Bg *u = g_new0(Bg, 1);
    u->p = b->p;
    u->win = win_ref(b->win);
    u->token = b->token;
    u->size_label = g_object_ref(b->size_label);
    u->count_label = g_object_ref(b->count_label);
    u->single_dir = b->single_dir;
    u->total = b->total;
    u->files = b->files;
    u->items = b->items;
    u->running = TRUE;
    /* set_sizes frees running updates too */
    g_idle_add((GSourceFunc) set_sizes, u);
}

static gpointer sizes_thread(gpointer data)
{
    Bg *b = data;
    if (b->single_dir) {
        DIR *d = opendir(b->path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                    b->items++;
            closedir(d);
        }
    }
    GPtrArray *stack = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < b->paths->len; i++) {
        struct stat st;
        const char *p = g_ptr_array_index(b->paths, i);
        if (lstat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            g_ptr_array_add(stack, g_strdup(p));
        else {
            b->total += st.st_size;
            b->files++;
        }
    }
    gint64 last = g_get_monotonic_time();
    while (stack->len) {
        if (!bg_current(b))
            break;
        char *dpath = g_ptr_array_steal_index(stack, stack->len - 1);
        DIR *d = opendir(dpath);
        if (d) {
            int fd = dirfd(d);
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                struct stat st;
                if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
                    continue;
                if (S_ISDIR(st.st_mode))
                    g_ptr_array_add(stack, g_build_filename(dpath, e->d_name, NULL));
                else {
                    b->total += st.st_size;
                    b->files++;
                }
            }
            closedir(d);
        }
        g_free(dpath);
        if (g_get_monotonic_time() - last > 300000) {
            last = g_get_monotonic_time();
            post_sizes(b, TRUE);
        }
    }
    g_ptr_array_unref(stack);
    post_sizes(b, FALSE);
    return NULL;
}

static void start_sizes(Preview *p, GPtrArray *paths, GtkWidget *size_l, GtkWidget *count_l,
                        const char *single_dir)
{
    Bg *b = bg_new(p, single_dir);
    b->paths = paths;
    b->single_dir = single_dir != NULL;
    b->size_label = g_object_ref(size_l);
    b->count_label = g_object_ref(count_l);
    spawn_bg(b, sizes_thread);
}

/* ---- public ------------------------------------------------------------------------ */
void preview_show_many(Preview *p, GList *files)
{
    preview_clear(p);
    GIcon *ic = g_themed_icon_new("edit-copy");
    show_icon(p, ic);
    g_object_unref(ic);
    char *m = g_strdup_printf("<b>%u items selected</b>", g_list_length(files));
    gtk_label_set_markup(GTK_LABEL(p->title), m);
    g_free(m);
    GtkWidget *size_l = row(p, 0, "Size", "…");
    GtkWidget *count_l = row(p, 1, "Files", "…");
    gtk_widget_show_all(p->details);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = files; l; l = l->next) {
        char *path = g_file_get_path(l->data);
        if (path)
            g_ptr_array_add(paths, path);
    }
    start_sizes(p, paths, size_l, count_l, NULL);
}

static void show_text_file(Preview *p, const char *path)
{
    FILE *fh = fopen(path, "rb");
    if (!fh)
        return;
    char *buf = g_malloc(TEXT_PREVIEW_BYTES);
    size_t n = fread(buf, 1, TEXT_PREVIEW_BYTES, fh);
    fclose(fh);
    if (n && !memchr(buf, 0, n)) {
        char *txt = g_utf8_make_valid(buf, n);
        gtk_widget_hide(p->icon);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->text)), txt, -1);
        gtk_widget_show_all(p->text_scroll);
        g_free(txt);
    }
    g_free(buf);
}

void preview_show_file(Preview *p, GFile *f)
{
    preview_clear(p);
    p->current = g_object_ref(f);
    GError *err = NULL;
    GFileInfo *info = g_file_query_info(f, ATTRS, G_FILE_QUERY_INFO_NONE, NULL, &err);
    if (!info) {
        gtk_label_set_text(GTK_LABEL(p->title), err->message);
        g_error_free(err);
        return;
    }
    gboolean isdir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    const char *ct = g_file_info_get_content_type(info);
    if (!ct)
        ct = "";
    char *m = g_markup_printf_escaped("<b>%s</b>", g_file_info_get_display_name(info));
    gtk_label_set_markup(GTK_LABEL(p->title), m);
    g_free(m);
    int r = 0;
    char *desc = *ct ? g_content_type_get_description(ct) : g_strdup("?");
    row(p, r++, "Type", desc);
    g_free(desc);
    char *sz = isdir ? g_strdup("…") : format_size_full(g_file_info_get_size(info));
    GtkWidget *size_l = row(p, r++, "Size", sz);
    g_free(sz);
    GtkWidget *count_l = isdir ? row(p, r++, "Items", "…") : NULL;
    GDateTime *mt = g_file_info_get_modification_date_time(info);
    if (mt) {
        GDateTime *loc = g_date_time_to_local(mt);
        char *s = g_date_time_format(loc, "%Y-%m-%d %H:%M");
        row(p, r++, "Modified", s);
        g_free(s);
        g_date_time_unref(loc);
        g_date_time_unref(mt);
    }
    if (g_file_info_has_attribute(info, "unix::mode")) {
        guint32 mode = g_file_info_get_attribute_uint32(info, "unix::mode");
        char s[11];
        const char *rwx = "rwxrwxrwx";
        s[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISCHR(mode) ? 'c' : S_ISBLK(mode) ? 'b'
             : S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
        for (int i = 0; i < 9; i++)
            s[i + 1] = mode & (1 << (8 - i)) ? rwx[i] : '-';
        if (mode & S_ISUID) s[3] = (mode & S_IXUSR) ? 's' : 'S';
        if (mode & S_ISGID) s[6] = (mode & S_IXGRP) ? 's' : 'S';
        if (mode & S_ISVTX) s[9] = (mode & S_IXOTH) ? 't' : 'T';
        s[10] = 0;
        row(p, r++, "Permissions", s);
    }
    GFile *parent = g_file_get_parent(f);
    char *where = parent ? file_path(parent) : g_strdup("/");
    row(p, r++, "Where", where);
    g_free(where);
    g_clear_object(&parent);
    gtk_widget_show_all(p->details);

    char *path = g_file_get_path(f);
    /* big type icon, unless a real picture replaces it */
    show_icon(p, g_file_info_get_icon(info));
    if (path) {
        if (isdir) {
            p->grid_timer = g_timeout_add(250, start_grid, p);
            GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(paths, g_strdup(path));
            start_sizes(p, paths, size_l, count_l, path);
        } else if (g_str_has_prefix(ct, "image/")) {
            spawn_bg(bg_new(p, path), image_thread);
        } else if (!strcmp(ct, "application/pdf")) {
            spawn_bg(bg_new(p, path), pdf_thread);
        } else if (is_archive_type(ct)) {
            spawn_bg(bg_new(p, path), archive_thread);
        } else if (g_content_type_is_a(ct, "text/plain") || g_str_has_prefix(ct, "text/")) {
            show_text_file(p, path);
        }
    }
    g_free(path);
    g_object_unref(info);
}

Preview *preview_new(Win *w)
{
    Preview *p = g_new0(Preview, 1);
    p->win = w;
    p->width = LOOK.preview_width;
    p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_object_set(p->box, "margin", 12, NULL);
    gtk_widget_set_size_request(p->box, p->width, -1);
    gtk_widget_set_hexpand(p->box, FALSE);
    p->image = gtk_image_new();
    p->icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(p->icon), 128);
    p->text = gtk_text_view_new();
    g_object_set(p->text, "editable", FALSE, "cursor-visible", FALSE, "monospace", TRUE,
                 "wrap-mode", GTK_WRAP_CHAR, NULL);
    p->text_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(p->text_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(p->text_scroll), p->text);
    p->grid = gtk_flow_box_new();
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(p->grid), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(p->grid), THUMB_COLS);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(p->grid), THUMB_COLS);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(p->grid), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(p->grid), 6);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(p->grid), FALSE);
    gtk_widget_set_valign(p->grid, GTK_ALIGN_START);
    g_signal_connect(p->grid, "child-activated", G_CALLBACK(on_thumb_activated), p);
    p->grid_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->grid_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(p->grid_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(p->grid_scroll), p->grid);
    gtk_widget_set_no_show_all(p->grid_scroll, TRUE);
    gtk_widget_show(p->grid);
    p->title = wrap_label("");
    p->details = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(p->details), 10);
    gtk_grid_set_row_spacing(GTK_GRID(p->details), 3);
    gtk_box_pack_start(GTK_BOX(p->box), p->image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->text_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->grid_scroll, TRUE, TRUE, 0);
    return p;
}

void preview_free(Preview *p)
{
    if (p->grid_timer)
        g_source_remove(p->grid_timer);
    g_clear_object(&p->current);
    g_free(p);
}
