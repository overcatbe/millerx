/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* One folder shown as a list. */
#include "millerx.h"
#include <string.h>

static const GtkTargetEntry URI_TARGET[] = { { (char *) "text/uri-list", 0, 0 } };

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
    win_unref(c->win);
    g_free(c);
}

void column_close(Column *c)
{
    if (c->closed)
        return;
    c->closed = TRUE;
    g_cancellable_cancel(c->cancel);
    if (c->monitor) {
        g_signal_handlers_disconnect_by_data(c->monitor, c);
        g_file_monitor_cancel(c->monitor);
        g_clear_object(&c->monitor);
    }
    if (c->view)
        g_signal_handlers_disconnect_by_data(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), c);
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
    gtk_list_store_insert_with_values(c->store, out, -1, C_ICON, g_file_info_get_icon(info),
                                      C_NAME, name, C_ISDIR, isdir, C_FILE, child, C_SORT, key,
                                      C_MTIME, mtime, C_SIZE, (guint64) g_file_info_get_size(info),
                                      C_EXT, ext, -1);
    g_free(ext);
    g_free(key);
    return out;
}

static void loading_done(Column *c)
{
    c->loaded = TRUE;
    column_continue_chain(c);
    if (c == win_last_column(c->win)) {
        char *p = file_path(c->gfile);
        win_status(c->win, "%s: %d items", p,
                   gtk_tree_model_iter_n_children(GTK_TREE_MODEL(c->store), NULL));
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
        append_info(c, l->data, child, &it);
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
    if (append_info(c, info, f, &it) && c->pending_select
        && !strcmp(c->pending_select, g_file_info_get_display_name(info))) {
        g_clear_pointer(&c->pending_select, g_free);
        column_select_iter(c, &it);
    }
    g_object_unref(info);
}

void column_remove_file(Column *c, GFile *f)
{
    GtkTreeIter it;
    if (find_iter(c, f, &it))
        gtk_list_store_remove(c->store, &it);
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
    win_column_selected(c->win, c);
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
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {      /* middle-click a folder: new window */
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
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), ev->x, ev->y, &p, NULL, NULL, NULL)) {
        if (!gtk_tree_selection_path_is_selected(sel, p)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, p);
        }
        gtk_tree_path_free(p);
    } else {
        gtk_tree_selection_unselect_all(sel);
    }
    gtk_widget_grab_focus(view);
    win_context_menu(c->win, c, ev);
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
        win_status(c->win, "Jump: %s", want);
    } else {
        win_status(c->win, "Jump: %s (no match)", c->typed->str);
    }
}

static gboolean on_key(GtkWidget *view, GdkEventKey *ev, Column *c)
{
    (void) view;
    guint k = ev->keyval;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK, shift = ev->state & GDK_SHIFT_MASK;
    gboolean alt = ev->state & GDK_MOD1_MASK;
    Win *w = c->win;
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
                win_status(w, "Inside a trashed folder: delete the whole folder, or put it back first");
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
static void on_drag_get(GtkWidget *view, GdkDragContext *ctx, GtkSelectionData *data,
                        guint info, guint t, Column *c)
{
    (void) ctx; (void) info; (void) t;
    GList *files = column_selected_files(c);
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
    } else if (srcs) {
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
    { "name", SORT_NAME, FALSE },
    { "size", SORT_SIZE, FALSE },          /* largest first */
    { "newest", SORT_DATE, FALSE },        /* deleted last first */
    { "oldest", SORT_DATE, TRUE },
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
    GtkWidget *l = gtk_label_new("Sort:");
    gtk_widget_set_opacity(l, 0.7);
    gtk_box_pack_start(GTK_BOX(bar), l, FALSE, FALSE, 2);
    for (guint i = 0; i < G_N_ELEMENTS(TRASH_SORTS); i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label(TRASH_SORTS[i].label);
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
                                  G_TYPE_STRING, G_TYPE_INT64, G_TYPE_UINT64, G_TYPE_STRING);
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
    gtk_widget_set_tooltip_text(x, "Sort by name again");
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
    g_signal_connect(c->view, "key-press-event", G_CALLBACK(on_key), c);
    g_signal_connect(c->view, "focus-in-event", G_CALLBACK(on_focus), c);

    if (c->computer) {             /* filled from the mount table and GIO, not a folder */
        c->objs = g_ptr_array_new_with_free_func(g_object_unref);
        computer_fill(c);
        c->loaded = TRUE;
        return c;
    }
    GdkDragAction actions = GDK_ACTION_COPY | GDK_ACTION_MOVE;
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(c->view), GDK_BUTTON1_MASK, URI_TARGET, 1, actions);
    gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(c->view), URI_TARGET, 1, actions);
    g_signal_connect(c->view, "drag-data-get", G_CALLBACK(on_drag_get), c);
    g_signal_connect(c->view, "drag-data-received", G_CALLBACK(on_drag_received), c);
    g_signal_connect(c->view, "drag-data-delete", G_CALLBACK(on_drag_delete), NULL);

    c->monitor = g_file_monitor_directory(gfile, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (c->monitor)
        g_signal_connect(c->monitor, "changed", G_CALLBACK(on_changed), c);
    column_load(c);
    return c;
}
