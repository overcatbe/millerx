/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Copy, move, extract or compress files in the background; asks on conflicts. */
#include "millerx.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

typedef enum { K_COPY, K_EXTRACT, K_EXTRACT_TO, K_COMPRESS } Kind;   /* K_EXTRACT_TO: into dest */

struct Job {
    Win *win;
    Kind kind;
    gboolean move;
    GFile *out;              /* K_COMPRESS: the archive to write */
    int format;              /* K_COMPRESS: index into ARC_FORMATS */
    guint done_n;            /* items finished, for the status line */
    GList *sources;          /* GFile* */
    GFile *dest;
    GCancellable *cancel;
    GPtrArray *errors;       /* char* */
    gboolean denied;         /* some "permission denied" among them */
    int all_choice;          /* -1 = ask */
    /* conflict question, answered on the main thread */
    GMutex lock;
    GCond cond;
    gboolean answered;
    Choice answer;
    gboolean for_all;
    GFile *q_src, *q_dst;
    gboolean q_merge;
    /* progress in bytes: counted first, then every file reports while it copies (not for a move
     * on the same disk, which is only a rename) */
    gboolean bytes;
    guint64 total;           /* everything to copy */
    guint64 done;            /* finished files */
    guint64 file_base;       /* done when the current file started */
    guint64 now;             /* done + the current file so far */
    gint64 shown;            /* the last update to the window (not more than ten a second) */
    char *label;             /* "Copying name (2/5)" */
};

void job_cancel(Job *j)
{
    g_cancellable_cancel(j->cancel);
}

/* ---- talking to the window (main thread) ----------------------------------------- */
typedef struct {
    Job *j;
    char *text;
    double frac;
} Progress;

static gboolean ui_progress(gpointer data)
{
    Progress *pr = data;
    Win *w = pr->j->win;
    if (!w->dead) {
        gtk_label_set_text(GTK_LABEL(w->statusbar), pr->text);
        if (pr->frac < 0)
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(w->progress));     /* still counting */
        else
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->progress), pr->frac);
    }
    g_free(pr->text);
    g_free(pr);
    return G_SOURCE_REMOVE;
}

static void job_free(Job *j)
{
    g_list_free_full(j->sources, g_object_unref);
    g_object_unref(j->dest);
    g_clear_object(&j->out);
    g_object_unref(j->cancel);
    g_ptr_array_unref(j->errors);
    g_mutex_clear(&j->lock);
    g_cond_clear(&j->cond);
    g_free(j->label);
    win_unref(j->win);
    g_free(j);
}

static gboolean ui_done(gpointer data)
{
    Job *j = data;
    Win *w = j->win;
    w->jobs = g_list_remove(w->jobs, j);
    if (!w->dead) {
        if (!w->jobs) {
            gtk_widget_hide(w->progress);
            gtk_widget_hide(w->cancel_btn);
        }
        if (j->errors->len) {
            win_status(w, _("Done with %u error(s)"), j->errors->len);
            GString *s = g_string_new(NULL);
            for (guint i = 0; i < j->errors->len && i < 20; i++)
                g_string_append_printf(s, "%s%s", i ? "\n" : "", (char *) g_ptr_array_index(j->errors, i));
            win_error_root(w, _("Some items failed"), s->str, j->denied ? j->dest : NULL);
            g_string_free(s, TRUE);
        } else if (g_cancellable_is_cancelled(j->cancel)) {
            win_status(w, _("Cancelled"));
        } else if (j->kind == K_COMPRESS) {
            char *to = g_file_get_basename(j->out);
            guint n = g_list_length(j->sources);
            win_status(w, ngettext("Packed %u item into %s", "Packed %u items into %s", n), n, to);
            g_free(to);
        } else if (j->kind != K_COPY) {
            win_status(w, ngettext("Extracted %u archive", "Extracted %u archives", j->done_n), j->done_n);
        } else {
            char *to = g_file_get_basename(j->dest);
            guint n = g_list_length(j->sources);
            if (j->move)
                win_status(w, ngettext("Moved %u item to %s", "Moved %u items to %s", n), n, to);
            else
                win_status(w, ngettext("Copied %u item to %s", "Copied %u items to %s", n), n, to);
            g_free(to);
        }
    }
    job_free(j);
    return G_SOURCE_REMOVE;
}

static gboolean ui_ask(gpointer data)
{
    Job *j = data;
    gboolean all = FALSE;
    Choice c = j->win->dead ? CH_CANCEL : dialog_conflict(j->win, j->q_src, j->q_dst, j->q_merge, &all);
    g_mutex_lock(&j->lock);
    j->answer = c;
    j->for_all = all;
    j->answered = TRUE;
    g_cond_signal(&j->cond);
    g_mutex_unlock(&j->lock);
    return G_SOURCE_REMOVE;
}

static Choice ask(Job *j, GFile *src, GFile *dst, gboolean merge)
{
    if (j->all_choice >= 0)
        return j->all_choice;
    g_mutex_lock(&j->lock);
    j->answered = FALSE;
    j->q_src = src;
    j->q_dst = dst;
    j->q_merge = merge;
    g_idle_add(ui_ask, j);
    while (!j->answered)
        g_cond_wait(&j->cond, &j->lock);
    Choice c = j->answer;
    gboolean all = j->for_all;
    g_mutex_unlock(&j->lock);
    if (all && c != CH_CANCEL)
        j->all_choice = c;
    return c;
}

/* ---- the work (worker thread) ---------------------------------------------------------- */
#define COPY_FLAGS (G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA)

static void post(Job *j, char *text, double frac)      /* takes text */
{
    Progress *pr = g_new0(Progress, 1);
    pr->j = j;
    pr->text = text;
    pr->frac = frac;
    g_idle_add(ui_progress, pr);
}

/* the size of everything under f, links not followed (as they are copied); the bar pulses meanwhile */
static guint64 tree_size(Job *j, GFile *f)
{
    GFileInfo *fi = g_file_query_info(f, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      j->cancel, NULL);
    if (!fi)
        return 0;
    guint64 n = g_file_info_get_size(fi);
    gboolean dir = g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY;
    g_object_unref(fi);
    if (!dir)
        return n;
    gint64 now = g_get_monotonic_time();
    if (now - j->shown > 200000) {
        j->shown = now;
        post(j, g_strdup(_("Counting what to copy …")), -1);
    }
    GFileEnumerator *en = g_file_enumerate_children(f, "standard::name,standard::type,standard::size",
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, NULL);
    GFileInfo *info;
    while (en && !g_cancellable_is_cancelled(j->cancel) && (info = g_file_enumerator_next_file(en, j->cancel, NULL))) {
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            GFile *c = g_file_get_child(f, g_file_info_get_name(info));
            n += tree_size(j, c);
            g_object_unref(c);
        } else {
            n += g_file_info_get_size(info);
        }
        g_object_unref(info);
    }
    g_clear_object(&en);
    return n;
}

static void show_bytes(Job *j, gboolean now_anyway)
{
    gint64 t = g_get_monotonic_time();
    if (!now_anyway && t - j->shown < 100000)
        return;
    j->shown = t;
    char *a = g_format_size(j->now), *b = g_format_size(j->total);
    /* TRANSLATORS: "Copying name (2/5) · 1.2 GB of 4.0 GB" */
    post(j, g_strdup_printf(_("%s · %s of %s"), j->label, a, b),
         j->total ? MIN(1.0, (double) j->now / (double) j->total) : 0);
    g_free(a);
    g_free(b);
}

static void copy_progress(goffset cur, goffset total, gpointer data)
{
    Job *j = data;
    j->now = j->file_base + (guint64) cur;
    show_bytes(j, FALSE);
}

static guint64 file_size(GFile *f)
{
    GFileInfo *fi = g_file_query_info(f, "standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    guint64 n = fi ? g_file_info_get_size(fi) : 0;
    g_clear_object(&fi);
    return n;
}

/* GLib only says "Cannot retrieve attribute standard::type" when it may not read a file
 * (brobr), so say what it really is. Root is refused only on a few mounts: one user's own
 * (sshfs, rclone, a phone, GVfs's /run/user/…/gvfs, without allow_other) and NFS with
 * root_squash. A normal user is refused inside a folder of someone else, like root's
 * trash in /root dragged from a root window (brobr, 0.8.8). */
static void root_explain(GFile *src, GError **err)
{
    if (!err || !*err)
        return;
    char *path = g_file_get_path(src);
    GStatBuf st;
    if (!path || !g_lstat(path, &st) || errno != EACCES) {
        g_free(path);
        return;
    }
    GError *e = NULL;
    if (is_root()) {
        e = g_error_new(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        _("root may not read it: it is on a mount that only its own user may open "
                          "(sshfs, a phone, a network share). Copy it in a normal millerX window."));
    } else {
        /* the first folder up the path we can see is the one we may not enter */
        char *dir = g_path_get_dirname(path);
        while (g_lstat(dir, &st) && strcmp(dir, "/")) {
            char *up = g_path_get_dirname(dir);
            g_free(dir);
            dir = up;
        }
        struct passwd *pw = getpwuid(st.st_uid);
        if (st.st_uid == 0)
            e = g_error_new(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            _("You may not read it: %s belongs to root. Copy it in a millerX window opened as root."),
                            dir);
        else if (st.st_uid == getuid())
            e = g_error_new(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            _("You may not read it: %s is yours, but its permissions shut you out."), dir);
        else
            e = g_error_new(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            _("You may not read it: %s belongs to %s."), dir,
                            pw && pw->pw_name ? pw->pw_name : _("another user"));
        g_free(dir);
    }
    g_clear_error(err);
    *err = e;
    g_free(path);
}

static gboolean copy_tree(Job *j, GFile *src, GFile *dst, gboolean replace, GError **err)
{
    if (g_cancellable_is_cancelled(j->cancel))
        return TRUE;
    if (is_dir(src, FALSE)) {
        if (!g_file_query_exists(dst, NULL) && !g_file_make_directory(dst, j->cancel, err))
            return FALSE;
        GFileEnumerator *en = g_file_enumerate_children(src, "standard::name",
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, err);
        if (!en)
            return FALSE;
        GFileInfo *info;
        gboolean ok = TRUE;
        while (ok && (info = g_file_enumerator_next_file(en, j->cancel, err))) {
            const char *n = g_file_info_get_name(info);
            GFile *s = g_file_get_child(src, n), *d = g_file_get_child(dst, n);
            ok = copy_tree(j, s, d, replace, err);
            g_object_unref(s);
            g_object_unref(d);
            g_object_unref(info);
        }
        g_object_unref(en);
        return ok && !(err && *err);
    }
    GFileCopyFlags flags = COPY_FLAGS;
    GFile *target = g_object_ref(dst);
    if (g_file_query_exists(dst, NULL)) {
        if (!replace) {
            Choice c = ask(j, src, dst, FALSE);
            if (c == CH_SKIP) {
                g_object_unref(target);
                return TRUE;
            }
            if (c == CH_CANCEL) {
                g_cancellable_cancel(j->cancel);
                g_object_unref(target);
                return TRUE;
            }
            if (c == CH_BOTH) {
                GFile *parent = g_file_get_parent(dst);
                char *base = g_file_get_basename(dst);
                g_object_unref(target);
                target = unique_child(parent, base);
                g_free(base);
                g_object_unref(parent);
            }
        }
        flags |= G_FILE_COPY_OVERWRITE;
    }
    j->file_base = j->done;
    gboolean ok = g_file_copy(src, target, flags, j->cancel, j->bytes ? copy_progress : NULL, j, err);
    if (j->bytes) {
        j->done = j->now = j->file_base + file_size(src);
        show_bytes(j, FALSE);
    }
    if (!ok)
        root_explain(src, err);
    g_object_unref(target);
    return ok;
}

static gboolean one(Job *j, GFile *src, GError **err)
{
    GFile *dest = j->dest;
    GFile *parent = g_file_get_parent(src);
    char *base = g_file_get_basename(src);
    gboolean ok = TRUE;
    if (parent && g_file_equal(parent, dest)) {          /* same folder */
        if (!j->move) {
            GFile *t = unique_child(dest, base);
            ok = copy_tree(j, src, t, FALSE, err);
            g_object_unref(t);
        }
        goto out;
    }
    if (g_file_equal(dest, src) || g_file_has_prefix(dest, src)) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("can't put a folder into itself"));
        ok = FALSE;
        goto out;
    }
    GFile *target = g_file_get_child(dest, base);
    gboolean replace = FALSE;
    if (g_file_query_exists(target, NULL)) {
        gboolean both_dirs = is_dir(src, FALSE) && is_dir(target, FALSE);
        Choice c = ask(j, src, target, both_dirs);
        if (c == CH_SKIP || c == CH_CANCEL) {
            if (c == CH_CANCEL)
                g_cancellable_cancel(j->cancel);
            g_object_unref(target);
            goto out;
        }
        if (c == CH_BOTH) {
            g_object_unref(target);
            target = unique_child(dest, base);
        } else {
            replace = TRUE;
        }
    }
    if (j->move) {
        gboolean merge = replace && is_dir(target, FALSE);
        if (!merge) {
            GError *e = NULL;
            j->file_base = j->done;              /* to another disk GLib copies a file, then deletes it */
            if (g_file_move(src, target, COPY_FLAGS | (replace ? G_FILE_COPY_OVERWRITE : 0),
                            j->cancel, j->bytes ? copy_progress : NULL, j, &e)) {
                g_object_unref(target);
                goto out;
            }
            if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)
                && !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
                root_explain(src, &e);
                g_propagate_error(err, e);
                g_object_unref(target);
                ok = FALSE;
                goto out;
            }
            g_error_free(e);
        }
        /* other file system or merging folders: copy, then delete the original */
        ok = copy_tree(j, src, target, replace, err)
             && !g_cancellable_is_cancelled(j->cancel)
             && delete_tree(src, j->cancel, err);
        if (g_cancellable_is_cancelled(j->cancel) && !(err && *err))
            ok = TRUE;
    } else {
        ok = copy_tree(j, src, target, replace, err);
    }
    g_object_unref(target);
out:
    g_free(base);
    g_clear_object(&parent);
    return ok;
}

/* ---- archives (worker thread) ------------------------------------------------------------ */
typedef struct {
    Job *j;
    const char *verb, *what;
    double base, span;      /* this archive's part of the whole bar */
} ArcCtx;

static void arc_progress(gpointer data, const char *name, double frac)
{
    ArcCtx *x = data;
    Progress *pr = g_new0(Progress, 1);
    pr->j = x->j;
    pr->text = g_strdup_printf("%s %s: %s", x->verb, x->what, name);
    pr->frac = x->base + frac * x->span;
    g_idle_add(ui_progress, pr);
}

/* "name", or "name (2)", "name (3)", … whichever is free in folder */
static GFile *free_child(GFile *folder, const char *name)
{
    GFile *f = g_file_get_child(folder, name);
    for (int n = 2; g_file_query_exists(f, NULL); n++) {
        g_object_unref(f);
        char *cand = g_strdup_printf("%s (%d)", name, n);
        f = g_file_get_child(folder, cand);
        g_free(cand);
    }
    return f;
}

/* Unpacks into a hidden temporary folder in the target first (next to the archive, or the folder
 * chosen with "Extract to…"), then puts the result in place: one top item moves there (asking on
 * conflicts, like a paste); several items become a folder named after the archive. Nothing
 * half-done is left behind. */
static gboolean extract_one(Job *j, GFile *archive, double base, double span, GError **err)
{
    char *path = g_file_get_path(archive);
    GFile *dest = j->kind == K_EXTRACT_TO ? g_object_ref(j->dest) : g_file_get_parent(archive);
    char *dpath = dest ? g_file_get_path(dest) : NULL;
    if (!path || !dpath) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("only local files can be extracted"));
        g_free(path);
        g_free(dpath);
        g_clear_object(&dest);
        return FALSE;
    }
    char *base_name = g_file_get_basename(archive);
    char *stem = arc_stem(base_name);
    char *tmpl = g_build_filename(dpath, ".millerx-extract-XXXXXX", NULL);
    gboolean ok = FALSE;
    if (!g_mkdtemp(tmpl)) {
        g_set_error(err, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
        goto out;
    }
    ArcCtx x = { j, "Extracting", base_name, base, span };
    ok = arc_extract(path, tmpl, j->cancel, arc_progress, &x, err);
    GFile *tmp = g_file_new_for_path(tmpl);
    if (ok && !g_cancellable_is_cancelled(j->cancel)) {
        GList *kids = NULL;
        GFileEnumerator *en = g_file_enumerate_children(tmp, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        NULL, NULL);
        GFileInfo *fi;
        while (en && (fi = g_file_enumerator_next_file(en, NULL, NULL))) {
            kids = g_list_prepend(kids, g_file_get_child(tmp, g_file_info_get_name(fi)));
            g_object_unref(fi);
        }
        g_clear_object(&en);
        guint n = g_list_length(kids);
        gboolean single = n == 1;
        if (n == 0) {
            if (err && !*err)
                g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_FAILED, _("the archive is empty"));
            ok = FALSE;
        } else if (single) {
            /* one top item: move it next to the archive, like a paste (asks if it exists) */
            GFile *saved = j->dest;
            gboolean saved_move = j->move;
            j->dest = dest;
            j->move = TRUE;
            ok = one(j, kids->data, err);
            j->dest = saved;
            j->move = saved_move;
        } else {
            /* a new folder (the umask applies, not the temporary folder's 0700), the items moved in */
            GFile *target = free_child(dest, stem);
            ok = g_file_make_directory(target, NULL, err);
            for (GList *l = kids; ok && l; l = l->next) {
                char *kn = g_file_get_basename(l->data);
                GFile *to = g_file_get_child(target, kn);
                ok = g_file_move(l->data, to, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, err);
                g_object_unref(to);
                g_free(kn);
            }
            if (!ok && g_file_query_exists(target, NULL))   /* nothing half-done is left behind */
                delete_tree(target, NULL, NULL);
            g_object_unref(target);
        }
        g_list_free_full(kids, g_object_unref);
    }
    if (g_file_query_exists(tmp, NULL))       /* cancelled, failed, skipped or moved out of */
        delete_tree(tmp, NULL, NULL);
    g_object_unref(tmp);
out:
    g_free(tmpl);
    g_free(stem);
    g_free(base_name);
    g_free(path);
    g_free(dpath);
    g_object_unref(dest);
    return ok;
}

static gpointer compress_thread(gpointer data)
{
    Job *j = data;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = j->sources; l; l = l->next) {
        char *p = g_file_get_path(l->data);
        if (p)
            g_ptr_array_add(paths, p);
    }
    g_ptr_array_add(paths, NULL);
    char *outp = g_file_get_path(j->out);
    char *dpath = g_file_get_path(j->dest);
    char *name = g_file_get_basename(j->out);
    GError *err = NULL;
    if (!outp || !dpath || paths->len < 2) {
        g_set_error_literal(&err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("only local files can be packed"));
    } else {
        /* written under a hidden name first, renamed when complete */
        char *dir = g_path_get_dirname(outp);
        char *tmp = g_strdup_printf("%s/.%s.part-XXXXXX", dir, name);
        int fd = g_mkstemp(tmp);
        if (fd < 0) {
            g_set_error(&err, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
        } else {
            close(fd);
            ArcCtx x = { j, "Packing", name, 0, 1 };
            gboolean ok = arc_create(tmp, j->format, (char **) paths->pdata, dpath, j->cancel, arc_progress, &x,
                                     &err);
            if (ok && !g_cancellable_is_cancelled(j->cancel) && g_rename(tmp, outp) != 0)
                g_set_error(&err, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
            g_unlink(tmp);          /* gone after the rename anyway */
        }
        g_free(tmp);
        g_free(dir);
    }
    if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_ptr_array_add(j->errors, g_strdup_printf("%s: %s", name, err->message));
        j->denied |= g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    }
    g_clear_error(&err);
    g_free(name);
    g_free(dpath);
    g_free(outp);
    g_ptr_array_unref(paths);
    g_idle_add(ui_done, j);
    return NULL;
}

/* bytes for the bar: a copy always; a move only when something goes to another disk (on the
 * same disk it is a rename, done at once, and counting a big folder would only slow it down) */
static gboolean wants_bytes(Job *j)
{
    if (j->kind != K_COPY)
        return FALSE;
    if (!j->move)
        return TRUE;
    dev_t to, from;
    if (!device_of(j->dest, &to))
        return TRUE;
    for (GList *l = j->sources; l; l = l->next)
        if (!device_of(l->data, &from) || from != to)
            return TRUE;
    return FALSE;
}

static gpointer job_thread(gpointer data)
{
    Job *j = data;
    guint n = g_list_length(j->sources), i = 0;
    guint64 *sizes = g_new0(guint64, n + 1);
    j->bytes = wants_bytes(j);
    for (GList *l = j->sources; j->bytes && l && !g_cancellable_is_cancelled(j->cancel); l = l->next, i++)
        j->total += sizes[i] = tree_size(j, l->data);
    i = 0;
    for (GList *l = j->sources; l; l = l->next) {
        if (g_cancellable_is_cancelled(j->cancel))
            break;
        i++;
        char *b = g_file_get_basename(l->data);
        g_free(j->label);
        j->label = g_strdup_printf("%s %s (%u/%u)", j->kind != K_COPY ? _("Extracting") : j->move ? _("Moving")
                                   : _("Copying"), b, i, n);
        guint64 before = j->done;
        if (j->bytes) {
            j->now = j->done;
            show_bytes(j, TRUE);
        } else {
            post(j, g_strdup(j->label), (double) (i - 1) / n);
        }
        GError *err = NULL;
        gboolean ok = j->kind == K_COPY ? one(j, l->data, &err)
                                        : extract_one(j, l->data, (double) (i - 1) / n, 1.0 / n, &err);
        if (j->bytes)                /* skipped, renamed or failed files count as done too */
            j->done = j->now = before + sizes[i - 1];
        if (ok)
            j->done_n++;
        if (err) {                 /* also when it worked but something was left out */
            if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_error_free(err);
                g_free(b);
                break;
            }
            g_ptr_array_add(j->errors, g_strdup_printf("%s: %s", b, err->message));
            j->denied |= g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        }
        g_clear_error(&err);
        g_free(b);
    }
    g_free(sizes);
    g_idle_add(ui_done, j);
    return NULL;
}

static Job *job_start(Win *w, Kind kind, gboolean move, GList *sources, GFile *dest, GFile *out, int format,
                      GThreadFunc fn)
{
    Job *j = g_new0(Job, 1);
    j->kind = kind;
    j->out = out ? g_object_ref(out) : NULL;
    j->format = format;
    j->win = win_ref(w);
    j->move = move;
    j->sources = g_list_copy_deep(sources, obj_copy, NULL);
    j->dest = g_object_ref(dest);
    j->cancel = g_cancellable_new();
    j->errors = g_ptr_array_new_with_free_func(g_free);
    j->all_choice = -1;
    g_mutex_init(&j->lock);
    g_cond_init(&j->cond);
    w->jobs = g_list_append(w->jobs, j);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->progress), 0);
    gtk_widget_show(w->progress);
    gtk_widget_show(w->cancel_btn);
    g_thread_unref(g_thread_new("millerx-job", fn, j));
    return j;
}

Job *job_run(Win *w, gboolean move, GList *sources, GFile *dest)
{
    return job_start(w, K_COPY, move, sources, dest, NULL, 0, job_thread);
}

/* to NULL: each archive is unpacked next to itself (dest, only for the job's bookkeeping, is the
 * first one's folder); otherwise all of them into the folder to */
Job *job_extract(Win *w, GList *archives, GFile *to)
{
    GFile *dest = to ? g_object_ref(to) : g_file_get_parent(archives->data);
    Job *j = job_start(w, to ? K_EXTRACT_TO : K_EXTRACT, FALSE, archives, dest, NULL, 0, job_thread);
    g_object_unref(dest);
    return j;
}

/* sources all sit in one folder (a column's selection); out is the archive to create there */
Job *job_compress(Win *w, GList *sources, GFile *out, int format)
{
    GFile *dest = g_file_get_parent(out);
    Job *j = job_start(w, K_COMPRESS, FALSE, sources, dest, out, format, compress_thread);
    g_object_unref(dest);
    return j;
}
