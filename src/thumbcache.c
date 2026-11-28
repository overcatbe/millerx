/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Keeping ~/.cache/thumbnails (shared with Thunar and every file dialog) from growing forever:
 * thumbnails of files that are gone, ones unused for too long, then the least used ones above a
 * size limit. Only .png files directly inside the cache's size folders are ever touched. */
#include "millerx.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *DIRS[] = { "normal", "large", "x-large", "xx-large" };

typedef struct {
    char *path;
    guint64 size;
    gint64 used;          /* last access or change, whichever is later */
} Thumb;

static void thumb_free(Thumb *t)
{
    g_free(t->path);
    g_free(t);
}

static char *cache_root(void)
{
    return g_build_filename(g_get_user_cache_dir(), "thumbnails", NULL);
}

/* every regular .png in the size folders (and in fail/<program>/) */
static GPtrArray *list_thumbs(guint64 *total)
{
    GPtrArray *all = g_ptr_array_new_with_free_func((GDestroyNotify) thumb_free);
    char *root = cache_root();
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < G_N_ELEMENTS(DIRS); i++)
        g_ptr_array_add(dirs, g_build_filename(root, DIRS[i], NULL));
    char *fail = g_build_filename(root, "fail", NULL);
    struct stat fst;
    GDir *fd = !lstat(fail, &fst) && S_ISDIR(fst.st_mode) ? g_dir_open(fail, 0, NULL) : NULL;
    if (fd) {
        const char *n;
        while ((n = g_dir_read_name(fd)))
            g_ptr_array_add(dirs, g_build_filename(fail, n, NULL));
        g_dir_close(fd);
    }
    *total = 0;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    for (guint i = 0; i < dirs->len; i++) {
        const char *dir = g_ptr_array_index(dirs, i);
        /* a size folder or fail/<program> that is a link points out of the cache: what is there
         * isn't ours to clean up (or delete) */
        struct stat dst;
        if (lstat(dir, &dst) || !S_ISDIR(dst.st_mode))
            continue;
        GDir *d = g_dir_open(dir, 0, NULL);
        if (!d)
            continue;
        const char *n;
        while ((n = g_dir_read_name(d))) {
            char *p = g_build_filename(dir, n, NULL);
            struct stat st;
            if (lstat(p, &st) || !S_ISREG(st.st_mode)) {
                g_free(p);
                continue;
            }
            /* our own half-written temp files, left by a crash */
            if (g_str_has_prefix(n, ".millerx-")) {
                if (now - st.st_mtime > 3600)
                    g_unlink(p);
                g_free(p);
                continue;
            }
            if (!g_str_has_suffix(n, ".png")) {
                g_free(p);
                continue;
            }
            Thumb *t = g_new0(Thumb, 1);
            t->path = p;
            t->size = st.st_size;
            t->used = MAX(st.st_atime, st.st_mtime);
            *total += t->size;
            g_ptr_array_add(all, t);
        }
        g_dir_close(d);
    }
    g_ptr_array_unref(dirs);
    g_free(fail);
    g_free(root);
    return all;
}

/* the Thumb::URI text chunk, read without decoding the picture; NULL if none */
static char *thumb_uri(const char *png)
{
    FILE *fh = fopen(png, "rb");
    if (!fh)
        return NULL;
    char *uri = NULL;
    unsigned char sig[8], hd[8];
    if (fread(sig, 1, 8, fh) == 8 && !memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) {
        while (!uri && fread(hd, 1, 8, fh) == 8) {
            guint32 len = (guint32) hd[0] << 24 | hd[1] << 16 | hd[2] << 8 | hd[3];
            if (!memcmp(hd + 4, "IDAT", 4) || !memcmp(hd + 4, "IEND", 4) || len > (1u << 24))
                break;
            if (!memcmp(hd + 4, "tEXt", 4) && len < 65536) {
                char *buf = g_malloc(len + 1);
                if (fread(buf, 1, len, fh) != len) {
                    g_free(buf);
                    break;
                }
                buf[len] = 0;
                size_t kl = strlen(buf);
                if (kl < len && !strcmp(buf, "Thumb::URI"))
                    uri = g_strdup(buf + kl + 1);
                g_free(buf);
                if (fseek(fh, 4, SEEK_CUR))
                    break;
            } else if (fseek(fh, (long) len + 4, SEEK_CUR)) {
                break;
            }
        }
    }
    fclose(fh);
    return uri;
}

/* The original is gone for good: missing, and not just on an unplugged drive. */
static gboolean original_gone(const char *png)
{
    char *uri = thumb_uri(png);
    if (!uri)
        return FALSE;
    gboolean gone = FALSE;
    char *path = g_str_has_prefix(uri, "file://") ? g_filename_from_uri(uri, NULL, NULL) : NULL;
    if (path && !g_file_test(path, G_FILE_TEST_EXISTS)) {
        gboolean removable = g_str_has_prefix(path, "/media/") || g_str_has_prefix(path, "/mnt/")
                             || g_str_has_prefix(path, "/run/media/");
        char *parent = g_path_get_dirname(path);
        gone = !removable || g_file_test(parent, G_FILE_TEST_IS_DIR);   /* drive there, file not */
        g_free(parent);
    }
    g_free(path);
    g_free(uri);
    return gone;
}

static int by_used(gconstpointer a, gconstpointer b)
{
    const Thumb *x = *(Thumb **) a, *y = *(Thumb **) b;
    return x->used < y->used ? -1 : x->used > y->used;
}

typedef struct {
    gboolean everything;
    guint64 max_bytes;
    int days;
    ThumbcacheDone done;
    gpointer data;
    guint64 bytes;          /* result: size after */
    guint files, removed;
} Clean;

static gboolean clean_report(gpointer data)
{
    Clean *c = data;
    if (c->done)
        c->done(c->bytes, c->files, c->removed, c->data);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static gpointer clean_thread(gpointer data)
{
    Clean *c = data;
    guint64 total = 0;
    GPtrArray *all = list_thumbs(&total);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    GPtrArray *keep = g_ptr_array_new();
    for (guint i = 0; i < all->len; i++) {
        Thumb *t = g_ptr_array_index(all, i);
        gboolean drop = c->everything || (c->days > 0 && now - t->used > (gint64) c->days * 86400)
                        || original_gone(t->path);
        if (drop && g_unlink(t->path) == 0) {
            total -= t->size;
            c->removed++;
        } else if (!drop) {
            g_ptr_array_add(keep, t);
        }
    }
    if (!c->everything && c->max_bytes && total > c->max_bytes) {
        g_ptr_array_sort(keep, by_used);
        for (guint i = 0; i < keep->len && total > c->max_bytes; i++) {
            Thumb *t = g_ptr_array_index(keep, i);
            if (g_unlink(t->path) == 0) {
                total -= t->size;
                c->removed++;
            }
        }
    }
    c->bytes = total;
    c->files = all->len - c->removed;
    g_ptr_array_unref(keep);
    g_ptr_array_unref(all);
    g_idle_add(clean_report, c);
    return NULL;
}

static void run(gboolean everything, ThumbcacheDone done, gpointer data)
{
    Clean *c = g_new0(Clean, 1);
    c->everything = everything;
    c->max_bytes = (guint64) LOOK.cache_max_mb * 1000000;      /* MB as shown (g_format_size) */
    c->days = LOOK.cache_days;
    c->done = done;
    c->data = data;
    g_thread_unref(g_thread_new("millerx-cache", clean_thread, c));
}

/* ---- public ------------------------------------------------------------------------- */
static gpointer measure_thread(gpointer data)
{
    Clean *c = data;
    guint64 total = 0;
    GPtrArray *all = list_thumbs(&total);
    c->bytes = total;
    c->files = all->len;
    g_ptr_array_unref(all);
    g_idle_add(clean_report, c);
    return NULL;
}

void thumbcache_measure(ThumbcacheDone done, gpointer data)
{
    Clean *c = g_new0(Clean, 1);
    c->done = done;
    c->data = data;
    g_thread_unref(g_thread_new("millerx-cache", measure_thread, c));
}

void thumbcache_clear(ThumbcacheDone done, gpointer data)
{
    run(TRUE, done, data);
}

void thumbcache_clean(ThumbcacheDone done, gpointer data)
{
    run(FALSE, done, data);
}

/* at start: at most once a day, quietly */
void thumbcache_auto_clean(void)
{
    if (!LOOK.cache_auto)
        return;
    char *dir = g_build_filename(g_get_user_cache_dir(), "millerx", NULL);
    char *stamp = g_build_filename(dir, "last-clean", NULL);
    struct stat st;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (g_stat(stamp, &st) || now - st.st_mtime > 86400) {
        g_mkdir_with_parents(dir, 0700);
        g_file_set_contents(stamp, "", 0, NULL);      /* also marks the time */
        thumbcache_clean(NULL, NULL);
    }
    g_free(stamp);
    g_free(dir);
}
