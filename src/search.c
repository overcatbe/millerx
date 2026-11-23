/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Search (Ctrl+F): names containing the typed text, in the current folder and all folders below
 * it, case-insensitive. A worker walks the folders (shallow ones first) while the results show up
 * in a list where the columns were; the preview shows the one you pick, Enter or a double-click
 * opens it in the columns, Escape goes back. */
#include "millerx.h"
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#define SEARCH_MAX 5000          /* results; a search that finds more stops there */
#define SEARCH_DELAY 300         /* ms after the last key before a search starts */

enum { S_ICON, S_TEXT, S_PATH, S_N };

/* one running search; the worker and the window share it */
typedef struct {
    int ref;
    GCancellable *cancel;
    GMutex lock;
    GPtrArray *found;            /* paths not shown yet (worker adds, window takes) */
    guint total, folders;
    gboolean done, full;
    char *root, *needle;         /* needle: casefolded */
    gboolean hidden;             /* look into hidden files and folders too */
} SJob;

struct Search {
    Win *w;
    GtkWidget *bar, *entry, *info, *stop, *scroll, *view;
    GtkListStore *store;
    GFile *root;
    SJob *job;
    guint timer, flush;
};

static SJob *job_ref(SJob *j)
{
    g_atomic_int_inc(&j->ref);
    return j;
}

static void job_unref(SJob *j)
{
    if (!g_atomic_int_dec_and_test(&j->ref))
        return;
    g_object_unref(j->cancel);
    g_mutex_clear(&j->lock);
    g_ptr_array_unref(j->found);
    g_free(j->root);
    g_free(j->needle);
    g_free(j);
}

/* pseudo file systems: never walked into from above (searching / stays on real files) */
static gboolean skip_dir(const char *path, const char *root)
{
    static const char *SKIP[] = { "/proc", "/sys", "/dev", "/run", NULL };
    for (int i = 0; SKIP[i]; i++)
        if (!strcmp(path, SKIP[i]) && strcmp(root, SKIP[i]))
            return TRUE;
    return FALSE;
}

static gpointer walk(gpointer data)
{
    SJob *j = data;
    GQueue dirs = G_QUEUE_INIT;            /* breadth first: near results come first */
    g_queue_push_tail(&dirs, g_strdup(j->root));
    char *dir;
    while ((dir = g_queue_pop_head(&dirs))) {
        if (g_cancellable_is_cancelled(j->cancel) || j->full) {
            g_free(dir);
            continue;
        }
        DIR *d = opendir(dir);
        if (!d) {
            g_free(dir);
            continue;
        }
        g_mutex_lock(&j->lock);
        j->folders++;
        g_mutex_unlock(&j->lock);
        struct dirent *e;
        while ((e = readdir(d)) && !g_cancellable_is_cancelled(j->cancel)) {
            const char *n = e->d_name;
            if (!strcmp(n, ".") || !strcmp(n, "..") || (n[0] == '.' && !j->hidden))
                continue;
            char *path = g_build_filename(dir, n, NULL);
            gboolean isdir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {     /* some file systems don't say: ask (links not followed) */
                struct stat st;
                isdir = !lstat(path, &st) && S_ISDIR(st.st_mode);
            }
            char *disp = g_filename_display_name(n);
            char *fold = g_utf8_casefold(disp, -1);
            if (strstr(fold, j->needle)) {
                g_mutex_lock(&j->lock);
                if (j->total < SEARCH_MAX) {
                    g_ptr_array_add(j->found, g_strdup(path));
                    j->total++;
                } else
                    j->full = TRUE;
                g_mutex_unlock(&j->lock);
            }
            g_free(fold);
            g_free(disp);
            if (isdir && !skip_dir(path, j->root))
                g_queue_push_tail(&dirs, path);
            else
                g_free(path);
        }
        closedir(d);
        g_free(dir);
    }
    g_mutex_lock(&j->lock);
    j->done = TRUE;
    g_mutex_unlock(&j->lock);
    job_unref(j);
    return NULL;
}

static void set_info(Search *s, const char *text)
{
    gtk_label_set_text(GTK_LABEL(s->info), text);
}

/* the window takes what the worker found so far */
static gboolean flush(gpointer data)
{
    Search *s = data;
    SJob *j = s->job;
    if (!j) {
        s->flush = 0;
        return G_SOURCE_REMOVE;
    }
    g_mutex_lock(&j->lock);
    GPtrArray *batch = j->found;
    j->found = g_ptr_array_new_with_free_func(g_free);
    gboolean done = j->done, full = j->full;
    guint total = j->total, folders = j->folders;
    g_mutex_unlock(&j->lock);
    gsize rlen = strlen(j->root);
    for (guint i = 0; i < batch->len; i++) {
        const char *path = g_ptr_array_index(batch, i);
        char *base = g_path_get_basename(path), *dirn = g_path_get_dirname(path);
        char *bd = g_filename_display_name(base);
        /* the folder it is in, from where the search started: "./photos/2024" */
        const char *rel = dirn + MIN(strlen(dirn), rlen);
        char *where = g_filename_display_name(*rel ? rel + (*rel == '/') : "");
        char *m = g_markup_printf_escaped("%s\n<small><span alpha='70%%'>%s%s</span></small>", bd,
                                          *where ? "in " : "here", where);
        gboolean isdir = g_file_test(path, G_FILE_TEST_IS_DIR);
        char *ct = isdir ? NULL : g_content_type_guess(base, NULL, 0, NULL);
        GIcon *ic = isdir ? g_themed_icon_new("folder") : g_content_type_get_icon(ct);
        gtk_list_store_insert_with_values(s->store, NULL, -1, S_ICON, ic, S_TEXT, m, S_PATH, path, -1);
        g_object_unref(ic);
        g_free(ct);
        g_free(m);
        g_free(where);
        g_free(bd);
        g_free(dirn);
        g_free(base);
    }
    g_ptr_array_unref(batch);
    char *t;
    if (!done)
        t = g_strdup_printf("%u found, searching… (%u folders)", total, folders);
    else if (full)
        t = g_strdup_printf("the first %u found (%u folders); type more to narrow it down", total, folders);
    else
        t = total == 1 ? g_strdup_printf("1 found in %u folders", folders)
                       : g_strdup_printf("%u found in %u folders", total, folders);
    set_info(s, t);
    g_free(t);
    gtk_widget_set_visible(s->stop, !done);
    if (done) {
        s->flush = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void stop_job(Search *s)
{
    if (s->job) {
        g_cancellable_cancel(s->job->cancel);
        job_unref(s->job);
        s->job = NULL;
    }
    if (s->flush) {
        g_source_remove(s->flush);
        s->flush = 0;
    }
    gtk_widget_hide(s->stop);
}

static gboolean start(gpointer data)
{
    Search *s = data;
    s->timer = 0;
    stop_job(s);
    gtk_list_store_clear(s->store);
    preview_clear(s->w->preview);
    const char *text = gtk_entry_get_text(GTK_ENTRY(s->entry));
    char *root = g_file_get_path(s->root);
    if (!*text || !root) {
        set_info(s, root ? "" : "only folders on this computer can be searched");
        g_free(root);
        return G_SOURCE_REMOVE;
    }
    SJob *j = g_new0(SJob, 1);
    j->ref = 1;
    j->cancel = g_cancellable_new();
    g_mutex_init(&j->lock);
    j->found = g_ptr_array_new_with_free_func(g_free);
    j->root = root;
    j->needle = g_utf8_casefold(text, -1);
    j->hidden = s->w->show_hidden;
    s->job = j;
    g_thread_unref(g_thread_new("millerx-search", walk, job_ref(j)));
    s->flush = g_timeout_add(150, flush, s);
    set_info(s, "searching…");
    gtk_widget_show(s->stop);
    return G_SOURCE_REMOVE;
}

static void on_changed(GtkEditable *e, Search *s)
{
    if (s->timer)
        g_source_remove(s->timer);
    s->timer = g_timeout_add(SEARCH_DELAY, start, s);
}

static void on_stop(GtkButton *b, Search *s)
{
    if (s->job)
        g_cancellable_cancel(s->job->cancel);   /* the flush shows what was found and ends */
}

static GFile *picked(Search *s)
{
    GtkTreeIter it;
    GtkTreeModel *m;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(s->view)), &m, &it))
        return NULL;
    char *path;
    gtk_tree_model_get(m, &it, S_PATH, &path, -1);
    GFile *f = g_file_new_for_path(path);
    g_free(path);
    return f;
}

static void on_pick(GtkTreeSelection *sel, Search *s)
{
    GFile *f = picked(s);
    if (f) {
        preview_show_file(s->w->preview, f);
        g_object_unref(f);
    }
}

/* Enter or a double-click: back to the columns, there */
static void on_activate(GtkTreeView *v, GtkTreePath *p, GtkTreeViewColumn *c, Search *s)
{
    GFile *f = picked(s);
    if (!f)
        return;
    search_close(s->w);
    win_open_path(s->w, f);
    g_object_unref(f);
}

static gboolean on_entry_key(GtkWidget *e, GdkEventKey *ev, Search *s)
{
    if (ev->keyval == GDK_KEY_Escape) {
        search_close(s->w);
        return TRUE;
    }
    if ((ev->keyval == GDK_KEY_Down || ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter)
        && gtk_tree_model_iter_n_children(GTK_TREE_MODEL(s->store), NULL)) {
        GtkTreePath *p = gtk_tree_path_new_first();
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(s->view), p, NULL, FALSE);
        gtk_tree_path_free(p);
        gtk_widget_grab_focus(s->view);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_list_key(GtkWidget *v, GdkEventKey *ev, Search *s)
{
    if (ev->keyval == GDK_KEY_Escape) {
        search_close(s->w);
        return TRUE;
    }
    return FALSE;
}

/* built hidden with the window: the bar goes under the toolbar, the list beside the columns */
void search_build(Win *w, GtkWidget *bar_box, GtkWidget *pane)
{
    Search *s = g_new0(Search, 1);
    s->w = w;
    w->search = s;
    s->entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(s->entry, TRUE);
    s->info = gtk_label_new(NULL);
    gtk_widget_set_opacity(s->info, 0.8);
    s->stop = gtk_button_new_with_label("Stop");
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Back to the columns (Escape)");
    s->bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_object_set(s->bar, "margin-start", 4, "margin-end", 4, "margin-bottom", 4, NULL);
    gtk_box_pack_start(GTK_BOX(s->bar), s->entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(s->bar), s->info, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->bar), s->stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->bar), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar_box), s->bar, FALSE, FALSE, 0);
    gtk_widget_show_all(s->bar);                 /* its contents; the bar itself only while searching */
    gtk_widget_hide(s->bar);
    gtk_widget_set_no_show_all(s->bar, TRUE);

    s->store = gtk_list_store_new(S_N, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING);
    s->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s->store));
    g_object_unref(s->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s->view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(s->view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *ri = gtk_cell_renderer_pixbuf_new();
    g_object_set(ri, "stock-size", GTK_ICON_SIZE_LARGE_TOOLBAR, "xpad", 4, NULL);
    gtk_tree_view_column_pack_start(col, ri, FALSE);
    gtk_tree_view_column_add_attribute(col, ri, "gicon", S_ICON);
    GtkCellRenderer *rt = gtk_cell_renderer_text_new();
    g_object_set(rt, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_tree_view_column_pack_start(col, rt, TRUE);
    gtk_tree_view_column_add_attribute(col, rt, "markup", S_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s->view), col);
    s->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s->scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(s->scroll), s->view);
    gtk_box_pack_start(GTK_BOX(pane), s->scroll, FALSE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(pane), s->scroll, 1);      /* where the columns are */
    gtk_widget_show(s->view);
    gtk_widget_set_no_show_all(s->scroll, TRUE);

    g_signal_connect(s->entry, "changed", G_CALLBACK(on_changed), s);
    g_signal_connect(s->entry, "key-press-event", G_CALLBACK(on_entry_key), s);
    g_signal_connect(s->stop, "clicked", G_CALLBACK(on_stop), s);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(search_close), w);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(s->view)), "changed", G_CALLBACK(on_pick), s);
    g_signal_connect(s->view, "row-activated", G_CALLBACK(on_activate), s);
    g_signal_connect(s->view, "key-press-event", G_CALLBACK(on_list_key), s);
}

void search_open(Win *w)
{
    Search *s = w->search;
    g_clear_object(&s->root);
    s->root = win_current_folder(w);
    char *name = g_file_get_basename(s->root);
    char *ph = g_strdup_printf("Search in “%s” and the folders in it", name && strcmp(name, "/") ? name : "/");
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->entry), ph);
    g_free(ph);
    g_free(name);
    int width = MAX(2 * LOOK.column_width, 420);
    gtk_widget_set_size_request(s->scroll, width, -1);
    gtk_widget_hide(w->hscroll);
    gtk_widget_show(s->scroll);
    gtk_widget_show(s->bar);
    gtk_widget_hide(s->stop);
    gtk_widget_grab_focus(s->entry);
    if (*gtk_entry_get_text(GTK_ENTRY(s->entry)))
        on_changed(NULL, s);      /* the same text again, in the new place */
    else
        set_info(s, "");
}

void search_close(Win *w)
{
    Search *s = w->search;
    if (!s || !gtk_widget_get_visible(s->bar))
        return;
    if (s->timer) {
        g_source_remove(s->timer);
        s->timer = 0;
    }
    stop_job(s);
    gtk_list_store_clear(s->store);
    gtk_widget_hide(s->bar);
    gtk_widget_hide(s->scroll);
    gtk_widget_show(w->hscroll);
    preview_clear(w->preview);
    if (w->columns)
        gtk_widget_grab_focus(win_last_column(w)->view);
}

void search_free(Win *w)
{
    Search *s = w->search;
    if (!s)
        return;
    if (s->timer)
        g_source_remove(s->timer);
    stop_job(s);
    g_clear_object(&s->root);
    g_free(s);
    w->search = NULL;
}
