/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Trusted programs (SCerovec's wish): a program, script or launcher you trusted runs without the
 * question, as long as it is the same file. ~/.config/millerx/trusted, one per line:
 * size TAB sha256 TAB path. The fingerprint is the file's SHA-256, so a file swapped in under the
 * same name (or updated) is asked about again. Settings → Programs lists them, ✕ forgets one.
 * Root's own programs (what your distro installed) need no list: see trust_is_system. */
#include "millerx.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <glib/gstdio.h>

#ifdef MILLERX_NAME
#define TRUST_DIR MILLERX_NAME              /* millerX Unleashed keeps its own */
#else
#define TRUST_DIR "millerx"
#endif

typedef struct {
    char *path, *sum;
    gint64 size;
} Trusted;

static char *trust_file(void)
{
    return g_build_filename(g_get_user_config_dir(), TRUST_DIR, "trusted", NULL);
}

static void t_free(gpointer p)
{
    Trusted *t = p;
    g_free(t->path);
    g_free(t->sum);
    g_free(t);
}

static gboolean is_sum(const char *s)
{
    if (strlen(s) != 64)
        return FALSE;
    for (; *s; s++)
        if (!g_ascii_isxdigit(*s))
            return FALSE;
    return TRUE;
}

/* read every time: another millerX may have changed it; odd lines are skipped */
static GPtrArray *load(void)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(t_free);
    char *file = trust_file(), *text = NULL;
    if (g_file_get_contents(file, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            char **f = g_strsplit(*l, "\t", 3);
            if (g_strv_length(f) == 3 && is_sum(f[1]) && g_path_is_absolute(f[2])) {
                Trusted *t = g_new0(Trusted, 1);
                t->size = g_ascii_strtoll(f[0], NULL, 10);
                t->sum = g_ascii_strdown(f[1], -1);
                t->path = g_strdup(f[2]);
                g_ptr_array_add(list, t);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(text);
    g_free(file);
    return list;
}

static gboolean save(GPtrArray *list)
{
    GString *s = g_string_new("# millerX: trusted programs (size, SHA-256, path). Settings → Programs.\n");
    for (guint i = 0; i < list->len; i++) {
        Trusted *t = list->pdata[i];
        g_string_append_printf(s, "%" G_GINT64_FORMAT "\t%s\t%s\n", t->size, t->sum, t->path);
    }
    char *file = trust_file(), *dir = g_path_get_dirname(file);
    g_mkdir_with_parents(dir, 0755);
    gboolean ok = g_file_set_contents_full(file, s->str, (gssize) s->len, G_FILE_SET_CONTENTS_CONSISTENT,
                                           0600, NULL);
    g_free(dir);
    g_free(file);
    g_string_free(s, TRUE);
    return ok;
}

static int find(GPtrArray *list, const char *path)
{
    for (guint i = 0; i < list->len; i++)
        if (!strcmp(((Trusted *) list->pdata[i])->path, path))
            return (int) i;
    return -1;
}

/* the file's SHA-256 (hex, new string) and size; NULL if it can't be read */
static char *file_sum(const char *path, gint64 *size)
{
    int fd = g_open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);     /* a FIFO put there can't hang us */
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    guchar buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        g_checksum_update(ck, buf, n);
    close(fd);
    char *sum = n < 0 ? NULL : g_strdup(g_checksum_get_string(ck));
    g_checksum_free(ck);
    *size = st.st_size;
    return sum;
}

/* only you (or root) can change it: owned by you or root, nobody else may write */
static gboolean only_us(const struct stat *st)
{
    return (st->st_uid == getuid() || st->st_uid == 0) && !(st->st_mode & (S_IWGRP | S_IWOTH));
}

/* between the fingerprint and the start someone else could swap the file, wherever someone else
 * may write: the file, its folder (unless sticky, like /tmp, where only the owner may replace it)
 * and the folder of a link that leads to it. There a trusted program asks again (review for 0.9.0). */
static gboolean nobody_else_can_swap(const char *path)
{
    struct stat st, lst, dst;
    char *real = realpath(path, NULL);
    gboolean ok = real && !stat(real, &st) && only_us(&st) && !lstat(path, &lst);
    const char *dirs[2] = { real, path };
    uid_t owners[2] = { ok ? st.st_uid : 0, ok ? lst.st_uid : 0 };   /* the file's, the link's */
    for (int i = 0; ok && i < 2; i++) {
        char *d = g_path_get_dirname(dirs[i]);
        ok = !stat(d, &dst) && (only_us(&dst) || ((dst.st_mode & S_ISVTX) && (owners[i] == getuid() || owners[i] == 0)));
        g_free(d);
    }
    free(real);
    return ok;
}

int trust_check(const char *path)
{
    GPtrArray *list = load();
    int i = find(list, path), r = TRUST_NO;
    if (i >= 0) {
        Trusted *t = list->pdata[i];
        struct stat st;
        gint64 size = 0;
        char *sum = NULL;
        if (g_stat(path, &st) || st.st_size != t->size)     /* a different size: no need to read it */
            r = TRUST_CHANGED;
        else if (!(sum = file_sum(path, &size)) || strcmp(sum, t->sum))
            r = TRUST_CHANGED;
        else
            r = nobody_else_can_swap(path) ? TRUST_YES : TRUST_NO;
        g_free(sum);
    }
    g_ptr_array_unref(list);
    return r;
}

gboolean trust_add(const char *path)
{
    if (!g_path_is_absolute(path) || strchr(path, '\n') || strchr(path, '\t'))
        return FALSE;
    gint64 size = 0;
    char *sum = file_sum(path, &size);
    if (!sum)
        return FALSE;
    GPtrArray *list = load();
    int i = find(list, path);
    if (i >= 0)
        g_ptr_array_remove_index(list, (guint) i);
    Trusted *t = g_new0(Trusted, 1);
    t->path = g_strdup(path);
    t->sum = sum;
    t->size = size;
    g_ptr_array_add(list, t);
    gboolean ok = save(list);
    g_ptr_array_unref(list);
    return ok;
}

void trust_remove(const char *path)
{
    GPtrArray *list = load();
    int i = find(list, path);
    if (i >= 0) {
        g_ptr_array_remove_index(list, (guint) i);
        save(list);
    }
    g_ptr_array_unref(list);
}

GList *trust_paths(void)
{
    GPtrArray *list = load();
    GList *out = NULL;
    for (guint i = 0; i < list->len; i++)
        out = g_list_prepend(out, g_strdup(((Trusted *) list->pdata[i])->path));
    g_ptr_array_unref(list);
    return g_list_sort(out, (GCompareFunc) g_strcmp0);
}

/* Root's own: the file and every folder up to / belong to root, and nobody else may write to them
 * (a group-writable one only when the group is root's too). That is what your package tool installed
 * in /usr, /opt and so on; whoever can change those already owns the machine, so asking protects
 * nothing, and updates (a new file every time) don't ask again.
 * Holes closed after the 0.8.8 review:
 * - a link is judged as a link too: a "README" link to /usr/bin/make in a downloaded folder would
 *   otherwise run make there, with that folder's Makefile. So the clicked path's own folder (and the
 *   link itself) must be root's as well, not only where it points.
 * - "owned by root" isn't "installed by root" on every file system: a FAT or NTFS stick, an ISO or a
 *   network share mounted by root shows every file as root's. Only Linux's own disk file systems
 *   count (ext2/3/4, XFS, Btrfs, F2FS, JFS, ReiserFS, bcachefs, ZFS, and the overlay a live system
 *   runs on).
 * Never anything under your home folder: as root, /root's files are root's too. */
static gboolean root_only(const struct stat *st)
{
    return st->st_uid == 0 && !(st->st_mode & S_IWOTH) && (!(st->st_mode & S_IWGRP) || st->st_gid == 0);
}

static gboolean system_fs(const char *path)
{
    struct statfs fs;
    if (statfs(path, &fs))
        return FALSE;
    switch ((unsigned long) fs.f_type) {
    case 0xEF53UL:          /* ext2, ext3, ext4 */
    case 0x58465342UL:      /* XFS */
    case 0x9123683EUL:      /* Btrfs */
    case 0xF2F52010UL:      /* F2FS */
    case 0x3153464AUL:      /* JFS */
    case 0x52654973UL:      /* ReiserFS */
    case 0xCA451A4EUL:      /* bcachefs */
    case 0x2FC12FC1UL:      /* ZFS */
    case 0x794C7630UL:      /* overlay (live systems) */
        return TRUE;
    default:
        return FALSE;
    }
}

/* dir (a real path) and every folder above it are root's alone */
static gboolean dirs_root_only(const char *real_dir)
{
    char *dir = g_strdup(real_dir);
    gboolean ok = TRUE;
    struct stat st;
    for (;;) {
        if (stat(dir, &st) || !S_ISDIR(st.st_mode) || !root_only(&st)) {
            ok = FALSE;
            break;
        }
        if (!strcmp(dir, "/"))
            break;
        char *up = g_path_get_dirname(dir);
        g_free(dir);
        dir = up;
    }
    g_free(dir);
    return ok;
}

static gboolean under_home(const char *real)
{
    const char *home = g_get_home_dir();
    size_t hl = home ? strlen(home) : 0;
    return hl > 1 && !strncmp(real, home, hl) && (real[hl] == '/' || !real[hl]);
}

/* on the system's own disk: the device of /, /usr, /opt … (each may be its own partition), never a
 * stick, a disc or a share mounted somewhere (/run/media, /media, /mnt), whatever its file system:
 * an ext4 stick made by root has root's files too (review for 0.9.0) */
static gboolean on_system_disk(const char *real, const struct stat *st)
{
    static const char *const AWAY[] = { "/run/", "/media/", "/mnt/" };
    for (guint i = 0; i < G_N_ELEMENTS(AWAY); i++)
        if (g_str_has_prefix(real, AWAY[i]))
            return FALSE;
    static const char *const SYS[] = { "/", "/usr", "/usr/local", "/opt", "/bin", "/sbin", "/lib", "/lib64" };
    for (guint i = 0; i < G_N_ELEMENTS(SYS); i++) {
        struct stat s;
        if (!stat(SYS[i], &s) && s.st_dev == st->st_dev)
            return TRUE;
    }
    return FALSE;
}

gboolean trust_is_system(const char *path)
{
    gboolean ok = FALSE;
    struct stat st;
    char *real = realpath(path, NULL);
    char *parent = g_path_get_dirname(path);
    char *real_parent = realpath(parent, NULL);
    if (!real || !real_parent || under_home(real) || under_home(real_parent))
        goto out;
    /* the clicked path itself: a link must be root's, in a folder of root's */
    if (lstat(path, &st) || (S_ISLNK(st.st_mode) && st.st_uid != 0) || !dirs_root_only(real_parent))
        goto out;
    /* where it really is */
    if (stat(real, &st) || !S_ISREG(st.st_mode) || !root_only(&st) || !system_fs(real) || !on_system_disk(real, &st))
        goto out;
    char *rdir = g_path_get_dirname(real);
    ok = dirs_root_only(rdir);
    g_free(rdir);
out:
    free(real);
    free(real_parent);
    g_free(parent);
    return ok;
}
