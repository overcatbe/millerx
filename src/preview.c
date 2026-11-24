/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Right-hand pane: picture, big type icon, text or archive contents, plus details. */
#define _GNU_SOURCE             /* strverscmp */
#include "millerx.h"
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif
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
    int media;             /* MEDIA_* for a picture job */
    guint gen;             /* sizes: which count this is (see sizes_gen) */
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
    b->width = MAX(64, p->image_w);
    b->max_height = MAX(64, p->image_h);
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

static void grid_stop(Preview *p);

void preview_clear(Preview *p)
{
    g_atomic_int_inc((gint *) &p->token);
    g_clear_object(&p->current);
    gtk_image_clear(GTK_IMAGE(p->image));
    gtk_widget_hide(p->image_scroll);
    gtk_widget_hide(p->sep);
    g_clear_pointer(&p->image_path, g_free);
    if (p->image_timer) {
        g_source_remove(p->image_timer);
        p->image_timer = 0;
    }
    gtk_image_clear(GTK_IMAGE(p->icon));
    gtk_widget_hide(p->icon);
    gtk_widget_hide(p->text_scroll);
    grid_stop(p);
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
    /* at least 16 characters wide: GtkGrid asks for the height at the narrowest width, and with
     * one letter per line a path made the details 800 px "high" (the header stretched, the window
     * couldn't be made lower) */
    gtk_label_set_width_chars(GTK_LABEL(l), 16);
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

#define ICON_PX 96             /* the type/folder icon left in the header */

static void show_icon(Preview *p, GIcon *gicon)
{
    if (gicon) {
        gtk_image_set_from_gicon(GTK_IMAGE(p->icon), gicon, GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(p->icon), ICON_PX);
        gtk_widget_show(p->icon);
    }
}

/* ---- background work ----------------------------------------------------------- */
static gboolean set_pixbuf(gpointer data)
{
    Bg *b = data;
    if (bg_current(b) && b->pixbuf) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(b->p->image), b->pixbuf);
        gtk_widget_show(b->p->sep);
    }
    bg_free(b);
    return G_SOURCE_REMOVE;
}

/* pb scaled down to fit max_w x max_h (never up); takes pb's reference */
static GdkPixbuf *fit_pixbuf(GdkPixbuf *pb, int max_w, int max_h)
{
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    if (w <= max_w && h <= max_h)
        return pb;
    double sc = MIN((double) max_w / w, (double) max_h / h);
    GdkPixbuf *s = gdk_pixbuf_scale_simple(pb, MAX(1, (int) (w * sc)), MAX(1, (int) (h * sc)),
                                           GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    return s;
}

#ifdef HAVE_WEBP
#define WEBP_MAX_BYTES (256u << 20)

static gboolean is_webp(const char *path)
{
    unsigned char h[12];
    FILE *fh = fopen(path, "rb");
    if (!fh)
        return FALSE;
    gboolean yes = fread(h, 1, 12, fh) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WEBP", 4);
    fclose(fh);
    return yes;
}

/* libwebp decodes straight to the wanted size (never up); animated WebP isn't handled: NULL */
static GdkPixbuf *load_webp(const char *path, int max_w, int max_h)
{
    gchar *data = NULL;
    gsize len = 0;
    struct stat st;
    if (g_stat(path, &st) || (guint64) st.st_size > WEBP_MAX_BYTES || !g_file_get_contents(path, &data, &len, NULL))
        return NULL;
    GdkPixbuf *pb = NULL;
    WebPDecoderConfig cfg;
    if (WebPInitDecoderConfig(&cfg) && WebPGetFeatures((const uint8_t *) data, len, &cfg.input) == VP8_STATUS_OK) {
        int w = cfg.input.width, h = cfg.input.height;
        if (w > max_w || h > max_h) {
            double sc = MIN((double) max_w / w, (double) max_h / h);
            cfg.options.use_scaling = 1;
            cfg.options.scaled_width = MAX(1, (int) (w * sc));
            cfg.options.scaled_height = MAX(1, (int) (h * sc));
        }
        cfg.output.colorspace = MODE_RGBA;
        if (WebPDecode((const uint8_t *) data, len, &cfg) == VP8_STATUS_OK) {
            int ow = cfg.output.width, oh = cfg.output.height;
            pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, ow, oh);
            if (pb) {
                guchar *dst = gdk_pixbuf_get_pixels(pb);
                int ds = gdk_pixbuf_get_rowstride(pb), ss = cfg.output.u.RGBA.stride;
                for (int y = 0; y < oh; y++)
                    memcpy(dst + (gsize) y * ds, cfg.output.u.RGBA.rgba + (gsize) y * ss, (gsize) ow * 4);
            }
            WebPFreeDecBuffer(&cfg.output);
        }
    }
    g_free(data);
    return pb;
}
#endif

static GdkPixbuf *render_ffmpeg(const char *path, int max_w, int max_h, int timeout_ms);

/* A picture no bigger than max_w x max_h; small pictures keep their own size. */
static GdkPixbuf *load_fitted(const char *path, int max_w, int max_h)
{
#ifdef HAVE_WEBP
    if (is_webp(path))
        return load_webp(path, max_w, max_h);
#endif
    int w = 0, h = 0;
    GdkPixbuf *pb = NULL;
    if (!gdk_pixbuf_get_file_info(path, &w, &h))
        return render_ffmpeg(path, max_w, max_h, 8000);     /* AVIF, HEIC, … */
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
    b->pixbuf = load_fitted(b->path, b->width, b->max_height);
    g_idle_add(set_pixbuf, b);
    return NULL;
}

/* ---- folder contents as a thumbnail grid ----------------------------------------- */
static GdkPixbuf *render_pdf(const char *path, int max_w, int max_h, int timeout_ms);
/* at least this many per row, so a narrow pane still shows the sizes apart: small 4, medium 3, large 2 */
static int thumb_min_cols(void)
{
    return LOOK.thumb_size == TSIZE_SMALL ? 4 : LOOK.thumb_size == TSIZE_LARGE ? 2 : 3;
}
/* wanted tile size (Settings → Thumbnails → Size): a wide pane gets more columns, not bigger tiles */
static int thumb_target(void)
{
    return LOOK.thumb_size == TSIZE_SMALL ? 100 : LOOK.thumb_size == TSIZE_LARGE ? 400 : 200;
}
#define THUMB_GAP 2             /* between thumbnails */
#define THUMB_PAD 2             /* around each one: room for the selection colour */
#define THUMB_MAX 5000          /* a folder with more shows the first ones */
#define THUMB_WORKERS 4         /* pictures decoded at the same time */
#define ADD_BATCH 400           /* tiles created per main-loop turn */

/* Same order as the column: folders first (if set), then names, case-insensitive. */
typedef struct {
    char *name, *key, *ct, *ext;
    gboolean dir;
    gint64 mtime;
    guint64 size;
} Entry;

static void entry_free(Entry *e)
{
    g_free(e->name);
    g_free(e->key);
    g_free(e->ct);
    g_free(e->ext);
    g_free(e);
}

/* the column's order: by name, or this folder's own */
static int by_folder_sort(gconstpointer a, gconstpointer b, gpointer data)
{
    const Entry *x = *(Entry **) a, *y = *(Entry **) b;
    return sorting_compare(*(FolderSort *) data, x->dir, y->dir, x->key, y->key, x->mtime, y->mtime,
                           x->size, y->size, x->ext, y->ext);
}

/* MEDIA_VIDEO and later may have no picture at all (handled alike: see make_thumb, the line above it) */
enum { MEDIA_NONE = -1, MEDIA_IMAGE, MEDIA_PDF, MEDIA_VIDEO, MEDIA_AUDIO, MEDIA_BLEND };

static gboolean is_exe(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_REGULAR) && g_file_test(path, G_FILE_TEST_IS_EXECUTABLE);
}

/* Blender's own helper copies the picture saved inside a .blend file out as a PNG. Blender is often
 * unpacked by hand into /opt, several versions side by side: PATH first, then /opt/blender (usually
 * the newest), then the newest /opt/blender-* by version number (4.10 after 4.2, not before it). */
char *blender_thumbnailer(void)
{
    char *f = g_find_program_in_path("blender-thumbnailer");
    if (f)
        return f;
    if (is_exe("/opt/blender/blender-thumbnailer"))
        return g_strdup("/opt/blender/blender-thumbnailer");
    GDir *d = g_dir_open("/opt", 0, NULL);
    char *best = NULL;
    for (const char *n; d && (n = g_dir_read_name(d));) {
        if (!g_str_has_prefix(n, "blender-") || (best && strverscmp(n, best) <= 0))
            continue;
        char *exe = g_build_filename("/opt", n, "blender-thumbnailer", NULL);
        if (is_exe(exe)) {
            g_free(best);
            best = g_strdup(n);
        }
        g_free(exe);
    }
    if (d)
        g_dir_close(d);
    f = best ? g_build_filename("/opt", best, "blender-thumbnailer", NULL) : NULL;
    g_free(best);
    return f;
}

/* looked up once per run (the workers ask too) */
static const char *blender_exe(void)
{
    static gsize once;
    static char *exe;
    if (g_once_init_enter(&once)) {
        exe = blender_thumbnailer();
        g_once_init_leave(&once, 1);
    }
    return exe;
}

static gboolean have_ffmpeg(void)
{
    static int have = -1;
    if (have < 0) {
        char *f = g_find_program_in_path("ffmpeg");
        have = f != NULL;
        g_free(f);
    }
    return have && LOOK.use_ffmpeg;
}

/* what kind of picture a file can have: itself, page 1, a frame, or its cover (the last two
 * only with ffmpeg) */
static int media_kind(const char *ct)
{
    if (!ct)
        return MEDIA_NONE;
    if (g_str_has_prefix(ct, "image/"))
        return MEDIA_IMAGE;
    if (!strcmp(ct, "application/pdf"))
        return LOOK.use_pdftoppm ? MEDIA_PDF : MEDIA_NONE;
    if (g_str_has_prefix(ct, "video/") && have_ffmpeg())
        return MEDIA_VIDEO;
    if (g_str_has_prefix(ct, "audio/") && have_ffmpeg())
        return MEDIA_AUDIO;
    if (!strcmp(ct, "application/x-blender") && LOOK.use_blender && blender_exe())
        return MEDIA_BLEND;
    return MEDIA_NONE;
}

static gboolean thumbable(const char *ct)
{
    return media_kind(ct) != MEDIA_NONE;
}

static GdkPixbuf *render_media(const char *path, int kind, int max_w, int max_h, int timeout_ms);

/* ---- the freedesktop thumbnail cache (~/.cache/thumbnails), shared with Thunar & co. ----
 * Looked up by hand as the spec says (md5 of the URI, Thumb::MTime must match) rather than through
 * GIO's thumbnail::path-<size> attributes: those only exist since GLib 2.76 (Slackware 15.0 has 2.70). */
static const struct { const char *dir; int px; } BUCKETS[] = {
    { "normal", 128 }, { "large", 256 }, { "x-large", 512 }, { "xx-large", 1024 },
};

/* a valid cached thumbnail at least size big, or NULL */
static GdkPixbuf *cached_thumb(const char *path, int size)
{
    struct stat st;
    char *uri = g_stat(path, &st) ? NULL : g_filename_to_uri(path, NULL, NULL);
    if (!uri)
        return NULL;
    char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    char *mtime = g_strdup_printf("%" G_GINT64_FORMAT, (gint64) st.st_mtime);
    GdkPixbuf *pb = NULL;
    for (guint k = 0; !pb && k < G_N_ELEMENTS(BUCKETS); k++) {
        if (BUCKETS[k].px < size)
            continue;
        char *name = g_strconcat(md5, ".png", NULL);
        char *tp = g_build_filename(g_get_user_cache_dir(), "thumbnails", BUCKETS[k].dir, name, NULL);
        GdkPixbuf *t = g_file_test(tp, G_FILE_TEST_IS_REGULAR) ? gdk_pixbuf_new_from_file(tp, NULL) : NULL;
        const char *tm = t ? gdk_pixbuf_get_option(t, "tEXt::Thumb::MTime") : NULL;
        const char *tu = t ? gdk_pixbuf_get_option(t, "tEXt::Thumb::URI") : NULL;
        if (tm && !strcmp(tm, mtime) && (!tu || !strcmp(tu, uri)))
            pb = fit_pixbuf(g_object_ref(t), size, size);
        g_clear_object(&t);
        g_free(tp);
        g_free(name);
    }
    g_free(mtime);
    g_free(md5);
    g_free(uri);
    return pb;
}

/* Saves pb as the bucket-size thumbnail of path, the way the spec wants it (written to a temporary
 * name, then renamed, so other programs never read half a file). */
static void save_thumb(const char *path, int k, GdkPixbuf *pb)
{
    struct stat st;
    if (g_stat(path, &st))
        return;
    char *base = g_build_filename(g_get_user_cache_dir(), "thumbnails", NULL);
    if (g_str_has_prefix(path, base)) {          /* no thumbnails of thumbnails */
        g_free(base);
        return;
    }
    char *uri = g_filename_to_uri(path, NULL, NULL);
    char *dir = g_build_filename(base, BUCKETS[k].dir, NULL);
    if (uri && g_mkdir_with_parents(dir, 0700) == 0) {
        char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
        char *final = g_strdup_printf("%s/%s.png", dir, md5);
        char *tmp = g_strdup_printf("%s/.millerx-XXXXXX", dir);
        int fd = g_mkstemp_full(tmp, O_WRONLY, 0600);
        if (fd >= 0) {
            close(fd);
            char *mt = g_strdup_printf("%" G_GINT64_FORMAT, (gint64) st.st_mtime);
            char *sz = g_strdup_printf("%" G_GINT64_FORMAT, (gint64) st.st_size);
            if (gdk_pixbuf_save(pb, tmp, "png", NULL, "tEXt::Thumb::URI", uri, "tEXt::Thumb::MTime", mt,
                                "tEXt::Thumb::Size", sz, "tEXt::Software", "millerX", NULL)
                && g_rename(tmp, final) == 0) {
                /* saved */
            } else {
                g_unlink(tmp);
            }
            g_free(mt);
            g_free(sz);
        }
        g_free(tmp);
        g_free(final);
        g_free(md5);
    }
    g_free(dir);
    g_free(uri);
    g_free(base);
}

/* A tile's picture: from the cache if one is big enough, else made at the next cache size, saved
 * there for next time (and for Thunar), and fitted to the tile. */
/* the picture for a file of this kind, fitted into px x px */
static GdkPixbuf *render_kind(const char *path, int kind, int px)
{
    switch (kind) {
    case MEDIA_PDF: return render_pdf(path, px, px, 4000);
    case MEDIA_VIDEO:
    case MEDIA_AUDIO:
    case MEDIA_BLEND: return render_media(path, kind, px, px, 8000);
    default: return load_fitted(path, px, px);
    }
}

/* files that had no picture this session (music without a cover, broken videos): not asked again
 * until they change. Key: path + mtime. */
static GHashTable *NO_PICTURE;
static GMutex NO_PICTURE_LOCK;

static char *no_picture_key(const char *path)
{
    GStatBuf st;
    return g_strdup_printf("%s\n%" G_GINT64_FORMAT, path, g_stat(path, &st) ? (gint64) 0 : (gint64) st.st_mtime);
}

static GdkPixbuf *make_thumb(const char *path, const char *ct, int size)
{
    GdkPixbuf *pb = cached_thumb(path, size);
    if (pb)
        return pb;
    int kind = media_kind(ct);
    char *key = no_picture_key(path);
    g_mutex_lock(&NO_PICTURE_LOCK);
    gboolean known_bad = NO_PICTURE && g_hash_table_contains(NO_PICTURE, key);
    g_mutex_unlock(&NO_PICTURE_LOCK);
    if (known_bad) {
        g_free(key);
        return NULL;
    }
    int k = 0;
    while (k < (int) G_N_ELEMENTS(BUCKETS) && BUCKETS[k].px < size)
        k++;
    int px = k == G_N_ELEMENTS(BUCKETS) ? size : BUCKETS[k].px;
    GdkPixbuf *big = render_kind(path, kind, px);
    if (!big && kind >= MEDIA_VIDEO) {
        g_mutex_lock(&NO_PICTURE_LOCK);
        if (!NO_PICTURE)
            NO_PICTURE = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_add(NO_PICTURE, g_strdup(key));
        g_mutex_unlock(&NO_PICTURE_LOCK);
    }
    g_free(key);
    if (!big)
        return NULL;
    if (k == G_N_ELEMENTS(BUCKETS))              /* tile bigger than any cache size: no saving */
        return big;
    save_thumb(path, k, big);
    return fit_pixbuf(big, size, size);
}

/* ---- worker pool: only tiles on screen (and one page ahead) get their picture ---------- */
static GtkWidget *thumb_for(Preview *p, const char *path);
typedef struct {
    Preview *p;
    Win *win;
    guint token;
    int index, size;
    char *path, *ct;
    GdkPixbuf *pixbuf;
    gboolean skipped;
} ThumbJob;

static gboolean thumb_done(gpointer data)
{
    ThumbJob *j = data;
    Preview *p = j->p;
    if (!j->win->dead && p->grid_token == j->token) {
        GtkFlowBoxChild *c = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(p->grid), j->index);
        if (!c || g_strcmp0(g_object_get_data(G_OBJECT(c), "path"), j->path))
            c = (GtkFlowBoxChild *) thumb_for(p, j->path);      /* tiles moved (one was deleted) */
        if (c) {
            g_object_set_data(G_OBJECT(c), "state", GINT_TO_POINTER(j->skipped ? 0 : 2));
            if (j->pixbuf)
                gtk_image_set_from_pixbuf(GTK_IMAGE(g_object_get_data(G_OBJECT(c), "img")), j->pixbuf);
        }
    }
    g_clear_object(&j->pixbuf);
    g_free(j->path);
    g_free(j->ct);
    win_unref(j->win);
    g_free(j);
    return G_SOURCE_REMOVE;
}

static void thumb_work(gpointer data, gpointer user)
{
    ThumbJob *j = data;
    Preview *p = j->p;
    gboolean current = !j->win->dead && (guint) g_atomic_int_get((gint *) &p->grid_token) == j->token;
    /* scrolled past while waiting: skip it, it is queued again when it comes back into view */
    gboolean wanted = current && j->index >= g_atomic_int_get(&p->vis_lo)
                      && j->index <= g_atomic_int_get(&p->vis_hi);
    if (wanted)
        j->pixbuf = make_thumb(j->path, j->ct, j->size);
    j->skipped = current && !wanted;
    g_idle_add(thumb_done, j);
}

static gboolean queue_visible_now(gpointer data)
{
    Preview *p = data;
    p->vis_idle = 0;
    if (p->win->dead || !gtk_widget_get_visible(p->grid_scroll) || p->tile <= 0)
        return G_SOURCE_REMOVE;
    GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->grid_scroll));
    double top = gtk_adjustment_get_value(va), page = gtk_adjustment_get_page_size(va);
    if (page < 1)
        page = 800;
    int rowh = p->tile + 2 * THUMB_PAD + THUMB_GAP;
    GtkFlowBoxChild *first = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(p->grid), 0);
    if (first && gtk_widget_get_allocated_height(GTK_WIDGET(first)) > 1)
        rowh = gtk_widget_get_allocated_height(GTK_WIDGET(first)) + THUMB_GAP;
    int lo = (int) (top / rowh) * p->cols;
    int hi = ((int) ((top + 2 * page) / rowh) + 1) * p->cols - 1;   /* the screen + one page ahead */
    g_atomic_int_set(&p->vis_lo, lo);
    g_atomic_int_set(&p->vis_hi, hi);
    for (int i = lo; i <= hi; i++) {
        GtkFlowBoxChild *c = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(p->grid), i);
        if (!c)
            break;
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "state")) != 0)
            continue;
        g_object_set_data(G_OBJECT(c), "state", GINT_TO_POINTER(1));
        ThumbJob *j = g_new0(ThumbJob, 1);
        j->p = p;
        j->win = win_ref(p->win);
        j->token = p->grid_token;
        j->index = i;
        j->size = p->tile;
        j->path = g_strdup(g_object_get_data(G_OBJECT(c), "path"));
        j->ct = g_strdup(g_object_get_data(G_OBJECT(c), "ct"));
        g_thread_pool_push(p->pool, j, NULL);
    }
    return G_SOURCE_REMOVE;
}

static void queue_visible(Preview *p)
{
    if (!p->vis_idle)
        p->vis_idle = g_idle_add(queue_visible_now, p);
}

static void on_grid_scrolled(GtkAdjustment *a, Preview *p)
{
    (void) a;
    queue_visible(p);
}

/* ---- the tiles themselves: all at once as type icons, pictures come later -------------- */
static gboolean add_batch(gpointer data)
{
    Preview *p = data;
    if (p->win->dead || !p->pending) {
        p->add_idle = 0;
        return G_SOURCE_REMOVE;
    }
    for (int k = 0; k < ADD_BATCH && p->pending_pos < p->pending->len; k++, p->pending_pos++) {
        Entry *e = g_ptr_array_index(p->pending, p->pending_pos);
        GIcon *ic = g_content_type_get_icon(e->ct);
        GtkWidget *img = gtk_image_new_from_gicon(ic, GTK_ICON_SIZE_DIALOG);
        g_object_unref(ic);
        gtk_image_set_pixel_size(GTK_IMAGE(img), p->tile / 2);
        gtk_widget_set_size_request(img, p->tile, p->tile);
        char *disp = g_filename_display_name(e->name);
        GtkWidget *child = gtk_flow_box_child_new();
        gtk_widget_set_tooltip_text(child, disp);
        g_object_set_data(G_OBJECT(child), "img", img);
        if (LOOK.gallery_names) {
            /* one line, cut in the middle so the extension stays: long-na…me.jpg */
            GtkWidget *l = gtk_label_new(disp);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
            gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
            gtk_widget_set_size_request(l, p->tile, -1);
            gtk_widget_set_opacity(l, 0.8);
            PangoAttrList *at = pango_attr_list_new();
            pango_attr_list_insert(at, pango_attr_scale_new(0.85));
            gtk_label_set_attributes(GTK_LABEL(l), at);
            pango_attr_list_unref(at);
            g_object_set_data(G_OBJECT(child), "label", l);
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(child), box);
        } else {
            gtk_container_add(GTK_CONTAINER(child), img);
        }
        g_free(disp);
        g_object_set_data_full(G_OBJECT(child), "path", g_build_filename(p->pending_dir, e->name, NULL), g_free);
        g_object_set_data_full(G_OBJECT(child), "ct", g_strdup(e->ct), g_free);
        g_object_set_data(G_OBJECT(child), "state", GINT_TO_POINTER(!e->dir && !p->grid_icons && thumbable(e->ct) ? 0 : 2));
        gtk_container_add(GTK_CONTAINER(p->grid), child);
        gtk_widget_show_all(child);
    }
    if (p->pending->len) {
        gtk_widget_show(p->sep);
        gtk_widget_show(p->grid_scroll);
    }
    queue_visible(p);
    if (p->pending_pos < p->pending->len)
        return G_SOURCE_CONTINUE;
    g_clear_pointer(&p->pending, g_ptr_array_unref);
    g_clear_pointer(&p->pending_dir, g_free);
    p->add_idle = 0;
    return G_SOURCE_REMOVE;
}

typedef struct {
    Preview *p;
    Win *win;
    guint token;
    char *dir;
    gboolean hidden;
    FolderSort sort;
    GPtrArray *entries;
} ListJob;

static gboolean list_done(gpointer data)
{
    ListJob *j = data;
    Preview *p = j->p;
    if (!j->win->dead && p->grid_token == j->token) {
        if (p->add_idle)
            g_source_remove(p->add_idle);
        g_clear_pointer(&p->pending, g_ptr_array_unref);
        g_free(p->pending_dir);
        p->pending = j->entries;
        j->entries = NULL;
        p->pending_dir = g_strdup(j->dir);
        p->pending_pos = 0;
        p->add_idle = g_idle_add(add_batch, p);
    }
    if (j->entries)
        g_ptr_array_unref(j->entries);
    win_unref(j->win);
    g_free(j->dir);
    g_free(j);
    return G_SOURCE_REMOVE;
}

/* reads and sorts the folder; names and types only (by name, no file is opened). Files only: a
 * folder is never an end point, and its tile (the same icon every time) only pushes the files
 * you're looking for further down; a folder of folders shows its details and no gallery */
static gpointer list_thread(gpointer data)
{
    ListJob *j = data;
    GPtrArray *names = g_ptr_array_new_with_free_func((GDestroyNotify) entry_free);
    GDir *d = g_dir_open(j->dir, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name(d)))
            if (j->hidden || n[0] != '.') {
                Entry *e = g_new0(Entry, 1);
                e->name = g_strdup(n);
                char *full = g_build_filename(j->dir, n, NULL);
                GStatBuf st;
                if (g_stat(full, &st) == 0) {
                    e->dir = S_ISDIR(st.st_mode);
                    e->mtime = st.st_mtime;
                    e->size = st.st_size;
                }
                g_free(full);
                if (e->dir) {               /* files only */
                    entry_free(e);
                    continue;
                }
                e->ext = e->dir ? g_strdup("") : sorting_ext(n);
                e->ct = e->dir ? g_strdup("inode/directory") : g_content_type_guess(n, NULL, 0, NULL);
                char *disp = g_filename_display_name(n), *fold = g_utf8_casefold(disp, -1);
                e->key = g_utf8_collate_key_for_filename(fold, -1);
                g_free(disp);
                g_free(fold);
                g_ptr_array_add(names, e);
            }
        g_dir_close(d);
    }
    g_ptr_array_sort_with_data(names, by_folder_sort, &j->sort);
    if (names->len > THUMB_MAX)
        g_ptr_array_set_size(names, THUMB_MAX);
    j->entries = names;
    g_idle_add(list_done, j);
    return NULL;
}

/* Started a moment after a folder is shown, so stepping through folders with the keys loads nothing. */
static gboolean start_grid(gpointer data);
static void start_sizes(Preview *p, GPtrArray *paths, GtkWidget *size_l, GtkWidget *count_l,
                        const char *single_dir);

/* a folder's size count and gallery, started once its column is filled */
typedef struct {
    Preview *p;
    char *dir;
    GtkWidget *size_l, *count_l;    /* refs */
    int waited;                     /* quarter seconds */
} Later;

static void later_free(gpointer data)
{
    Later *l = data;
    g_free(l->dir);
    g_object_unref(l->size_l);
    g_object_unref(l->count_l);
    g_free(l);
}

/* the folder's column is still reading its names? */
static gboolean column_busy(Preview *p)
{
    if (!p->current)
        return FALSE;
    for (GList *c = p->win->columns; c; c = c->next) {
        Column *col = c->data;
        if (col->gfile && g_file_equal(col->gfile, p->current))
            return !col->loaded;
    }
    return FALSE;
}

static gboolean folder_ready(gpointer data)
{
    Later *l = data;
    Preview *p = l->p;
    if (column_busy(p) && ++l->waited < 40)         /* wait for it, ten seconds at most */
        return G_SOURCE_CONTINUE;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(paths, g_strdup(l->dir));
    start_sizes(p, paths, l->size_l, l->count_l, l->dir);
    start_grid(p);                                  /* clears p->grid_timer: this one ends now */
    return G_SOURCE_REMOVE;
}

static gboolean start_grid(gpointer data)
{
    Preview *p = data;
    p->grid_timer = 0;
    char *dir = p->current ? g_file_get_path(p->current) : NULL;
    if (!dir)
        return G_SOURCE_REMOVE;
    /* the pane is often wider than preview_width (the window gives it the rest): use what it has */
    int avail = gtk_widget_get_allocated_width(p->box);
    if (avail < 100)
        avail = p->width - 24;
    p->grid_w = avail;
    p->grid_icons = LOOK.thumbs == THUMBS_NEVER || (LOOK.thumbs == THUMBS_LOCAL && path_on_network(dir));
    p->cols = MAX(thumb_min_cols(), (avail + THUMB_GAP) / (thumb_target() + THUMB_GAP + 2 * THUMB_PAD));
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(p->grid), p->cols);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(p->grid), p->cols);
    p->tile = (avail - (p->cols - 1) * THUMB_GAP) / p->cols - 2 * THUMB_PAD;
    g_atomic_int_set(&p->vis_lo, 0);
    g_atomic_int_set(&p->vis_hi, -1);
    GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->grid_scroll));
    gtk_adjustment_set_value(va, 0);
    ListJob *j = g_new0(ListJob, 1);
    j->p = p;
    j->win = win_ref(p->win);
    j->token = p->grid_token;
    j->dir = dir;
    j->hidden = p->win->show_hidden;
    sorting_get(dir, &j->sort);
    g_thread_unref(g_thread_new("millerx-grid", list_thread, j));
    return G_SOURCE_REMOVE;
}

/* Stops loading and watching the grid's folder. */
static void grid_stop(Preview *p)
{
    g_atomic_int_inc((gint *) &p->grid_token);
    if (p->add_idle) {
        g_source_remove(p->add_idle);
        p->add_idle = 0;
    }
    if (p->vis_idle) {
        g_source_remove(p->vis_idle);
        p->vis_idle = 0;
    }
    g_clear_pointer(&p->pending, g_ptr_array_unref);
    g_clear_pointer(&p->pending_dir, g_free);
    if (p->grid_timer) {
        g_source_remove(p->grid_timer);
        p->grid_timer = 0;
    }
    if (p->grid_refresh) {
        g_source_remove(p->grid_refresh);
        p->grid_refresh = 0;
    }
    g_clear_object(&p->dir_size_l);
    g_clear_object(&p->dir_count_l);
    if (p->grid_monitor) {
        g_signal_handlers_disconnect_by_data(p->grid_monitor, p);
        g_file_monitor_cancel(p->grid_monitor);
        g_clear_object(&p->grid_monitor);
    }
}

static GtkWidget *thumb_for(Preview *p, const char *path)
{
    GtkWidget *found = NULL;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(p->grid));
    for (GList *l = kids; l && !found; l = l->next)
        if (!g_strcmp0(g_object_get_data(G_OBJECT(l->data), "path"), path))
            found = l->data;
    g_list_free(kids);
    return found;
}

static void grid_empty_check(Preview *p)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(p->grid));
    if (!kids) {                  /* last one gone: no line, no grid */
        gtk_widget_hide(p->grid_scroll);
        gtk_widget_hide(p->sep);
    }
    g_list_free(kids);
}

/* something new in the folder: load the grid again (the scroll position can't be kept then) */
static gboolean grid_reload(gpointer data)
{
    Preview *p = data;
    p->grid_refresh = 0;
    g_atomic_int_inc((gint *) &p->grid_token);
    if (p->add_idle) {
        g_source_remove(p->add_idle);
        p->add_idle = 0;
    }
    g_clear_pointer(&p->pending, g_ptr_array_unref);
    g_clear_pointer(&p->pending_dir, g_free);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(p->grid));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    start_grid(p);
    return G_SOURCE_REMOVE;
}

/* Gone or renamed thumbnails are changed in place, so the grid doesn't jump back to the top.
 * The folder's size and items are not counted again: a busy folder (slackpkg, a download, a
 * build) would recount its whole tree over and over. They are counted when the folder is
 * opened and on F5. */
static void on_grid_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, Preview *p)
{
    (void) m;
    char *path = g_file_get_path(f);
    GtkWidget *t = path ? thumb_for(p, path) : NULL;
    switch (ev) {
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        if (t) {
            gtk_widget_destroy(t);
            grid_empty_check(p);
        }
        break;
    case G_FILE_MONITOR_EVENT_RENAMED: {
        char *np = other ? g_file_get_path(other) : NULL, *base = other ? g_file_get_basename(other) : NULL;
        if (t && np && (base[0] != '.' || p->win->show_hidden)) {
            g_object_set_data_full(G_OBJECT(t), "path", g_strdup(np), g_free);
            char *disp = g_filename_display_name(base);
            gtk_widget_set_tooltip_text(t, disp);
            GtkWidget *l = g_object_get_data(G_OBJECT(t), "label");
            if (l)
                gtk_label_set_text(GTK_LABEL(l), disp);
            g_free(disp);
        } else if (t) {
            gtk_widget_destroy(t);
            grid_empty_check(p);
        } else if (!p->grid_refresh) {
            p->grid_refresh = g_timeout_add(400, grid_reload, p);
        }
        g_free(np);
        g_free(base);
        break;
    }
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        if (!p->grid_refresh)
            p->grid_refresh = g_timeout_add(400, grid_reload, p);
        break;
    default:
        break;
    }
    g_free(path);
}

static void grid_watch(Preview *p)
{
    p->grid_monitor = g_file_monitor_directory(p->current, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (p->grid_monitor)
        g_signal_connect(p->grid_monitor, "changed", G_CALLBACK(on_grid_changed), p);
}

static GFile *thumb_file(GtkFlowBoxChild *child)
{
    return g_file_new_for_path(g_object_get_data(G_OBJECT(child), "path"));
}

static int by_index(gconstpointer a, gconstpointer b)
{
    return gtk_flow_box_child_get_index((GtkFlowBoxChild *) a) - gtk_flow_box_child_get_index((GtkFlowBoxChild *) b);
}

/* the selected thumbnails, in the order they are shown */
static GList *thumbs_selected(Preview *p)
{
    return g_list_sort(gtk_flow_box_get_selected_children(GTK_FLOW_BOX(p->grid)), by_index);
}

/* their files (GFile*, owned by the caller) */
static GList *thumbs_files(GList *thumbs)
{
    GList *files = NULL;
    for (GList *l = thumbs; l; l = l->next)
        files = g_list_prepend(files, thumb_file(l->data));
    return g_list_reverse(files);
}

static GtkFlowBoxChild *thumb_by_path(Preview *p, const char *path)
{
    GtkFlowBoxChild *c = NULL;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(p->grid));
    for (GList *k = kids; k && !c; k = k->next)
        if (!g_strcmp0(g_object_get_data(G_OBJECT(k->data), "path"), path))
            c = k->data;
    g_list_free(kids);
    return c;
}

/* what is picked in the gallery is highlighted in the folder's column too, so you see where it is */
static void on_thumbs_picked(GtkFlowBox *fb, Preview *p)
{
    if (!p->current)
        return;
    for (GList *l = p->win->columns; l; l = l->next) {
        Column *c = l->data;
        if (!c->closed && !c->computer && g_file_equal(c->gfile, p->current)) {
            GList *sel = thumbs_selected(p), *files = thumbs_files(sel);
            column_mark_files(c, files);
            g_list_free_full(files, g_object_unref);
            g_list_free(sel);
            break;
        }
    }
}

/* an event's position in the grid's own coordinates (events may come from a child's window) */
static void grid_pos(GtkWidget *grid, GdkWindow *win, double ex, double ey, int *x, int *y)
{
    GtkWidget *from = NULL;
    gdk_window_get_user_data(win, (gpointer *) &from);
    *x = (int) ex;
    *y = (int) ey;
    if (from && from != grid)
        gtk_widget_translate_coordinates(from, grid, *x, *y, x, y);
}

/* ---- dragging thumbnails out, like rows in a column (a file list, text/uri-list) ---- */
static gboolean on_grid_motion(GtkWidget *grid, GdkEventMotion *ev, Preview *p)
{
    if (!p->drag_path || !(ev->state & GDK_BUTTON1_MASK))
        return FALSE;
    int x, y;
    grid_pos(grid, ev->window, ev->x, ev->y, &x, &y);
    if (!gtk_drag_check_threshold(grid, p->press_x, p->press_y, x, y))
        return FALSE;
    /* a selected thumbnail takes the whole selection along, any other just itself */
    GtkFlowBoxChild *c = thumb_by_path(p, p->drag_path);
    g_list_free_full(p->drag_files, g_object_unref);
    p->drag_files = NULL;
    if (c && gtk_flow_box_child_is_selected(c)) {
        GList *sel = thumbs_selected(p);
        p->drag_files = thumbs_files(sel);
        g_list_free(sel);
    } else {
        p->drag_files = g_list_append(NULL, g_file_new_for_path(p->drag_path));
    }
    GtkTargetList *tl = gtk_target_list_new(NULL, 0);
    gtk_target_list_add_uri_targets(tl, 0);
    gtk_drag_begin_with_coordinates(grid, tl, GDK_ACTION_COPY | GDK_ACTION_MOVE, 1, (GdkEvent *) ev,
                                    p->press_x, p->press_y);
    gtk_target_list_unref(tl);
    return TRUE;
}

static void on_grid_drag_begin(GtkWidget *grid, GdkDragContext *ctx, Preview *p)
{
    (void) grid;
    GtkFlowBoxChild *c = thumb_by_path(p, p->drag_path);
    GtkImage *img = c ? g_object_get_data(G_OBJECT(c), "img") : NULL;
    if (img && gtk_image_get_storage_type(img) == GTK_IMAGE_PIXBUF)   /* the thumbnail under the mouse */
        gtk_drag_set_icon_pixbuf(ctx, gtk_image_get_pixbuf(img), 0, 0);
    else
        gtk_drag_set_icon_name(ctx, p->drag_files && p->drag_files->next ? "edit-copy" : "text-x-generic", 0, 0);
}

static void on_grid_drag_get(GtkWidget *grid, GdkDragContext *ctx, GtkSelectionData *data, guint info,
                             guint t, Preview *p)
{
    (void) grid; (void) ctx; (void) info; (void) t;
    guint n = g_list_length(p->drag_files), i = 0;
    if (!n)
        return;
    char **uris = g_new0(char *, n + 1);
    for (GList *l = p->drag_files; l; l = l->next)
        uris[i++] = g_file_get_uri(l->data);
    gtk_selection_data_set_uris(data, uris);
    g_strfreev(uris);
}

static void on_grid_drag_end(GtkWidget *grid, GdkDragContext *ctx, Preview *p)
{
    (void) grid; (void) ctx;
    g_clear_pointer(&p->drag_path, g_free);
    g_list_free_full(p->drag_files, g_object_unref);
    p->drag_files = NULL;
}

static gboolean on_grid_release(GtkWidget *grid, GdkEventButton *ev, Preview *p)
{
    (void) ev;
    if (!gtk_drag_get_source_widget(NULL)) {         /* no drag going on: the press was just a click */
        if (p->press_keep && p->drag_path) {         /* … on one of several selected: now just that one */
            GtkFlowBoxChild *c = thumb_by_path(p, p->drag_path);
            gtk_flow_box_unselect_all(GTK_FLOW_BOX(grid));
            if (c)
                gtk_flow_box_select_child(GTK_FLOW_BOX(grid), c);
        }
        g_clear_pointer(&p->drag_path, g_free);
    }
    p->press_keep = FALSE;
    return FALSE;
}

/* Right-click on a thumbnail: the menu for the selection (a thumbnail outside it becomes the
 * selection first). Button 1 only remembers the thumbnail, for a drag. */
static gboolean on_grid_button(GtkWidget *grid, GdkEventButton *ev, Preview *p)
{
    int x, y;
    grid_pos(grid, ev->window, ev->x, ev->y, &x, &y);
    if (ev->type == GDK_BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        GtkFlowBoxChild *c = gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(grid), x, y);
        g_free(p->drag_path);
        p->drag_path = c ? g_strdup(g_object_get_data(G_OBJECT(c), "path")) : NULL;
        p->press_x = x;
        p->press_y = y;
        /* pressing a thumbnail of a bigger selection keeps it (to drag them all); the flow box
         * would otherwise select just that one right away */
        if (c && gtk_flow_box_child_is_selected(c)
            && !(ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
            GList *sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(grid));
            gboolean many = sel && sel->next;
            g_list_free(sel);
            if (many) {
                gtk_widget_grab_focus(GTK_WIDGET(c));
                p->press_keep = TRUE;
                return TRUE;
            }
        }
        p->press_keep = FALSE;
        return FALSE;              /* selecting and double-click work as before */
    }
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkFlowBoxChild *child = gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(grid), x, y);
    if (!child)
        return FALSE;
    if (!gtk_flow_box_child_is_selected(child)) {
        gtk_flow_box_unselect_all(GTK_FLOW_BOX(grid));
        gtk_flow_box_select_child(GTK_FLOW_BOX(grid), child);
    }
    gtk_widget_grab_focus(GTK_WIDGET(child));
    GList *sel = thumbs_selected(p), *files = thumbs_files(sel);
    win_file_menu(p->win, files, ev, grid);
    g_list_free_full(files, g_object_unref);
    g_list_free(sel);
    return TRUE;
}

/* keys on the selected thumbnails: menu key, Ctrl+C/X, F2, Delete, Shift+Delete, Alt+Return */
static gboolean on_grid_key(GtkWidget *grid, GdkEventKey *ev, Preview *p)
{
    GList *sel = thumbs_selected(p);
    if (!sel)
        return FALSE;
    gboolean shift = ev->state & GDK_SHIFT_MASK, alt = ev->state & GDK_MOD1_MASK;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK;
    guint k = ev->keyval;
    GList *files = thumbs_files(sel);
    gboolean one = !files->next, done = TRUE;
    if (k == GDK_KEY_Menu || (shift && k == GDK_KEY_F10))
        win_file_menu(p->win, files, NULL, GTK_WIDGET(sel->data));
    else if (ctrl && !shift && (k == GDK_KEY_c || k == GDK_KEY_C || k == GDK_KEY_x || k == GDK_KEY_X))
        win_clip_set(p->win, g_list_copy_deep(files, obj_copy, NULL), k == GDK_KEY_x || k == GDK_KEY_X);
    else if (k == GDK_KEY_F2 && one)
        win_rename_file(p->win, files->data);
    else if (k == GDK_KEY_Delete && shift)
        win_delete_forever(p->win, files);
    else if (k == GDK_KEY_Delete)
        win_trash(p->win, files);
    else if (alt && (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter))
        one ? dialog_properties(p->win, files->data) : dialog_properties_multi(p->win, files);
    else
        done = FALSE;
    (void) grid;
    g_list_free_full(files, g_object_unref);
    g_list_free(sel);
    return done;
}

/* ---- the picture area follows the pane's size ----------------------------------------- */
static gpointer pdf_thread(gpointer data);
static gpointer media_thread(gpointer data);
static gboolean image_load(gpointer data)
{
    Preview *p = data;
    p->image_timer = 0;
    if (!p->image_path)
        return G_SOURCE_REMOVE;
    p->image_w = gtk_widget_get_allocated_width(p->image_scroll);
    p->image_h = gtk_widget_get_allocated_height(p->image_scroll);
    if (p->image_w < 64)                         /* not laid out yet */
        p->image_w = p->width - 24;
    if (p->image_h < 64)
        p->image_h = 400;
    Bg *b = bg_new(p, p->image_path);
    b->media = p->image_kind;          /* read on the main thread, used on the worker */
    spawn_bg(b, p->image_kind == MEDIA_PDF ? pdf_thread : p->image_kind >= MEDIA_VIDEO ? media_thread : image_thread);
    return G_SOURCE_REMOVE;
}

static void image_schedule(Preview *p, guint ms)
{
    if (p->image_timer)
        g_source_remove(p->image_timer);
    p->image_timer = g_timeout_add(ms, image_load, p);
}

static void on_image_area(GtkWidget *wid, GdkRectangle *a, Preview *p)
{
    (void) wid;
    if (p->image_path && (abs(a->width - p->image_w) >= 8 || abs(a->height - p->image_h) >= 8))
        image_schedule(p, 150);
}

static gboolean grid_resized(gpointer data)
{
    Preview *p = data;
    p->resize_timer = 0;
    if (gtk_widget_get_visible(p->grid_scroll))
        grid_reload(p);
    return G_SOURCE_REMOVE;
}

/* the thumbnails are made for one width: a clearly different one makes them again */
/* Narrow pane: the icon goes above the details, so they get the whole width instead of being
 * squeezed into a few letters per line. Two thresholds, so it doesn't flip back and forth. */
#define HEADER_NARROW 440
#define HEADER_WIDE 480

static gboolean header_apply(gpointer data)
{
    Preview *p = data;
    p->header_idle = 0;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(p->header),
                                   p->header_narrow ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_halign(p->icon, p->header_narrow ? GTK_ALIGN_START : GTK_ALIGN_FILL);
    return G_SOURCE_REMOVE;
}

static void on_pane_size(GtkWidget *wid, GdkRectangle *a, Preview *p)
{
    (void) wid;
    gboolean narrow = p->header_narrow ? a->width < HEADER_WIDE : a->width < HEADER_NARROW;
    if (narrow != p->header_narrow) {
        p->header_narrow = narrow;          /* changed after this layout pass, not inside it */
        if (!p->header_idle)
            p->header_idle = g_idle_add(header_apply, p);
    }
    if (p->grid_w && abs(a->width - p->grid_w) >= 12 && gtk_widget_get_visible(p->grid_scroll)) {
        if (p->resize_timer)
            g_source_remove(p->resize_timer);
        p->resize_timer = g_timeout_add(300, grid_resized, p);
    }
}

/* double-click (or Enter) on a thumbnail: go to that file */
static void on_thumb_activated(GtkFlowBox *box, GtkFlowBoxChild *child, Preview *p)
{
    (void) box;
    GFile *f = g_file_new_for_path(g_object_get_data(G_OBJECT(child), "path"));
    win_open_path(p->win, f);
    g_object_unref(f);
}

/* Page 1 of a PDF, fitted into max_w x max_h; pdftoppm (poppler) gets timeout_ms, then is killed. */
/* Runs argv (which writes png), waits up to timeout_ms (then kills it) and loads the result
 * fitted into max_w x max_h. */
static GdkPixbuf *run_to_png(char **argv, const char *png, int max_w, int max_h, int timeout_ms)
{
    GdkPixbuf *pb = NULL;
    GPid pid;
    if (g_spawn_async(NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL
                      | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &pid, NULL)) {
        int status = -1;
        for (int i = 0; i < timeout_ms / 50; i++) {
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
            pb = gdk_pixbuf_new_from_file(png, NULL);
        if (pb)
            pb = fit_pixbuf(pb, max_w, max_h);
    }
    g_unlink(png);
    return pb;
}

/* Page 1 of a PDF, fitted into max_w x max_h; pdftoppm (poppler). */
static GdkPixbuf *render_pdf(const char *path, int max_w, int max_h, int timeout_ms)
{
    char *dir = g_dir_make_tmp("millerx-XXXXXX", NULL);
    if (!dir)
        return NULL;
    char *out = g_build_filename(dir, "p", NULL);
    char *png = g_strconcat(out, ".png", NULL);
    char *w = g_strdup_printf("%d", MAX(max_w, max_h));
    char *argv[] = { (char *) "pdftoppm", (char *) "-f", (char *) "1", (char *) "-l", (char *) "1",
                     (char *) "-png", (char *) "-singlefile", (char *) "-scale-to", w, (char *) path, out, NULL };
    GdkPixbuf *pb = run_to_png(argv, png, max_w, max_h, timeout_ms);
    g_rmdir(dir);
    g_free(w);
    g_free(png);
    g_free(out);
    g_free(dir);
    return pb;
}

/* Pictures gdk-pixbuf can't read (AVIF, HEIC, …) through ffmpeg, if it is installed: scaled
 * down to fit (never up), first frame only. */
static GdkPixbuf *ffmpeg_frame(const char *path, const char *seek, int max_w, int max_h, int timeout_ms);

static GdkPixbuf *render_ffmpeg(const char *path, int max_w, int max_h, int timeout_ms)
{
    return ffmpeg_frame(path, NULL, max_w, max_h, timeout_ms);
}

/* A video's frame a few seconds in (the very start is often black; short clips fall back to
 * the first frame), or a music file's embedded cover (ffmpeg shows it as the video stream). */
static GdkPixbuf *render_blend(const char *path, int max_w, int max_h, int timeout_ms);

static GdkPixbuf *render_media(const char *path, int kind, int max_w, int max_h, int timeout_ms)
{
    if (kind == MEDIA_BLEND)
        return render_blend(path, max_w, max_h, timeout_ms);
    GdkPixbuf *pb = kind == MEDIA_VIDEO ? ffmpeg_frame(path, "3", max_w, max_h, timeout_ms) : NULL;
    return pb ? pb : ffmpeg_frame(path, NULL, max_w, max_h, timeout_ms);
}

/* one frame through ffmpeg, scaled down to fit (never up); seek = seconds into it, or NULL */
static GdkPixbuf *ffmpeg_frame(const char *path, const char *seek, int max_w, int max_h, int timeout_ms)
{
    if (!have_ffmpeg())
        return NULL;
    char *dir = g_dir_make_tmp("millerx-XXXXXX", NULL);
    if (!dir)
        return NULL;
    char *png = g_build_filename(dir, "p.png", NULL);
    char *in = g_strconcat("file:", path, NULL);          /* never read as an ffmpeg protocol */
    char *vf = g_strdup_printf("scale='min(%d,iw)':'min(%d,ih)':force_original_aspect_ratio=decrease",
                               max_w, max_h);
    /* no -ss at all without a seek: even "-ss 0" skips a music file's embedded cover */
    const char *argv[20];
    int n = 0;
    for (const char *a0[] = { "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", NULL }, **a = a0; *a; a++)
        argv[n++] = *a;
    if (seek) {
        argv[n++] = "-ss";
        argv[n++] = seek;
    }
    for (const char *a1[] = { "-i", in, "-map", "0:v:0", "-frames:v", "1", "-vf", vf, "-y", png, NULL }, **a = a1;
         *a; a++)
        argv[n++] = *a;
    argv[n] = NULL;
    GdkPixbuf *pb = run_to_png((char **) argv, png, max_w, max_h, timeout_ms);
    g_rmdir(dir);
    g_free(vf);
    g_free(in);
    g_free(png);
    g_free(dir);
    return pb;
}

/* A .blend file's saved picture (files saved without one have none: NULL). */
static GdkPixbuf *render_blend(const char *path, int max_w, int max_h, int timeout_ms)
{
    const char *exe = blender_exe();
    char *dir = exe ? g_dir_make_tmp("millerx-XXXXXX", NULL) : NULL;
    if (!dir)
        return NULL;
    char *png = g_build_filename(dir, "b.png", NULL);
    char *argv[] = { (char *) exe, (char *) path, png, NULL };
    GdkPixbuf *pb = run_to_png(argv, png, max_w, max_h, timeout_ms);
    g_rmdir(dir);
    g_free(png);
    g_free(dir);
    return pb;
}

static gpointer media_thread(gpointer data)
{
    Bg *b = data;
    b->pixbuf = render_media(b->path, b->media, b->width, b->max_height, 15000);
    g_idle_add(set_pixbuf, b);
    return NULL;
}

static gpointer pdf_thread(gpointer data)
{
    Bg *b = data;
    b->pixbuf = render_pdf(b->path, b->width, b->max_height, 15000);
    g_idle_add(set_pixbuf, b);
    return NULL;
}

static gboolean set_text(gpointer data)
{
    Bg *b = data;
    if (bg_current(b) && b->text && *b->text) {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(b->p->text)), b->text, -1);
        gtk_widget_show(b->p->sep);
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
                b->text = g_strdup_printf(ngettext("%llu entry:\n%s… and %llu more",
                                                   "%llu entries:\n%s… and %llu more",
                                                   total), (unsigned long long) total, body, (unsigned long long) (total - shown));
            else
                b->text = g_strdup_printf(ngettext("%llu entry:\n%s", "%llu entries:\n%s",
                                                   total), (unsigned long long) total, body);
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
    if (bg_current(b) && b->gen == (guint) g_atomic_int_get((gint *) &b->p->sizes_gen)) {
        char *size = format_size_full(b->total);
        char *txt = g_strconcat(size, b->running ? " …" : "", NULL);
        gtk_label_set_text(GTK_LABEL(b->size_label), txt);
        g_free(txt);
        g_free(size);
        char *cnt = b->single_dir
            ? g_strdup_printf(ngettext("%llu here, %llu file in total%s",
                                       "%llu here, %llu files in total%s", b->files),
                              (unsigned long long) b->items, (unsigned long long) b->files, b->running ? " …" : "")
            : g_strdup_printf(ngettext("%llu file in total%s", "%llu files in total%s",
                                       b->files), (unsigned long long) b->files, b->running ? " …" : "");
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
    u->gen = b->gen;
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
        if (!bg_current(b) || b->gen != (guint) g_atomic_int_get((gint *) &b->p->sizes_gen))
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
    b->gen = (guint) g_atomic_int_add((gint *) &p->sizes_gen, 1) + 1;   /* older counts stop writing */
    spawn_bg(b, sizes_thread);
}

/* ---- public ------------------------------------------------------------------------ */
void preview_show_many(Preview *p, GList *files)
{
    preview_clear(p);
    GIcon *ic = g_themed_icon_new("edit-copy");
    show_icon(p, ic);
    g_object_unref(ic);
    char *m = g_strdup_printf(ngettext("<b>%u item selected</b>", "<b>%u items selected</b>", g_list_length(files)),
                              g_list_length(files));
    gtk_label_set_markup(GTK_LABEL(p->title), m);
    g_free(m);
    GtkWidget *size_l = row(p, 0, _("Size"), "…");
    GtkWidget *count_l = row(p, 1, _("Files"), "…");
    gtk_widget_show_all(p->details);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = files; l; l = l->next) {
        char *path = g_file_get_path(l->data);
        if (path)
            g_ptr_array_add(paths, path);
    }
    start_sizes(p, paths, size_l, count_l, NULL);
}

/* How full a disk is, in the highlight colour (yours or the theme's); red above 95 %. Not GTK's own
 * levels: those are made for batteries, where little is bad and a nearly full disk looks fine. */
GtkWidget *disk_bar(double used)
{
    static GtkCssProvider *css;
    if (!css) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "levelbar block.mx-ok { background-color: @theme_selected_bg_color;"
            " border-color: @theme_selected_bg_color; background-image: none; }\n"
            "levelbar block.mx-full { background-color: #e01b24; border-color: #e01b24;"
            " background-image: none; }\n", -1, NULL);
    }
    GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
    GtkLevelBar *lb = GTK_LEVEL_BAR(bar);
    gtk_level_bar_remove_offset_value(lb, GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(lb, GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(lb, GTK_LEVEL_BAR_OFFSET_FULL);
    gtk_level_bar_add_offset_value(lb, "mx-ok", 0.95);
    gtk_level_bar_add_offset_value(lb, "mx-full", 1.0);
    gtk_level_bar_set_value(lb, used);
    gtk_style_context_add_provider(gtk_widget_get_style_context(bar), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    return bar;
}

/* a disk in the Computer column: where it is, what it is, how full; never its contents (counting
 * a whole disk would take ages). Free space only for local disks: a dead share must not hang us. */
void preview_show_disk(Preview *p, const char *name, GIcon *icon, GFile *root, const char *where,
                       const char *device, const char *fs, const char *note)
{
    preview_clear(p);
    show_icon(p, icon);
    char *m = g_markup_printf_escaped("<b>%s</b>", name ? name : "");
    gtk_label_set_markup(GTK_LABEL(p->title), m);
    g_free(m);
    int r = 0;
    if (note)
        row(p, r++, "", note);
    if (where && *where)
        row(p, r++, _("Where"), where);
    if (device && *device)
        row(p, r++, _("Device"), device);
    if (fs && *fs)
        row(p, r++, _("File system"), fs);
    char *path = root ? g_file_get_path(root) : NULL;
    if (path && !path_on_network(path)) {
        GFileInfo *fi = g_file_query_filesystem_info(root, G_FILE_ATTRIBUTE_FILESYSTEM_FREE ","
                                                     G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, NULL, NULL);
        if (fi && g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
            guint64 size = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
            guint64 free_ = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
            char *fr = g_format_size(free_), *sz = g_format_size(size);
            char *t = g_strdup_printf(_("%s free of %s"), fr, sz);
            row(p, r++, _("Space"), t);
            if (size) {
                double used = 1.0 - (double) free_ / (double) size;
                GtkWidget *k = gtk_label_new(_("Used"));
                gtk_label_set_xalign(GTK_LABEL(k), 1);
                gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
                GtkWidget *bar = disk_bar(used);
                gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
                gtk_widget_set_hexpand(bar, TRUE);
                char *pc = g_strdup_printf("%.0f %%", used * 100);
                GtkWidget *pl = gtk_label_new(pc);
                GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_box_pack_start(GTK_BOX(box), bar, TRUE, TRUE, 0);
                gtk_box_pack_start(GTK_BOX(box), pl, FALSE, FALSE, 0);
                gtk_grid_attach(GTK_GRID(p->details), k, 0, r, 1, 1);
                gtk_grid_attach(GTK_GRID(p->details), box, 1, r++, 1, 1);
                g_free(pc);
            }
            g_free(t);
            g_free(fr);
            g_free(sz);
        }
        g_clear_object(&fi);
    }
    g_free(path);
    gtk_widget_show_all(p->details);
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
        gtk_widget_show(p->sep);
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
    row(p, r++, _("Type"), desc);
    g_free(desc);
    char *sz = isdir ? g_strdup("…") : format_size_full(g_file_info_get_size(info));
    GtkWidget *size_l = row(p, r++, _("Size"), sz);
    g_free(sz);
    GtkWidget *count_l = isdir ? row(p, r++, _("Items"), "…") : NULL;
    GDateTime *mt = g_file_info_get_modification_date_time(info);
    if (mt) {
        GDateTime *loc = g_date_time_to_local(mt);
        char *s = g_date_time_format(loc, "%Y-%m-%d %H:%M");
        row(p, r++, _("Modified"), s);
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
        row(p, r++, _("Permissions"), s);
    }
    char *path = g_file_get_path(f);
    GFileInfo *ti = file_in_trash(f) ? g_file_query_info(f, "trash::orig-path,trash::deletion-date,standard::target-uri",
                                                         G_FILE_QUERY_INFO_NONE, NULL, NULL) : NULL;
    const char *orig = ti ? g_file_info_get_attribute_byte_string(ti, "trash::orig-path") : NULL;
    if (orig) {                          /* in the trash: where it was, and since when */
        char *from = g_path_get_dirname(orig);
        row(p, r++, _("Deleted from"), from);
        g_free(from);
        GDateTime *dd = g_file_info_get_deletion_date(ti);
        if (dd) {
            char *s = g_date_time_format(dd, "%Y-%m-%d %H:%M");
            row(p, r++, _("Deleted"), s);
            g_free(s);
            g_date_time_unref(dd);
        }
    } else {
        GFile *parent = g_file_get_parent(f);
        char *where = parent ? file_path(parent) : g_strdup("/");
        row(p, r++, _("Where"), where);
        g_free(where);
        g_clear_object(&parent);
    }
    /* a trashed file is still a file on disk (…/Trash/files/…): its picture or text can show */
    const char *target = ti ? g_file_info_get_attribute_string(ti, "standard::target-uri") : NULL;
    if (!path && target) {
        GFile *tf = g_file_new_for_uri(target);
        path = g_file_get_path(tf);
        g_object_unref(tf);
    }
    g_clear_object(&ti);
    gtk_widget_show_all(p->details);

    /* big type icon, unless a real picture replaces it */
    show_icon(p, g_file_info_get_icon(info));
    if (path) {
        if (isdir) {
            grid_watch(p);
            p->dir_size_l = g_object_ref(size_l);
            p->dir_count_l = g_object_ref(count_l);
            /* the size count and the gallery wait for the folder's column: first the names, then
             * the rest, not all three at once on the disk */
            Later *l = g_new0(Later, 1);
            l->p = p;
            l->dir = g_strdup(path);
            l->size_l = g_object_ref(size_l);
            l->count_l = g_object_ref(count_l);
            p->grid_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 250, folder_ready, l, later_free);
        } else if (media_kind(ct) != MEDIA_NONE) {
            p->image_path = g_strdup(path);
            p->image_kind = media_kind(ct);
            /* the area is laid out right away (so the picture gets the pane's size); for video
             * frames and covers the line above it only shows once there is one */
            if (p->image_kind < MEDIA_VIDEO)
                gtk_widget_show(p->sep);
            gtk_widget_show(p->image_scroll);
            image_schedule(p, 30);      /* after the area got its size */
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
    gtk_widget_set_size_request(p->box, p->width, -1);   /* the least it gets; it takes the rest */
    gtk_widget_set_hexpand(p->box, TRUE);
    g_signal_connect(p->box, "size-allocate", G_CALLBACK(on_pane_size), p);
    p->image = gtk_image_new();
    gtk_widget_set_halign(p->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->image, GTK_ALIGN_START);
    /* EXTERNAL: the picture never sets the pane's size, the pane sets the picture's */
    p->image_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->image_scroll), GTK_POLICY_EXTERNAL,
                                   GTK_POLICY_EXTERNAL);
    gtk_widget_set_vexpand(p->image_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(p->image_scroll), p->image);
    gtk_widget_set_no_show_all(p->image_scroll, TRUE);
    gtk_widget_show(p->image);
    g_signal_connect(p->image_scroll, "size-allocate", G_CALLBACK(on_image_area), p);
    p->icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(p->icon), ICON_PX);
    gtk_widget_set_valign(p->icon, GTK_ALIGN_START);
    p->text = gtk_text_view_new();
    g_object_set(p->text, "editable", FALSE, "cursor-visible", FALSE, "monospace", TRUE,
                 "wrap-mode", GTK_WRAP_CHAR, NULL);
    p->text_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(p->text_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(p->text_scroll), p->text);
    p->grid = gtk_flow_box_new();
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(p->grid), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(p->grid), thumb_min_cols());
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(p->grid), thumb_min_cols());
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(p->grid), THUMB_GAP);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(p->grid), THUMB_GAP);
    /* the theme's padding around flow box children ate a good part of the thumbnails' room */
    static GtkCssProvider *css;
    if (!css) {
        css = gtk_css_provider_new();
        char *rule = g_strdup_printf(".millerx-thumbs flowboxchild { padding: %dpx; margin: 0; }", THUMB_PAD);
        gtk_css_provider_load_from_data(css, rule, -1, NULL);
        g_free(rule);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(p->grid), "millerx-thumbs");
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(p->grid), FALSE);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(p->grid), GTK_SELECTION_MULTIPLE);   /* Ctrl/Shift+click */
    gtk_widget_set_valign(p->grid, GTK_ALIGN_START);
    g_signal_connect(p->grid, "child-activated", G_CALLBACK(on_thumb_activated), p);
    g_signal_connect(p->grid, "selected-children-changed", G_CALLBACK(on_thumbs_picked), p);
    g_signal_connect(p->grid, "button-press-event", G_CALLBACK(on_grid_button), p);
    gtk_widget_add_events(p->grid, GDK_BUTTON1_MOTION_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(p->grid, "motion-notify-event", G_CALLBACK(on_grid_motion), p);
    g_signal_connect(p->grid, "button-release-event", G_CALLBACK(on_grid_release), p);
    g_signal_connect(p->grid, "drag-begin", G_CALLBACK(on_grid_drag_begin), p);
    g_signal_connect(p->grid, "drag-data-get", G_CALLBACK(on_grid_drag_get), p);
    g_signal_connect(p->grid, "drag-end", G_CALLBACK(on_grid_drag_end), p);
    g_signal_connect(p->grid, "key-press-event", G_CALLBACK(on_grid_key), p);
    p->grid_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->grid_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(p->grid_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(p->grid_scroll), p->grid);
    GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->grid_scroll));
    g_signal_connect(va, "value-changed", G_CALLBACK(on_grid_scrolled), p);
    g_signal_connect(va, "changed", G_CALLBACK(on_grid_scrolled), p);
    p->pool = g_thread_pool_new(thumb_work, NULL, THUMB_WORKERS, FALSE, NULL);
    gtk_widget_set_no_show_all(p->grid_scroll, TRUE);
    gtk_widget_show(p->grid);
    p->title = wrap_label("");
    p->details = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(p->details), 10);
    gtk_grid_set_row_spacing(GTK_GRID(p->details), 3);
    /* the name as a title across the whole pane (long names get the full width); under it the
     * header: icon left, details right; then a line; then picture, text or gallery */
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(info, TRUE);
    gtk_widget_set_valign(info, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(info), p->details, FALSE, FALSE, 0);
    p->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(p->header), p->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->header), info, TRUE, TRUE, 0);
    p->sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_no_show_all(p->sep, TRUE);
    gtk_box_pack_start(GTK_BOX(p->box), p->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->image_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->text_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->grid_scroll, TRUE, TRUE, 0);
    /* GTK asks the window for the height of everything at its narrowest width, where the
     * details wrap line by line: without this the window couldn't be made lower than that,
     * however wide it is. Normally it is never scrolled: the picture and gallery areas inside
     * take the room there is. */
    p->outer = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->outer), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(p->outer), GTK_SHADOW_NONE);
    gtk_widget_set_hexpand(p->outer, TRUE);
    gtk_container_add(GTK_CONTAINER(p->outer), p->box);
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(gtk_bin_get_child(GTK_BIN(p->outer))), GTK_SHADOW_NONE);
    return p;
}

void preview_free(Preview *p)
{
    grid_stop(p);
    if (p->header_idle)
        g_source_remove(p->header_idle);
    /* every job holds a reference on the window, so none is left when we get here */
    g_thread_pool_free(p->pool, TRUE, TRUE);
    if (p->image_timer)
        g_source_remove(p->image_timer);
    if (p->resize_timer)
        g_source_remove(p->resize_timer);
    g_free(p->image_path);
    g_free(p->drag_path);
    g_list_free_full(p->drag_files, g_object_unref);
    g_clear_object(&p->current);
    g_free(p);
}
