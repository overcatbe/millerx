/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Packing and unpacking archives with libarchive (no external programs). Runs on a job's worker
 * thread: reports progress through a callback and stops when the cancellable is cancelled. */
#include "millerx.h"
#include <archive.h>
#include <archive_entry.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const ArcFormat ARC_FORMATS[] = {
    { "zip", ".zip" }, { "tar.gz", ".tar.gz" }, { "tar.xz", ".tar.xz" },
    { "tar.zst", ".tar.zst" }, { "7z", ".7z" },
};
const int ARC_N_FORMATS = G_N_ELEMENTS(ARC_FORMATS);

/* longest first, so "x.tar.gz" loses ".tar.gz" and not just ".gz" */
static const char *ARC_EXTS[] = {
    ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.lzma", ".tar.Z",
    ".tgz", ".tbz", ".tbz2", ".txz", ".tzst", ".tlz", ".tar", ".zip", ".7z", ".rar", ".cpio",
    ".iso", ".jar", ".cbz", ".cbr", ".epub", NULL
};

char *arc_stem(const char *name)
{
    for (int i = 0; ARC_EXTS[i]; i++) {
        size_t n = strlen(name), e = strlen(ARC_EXTS[i]);
        if (n > e && !g_ascii_strcasecmp(name + n - e, ARC_EXTS[i]))
            return g_strndup(name, n - e);
    }
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? g_strndup(name, dot - name) : g_strconcat(name, _(" (extracted)"), NULL);
}

static void set_err(GError **err, struct archive *a, const char *what)
{
    if (err && !*err)
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what,
                    a && archive_error_string(a) ? archive_error_string(a) : _("unknown error"));
}

/* An entry name is only accepted relative and without "..": nothing may land outside the folder. */
static char *safe_name(const char *name)
{
    if (!name)
        return NULL;
    while (name[0] == '.' && name[1] == '/')
        name += 2;
    if (!*name || name[0] == '/')
        return NULL;
    char **parts = g_strsplit(name, "/", -1);
    gboolean ok = TRUE;
    for (int i = 0; parts[i]; i++)
        if (!strcmp(parts[i], ".."))
            ok = FALSE;
    g_strfreev(parts);
    return ok ? g_strdup(name) : NULL;
}

gboolean arc_extract(const char *archive_path, const char *dir, GCancellable *cancel, ArcProgress cb,
                     gpointer data, GError **err)
{
    struct archive *a = archive_read_new(), *w = archive_write_disk_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    /* times yes, owners no; permissions masked by the umask; no writing through symlinks */
    archive_write_disk_set_options(w, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS
                                      | ARCHIVE_EXTRACT_SECURE_NODOTDOT);
    archive_write_disk_set_standard_lookup(w);
    /* the real path: libarchive refuses to write through symlinks, also ones above the folder */
    char *real = realpath(dir, NULL);
    if (real)
        dir = real;
    gboolean ok = TRUE;
    GStatBuf st;
    double total = g_stat(archive_path, &st) == 0 && st.st_size > 0 ? (double) st.st_size : 1;
    if (archive_read_open_filename(a, archive_path, 65536) != ARCHIVE_OK) {
        set_err(err, a, _("Can't open the archive"));
        ok = FALSE;
    }
    struct archive_entry *e;
    int r, skipped = 0;
    gint64 last = 0;
    while (ok && (r = archive_read_next_header(a, &e)) != ARCHIVE_EOF) {
        if (r < ARCHIVE_WARN) {
            set_err(err, a, _("Broken archive"));
            ok = FALSE;
            break;
        }
        if (g_cancellable_is_cancelled(cancel))
            break;
        const char *orig = archive_entry_pathname(e);
        /* "./" (or "."): the folder itself, which tar -C dir . and every Slackware package start
         * with. Nothing to make (the folder is there) and nothing unsafe: no "left out" for it */
        const char *o = orig ? orig : "";
        while (o[0] == '.' && (o[1] == '/' || !o[1]))
            o += o[1] ? 2 : 1;
        if (orig && !*o) {
            archive_read_data_skip(a);
            continue;
        }
        char *rel = safe_name(orig);
        const char *hl = archive_entry_hardlink(e);
        char *hrel = hl ? safe_name(hl) : NULL;
        if (!rel || (hl && !hrel)) {             /* "../x", "/etc/x": refused, the rest goes on */
            skipped++;
            g_free(rel);
            g_free(hrel);
            continue;
        }
        char *full = g_build_filename(dir, rel, NULL);
        archive_entry_set_pathname(e, full);
        if (hrel) {
            char *hfull = g_build_filename(dir, hrel, NULL);
            archive_entry_set_hardlink(e, hfull);
            g_free(hfull);
        }
        gint64 now = g_get_monotonic_time();
        if (cb && now - last > 100000) {
            last = now;
            cb(data, rel, MIN(1.0, archive_filter_bytes(a, -1) / total));
        }
        if (archive_write_header(w, e) < ARCHIVE_WARN) {
            set_err(err, w, rel);
            ok = FALSE;
        } else if (archive_entry_size(e) > 0) {
            const void *buf;
            size_t len;
            la_int64_t off;
            while ((r = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK) {
                if (g_cancellable_is_cancelled(cancel))
                    break;
                if (archive_write_data_block(w, buf, len, off) < ARCHIVE_WARN) {
                    set_err(err, w, rel);
                    ok = FALSE;
                    break;
                }
            }
            if (ok && r < ARCHIVE_WARN && !g_cancellable_is_cancelled(cancel)) {
                set_err(err, a, rel);
                ok = FALSE;
            }
        }
        if (ok && archive_write_finish_entry(w) < ARCHIVE_WARN) {
            set_err(err, w, rel);
            ok = FALSE;
        }
        g_free(full);
        g_free(rel);
        g_free(hrel);
    }
    archive_write_close(w);
    archive_write_free(w);
    archive_read_free(a);
    free(real);
    if (ok && skipped && err && !*err)
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                    ngettext("%d item with an unsafe name (absolute or with “..”) was left out",
                             "%d items with unsafe names (absolute or with “..”) were left out", skipped), skipped);
    return ok;
}

/* the size of everything under the paths, for the progress bar */
static guint64 measure(char **paths)
{
    guint64 sum = 0;
    struct archive *d = archive_read_disk_new();
    archive_read_disk_set_symlink_physical(d);
    for (int i = 0; paths[i]; i++) {
        if (archive_read_disk_open(d, paths[i]) != ARCHIVE_OK)
            continue;
        struct archive_entry *e = archive_entry_new();
        while (archive_read_next_header2(d, e) == ARCHIVE_OK) {
            if (archive_entry_filetype(e) == AE_IFREG)
                sum += archive_entry_size(e);
            archive_read_disk_descend(d);
        }
        archive_entry_free(e);
        archive_read_close(d);
    }
    archive_read_free(d);
    return sum;
}

gboolean arc_create(const char *out, int fmt, char **paths, const char *base, GCancellable *cancel,
                    ArcProgress cb, gpointer data, GError **err)
{
    struct archive *a = archive_write_new();
    int r = ARCHIVE_OK;
    static int (*const FILTERS[])(struct archive *) = {
        NULL, archive_write_add_filter_gzip, archive_write_add_filter_xz, archive_write_add_filter_zstd, NULL
    };
    if (fmt == 0)
        r = archive_write_set_format_zip(a);
    else if (fmt == 4)
        r = archive_write_set_format_7zip(a);
    else
        r = archive_write_set_format_pax_restricted(a);
    if (r == ARCHIVE_OK && fmt > 0 && fmt < 4)
        r = FILTERS[fmt](a);
    if (r != ARCHIVE_OK || archive_write_open_filename(a, out) != ARCHIVE_OK) {
        set_err(err, a, _("Can't write this format"));
        archive_write_free(a);
        return FALSE;
    }
    double total = MAX(1.0, (double) measure(paths)), done = 0;
    size_t blen = strlen(base);
    gboolean ok = TRUE;
    gint64 last = 0;
    char *buf = g_malloc(65536);
    struct archive *d = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(d);
    archive_read_disk_set_symlink_physical(d);       /* links stay links */
    for (int i = 0; ok && paths[i] && !g_cancellable_is_cancelled(cancel); i++) {
        if (archive_read_disk_open(d, paths[i]) != ARCHIVE_OK) {
            set_err(err, d, paths[i]);
            ok = FALSE;
            break;
        }
        struct archive_entry *e = archive_entry_new();
        while (ok && !g_cancellable_is_cancelled(cancel)) {
            archive_entry_clear(e);
            r = archive_read_next_header2(d, e);
            if (r == ARCHIVE_EOF)
                break;
            if (r < ARCHIVE_WARN) {
                set_err(err, d, paths[i]);
                ok = FALSE;
                break;
            }
            archive_read_disk_descend(d);
            const char *src = archive_entry_sourcepath(e);
            const char *p = archive_entry_pathname(e);
            if (!strncmp(p, base, blen) && p[blen] == '/')
                archive_entry_set_pathname(e, p + blen + 1);   /* stored relative to the folder */
            gint64 now = g_get_monotonic_time();
            if (cb && now - last > 100000) {
                last = now;
                cb(data, archive_entry_pathname(e), MIN(1.0, done / total));
            }
            if (archive_write_header(a, e) < ARCHIVE_WARN) {
                set_err(err, a, archive_entry_pathname(e));
                ok = FALSE;
                break;
            }
            if (archive_entry_filetype(e) == AE_IFREG && archive_entry_size(e) > 0) {
                int fd = open(src, O_RDONLY | O_CLOEXEC);
                if (fd < 0) {
                    g_set_error(err, G_IO_ERROR, g_io_error_from_errno(errno), "%s: %s",
                                archive_entry_pathname(e), g_strerror(errno));
                    ok = FALSE;
                    break;
                }
                ssize_t n;
                gint64 got = 0;
                while ((n = read(fd, buf, 65536)) > 0) {
                    if (g_cancellable_is_cancelled(cancel))
                        break;
                    if (archive_write_data(a, buf, n) < 0) {
                        set_err(err, a, archive_entry_pathname(e));
                        ok = FALSE;
                        break;
                    }
                    done += n;
                    got += n;
                }
                /* a read error (a dead network share) or a file that shrank while it was packed:
                 * libarchive would pad the entry out, and the archive would hold a broken file
                 * without a word. Say so and stop. */
                if (ok && n < 0) {
                    g_set_error(err, G_IO_ERROR, g_io_error_from_errno(errno), "%s: %s",
                                archive_entry_pathname(e), g_strerror(errno));
                    ok = FALSE;
                } else if (ok && !g_cancellable_is_cancelled(cancel) && got < archive_entry_size(e)) {
                    g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, _("%s: it changed while it was packed"),
                                archive_entry_pathname(e));
                    ok = FALSE;
                }
                close(fd);
                if (!ok)
                    break;
            }
        }
        archive_entry_free(e);
        archive_read_close(d);
    }
    archive_read_free(d);
    g_free(buf);
    if (archive_write_close(a) != ARCHIVE_OK && ok) {
        set_err(err, a, _("Finishing the archive"));
        ok = FALSE;
    }
    archive_write_free(a);
    return ok;
}
