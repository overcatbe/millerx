/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Copy or move files in the background; asks on conflicts. */
#include "millerx.h"

struct Job {
    Win *win;
    gboolean move;
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
    /* progress */
    char *text;
    double frac;
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
    g_object_unref(j->cancel);
    g_ptr_array_unref(j->errors);
    g_mutex_clear(&j->lock);
    g_cond_clear(&j->cond);
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
            win_status(w, "Done with %u error(s)", j->errors->len);
            GString *s = g_string_new(NULL);
            for (guint i = 0; i < j->errors->len && i < 20; i++)
                g_string_append_printf(s, "%s%s", i ? "\n" : "", (char *) g_ptr_array_index(j->errors, i));
            win_error_root(w, "Some items failed", s->str, j->denied ? j->dest : NULL);
            g_string_free(s, TRUE);
        } else if (g_cancellable_is_cancelled(j->cancel)) {
            win_status(w, "Cancelled");
        } else {
            char *to = g_file_get_basename(j->dest);
            win_status(w, "%s %u item(s) to %s", j->move ? "Moved" : "Copied", g_list_length(j->sources), to);
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
    gboolean ok = g_file_copy(src, target, flags, j->cancel, NULL, NULL, err);
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
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "can't put a folder into itself");
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
            if (g_file_move(src, target, COPY_FLAGS | (replace ? G_FILE_COPY_OVERWRITE : 0),
                            j->cancel, NULL, NULL, &e)) {
                g_object_unref(target);
                goto out;
            }
            if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)
                && !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
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

static gpointer job_thread(gpointer data)
{
    Job *j = data;
    guint n = g_list_length(j->sources), i = 0;
    for (GList *l = j->sources; l; l = l->next) {
        if (g_cancellable_is_cancelled(j->cancel))
            break;
        i++;
        char *b = g_file_get_basename(l->data);
        Progress *pr = g_new0(Progress, 1);
        pr->j = j;
        pr->text = g_strdup_printf("%s %s (%u/%u)", j->move ? "Moving" : "Copying", b, i, n);
        pr->frac = (double) (i - 1) / n;
        g_idle_add(ui_progress, pr);
        GError *err = NULL;
        if (!one(j, l->data, &err) && err) {
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
    g_idle_add(ui_done, j);
    return NULL;
}

Job *job_run(Win *w, gboolean move, GList *sources, GFile *dest)
{
    Job *j = g_new0(Job, 1);
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
    g_thread_unref(g_thread_new("millerx-job", job_thread, j));
    return j;
}
