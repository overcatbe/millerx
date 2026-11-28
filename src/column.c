/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* One folder shown as a list. */
#include "millerx.h"
#include <string.h>

static const GtkTargetEntry URI_TARGET[] = { { (char *) "text/uri-list", 0, 0 } };
static gboolean DRAG_ASK;             /* the running drag started with the right button: its drop asks */

/* Clicks and drags: Unleashed (Settings → Unleash): a drag carries what was picked up, not what is selected, and a folder held
 * still under a drag for a moment opens (only where that can't take away the column it came from) */
static GList *DRAG_FILES;             /* GFile*, what the running drag from a column carries */
static Column *DRAG_SRC;              /* the column it started in (a ref) */
static guint SPRING_ID;               /* the timer for opening the folder under the drag */
static Column *SPRING_COL;            /* (a ref) */
static GtkTreeRowReference *SPRING_ROW;
#define SPRING_MS 700

static void held_clear(Column *c);
static void left_prev_free(Column *c);

void column_ref(Column *c)
{
    c->ref++;
}

void column_unref(Column *c)
{
    if (--c->ref > 0)
        return;
    g_clear_object(&c->gfile);
    g_clear_object(&c->store);
    g_clear_object(&c->cancel);
    if (c->chain)
        g_ptr_array_unref(c->chain);
    if (c->objs)
        g_ptr_array_unref(c->objs);
    g_free(c->pending_select);
    g_string_free(c->typed, TRUE);
    if (c->held)
        gtk_tree_row_reference_free(c->held);
    left_prev_free(c);
    win_unref(c->win);
    g_free(c);
}

void column_close(Column *c)
{
    if (c->closed)
        return;
    c->closed = TRUE;
    g_cancellable_cancel(c->cancel);
    git_colours_stop(c);
    if (c->monitor) {
        g_signal_handlers_disconnect_by_data(c->monitor, c);
        g_file_monitor_cancel(c->monitor);
        g_clear_object(&c->monitor);
    }
    if (c->view)
        g_signal_handlers_disconnect_by_data(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), c);
}

/* the row held without being selected (Unleashed): right-clicked, or picked up by a drag */
static gboolean is_held(Column *c, GtkTreeModel *m, GtkTreeIter *it)
{
    if (!c || !c->held || !gtk_tree_row_reference_valid(c->held))
        return FALSE;
    GtkTreePath *a = gtk_tree_row_reference_get_path(c->held), *b = gtk_tree_model_get_path(m, it);
    gboolean eq = !gtk_tree_path_compare(a, b);
    gtk_tree_path_free(a);
    gtk_tree_path_free(b);
    return eq;
}

/* links in italics, like other file managers show them; the held row bold and underlined */
static void name_func(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    (void) col;
    gboolean link = FALSE, held = is_held(d, m, it);
    int git = GIT_CLEAN;
    gtk_tree_model_get(m, it, C_LINK, &link, C_GIT, &git, -1);
    g_object_set(r, "style", link ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
                 "underline", held ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE,
                 "weight", held ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, NULL);
    /* Git colours (git.c): green new, red changed; readable on light and dark themes */
    static const GdkRGBA green = { 0.22, 0.66, 0.30, 1 }, red = { 0.88, 0.30, 0.27, 1 };
    if (git == GIT_NEW)
        g_object_set(r, "foreground-rgba", &green, NULL);
    else if (git == GIT_CHANGED)
        g_object_set(r, "foreground-rgba", &red, NULL);
    else
        g_object_set(r, "foreground-set", FALSE, NULL);
}

static void held_set(Column *c, GtkTreePath *p)
{
    held_clear(c);
    c->held = gtk_tree_row_reference_new(GTK_TREE_MODEL(c->store), p);
    gtk_widget_queue_draw(c->view);
}

static void held_clear(Column *c)
{
    if (!c->held)
        return;
    gtk_tree_row_reference_free(c->held);
    c->held = NULL;
    if (!c->closed && c->view)
        gtk_widget_queue_draw(c->view);
}

/* the held row's file (a new ref), or NULL */
static GFile *held_file(Column *c)
{
    GFile *f = NULL;
    if (c->held && gtk_tree_row_reference_valid(c->held)) {
        GtkTreePath *p = gtk_tree_row_reference_get_path(c->held);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(c->store), &it, p))
            gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, C_FILE, &f, -1);
        gtk_tree_path_free(p);
    }
    return f;
}

static void left_prev_free(Column *c)
{
    g_list_free_full(c->left_prev, (GDestroyNotify) gtk_tree_row_reference_free);
    c->left_prev = NULL;
    if (c->left_row)
        gtk_tree_row_reference_free(c->left_row);
    c->left_row = NULL;
}

/* ---- loading ------------------------------------------------------------- */
static GtkTreeIter *append_info(Column *c, GFileInfo *info, GFile *child, GtkTreeIter *out)
{
    if (!c->win->show_hidden && (g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info)))
        return NULL;
    gboolean isdir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    const char *name = g_file_info_get_display_name(info);
    char *key = sort_key(name, isdir);
    char *ext = isdir ? g_strdup("") : sorting_ext(name);
    /* in the trash itself: when it was deleted (what "newest" and "oldest" sort by) */
    GDateTime *mt = c->trash_root ? g_file_info_get_deletion_date(info) : g_file_info_get_modification_date_time(info);
    gint64 mtime = mt ? g_date_time_to_unix(mt) : 0;
    if (mt)
        g_date_time_unref(mt);
    /* a link: its target's icon with the link emblem, and (see name_func) its name in italics */
    gboolean link = g_file_info_get_is_symlink(info);
    GIcon *icon = g_file_info_get_icon(info);
    GIcon *shown = link && icon ? link_icon(icon) : (icon ? g_object_ref(icon) : NULL);
    gtk_list_store_insert_with_values(c->store, out, -1, C_ICON, shown,
                                      C_NAME, name, C_ISDIR, isdir, C_FILE, child, C_SORT, key,
                                      C_MTIME, mtime, C_SIZE, (guint64) g_file_info_get_size(info),
                                      C_EXT, ext, C_LINK, link, -1);
    if (shown)
        g_object_unref(shown);
    g_free(ext);
    g_free(key);
    return out;
}

static void loading_done(Column *c)
{
    c->loaded = TRUE;
    git_colours_schedule(c);
    column_continue_chain(c);
    if (c == win_last_column(c->win)) {
        char *p = file_path(c->gfile);
        int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(c->store), NULL);
        win_status(c->win, ngettext("%s: %d item", "%s: %d items", n), p, n);
        g_free(p);
    }
}

/* when opening a path: select the next name, then let the new column carry on */
void column_continue_chain(Column *c)
{
    if (!c->chain || !c->loaded)
        return;
    GPtrArray *chain = c->chain;
    c->chain = NULL;
    if (chain->len && column_select_name(c, g_ptr_array_index(chain, 0))) {
        Column *last = win_last_column(c->win);
        if (last != c && chain->len > 1) {
            g_ptr_array_remove_index(chain, 0);
            last->chain = chain;
            column_continue_chain(last);
            return;
        }
    }
    g_ptr_array_unref(chain);
    win_chain_finished(c->win);
}

static gboolean find_iter(Column *c, GFile *f, GtkTreeIter *it);

static void on_files(GObject *src, GAsyncResult *res, gpointer data)
{
    Column *c = data;
    GFileEnumerator *en = G_FILE_ENUMERATOR(src);
    GError *err = NULL;
    GList *infos = g_file_enumerator_next_files_finish(en, res, &err);
    if (c->closed || err) {
        g_list_free_full(infos, g_object_unref);
        if (err && !c->closed)
            loading_done(c);
        g_clear_error(&err);
        column_unref(c);
        return;
    }
    for (GList *l = infos; l; l = l->next) {
        GFile *child = g_file_enumerator_get_child(en, l->data);
        GtkTreeIter it;
        if (!c->added_early || !find_iter(c, child, &it))   /* not twice (the trash reports its */
            append_info(c, l->data, child, &it);              /* items as new while being read) */
        g_object_unref(child);
    }
    if (infos) {
        g_list_free_full(infos, g_object_unref);
        g_file_enumerator_next_files_async(en, 200, G_PRIORITY_DEFAULT, c->cancel, on_files, c);
        return;
    }
    g_file_enumerator_close_async(en, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    loading_done(c);
    column_unref(c);
}

static void on_enum(GObject *src, GAsyncResult *res, gpointer data)
{
    Column *c = data;
    GError *err = NULL;
    GFileEnumerator *en = g_file_enumerate_children_finish(G_FILE(src), res, &err);
    if (c->closed) {
        g_clear_error(&err);
        g_clear_object(&en);
        column_unref(c);
        return;
    }
    if (!en) {
        char *p = file_path(c->gfile);
        win_status(c->win, "%s: %s", p, err->message);
        g_free(p);
        g_clear_error(&err);
        loading_done(c);
        column_unref(c);
        return;
    }
    g_file_enumerator_next_files_async(en, 200, G_PRIORITY_DEFAULT, c->cancel, on_files, c);
    g_object_unref(en);   /* the async call keeps its own ref */
}

static void column_load(Column *c)
{
    gtk_list_store_clear(c->store);
    c->loaded = FALSE;
    c->added_early = FALSE;
    column_ref(c);
    g_file_enumerate_children_async(c->gfile, ATTRS, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                    c->cancel, on_enum, c);
}

/* ---- changes on disk ---------------------------------------------------------- */
static gboolean find_iter(Column *c, GFile *f, GtkTreeIter *it)
{
    GtkTreeModel *m = GTK_TREE_MODEL(c->store);
    gboolean ok = gtk_tree_model_get_iter_first(m, it);
    while (ok) {
        GFile *x;
        gtk_tree_model_get(m, it, C_FILE, &x, -1);
        gboolean eq = x && g_file_equal(x, f);
        g_clear_object(&x);
        if (eq)
            return TRUE;
        ok = gtk_tree_model_iter_next(m, it);
    }
    return FALSE;
}

void column_add_file(Column *c, GFile *f)
{
    GtkTreeIter it;
    if (find_iter(c, f, &it))
        return;
    GFileInfo *info = g_file_query_info(f, ATTRS, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (!info)
        return;
    if (!c->loaded)
        c->added_early = TRUE;
    if (append_info(c, info, f, &it) && c->pending_select
        && !strcmp(c->pending_select, g_file_info_get_display_name(info))) {
        g_clear_pointer(&c->pending_select, g_free);
        column_select_iter(c, &it);
    }
    g_object_unref(info);
}

static void on_selection(GtkTreeSelection *sel, Column *c);

/* GTK moves the cursor to the next row when the cursor's row goes and selects that one, so a
 * moved or trashed folder would open its neighbour. Keep the rest of the selection as it was:
 * if the gone row was selected, this column's folder is what's left to show. */
void column_remove_file(Column *c, GFile *f)
{
    GtkTreeIter it;
    if (!find_iter(c, f, &it))
        return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
    gboolean was = gtk_tree_selection_iter_is_selected(sel, &it);
    GList *keep = column_selected_files(c);
    g_signal_handlers_block_by_func(sel, on_selection, c);
    gtk_list_store_remove(c->store, &it);
    gtk_tree_selection_unselect_all(sel);
    for (GList *l = keep; l; l = l->next)
        if (!g_file_equal(l->data, f) && find_iter(c, l->data, &it))
            gtk_tree_selection_select_iter(sel, &it);
    g_signal_handlers_unblock_by_func(sel, on_selection, c);
    g_list_free_full(keep, g_object_unref);
    if (was) {
        win_column_selected(c->win, c);
        if (!column_n_selected(c)) {
            char *p = file_path(c->gfile);
            int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(c->store), NULL);
            win_status(c->win, ngettext("%s: %d item", "%s: %d items", n), p, n);
            g_free(p);
        }
    }
}

static void on_changed(GFileMonitor *mon, GFile *f, GFile *other, GFileMonitorEvent ev, Column *c)
{
    (void) mon;
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        column_add_file(c, f);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        column_remove_file(c, f);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED: {
        GFile *sel;
        gboolean was = column_selected_single(c, &sel, NULL) && g_file_equal(sel, f);
        if (sel)
            g_object_unref(sel);
        column_remove_file(c, f);
        if (other) {
            if (was) {
                g_free(c->pending_select);
                c->pending_select = g_file_get_basename(other);
            }
            column_add_file(c, other);
        }
        break;
    }
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT: {
        GtkTreeIter it;
        GFileInfo *fi = find_iter(c, f, &it) ? g_file_query_info(f, ATTRS, G_FILE_QUERY_INFO_NONE, NULL, NULL)
                                              : NULL;
        if (fi) {
            GDateTime *mt = c->trash_root ? g_file_info_get_deletion_date(fi)
                                          : g_file_info_get_modification_date_time(fi);
            gtk_list_store_set(c->store, &it, C_MTIME, mt ? g_date_time_to_unix(mt) : (gint64) 0,
                               C_SIZE, (guint64) g_file_info_get_size(fi), -1);
            if (mt)
                g_date_time_unref(mt);
            g_object_unref(fi);
        }
        GFile *sel;
        if (column_selected_single(c, &sel, NULL)) {
            if (g_file_equal(sel, f))
                preview_show_file(c->win->preview, f);
            g_object_unref(sel);
        }
        break;
    }
    default:
        break;
    }
    git_colours_schedule(c);          /* a file changed: its colour may too */
}

/* ---- selection ------------------------------------------------------------------ */
/* Scrolling right after a column was made can use its old (tiny) height and do nothing:
 * once layout has run, scroll to the cursor again. */
static gboolean rescroll(gpointer data)
{
    Column *c = data;
    if (!c->closed && !c->win->dead) {
        GtkTreePath *p = NULL;
        gtk_tree_view_get_cursor(GTK_TREE_VIEW(c->view), &p, NULL);
        if (p) {
            GdkRectangle vis, row;
            gtk_tree_view_get_visible_rect(GTK_TREE_VIEW(c->view), &vis);
            gtk_tree_view_get_background_area(GTK_TREE_VIEW(c->view), p, NULL, &row);
            /* row in tree coordinates: visible if inside the visible rect */
            int ty;
            gtk_tree_view_convert_bin_window_to_tree_coords(GTK_TREE_VIEW(c->view), 0, row.y, NULL, &ty);
            if (ty < vis.y || ty + row.height > vis.y + vis.height)
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(c->view), p, NULL, TRUE, 0.33, 0);
            gtk_tree_path_free(p);
        }
    }
    column_unref(c);
    return G_SOURCE_REMOVE;
}

void column_select_iter(Column *c, GtkTreeIter *it)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
    gtk_tree_selection_unselect_all(sel);
    gtk_tree_selection_select_iter(sel, it);
    GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(c->store), it);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(c->view), p, NULL, FALSE);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(c->view), p, NULL, FALSE, 0, 0);
    gtk_tree_path_free(p);
    column_ref(c);
    g_timeout_add(60, rescroll, c);        /* after GTK's resize + redraw */
}

gboolean column_select_name(Column *c, const char *name)
{
    GtkTreeModel *m = GTK_TREE_MODEL(c->store);
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        char *n;
        gtk_tree_model_get(m, &it, C_NAME, &n, -1);
        gboolean eq = !g_strcmp0(n, name);
        g_free(n);
        if (eq) {
            column_select_iter(c, &it);
            return TRUE;
        }
        ok = gtk_tree_model_iter_next(m, &it);
    }
    return FALSE;
}

GList *column_selected_files(Column *c)
{
    GtkTreeModel *m;
    GList *rows = gtk_tree_selection_get_selected_rows(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), &m), *out = NULL;
    for (GList *l = rows; l; l = l->next) {
        GtkTreeIter it;
        GFile *f;
        gtk_tree_model_get_iter(m, &it, l->data);
        gtk_tree_model_get(m, &it, C_FILE, &f, -1);
        out = g_list_prepend(out, f);
    }
    g_list_free_full(rows, (GDestroyNotify) gtk_tree_path_free);
    return g_list_reverse(out);
}

int column_n_selected(Column *c)
{
    return gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)));
}

/* exactly one row selected: its file (new ref) and whether it is a folder */
gboolean column_selected_single(Column *c, GFile **f, gboolean *isdir)
{
    *f = NULL;
    GtkTreeModel *m;
    GList *rows = gtk_tree_selection_get_selected_rows(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), &m);
    gboolean one = rows && !rows->next;
    if (one) {
        GtkTreeIter it;
        gboolean d;
        gtk_tree_model_get_iter(m, &it, rows->data);
        gtk_tree_model_get(m, &it, C_FILE, f, C_ISDIR, &d, -1);
        if (isdir)
            *isdir = d;
    }
    g_list_free_full(rows, (GDestroyNotify) gtk_tree_path_free);
    return one;
}

static void on_selection(GtkTreeSelection *sel, Column *c)
{
    (void) sel;
    if (c->left_wait)           /* a left press: a click or a drag? the release or the drag says */
        return;
    win_column_selected(c->win, c);
}

/* the left press was a click after all: now it selects (and opens) as always */
static void left_commit(Column *c)
{
    if (!c->left_wait)
        return;
    c->left_wait = FALSE;
    left_prev_free(c);
    if (!c->closed)
        win_column_selected(c->win, c);
}

static gboolean left_commit_idle(gpointer data)
{
    Column *c = data;
    left_commit(c);
    column_unref(c);
    return G_SOURCE_REMOVE;
}

/* The gallery's picks shown in this column (the folder's own one): the same rows selected and the
 * first scrolled into view, without the usual consequences (the preview keeps the gallery). */
void column_mark_files(Column *c, GList *files)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
    g_signal_handlers_block_by_func(sel, on_selection, c);
    gtk_tree_selection_unselect_all(sel);
    GtkTreeModel *m = GTK_TREE_MODEL(c->store);
    GtkTreeIter it;
    gboolean first = TRUE;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        GFile *f;
        gtk_tree_model_get(m, &it, C_FILE, &f, -1);
        for (GList *l = files; l && f; l = l->next)
            if (g_file_equal(f, l->data)) {
                gtk_tree_selection_select_iter(sel, &it);
                if (first) {
                    GtkTreePath *p = gtk_tree_model_get_path(m, &it);
                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(c->view), p, NULL, FALSE, 0, 0);
                    gtk_tree_path_free(p);
                    first = FALSE;
                }
                break;
            }
        g_clear_object(&f);
    }
    g_signal_handlers_unblock_by_func(sel, on_selection, c);
}

static void on_activated(GtkTreeView *v, GtkTreePath *path, GtkTreeViewColumn *col, Column *c)
{
    (void) v; (void) col;
    GtkTreeIter it;
    GFile *f;
    gboolean isdir;
    gtk_tree_model_get_iter(GTK_TREE_MODEL(c->store), &it, path);
    gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, C_FILE, &f, C_ISDIR, &isdir, -1);
    if (!f) {                                   /* Computer: a stick to mount, or connect */
        if (c->computer)
            computer_activate(c);
        return;
    }
    if (isdir)
        win_focus_next(c->win, c);
    else
        win_open_default(c->win, f);
    g_object_unref(f);
}

static gboolean on_focus(GtkWidget *w, GdkEvent *ev, Column *c)
{
    (void) w; (void) ev;
    c->win->focused = c;
    return FALSE;
}

static gboolean on_button(GtkWidget *view, GdkEventButton *ev, Column *c)
{
    GtkTreePath *p = NULL;
    if (ev->type == GDK_BUTTON_PRESS)
        left_commit(c);         /* a release we never saw: don't stay waiting */
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {      /* middle-click: a folder in a new window, a file opens */
        /* a double middle-click is two presses: the second one doesn't open it again */
        static guint32 last_middle;
        int dbl = 400;
        g_object_get(gtk_widget_get_settings(view), "gtk-double-click-time", &dbl, NULL);
        gboolean again = last_middle && ev->time - last_middle < (guint32) dbl;
        last_middle = ev->time;
        if (again)
            return TRUE;
        gboolean done = FALSE;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), ev->x, ev->y, &p, NULL, NULL, NULL)) {
            GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
            GtkTreeIter it;
            GFile *f = NULL;
            gboolean isdir = FALSE;
            if (gtk_tree_model_get_iter(m, &it, p))
                gtk_tree_model_get(m, &it, C_FILE, &f, C_ISDIR, &isdir, -1);
            if (f && isdir) {
                win_new(c->win->app, f);
                done = TRUE;
            } else if (f && !c->computer && !c->trash) {   /* a file: opened, as a double-click would (SCerovec) */
                g_object_ref(f);
                win_open_default(c->win, f);
                g_object_unref(f);
                done = TRUE;
            }
            g_clear_object(&f);
            gtk_tree_path_free(p);
        }
        return done;
    }
    if (c->computer && ev->type == GDK_BUTTON_PRESS && ev->button == 1 &&
        gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), ev->x, ev->y, &p, NULL, NULL, NULL)) {
        /* a stick that isn't mounted, or "Connect to server…": one click does it */
        GtkTreeIter it;
        GFile *f = NULL;
        gtk_tree_model_get_iter(GTK_TREE_MODEL(c->store), &it, p);
        gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, C_FILE, &f, -1);
        gtk_tree_path_free(p);
        p = NULL;
        if (!f) {
            column_select_iter(c, &it);
            gtk_widget_grab_focus(view);
            computer_activate(c);
            return TRUE;
        }
        g_object_unref(f);
    }
    /* Unleashed: the left button on an unselected row. GtkTreeView selects it now, but nothing
     * follows yet: let go without moving and it's a click (selected and opened, as always); move
     * and it's a drag, which carries the row while the old selection and the columns come back. */
    if (LOOK.clicks == CLICKS_UNLEASHED && !c->computer && ev->type == GDK_BUTTON_PRESS && ev->button == 1 &&
        !(ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) &&
        gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), ev->x, ev->y, &p, NULL, NULL, NULL)) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
        if (!gtk_tree_selection_path_is_selected(sel, p)) {
            left_prev_free(c);
            GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
            for (GList *l = rows; l; l = l->next)
                c->left_prev = g_list_append(c->left_prev,
                                             gtk_tree_row_reference_new(GTK_TREE_MODEL(c->store), l->data));
            g_list_free_full(rows, (GDestroyNotify) gtk_tree_path_free);
            c->left_row = gtk_tree_row_reference_new(GTK_TREE_MODEL(c->store), p);
            c->left_wait = TRUE;
        }
        gtk_tree_path_free(p);
        return FALSE;
    }
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    /* the right button: the menu comes when it's let go, unless it became a drag first (then
     * the drop asks Copy / Move / Link). Unleashed: an unselected row isn't selected, only held
     * (underlined); its menu and its drag act on it, and the columns stay where they are. */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    held_clear(c);
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), ev->x, ev->y, &p, NULL, NULL, NULL)) {
        if (!gtk_tree_selection_path_is_selected(sel, p)) {
            if (LOOK.clicks == CLICKS_UNLEASHED && !c->computer && !c->trash) {
                held_set(c, p);
            } else {
                gtk_tree_selection_unselect_all(sel);
                gtk_tree_selection_select_path(sel, p);
            }
        }
        gtk_tree_path_free(p);
    } else {
        gtk_tree_selection_unselect_all(sel);
    }
    gtk_widget_grab_focus(view);
    c->right_down = TRUE;
    c->right_x = ev->x, c->right_y = ev->y;
    return TRUE;
}

/* the right button held and moved: a drag we start ourselves (GtkTreeView starts its own only with
 * the left one); its drop asks what to do */
static gboolean on_motion(GtkWidget *view, GdkEventMotion *ev, Column *c)
{
    if (!c->right_down || !(ev->state & GDK_BUTTON3_MASK) ||
        !gtk_drag_check_threshold(view, (int) c->right_x, (int) c->right_y, (int) ev->x, (int) ev->y))
        return FALSE;
    c->right_down = FALSE;
    GtkTargetList *tl = gtk_target_list_new(URI_TARGET, 1);
    gtk_drag_begin_with_coordinates(view, tl, GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK, 3,
                                    (GdkEvent *) ev, (int) c->right_x, (int) c->right_y);
    gtk_target_list_unref(tl);
    DRAG_ASK = TRUE;
    return TRUE;
}

/* after the tree view has had the release too (it holds the mouse since the press, and only lets
 * go when it sees it): then the menu can take the mouse */
typedef struct {
    Column *c;
    GdkEvent *ev;
} MenuLater;

/* the held row's menu closed: its mark goes (later: "deactivate" comes before the item's "activate") */
static gboolean held_menu_gone(gpointer data)
{
    Column *c = data;
    held_clear(c);
    column_unref(c);
    return G_SOURCE_REMOVE;
}

static void on_held_menu_done(GtkMenuShell *menu, Column *c)
{
    (void) menu;
    column_ref(c);
    g_idle_add(held_menu_gone, c);
}

/* the menu's own hold on its column: it may stay open while the column is dropped (the folder it
 * shows deleted by another program meanwhile) */
static void held_menu_unref(gpointer c, GClosure *closure)
{
    (void) closure;
    column_unref(c);
}

static gboolean menu_later(gpointer data)
{
    MenuLater *m = data;
    GFile *f = m->c->closed ? NULL : held_file(m->c);
    if (f) {
        GtkWidget *menu = win_held_menu(m->c->win, m->c, f, (GdkEventButton *) m->ev);
        column_ref(m->c);
        g_signal_connect_data(menu, "deactivate", G_CALLBACK(on_held_menu_done), m->c, held_menu_unref, 0);
        g_object_unref(f);
    } else if (!m->c->closed) {
        held_clear(m->c);
        win_context_menu(m->c->win, m->c, (GdkEventButton *) m->ev);
    }
    gdk_event_free(m->ev);
    column_unref(m->c);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static gboolean on_button_release(GtkWidget *view, GdkEventButton *ev, Column *c)
{
    (void) view;
    if (ev->button == 1 && c->left_wait) {      /* after the tree view's own release */
        column_ref(c);
        g_idle_add(left_commit_idle, c);
        return FALSE;
    }
    if (ev->button != 3 || !c->right_down)
        return FALSE;
    c->right_down = FALSE;
    MenuLater *m = g_new0(MenuLater, 1);
    column_ref(c);
    m->c = c;
    m->ev = gdk_event_copy((GdkEvent *) ev);
    g_idle_add(menu_later, m);
    return TRUE;
}

/* Typing jumps to the first name starting with the typed letters (case-insensitive).
 * Pause for a second to start over; the same letter again goes to the next match. */
static void type_jump(Column *c, gunichar ch)
{
    gint64 now = g_get_monotonic_time();
    if (now - c->typed_at > G_USEC_PER_SEC)
        g_string_truncate(c->typed, 0);
    c->typed_at = now;
    char buf[8];
    buf[g_unichar_to_utf8(ch, buf)] = 0;
    char *fold = g_utf8_casefold(buf, -1);
    g_string_append(c->typed, fold);
    g_free(fold);

    GtkTreeModel *m = GTK_TREE_MODEL(c->store);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_first(m, &it))
        return;

    /* "aaa": cycle through the names starting with "a" */
    gboolean same = g_utf8_strlen(c->typed->str, -1) > 1;
    gunichar first = g_utf8_get_char(c->typed->str);
    for (const char *p = c->typed->str; *p && same; p = g_utf8_next_char(p))
        if (g_utf8_get_char(p) != first)
            same = FALSE;
    char first_s[8];
    first_s[g_unichar_to_utf8(first, first_s)] = 0;
    const char *want = same ? first_s : c->typed->str;

    GFile *cur = NULL;
    column_selected_single(c, &cur, NULL);
    GtkTreeIter hit, first_hit;
    gboolean have_first = FALSE, after_cur = FALSE, found = FALSE;
    do {
        char *n, *nf;
        GFile *f;
        gtk_tree_model_get(m, &it, C_NAME, &n, C_FILE, &f, -1);
        nf = g_utf8_casefold(n, -1);
        gboolean match = g_str_has_prefix(nf, want);
        if (match) {
            if (!have_first) {
                first_hit = it;
                have_first = TRUE;
            }
            if (!same || after_cur) {
                hit = it;
                found = TRUE;
            }
        }
        if (same && cur && f && g_file_equal(f, cur))
            after_cur = TRUE;
        g_free(n);
        g_free(nf);
        g_clear_object(&f);
    } while (!found && gtk_tree_model_iter_next(m, &it));
    if (cur)
        g_object_unref(cur);
    if (!found && have_first) {   /* wrap around */
        hit = first_hit;
        found = TRUE;
    }
    if (found) {
        column_select_iter(c, &hit);
        win_status(c->win, _("Jump: %s"), want);
    } else {
        win_status(c->win, _("Jump: %s (no match)"), c->typed->str);
    }
}

static gboolean on_key(GtkWidget *view, GdkEventKey *ev, Column *c)
{
    (void) view;
    guint k = ev->keyval;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK, shift = ev->state & GDK_SHIFT_MASK;
    gboolean alt = ev->state & GDK_MOD1_MASK;
    Win *w = c->win;
    left_commit(c);
    if (k == GDK_KEY_Escape && !ctrl && !shift && !alt && !c->computer) {
        /* Esc: nothing selected here any more; the columns to the right close, the status line
         * shows the folder's count again (random_guy) */
        g_string_truncate(c->typed, 0);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
        if (gtk_tree_selection_count_selected_rows(sel) == 0)
            return FALSE;
        gtk_tree_selection_unselect_all(sel);
        return TRUE;
    }
    if (k == GDK_KEY_Right && !ctrl) {
        win_focus_next(w, c);
        return TRUE;
    }
    if (k == GDK_KEY_Left && !ctrl) {
        win_focus_prev(w, c);
        return TRUE;
    }
    if (k == GDK_KEY_Menu || (shift && k == GDK_KEY_F10)) {
        win_context_menu(w, c, NULL);
        return TRUE;
    }
    if (c->computer) {             /* the disks themselves: no copying, deleting, renaming … */
        if (k == GDK_KEY_F5) {
            computer_refresh(w);
            return TRUE;
        }
        if (!ctrl && !alt) {
            gunichar ch = gdk_keyval_to_unicode(k);
            if (ch && g_unichar_isprint(ch) && ch != ' ') {
                type_jump(c, ch);
                return TRUE;
            }
        }
        return ctrl || k == GDK_KEY_Delete || k == GDK_KEY_F2;
    }
    if (c->trash) {                /* only putting back or deleting for good here */
        if (k == GDK_KEY_Delete) {
            GList *files = column_selected_files(c);
            gboolean top = files != NULL;
            for (GList *l = files; l; l = l->next)
                top = top && trash_top(l->data);
            if (top)
                win_delete_forever(w, files);
            else if (files)
                win_status(w, _("Inside a trashed folder: delete the whole folder, or put it back first"));
            g_list_free_full(files, g_object_unref);
            return TRUE;
        }
        if (k == GDK_KEY_F5) {
            win_reload_all(w);
            return TRUE;
        }
        if (!ctrl && !alt) {
            gunichar ch = gdk_keyval_to_unicode(k);
            if (ch && g_unichar_isprint(ch) && ch != ' ') {
                type_jump(c, ch);
                return TRUE;
            }
        }
        return (ctrl && k != GDK_KEY_a && k != GDK_KEY_A) || k == GDK_KEY_F2;
    }
    if (ctrl && !shift && (k == GDK_KEY_c || k == GDK_KEY_C)) {
        win_clip_set(w, column_selected_files(c), FALSE);
        return TRUE;
    }
    if (ctrl && (k == GDK_KEY_x || k == GDK_KEY_X)) {
        win_clip_set(w, column_selected_files(c), TRUE);
        return TRUE;
    }
    if (ctrl && (k == GDK_KEY_v || k == GDK_KEY_V)) {
        /* a highlighted folder gets the paste; otherwise this column's folder */
        GFile *f = NULL;
        gboolean isdir = FALSE;
        if (column_selected_single(c, &f, &isdir) && isdir)
            win_paste_into(w, f);
        else
            win_paste_into(w, c->gfile);
        g_clear_object(&f);
        return TRUE;
    }
    if (ctrl && shift && (k == GDK_KEY_n || k == GDK_KEY_N)) {
        win_new_folder(w, c);
        return TRUE;
    }
    if (k == GDK_KEY_F2) {
        win_rename(w, c);
        return TRUE;
    }
    if (k == GDK_KEY_Delete) {
        GList *files = column_selected_files(c);
        if (shift)
            win_delete_forever(w, files);
        else
            win_trash(w, files);
        g_list_free_full(files, g_object_unref);
        return TRUE;
    }
    if (k == GDK_KEY_F5) {
        win_reload_all(w);
        return TRUE;
    }
    if (!ctrl && !alt) {
        gunichar ch = gdk_keyval_to_unicode(k);
        if (ch && g_unichar_isprint(ch) && (ch != ' ' || c->typed->len)) {
            type_jump(c, ch);
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- drag & drop ----------------------------------------------------------------- */
/* A drag with the right button (or a drop with Alt held, also from other programs) asks what to
 * do: Copy here, Move here, Link here, Cancel. The left button does what it always did. */

static void spring_cancel(void)
{
    if (SPRING_ID)
        g_source_remove(SPRING_ID);
    SPRING_ID = 0;
    if (SPRING_ROW)
        gtk_tree_row_reference_free(SPRING_ROW);
    SPRING_ROW = NULL;
    if (SPRING_COL)
        column_unref(SPRING_COL);
    SPRING_COL = NULL;
}

static void drag_reset(void)
{
    spring_cancel();
    g_list_free_full(DRAG_FILES, g_object_unref);
    DRAG_FILES = NULL;
    if (DRAG_SRC) {
        held_clear(DRAG_SRC);
        column_unref(DRAG_SRC);
    }
    DRAG_SRC = NULL;
}

/* Unleashed: what the drag carries is fixed now. A left drag from an unselected row: that row,
 * held, and the selection from before the press back (nothing had followed it). */
static void on_drag_begin(GtkWidget *view, GdkDragContext *ctx, Column *c)
{
    (void) ctx;
    if (win_pointer_mask(c->win) & GDK_BUTTON3_MASK)
        DRAG_ASK = TRUE;            /* (on_motion set it already for the right button's drag) */
    c->right_down = FALSE;          /* a drag, not a click: no context menu at the end */
    drag_reset();
    column_ref(c);
    DRAG_SRC = c;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    if (c->left_wait && c->left_row && gtk_tree_row_reference_valid(c->left_row)) {
        GtkTreePath *row = gtk_tree_row_reference_get_path(c->left_row);
        held_set(c, row);
        gtk_tree_path_free(row);
        GFile *f = held_file(c);
        DRAG_FILES = f ? g_list_append(NULL, f) : NULL;
        gtk_tree_selection_unselect_all(sel);
        for (GList *l = c->left_prev; l; l = l->next)
            if (gtk_tree_row_reference_valid(l->data)) {
                GtkTreePath *p = gtk_tree_row_reference_get_path(l->data);
                gtk_tree_selection_select_path(sel, p);
                gtk_tree_path_free(p);
            }
        left_prev_free(c);
        c->left_wait = FALSE;       /* only now: the restoring above changed nothing to the right */
    } else {
        GFile *f = held_file(c);
        DRAG_FILES = f ? g_list_append(NULL, f) : column_selected_files(c);
        left_prev_free(c);
        c->left_wait = FALSE;
    }
}

static void on_drag_end(GtkWidget *view, GdkDragContext *ctx, gpointer d)
{
    (void) view; (void) ctx; (void) d;
    DRAG_ASK = FALSE;
    drag_reset();
}

/* Unleashed: a folder held still under a drag opens. Not to the left of the column the drag came
 * from in its own window: that would close that column under the drag. */
static gboolean spring_allowed(Column *c)
{
    if (c->computer || c->closed || c->win->dead)
        return FALSE;
    if (!DRAG_SRC || DRAG_SRC->win != c->win)
        return TRUE;
    if (DRAG_SRC->closed)
        return FALSE;
    return g_list_index(c->win->columns, c) >= g_list_index(c->win->columns, DRAG_SRC);
}

static gboolean spring_fire(gpointer data)
{
    (void) data;
    SPRING_ID = 0;
    Column *c = SPRING_COL;
    if (c && !c->closed && !c->win->dead && SPRING_ROW && gtk_tree_row_reference_valid(SPRING_ROW)) {
        GtkTreePath *p = gtk_tree_row_reference_get_path(SPRING_ROW);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(c->store), &it, p))
            column_select_iter(c, &it);         /* its column comes to the right, as a click does */
        gtk_tree_path_free(p);
    }
    spring_cancel();
    return G_SOURCE_REMOVE;
}

static gboolean on_drag_motion(GtkWidget *view, GdkDragContext *ctx, int x, int y, guint t, Column *c)
{
    (void) ctx; (void) t;
    GtkTreePath *p = NULL;
    GtkTreeViewDropPosition pos;
    if (LOOK.clicks != CLICKS_UNLEASHED || !spring_allowed(c) || !gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(view), x, y, &p, &pos) ||
        (pos != GTK_TREE_VIEW_DROP_INTO_OR_BEFORE && pos != GTK_TREE_VIEW_DROP_INTO_OR_AFTER)) {
        if (p)
            gtk_tree_path_free(p);
        spring_cancel();
        return FALSE;
    }
    if (SPRING_COL == c && SPRING_ROW && gtk_tree_row_reference_valid(SPRING_ROW)) {
        GtkTreePath *q = gtk_tree_row_reference_get_path(SPRING_ROW);
        gboolean same = !gtk_tree_path_compare(p, q);
        gtk_tree_path_free(q);
        if (same) {                 /* still the same folder: the timer runs on */
            gtk_tree_path_free(p);
            return FALSE;
        }
    }
    spring_cancel();
    GtkTreeIter it;
    GFile *f = NULL;
    gboolean isdir = FALSE;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(c->store), &it, p))
        gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, C_FILE, &f, C_ISDIR, &isdir, -1);
    gboolean ok = f && isdir;
    for (GList *l = DRAG_FILES; l && ok; l = l->next)      /* the dragged folder itself: never */
        if (g_file_equal(l->data, f))
            ok = FALSE;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    if (ok && gtk_tree_selection_count_selected_rows(sel) == 1 && gtk_tree_selection_path_is_selected(sel, p))
        ok = FALSE;                 /* already open */
    if (ok) {
        column_ref(c);
        SPRING_COL = c;
        SPRING_ROW = gtk_tree_row_reference_new(GTK_TREE_MODEL(c->store), p);
        SPRING_ID = g_timeout_add(SPRING_MS, spring_fire, NULL);
    }
    g_clear_object(&f);
    gtk_tree_path_free(p);
    return FALSE;
}

static void on_drag_leave(GtkWidget *view, GdkDragContext *ctx, guint t, Column *c)
{
    (void) view; (void) ctx; (void) t;
    if (SPRING_COL == c)
        spring_cancel();
}

typedef struct {
    Win *win;
    GList *srcs;
    GFile *target;
} Drop;

static void drop_free(Drop *d)
{
    g_list_free_full(d->srcs, g_object_unref);
    g_object_unref(d->target);
    win_unref(d->win);
    g_free(d);
}

/* symbolic links in the target, one per item, pointing at where it is */
static void link_into(Win *w, GList *srcs, GFile *target)
{
    int failed = 0;
    char *why = NULL;
    for (GList *l = srcs; l; l = l->next) {
        char *src = g_file_get_path(l->data), *base = g_file_get_basename(l->data);
        GFile *dest = g_file_get_child(target, base);
        GError *err = NULL;
        if (!src || !g_file_make_symbolic_link(dest, src, NULL, &err)) {
            failed++;
            if (err && !why)
                why = g_strdup(err->message);
        }
        g_clear_error(&err);
        g_object_unref(dest);
        g_free(src);
        g_free(base);
    }
    if (failed)
        win_error(w, _("Couldn't make the link"), why ? why : "");
    g_free(why);
}

static void on_drop_choice(GtkMenuItem *mi, Drop *d)
{
    int what = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "what"));
    if (what == 2)
        link_into(d->win, d->srcs, d->target);
    else
        job_run(d->win, what == 1, d->srcs, d->target);
}

static gpointer ref_copy(gconstpointer src, gpointer data)
{
    (void) data;
    return g_object_ref((gpointer) src);
}

static void ask_drop(Column *c, GList *srcs, GFile *target)
{
    Drop *d = g_new0(Drop, 1);
    d->win = win_ref(c->win);
    d->srcs = g_list_copy_deep(srcs, ref_copy, NULL);
    d->target = g_object_ref(target);
    GtkWidget *menu = gtk_menu_new();
    const char *names[] = { _("Copy here"), _("Move here"), _("Link here") };
    for (int i = 0; i < 3; i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(names[i]);
        g_object_set_data(G_OBJECT(mi), "what", GINT_TO_POINTER(i));
        g_signal_connect(mi, "activate", G_CALLBACK(on_drop_choice), d);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_menu_item_new_with_label(_("Cancel")));
    g_object_set_data_full(G_OBJECT(menu), "drop", d, (GDestroyNotify) drop_free);
    gtk_menu_attach_to_widget(GTK_MENU(menu), c->view, NULL);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}
static void on_drag_get(GtkWidget *view, GdkDragContext *ctx, GtkSelectionData *data,
                        guint info, guint t, Column *c)
{
    (void) ctx; (void) info; (void) t;
    GList *files = DRAG_SRC == c && DRAG_FILES ? g_list_copy_deep(DRAG_FILES, ref_copy, NULL)
                                               : column_selected_files(c);
    int n = g_list_length(files), i = 0;
    char **uris = g_new0(char *, n + 1);
    for (GList *l = files; l; l = l->next)
        uris[i++] = g_file_get_uri(l->data);
    gtk_selection_data_set_uris(data, uris);
    g_strfreev(uris);
    g_list_free_full(files, g_object_unref);
    g_signal_stop_emission_by_name(view, "drag-data-get");
}

static void on_drag_received(GtkWidget *view, GdkDragContext *ctx, int x, int y,
                             GtkSelectionData *data, guint info, guint t, Column *c)
{
    (void) info;
    g_signal_stop_emission_by_name(view, "drag-data-received");
    char **uris = gtk_selection_data_get_uris(data);
    GFile *target = g_object_ref(c->gfile);
    GtkTreePath *path;
    GtkTreeViewDropPosition pos;
    if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(view), x, y, &path, &pos)) {
        GtkTreeIter it;
        gboolean isdir;
        GFile *f;
        gtk_tree_model_get_iter(GTK_TREE_MODEL(c->store), &it, path);
        gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, C_ISDIR, &isdir, C_FILE, &f, -1);
        if (isdir && (pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE || pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)) {
            g_object_unref(target);
            target = f;
        } else {
            g_object_unref(f);
        }
        gtk_tree_path_free(path);
    }
    GList *srcs = NULL;
    for (int i = 0; uris && uris[i]; i++) {
        GFile *s = g_file_new_for_uri(uris[i]);
        if (g_file_equal(s, target))
            g_object_unref(s);
        else
            srcs = g_list_append(srcs, s);
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, srcs != NULL, FALSE, t);
    if (srcs && file_in_trash(target)) {    /* dropped on the trash: into the trash */
        GList *outside = NULL;
        for (GList *l = srcs; l; l = l->next)
            if (!file_in_trash(l->data))
                outside = g_list_append(outside, g_object_ref(l->data));
        win_trash(c->win, outside);
        g_list_free_full(outside, g_object_unref);
    } else if (srcs && (DRAG_ASK || (win_pointer_mask(c->win) & GDK_MOD1_MASK))) {
        DRAG_ASK = FALSE;
        ask_drop(c, srcs, target);
        g_list_free_full(srcs, g_object_unref);
    } else if (srcs && (win_pointer_mask(c->win) & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
                       == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        link_into(c->win, srcs, target);    /* Ctrl+Shift: links, like other file managers */
        g_list_free_full(srcs, g_object_unref);
    } else if (srcs) {      /* Ctrl: copy, Shift: move, neither: move on the same disk, else copy */
        GdkModifierType mask = win_pointer_mask(c->win);
        gboolean move;
        if (mask & GDK_CONTROL_MASK)
            move = FALSE;
        else if (mask & GDK_SHIFT_MASK)
            move = TRUE;
        else {   /* same file system: move, else copy */
            dev_t a, b;
            move = device_of(srcs->data, &a) && device_of(target, &b) && a == b;
        }
        job_run(c->win, move, srcs, target);
        g_list_free_full(srcs, g_object_unref);
    }
    g_object_unref(target);
}

/* we move files ourselves (Job); don't let GTK delete rows from the model */
static void on_drag_delete(GtkWidget *view, GdkDragContext *ctx, gpointer d)
{
    (void) ctx; (void) d;
    g_signal_stop_emission_by_name(view, "drag-data-delete");
}

/* ---- construction ------------------------------------------------------------------ */
static int sort_func(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer d)
{
    Column *c = d;
    char *ka, *kb;
    if (!c->custom) {
        gtk_tree_model_get(m, a, C_SORT, &ka, -1);
        gtk_tree_model_get(m, b, C_SORT, &kb, -1);
        int r = g_strcmp0(ka, kb);    /* collate keys compare with strcmp */
        g_free(ka);
        g_free(kb);
        return r;
    }
    gboolean da, db;
    gint64 ma, mb;
    guint64 sa, sb;
    char *ea, *eb;
    gtk_tree_model_get(m, a, C_SORT, &ka, C_ISDIR, &da, C_MTIME, &ma, C_SIZE, &sa, C_EXT, &ea, -1);
    gtk_tree_model_get(m, b, C_SORT, &kb, C_ISDIR, &db, C_MTIME, &mb, C_SIZE, &sb, C_EXT, &eb, -1);
    int r = sorting_compare(c->sort, da, db, ka, kb, ma, mb, sa, sb, ea, eb);
    g_free(ka);
    g_free(kb);
    g_free(ea);
    g_free(eb);
    return r;
}

/* The trash's own order, one of its four buttons (name at first). The same in every trash window
 * of this run. */
static FolderSort TRASH_SORT = { SORT_NAME, FALSE };
static const struct { const char *label; SortBy by; gboolean reverse; } TRASH_SORTS[] = {
    { N_("name"), SORT_NAME, FALSE },
    { N_("size"), SORT_SIZE, FALSE },          /* largest first */
    { N_("newest"), SORT_DATE, FALSE },        /* deleted last first */
    { N_("oldest"), SORT_DATE, TRUE },
};

/* like radio buttons: the pressed one is the order; pressing it again keeps it */
static void trash_sort_toggled(GtkToggleButton *b, Column *c)
{
    static gboolean busy;                  /* setting the others toggles them too */
    if (busy)
        return;
    busy = TRUE;
    gtk_toggle_button_set_active(b, TRUE);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(gtk_widget_get_parent(GTK_WIDGET(b))));
    for (GList *l = kids; l; l = l->next)
        if (GTK_IS_TOGGLE_BUTTON(l->data) && l->data != (gpointer) b)
            gtk_toggle_button_set_active(l->data, FALSE);
    g_list_free(kids);
    busy = FALSE;
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "i"));
    TRASH_SORT.by = TRASH_SORTS[i].by;
    TRASH_SORT.reverse = TRASH_SORTS[i].reverse;
    column_apply_sort(c);
}

/* a bar on top of the trash: "Sort:" and its buttons */
static GtkWidget *trash_sort_bar(Column *c)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_object_set(bar, "margin", 4, NULL);
    GtkWidget *l = gtk_label_new(_("Sort:"));
    gtk_widget_set_opacity(l, 0.7);
    gtk_box_pack_start(GTK_BOX(bar), l, FALSE, FALSE, 2);
    for (guint i = 0; i < G_N_ELEMENTS(TRASH_SORTS); i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label(_(TRASH_SORTS[i].label));
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        g_object_set_data(G_OBJECT(b), "i", GUINT_TO_POINTER(i));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRASH_SORTS[i].by == TRASH_SORT.by
                                                           && TRASH_SORTS[i].reverse == TRASH_SORT.reverse);
        g_signal_connect(b, "toggled", G_CALLBACK(trash_sort_toggled), c);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    return bar;
}

/* this folder's order from sorting.conf, the bar on top showing it, and the rows in that order */
void column_apply_sort(Column *c)
{
    if (c->trash_root) {               /* its own button instead of sorting.conf */
        c->sort = TRASH_SORT;
        c->custom = TRASH_SORT.by != SORT_NAME;
        gtk_widget_hide(c->sortbar);
        gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(c->store), C_SORT, sort_func, c, NULL);
        return;
    }
    char *path = g_file_get_path(c->gfile);
    c->custom = sorting_get(path, &c->sort);
    g_free(path);
    if (c->custom) {
        char *d = sorting_describe(c->sort);
        char *m = g_markup_printf_escaped("<small>sorted by %s</small>", d);
        gtk_label_set_markup(GTK_LABEL(c->sortlabel), m);
        g_free(m);
        g_free(d);
        gtk_widget_show(c->sortbar);
    } else {
        gtk_widget_hide(c->sortbar);
    }
    /* setting the function again for the current sort column sorts the rows again */
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(c->store), C_SORT, sort_func, c, NULL);
}

static void sortbar_reset(GtkButton *b, Column *c)
{
    (void) b;
    char *path = g_file_get_path(c->gfile);
    if (path) {
        sorting_clear(path);
        app_sorting_changed(c->win->app, path);
    }
    g_free(path);
}

static void arrow_func(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *m,
                       GtkTreeIter *it, gpointer d)
{
    (void) col; (void) d;
    gboolean isdir;
    GFile *f = NULL;
    gtk_tree_model_get(m, it, C_ISDIR, &isdir, C_FILE, &f, -1);
    char *path = isdir && f ? g_file_get_path(f) : NULL;
    if (path && sorting_get(path, NULL))
        g_object_set(cell, "markup", "<span alpha='45%'>⇅</span> ›", NULL);
    else
        g_object_set(cell, "text", isdir ? "›" : "", NULL);
    g_free(path);
    g_clear_object(&f);
}

void column_set_width(Column *c, int w)
{
    c->width = CLAMP(w, COL_MIN, COL_MAX);
    gtk_widget_set_size_request(c->scroller, c->width, -1);
    char *uri = g_file_get_uri(c->gfile);
    if (c->width == LOOK.column_width) {        /* back to the setting: follows it again */
        g_hash_table_remove(c->win->col_widths, uri);
        g_free(uri);
    } else {
        g_hash_table_insert(c->win->col_widths, uri, GINT_TO_POINTER(c->width));
    }
}

Column *column_new(Win *w, GFile *gfile, GPtrArray *chain)
{
    Column *c = g_new0(Column, 1);
    c->ref = 1;
    c->win = win_ref(w);
    c->gfile = g_object_ref(gfile);
    c->chain = chain ? g_ptr_array_ref(chain) : NULL;
    c->cancel = g_cancellable_new();
    c->typed = g_string_new(NULL);
    c->drag_start = -1;
    c->store = gtk_list_store_new(C_N, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_FILE,
                                  G_TYPE_STRING, G_TYPE_INT64, G_TYPE_UINT64, G_TYPE_STRING, G_TYPE_BOOLEAN,
                                  G_TYPE_INT);
    char *own = g_file_get_path(gfile);
    c->custom = sorting_get(own, &c->sort);
    g_free(own);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(c->store), C_SORT, sort_func, c, NULL);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(c->store), C_SORT, GTK_SORT_ASCENDING);

    /* own type-to-jump instead of GTK's search popup */
    c->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(c->view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(c->view), FALSE);
    c->computer = file_is_computer(gfile);
    c->trash = file_in_trash(gfile);
    char *turi = c->trash ? g_file_get_uri(gfile) : NULL;
    c->trash_root = turi && (!strcmp(turi, "trash:///") || !strcmp(turi, "trash:"));
    g_free(turi);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)),
                                c->computer ? GTK_SELECTION_SINGLE : GTK_SELECTION_MULTIPLE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *r_icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(r_icon, "stock-size", GTK_ICON_SIZE_MENU, NULL);
    gtk_tree_view_column_pack_start(col, r_icon, FALSE);
    gtk_tree_view_column_add_attribute(col, r_icon, "gicon", C_ICON);
    GtkCellRenderer *r_name = gtk_cell_renderer_text_new();
    g_object_set(r_name, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(col, r_name, TRUE);
    gtk_tree_view_column_add_attribute(col, r_name, "text", C_NAME);
    gtk_tree_view_column_set_cell_data_func(col, r_name, name_func, c, NULL);
    GtkCellRenderer *r_arrow = gtk_cell_renderer_text_new();
    g_object_set(r_arrow, "xalign", 1.0, NULL);
    gtk_tree_view_column_pack_end(col, r_arrow, FALSE);
    gtk_tree_view_column_set_cell_data_func(col, r_arrow, arrow_func, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(c->view), col);

    c->scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    char *uri = g_file_get_uri(gfile);
    gpointer wv = g_hash_table_lookup(w->col_widths, uri);
    g_free(uri);
    c->width = wv ? GPOINTER_TO_INT(wv) : LOOK.column_width;
    gtk_widget_set_size_request(c->scroller, c->width, -1);
    gtk_container_add(GTK_CONTAINER(c->scroller), c->view);
    /* a thin bar on top when this folder is sorted its own way; its x goes back to by name */
    c->sortlabel = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(c->sortlabel), 0);
    gtk_label_set_ellipsize(GTK_LABEL(c->sortlabel), PANGO_ELLIPSIZE_END);
    gtk_widget_set_opacity(c->sortlabel, 0.7);
    GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(x, _("Sort by name again"));
    g_signal_connect(x, "clicked", G_CALLBACK(sortbar_reset), c);
    c->sortbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_object_set(c->sortbar, "margin-start", 6, NULL);
    gtk_box_pack_start(GTK_BOX(c->sortbar), c->sortlabel, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(c->sortbar), x, FALSE, FALSE, 0);
    gtk_widget_show_all(c->sortbar);              /* its insides now; the bar itself only when needed */
    gtk_widget_hide(c->sortbar);
    gtk_widget_set_no_show_all(c->sortbar, TRUE);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), c->sortbar, FALSE, FALSE, 0);
    if (c->trash_root)
        gtk_box_pack_start(GTK_BOX(vbox), trash_sort_bar(c), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), c->scroller, TRUE, TRUE, 0);
    c->frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(c->frame), GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(c->frame), vbox);
    column_apply_sort(c);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), "changed",
                     G_CALLBACK(on_selection), c);
    g_signal_connect(c->view, "row-activated", G_CALLBACK(on_activated), c);
    g_signal_connect(c->view, "button-press-event", G_CALLBACK(on_button), c);
    g_signal_connect(c->view, "button-release-event", G_CALLBACK(on_button_release), c);
    g_signal_connect(c->view, "motion-notify-event", G_CALLBACK(on_motion), c);
    g_signal_connect(c->view, "key-press-event", G_CALLBACK(on_key), c);
    g_signal_connect(c->view, "focus-in-event", G_CALLBACK(on_focus), c);

    if (c->computer) {             /* filled from the mount table and GIO, not a folder */
        c->objs = g_ptr_array_new_with_free_func(g_object_unref);
        computer_fill(c);
        c->loaded = TRUE;
        return c;
    }
    GdkDragAction actions = GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK;
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(c->view), GDK_BUTTON1_MASK, URI_TARGET, 1, actions);
    gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(c->view), URI_TARGET, 1, actions);
    g_signal_connect(c->view, "drag-begin", G_CALLBACK(on_drag_begin), c);
    g_signal_connect(c->view, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(c->view, "drag-data-get", G_CALLBACK(on_drag_get), c);
    g_signal_connect(c->view, "drag-data-received", G_CALLBACK(on_drag_received), c);
    g_signal_connect(c->view, "drag-data-delete", G_CALLBACK(on_drag_delete), NULL);
    g_signal_connect(c->view, "drag-motion", G_CALLBACK(on_drag_motion), c);
    g_signal_connect(c->view, "drag-leave", G_CALLBACK(on_drag_leave), c);

    c->monitor = g_file_monitor_directory(gfile, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (c->monitor)
        g_signal_connect(c->monitor, "changed", G_CALLBACK(on_changed), c);
    column_load(c);
    return c;
}
