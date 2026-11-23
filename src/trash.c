/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* The trash (trash:///, through GIO): open it from the bookmarks menu, see where each item came
 * from, put items back, delete them for good, empty it. It shows the trash of every disk. */
#include "millerx.h"
#include <string.h>

gboolean file_in_trash(GFile *f)
{
    return f && g_file_has_uri_scheme(f, "trash");
}

/* an item directly in the trash (not inside a trashed folder): only those can go back or be
 * deleted, the trash refuses the rest */
gboolean trash_top(GFile *f)
{
    GFile *parent = g_file_get_parent(f);
    char *uri = parent ? g_file_get_uri(parent) : NULL;
    gboolean top = uri && (!strcmp(uri, "trash:///") || !strcmp(uri, "trash:"));
    g_free(uri);
    g_clear_object(&parent);
    return top;
}

/* Put items back where they were deleted from. The folder they came from is made again if it is
 * gone; something new in their place goes through the usual question (skip, keep both, replace). */
void win_restore(Win *w, GList *files)
{
    guint done = 0;
    gboolean for_all = FALSE;
    Choice all = CH_SKIP;
    for (GList *l = files; l; l = l->next) {
        GFileInfo *fi = g_file_query_info(l->data, "trash::orig-path", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        const char *orig = fi ? g_file_info_get_attribute_byte_string(fi, "trash::orig-path") : NULL;
        if (!orig) {
            g_clear_object(&fi);
            continue;
        }
        GFile *dest = g_file_new_for_path(orig);
        GFile *parent = g_file_get_parent(dest);
        GError *err = NULL;
        if (parent && !g_file_query_exists(parent, NULL))
            g_file_make_directory_with_parents(parent, NULL, NULL);
        gboolean go = TRUE;
        if (g_file_query_exists(dest, NULL)) {
            Choice ch = for_all ? all : dialog_conflict(w, l->data, dest, FALSE, &for_all);
            if (for_all)
                all = ch;
            if (ch == CH_CANCEL) {
                g_object_unref(dest);
                g_clear_object(&parent);
                g_object_unref(fi);
                break;
            }
            if (ch == CH_SKIP)
                go = FALSE;
            else if (ch == CH_BOTH) {
                char *base = g_file_get_basename(dest);
                g_object_unref(dest);
                dest = unique_child(parent, base);
                g_free(base);
            } else if (!delete_tree(dest, NULL, &err))
                go = FALSE;
        }
        if (go && !err && g_file_move(l->data, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &err))
            done++;
        if (err) {
            char *p = file_path(dest);
            char *m = g_strdup_printf("%s: %s", p, err->message);
            win_error(w, "Couldn't put it back", m);
            g_free(m);
            g_free(p);
            g_error_free(err);
        }
        g_object_unref(dest);
        g_clear_object(&parent);
        g_object_unref(fi);
    }
    if (done)
        win_status(w, "%u item(s) put back where they were", done);
}

/* ---- emptying: on a worker, every item in the trash (of every disk) ------------------------------ */
typedef struct {
    Win *w;
    guint removed, failed;
} Empty;

static void empty_work(GTask *t, gpointer src, gpointer data, GCancellable *cancel)
{
    Empty *e = data;
    GFile *trash = g_file_new_for_uri("trash:///");
    GFileEnumerator *en = g_file_enumerate_children(trash, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                    cancel, NULL);
    GFileInfo *fi;
    while (en && (fi = g_file_enumerator_next_file(en, cancel, NULL))) {
        GFile *f = g_file_get_child(trash, g_file_info_get_name(fi));
        if (g_file_delete(f, cancel, NULL))   /* the trash removes a whole trashed folder at once */
            e->removed++;
        else
            e->failed++;
        g_object_unref(f);
        g_object_unref(fi);
    }
    g_clear_object(&en);
    g_object_unref(trash);
    g_task_return_boolean(t, TRUE);
}

static void empty_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Empty *e = data;
    if (!e->w->dead) {
        if (e->failed)
            win_status(e->w, "Trash emptied: %u item(s) deleted, %u couldn't be", e->removed, e->failed);
        else
            win_status(e->w, "Trash emptied: %u item(s) deleted", e->removed);
    }
    win_unref(e->w);
    g_free(e);
}

void win_empty_trash(Win *w)
{
    GFile *trash = g_file_new_for_uri("trash:///");
    GFileInfo *fi = g_file_query_info(trash, "trash::item-count", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    guint n = fi ? g_file_info_get_attribute_uint32(fi, "trash::item-count") : 0;
    g_clear_object(&fi);
    g_object_unref(trash);
    if (!n) {
        win_status(w, "The trash is empty");
        return;
    }
    char *msg = g_strdup_printf("All %u item(s) in the trash are deleted for good, on every disk. "
                                "This can't be undone.", n);
    gboolean ok = win_confirm(w, "Empty the trash?", msg, "Empty trash");
    g_free(msg);
    if (!ok)
        return;
    Empty *e = g_new0(Empty, 1);
    e->w = win_ref(w);
    win_status(w, "Emptying the trash…");
    GTask *t = g_task_new(NULL, NULL, empty_done, e);
    g_task_set_task_data(t, e, NULL);
    g_task_run_in_thread(t, empty_work);
    g_object_unref(t);
}

/* ---- the trash's own right-click menu ---------------------------------------------------------- */
typedef struct {
    Win *w;
    GList *files;
} TAct;

static void tact_free(gpointer data, GClosure *cl)
{
    TAct *a = data;
    g_list_free_full(a->files, g_object_unref);
    win_unref(a->w);
    g_free(a);
}

static void on_restore(GtkMenuItem *mi, TAct *a)
{
    if (!a->w->dead)
        win_restore(a->w, a->files);
}

static void on_delete(GtkMenuItem *mi, TAct *a)
{
    if (!a->w->dead)
        win_delete_forever(a->w, a->files);
}

static void on_empty(GtkMenuItem *mi, TAct *a)
{
    if (!a->w->dead)
        win_empty_trash(a->w);
}

static void titem(GtkWidget *menu, Win *w, GList *files, const char *label, const char *accel, GCallback cb,
                  gboolean sensitive)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    if (accel) {
        guint key;
        GdkModifierType mods;
        gtk_accelerator_parse(accel, &key, &mods);
        gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(mi))), key, mods);
    }
    gtk_widget_set_sensitive(mi, sensitive);
    TAct *a = g_new0(TAct, 1);
    a->w = win_ref(w);
    a->files = g_list_copy_deep(files, obj_copy, NULL);
    g_signal_connect_data(mi, "activate", cb, a, tact_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static gboolean menu_gone(gpointer m)
{
    gtk_widget_destroy(m);
    return G_SOURCE_REMOVE;
}

static void on_deactivate(GtkMenuShell *m, gpointer d)
{
    g_idle_add(menu_gone, m);            /* after the item's "activate" ran */
}

void trash_menu(Win *w, Column *c, GdkEventButton *ev)
{
    GList *files = column_selected_files(c);
    gboolean top = files != NULL;
    for (GList *l = files; l; l = l->next)
        top = top && trash_top(l->data);
    GtkWidget *menu = gtk_menu_new();
    if (files) {
        titem(menu, w, files, "Put back", NULL, G_CALLBACK(on_restore), top);
        titem(menu, w, files, "Delete permanently…", "Delete", G_CALLBACK(on_delete), top);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }
    titem(menu, w, NULL, "Empty trash…", NULL, G_CALLBACK(on_empty), TRUE);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), c->view, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_deactivate), NULL);
    if (ev)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) ev);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), c->view, GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST, NULL);
    g_list_free_full(files, g_object_unref);
}
