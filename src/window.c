/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* The window: toolbar, the strip of columns, preview, status bar, actions, clipboard, menu. */
#include "millerx.h"
#include <string.h>
#include <unistd.h>

#ifndef MILLERX_BINDIR
#define MILLERX_BINDIR "/usr/bin"
#endif

static GdkAtom ATOM_GNOME, ATOM_URIS;

enum { T_GNOME, T_URIS, T_TEXT };

Win *win_ref(Win *w)
{
    w->ref++;
    return w;
}

void win_unref(Win *w)
{
    if (--w->ref > 0)
        return;
    preview_free(w->preview);
    g_hash_table_unref(w->col_widths);
    g_list_free_full(w->clip, g_object_unref);
    g_clear_object(&w->volumes);
    g_free(w);
}

void win_status(Win *w, const char *fmt, ...)
{
    if (w->dead)
        return;
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(w->statusbar), s);
    g_free(s);
}

/* ---- messages ---------------------------------------------------------------------- */
void win_error(Win *w, const char *title, const char *text)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                          GTK_BUTTONS_CLOSE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

gboolean is_root(void)
{
    return geteuid() == 0;
}

/* A second millerX as root, through pkexec: polkit asks for the password in its own
 * window, millerX never sees it. Needs /usr/share/polkit-1/actions/org.mix.millerx.policy
 * and a polkit authentication agent in the session. */
#define ROOT_POLICY "/usr/share/polkit-1/actions/org.mix.millerx.policy"

static void root_finished(GObject *src, GAsyncResult *res, gpointer data)
{
    Win *w = data;
    GSubprocess *p = G_SUBPROCESS(src);
    char *err_out = NULL;
    GError *err = NULL;
    g_subprocess_communicate_utf8_finish(p, res, NULL, &err_out, &err);
    int code = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
    if (!w->dead) {
        if (err_out && strstr(err_out, "authentication agent")) {
            win_error(w, "Can't open as root",
                      "No polkit authentication agent is running, so nothing can ask for the "
                      "root password.\n\nStart one in your session, for example polkit-gnome "
                      "(/usr/libexec/polkit-gnome-authentication-agent-1), lxpolkit, mate-polkit "
                      "or polkit-kde-agent, and try again.");
        } else if (code == 126) {
            win_status(w, "Open as root: cancelled");
        } else if (code == 127) {
            win_error(w, "Can't open as root", err_out && *err_out ? err_out : "Not authorized.");
        } else if (code != 0 && err_out && strstr(err_out, "cannot open display")) {
            win_error(w, "Can't open as root", "The root window couldn't reach the display. "
                      "Is millerX's polkit policy installed (" ROOT_POLICY ")?");
        } else {
            win_status(w, "Root window closed");
        }
    }
    g_clear_error(&err);
    g_free(err_out);
    win_unref(w);
}

void win_open_as_root(Win *w, GFile *folder)
{
    char *path = g_file_get_path(folder);
    char *pkexec = g_find_program_in_path("pkexec");
    char *exe = g_build_filename(MILLERX_BINDIR, "millerx", NULL);
    if (!pkexec) {
        win_error(w, "Can't open as root",
                  "This needs polkit (the pkexec program), which isn't installed. "
                  "Install polkit and a polkit authentication agent, then try again.");
    } else if (!g_file_test(ROOT_POLICY, G_FILE_TEST_EXISTS)) {
        win_error(w, "Can't open as root",
                  "millerX's polkit policy isn't installed (" ROOT_POLICY "). "
                  "Install millerX as a package or with “ninja install”.");
    } else if (!path) {
        win_error(w, "Can't open as root", "Only local folders can be opened as root.");
    } else {
        GError *err = NULL;
        GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                          &err, pkexec, exe, path, NULL);
        if (!p) {
            win_error(w, "Can't open as root", err->message);
            g_error_free(err);
        } else {
            win_status(w, "Asking for the root password …");
            /* reads stderr until the root window closes, then tells us how it went */
            g_subprocess_communicate_utf8_async(p, NULL, NULL, root_finished, win_ref(w));
            g_object_unref(p);
        }
    }
    g_free(exe);
    g_free(pkexec);
    g_free(path);
}

/* an error message; if we're not root and know the folder, offer to open it as root */
void win_error_root(Win *w, const char *title, const char *text, GFile *folder)
{
    if (!folder || is_root()) {
        win_error(w, title, text);
        return;
    }
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                          GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s\n\nThis needs root rights.", text);
    gtk_dialog_add_button(GTK_DIALOG(d), "Close", GTK_RESPONSE_CLOSE);
    gtk_dialog_add_button(GTK_DIALOG(d), "Open as root…", GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_ACCEPT)
        win_open_as_root(w, folder);
}

gboolean win_confirm(Win *w, const char *title, const char *text, const char *ok)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), ok, GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_OK;
}

char *win_ask_name(Win *w, const char *title, const char *initial, gboolean select_stem)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), GTK_DIALOG_MODAL,
                                               "Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), initial);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 40);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_widget_set_margin_start(area, 8);
    gtk_widget_set_margin_end(area, 8);
    gtk_box_pack_start(GTK_BOX(area), e, TRUE, TRUE, 8);
    gtk_widget_show_all(d);
    gtk_widget_grab_focus(e);
    int stem = -1;
    if (select_stem && initial[0] != '.') {
        const char *dot = strrchr(initial, '.');
        if (dot && dot != initial)
            stem = g_utf8_pointer_to_offset(initial, dot);
    }
    gtk_editable_select_region(GTK_EDITABLE(e), 0, stem);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
    gtk_widget_destroy(d);
    if (r != GTK_RESPONSE_OK || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        g_free(name);
        return NULL;
    }
    return name;
}

/* ---- navigation --------------------------------------------------------------------- */
Column *win_last_column(Win *w)
{
    GList *l = g_list_last(w->columns);
    return l ? l->data : NULL;
}

static void update_location(Win *w, GFile *f)
{
    char *p = file_path(f);
    gtk_entry_set_text(GTK_ENTRY(w->location), p);
    char *t = is_root() ? g_strconcat("ROOT: ", p, NULL) : g_strdup(p);
    gtk_window_set_title(GTK_WINDOW(w->window), t);   /* the yellow M icon says millerX */
    g_free(t);
    g_free(p);
}

static void drop_column(Win *w, Column *c)
{
    column_close(c);
    gtk_widget_destroy(c->frame);
    gtk_widget_destroy(c->divider);
    if (w->focused == c)
        w->focused = NULL;
    column_unref(c);
}

static void clear_columns(Win *w)
{
    for (GList *l = w->columns; l; l = l->next)
        drop_column(w, l->data);
    g_list_free(w->columns);
    w->columns = NULL;
    w->focused = NULL;
    preview_clear(w->preview);
}

gboolean win_scroll_end(gpointer data)
{
    Win *w = data;
    if (w->dead || w->resizing)     /* don't pull the strip away under the mouse while resizing */
        return G_SOURCE_REMOVE;
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(w->hscroll));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    return G_SOURCE_REMOVE;
}

static void idle_scroll_end(Win *w)
{
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, win_scroll_end, win_ref(w), (GDestroyNotify) win_unref);
}

static void col_drag(double dx, gpointer data)
{
    Column *c = data;
    if (c->drag_start < 0) {
        c->drag_start = c->width;
        c->win->resizing = TRUE;
    }
    column_set_width(c, (int) (c->drag_start + dx));
}

static void col_done(gpointer data)
{
    Column *c = data;
    c->drag_start = -1;
    c->win->resizing = FALSE;
}

static void col_reset(gpointer data)
{
    column_set_width(data, COL_WIDTH);
}

static Column *add_column(Win *w, GFile *f, GPtrArray *chain)
{
    Column *c = column_new(w, f, chain);
    w->columns = g_list_append(w->columns, c);
    gtk_box_pack_start(GTK_BOX(w->colbox), c->frame, FALSE, FALSE, 0);
    c->divider = divider_new(col_drag, col_reset, col_done, c, TRUE);
    gtk_box_pack_start(GTK_BOX(w->colbox), c->divider, FALSE, FALSE, 0);
    gtk_widget_show_all(c->frame);
    gtk_widget_show(c->divider);
    idle_scroll_end(w);
    return c;
}

/* show the whole chain of folders from / down to f, one column each */
void win_open_path(Win *w, GFile *f)
{
    GFile *keep = g_object_ref(f);     /* f may belong to a column we're about to drop */
    char *path = g_file_get_path(keep);
    clear_columns(w);
    if (!path) {
        add_column(w, keep, NULL);
    } else {
        char *norm = g_canonicalize_filename(path, "/");
        char **parts = g_strsplit(norm, "/", -1);
        GPtrArray *chain = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; parts[i]; i++)
            if (*parts[i])
                g_ptr_array_add(chain, g_strdup(parts[i]));
        GFile *root = g_file_new_for_path("/");
        add_column(w, root, chain->len ? chain : NULL);
        g_object_unref(root);
        g_ptr_array_unref(chain);
        g_strfreev(parts);
        g_free(norm);
    }
    update_location(w, keep);
    g_free(path);
    g_object_unref(keep);
}

void win_chain_finished(Win *w)
{
    GtkWidget *focus = NULL;
    for (GList *l = g_list_last(w->columns); l; l = l->prev)
        if (column_n_selected(l->data)) {
            focus = ((Column *) l->data)->view;
            break;
        }
    if (!focus && w->columns)
        focus = win_last_column(w)->view;
    if (focus)
        gtk_widget_grab_focus(focus);
    idle_scroll_end(w);
}

static void truncate_after(Win *w, Column *c)
{
    GList *l = g_list_find(w->columns, c);
    if (!l)
        return;
    GList *rest = l->next;
    if (rest) {
        l->next = NULL;
        rest->prev = NULL;
        for (GList *r = rest; r; r = r->next)
            drop_column(w, r->data);
        g_list_free(rest);
    }
}

void win_column_selected(Win *w, Column *c)
{
    if (!g_list_find(w->columns, c))
        return;
    truncate_after(w, c);
    int n = column_n_selected(c);
    if (n == 0) {
        preview_clear(w->preview);
        update_location(w, c->gfile);
        return;
    }
    if (n > 1) {
        GList *files = column_selected_files(c);
        update_location(w, c->gfile);
        preview_show_many(w->preview, files);
        win_status(w, "%d items selected", n);
        g_list_free_full(files, g_object_unref);
        return;
    }
    GFile *f;
    gboolean isdir;
    column_selected_single(c, &f, &isdir);
    update_location(w, f);
    if (isdir)
        add_column(w, f, NULL);
    preview_show_file(w->preview, f);
    g_object_unref(f);
    idle_scroll_end(w);
}

void win_focus_next(Win *w, Column *c)
{
    GList *l = g_list_find(w->columns, c);
    if (l && l->next) {
        Column *nx = l->next->data;
        gtk_widget_grab_focus(nx->view);
        GtkTreeIter it;
        if (!column_n_selected(nx) && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(nx->store), &it))
            column_select_iter(nx, &it);
    }
}

void win_focus_prev(Win *w, Column *c)
{
    GList *l = g_list_find(w->columns, c);
    if (l && l->prev) {
        Column *pv = l->prev->data;
        gtk_widget_grab_focus(pv->view);
        win_column_selected(w, pv);
    }
}

static void go_up(Win *w)
{
    Column *c = g_list_find(w->columns, w->focused) ? w->focused : win_last_column(w);
    if (c)
        win_focus_prev(w, c);
}

static void go_home(Win *w)
{
    GFile *h = g_file_new_for_path(g_get_home_dir());
    win_open_path(w, h);
    g_object_unref(h);
}

static void go_root(Win *w)
{
    GFile *r = g_file_new_for_path("/");
    win_open_path(w, r);
    g_object_unref(r);
}

GFile *win_current_folder(Win *w)
{
    Column *c = win_last_column(w);
    return c ? g_object_ref(c->gfile) : g_file_new_for_path(g_get_home_dir());
}

static void on_location(GtkEntry *e, Win *w)
{
    char *t = g_strstrip(g_strdup(gtk_entry_get_text(e)));
    char *exp = t[0] == '~' ? g_build_filename(g_get_home_dir(), t + 1, NULL) : g_strdup(t);
    GFile *f = g_file_new_for_commandline_arg(exp);
    if (g_file_query_exists(f, NULL))
        win_open_path(w, f);
    else
        win_status(w, "Not found: %s", t);
    g_object_unref(f);
    g_free(exp);
    g_free(t);
}

void win_reload_all(Win *w)
{
    Column *c = g_list_find(w->columns, w->focused) ? w->focused : NULL;
    GFile *target = NULL, *one = NULL;
    if (c && column_selected_single(c, &one, NULL))
        target = one;
    else if (c)
        target = g_object_ref(c->gfile);
    else if (w->columns)
        target = g_object_ref(win_last_column(w)->gfile);
    else
        target = g_file_new_for_path(g_get_home_dir());
    win_open_path(w, target);
    g_object_unref(target);
}

void win_toggle_hidden(Win *w)
{
    w->show_hidden = !w->show_hidden;
    win_reload_all(w);
    win_status(w, w->show_hidden ? "Hidden files shown" : "Hidden files hidden");
}

GdkModifierType win_pointer_mask(Win *w)
{
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(w->window));
    GdkModifierType mask = 0;
    gdk_window_get_device_position(gtk_widget_get_window(w->window), gdk_seat_get_pointer(seat),
                                   NULL, NULL, &mask);
    return mask;
}

/* ---- preview width by dragging its edge -------------------------------------------- */
static void preview_drag(double dx, gpointer data)
{
    Win *w = data;
    if (w->pv_start < 0) {
        w->pv_start = gtk_widget_get_allocated_width(w->preview->box);
        w->resizing = TRUE;
    }
    /* dragging left makes it wider; the width is the preview's minimum, it still takes the rest */
    int most = CLAMP(gtk_widget_get_allocated_width(w->window) - 200, 200, 900);   /* 900: settings cap */
    int nw = CLAMP((int) (w->pv_start - dx), 200, most);
    w->preview->width = nw;
    gtk_widget_set_size_request(w->preview->box, nw, -1);
}

static void preview_set_width(Win *w, int width)
{
    Look nl = LOOK;
    nl.preview_width = width;
    w->preview->width = -1;          /* -1: look_changed redraws the preview */
    app_apply_look(w->app, &nl);
    look_save(NULL);
}

static void preview_drag_done(gpointer data)
{
    Win *w = data;
    int width = w->preview->width;
    w->pv_start = -1;
    w->resizing = FALSE;
    preview_set_width(w, width);
}

static void preview_reset(gpointer data)
{
    preview_set_width(data, PREVIEW_DEFAULT);
}

void win_look_changed(Win *w, gboolean resort)
{
    int dw = divider_hit_width();
    for (GList *l = w->columns; l; l = l->next) {
        Column *c = l->data;
        gtk_widget_set_size_request(c->divider, dw, -1);
        gtk_widget_queue_draw(c->divider);
    }
    gtk_widget_set_size_request(w->pane_divider, dw, -1);
    if (w->preview->width != LOOK.preview_width) {
        w->preview->width = LOOK.preview_width;
        gtk_widget_set_size_request(w->preview->box, w->preview->width, -1);
        if (w->preview->current) {
            GFile *f = g_object_ref(w->preview->current);
            preview_show_file(w->preview, f);
            g_object_unref(f);
        }
    }
    if (resort)
        win_reload_all(w);
}

/* ---- actions ------------------------------------------------------------------------- */
void win_open_default(Win *w, GFile *f)
{
    char *uri = g_file_get_uri(f);
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        win_status(w, "%s", err->message);
        g_error_free(err);
    }
    g_free(uri);
}

static void open_with(Win *w, GAppInfo *app, GList *files)
{
    GError *err = NULL;
    if (!g_app_info_launch(app, files, NULL, &err)) {
        win_status(w, "%s", err->message);
        g_error_free(err);
    }
}

/* Known terminals and how to tell them the folder. All of them are also started *in*
 * the folder, so the ones without a flag open there too. */
static const struct { const char *exe, *flag; } TERMINALS[] = {
    { "xfce4-terminal", "--working-directory=" },
    { "x-terminal-emulator", NULL },          /* Debian's alternatives link */
    { "konsole", "--workdir" },
    { "gnome-terminal", "--working-directory=" },
    { "kgx", "--working-directory=" },        /* GNOME Console */
    { "ptyxis", "--working-directory=" },
    { "mate-terminal", "--working-directory=" },
    { "lxterminal", "--working-directory=" },
    { "qterminal", "-w" },
    { "tilix", "--working-directory=" },
    { "terminator", "--working-directory=" },
    { "alacritty", "--working-directory" },
    { "kitty", "--directory" },
    { "foot", "--working-directory=" },
    { "wezterm", NULL },
    { "urxvt", NULL },
    { "st", NULL },
    { "xterm", NULL },
};

static void terminal_here(Win *w, GFile *folder)
{
    char *path = g_file_get_path(folder);
    if (!path)
        return;
    gboolean ok = FALSE;
    /* the user's choice first: $TERMINAL (may contain arguments) */
    const char *env = g_getenv("TERMINAL");
    if (env && *env) {
        char **argv = NULL;
        if (g_shell_parse_argv(env, NULL, &argv, NULL))
            ok = g_spawn_async(path, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        g_strfreev(argv);
    }
    for (guint i = 0; !ok && i < G_N_ELEMENTS(TERMINALS); i++) {
        char *exe = g_find_program_in_path(TERMINALS[i].exe);
        if (!exe)
            continue;
        const char *flag = TERMINALS[i].flag;
        char *a1 = NULL, *a2 = NULL;
        if (flag && g_str_has_suffix(flag, "="))
            a1 = g_strconcat(flag, path, NULL);          /* --working-directory=/path */
        else if (flag) {
            a1 = g_strdup(flag);                          /* --workdir /path */
            a2 = g_strdup(path);
        }
        char *argv[] = { exe, a1, a2, NULL };
        ok = g_spawn_async(path, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
        g_free(a1);
        g_free(a2);
        g_free(exe);
    }
    if (!ok)
        win_error(w, "No terminal found",
                  "Install a terminal (xterm, xfce4-terminal, konsole, …) or set the TERMINAL "
                  "environment variable to the one you use.");
    g_free(path);
}

void win_new_folder(Win *w, Column *c)
{
    char *name = win_ask_name(w, "New folder", "New Folder", FALSE);
    if (!name)
        return;
    GFile *f = g_file_get_child(c->gfile, name);
    GError *err = NULL;
    if (!g_file_make_directory(f, NULL, &err)) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
            win_error_root(w, "Couldn't create the folder", err->message, c->gfile);
        else
            win_error(w, "Couldn't create the folder", err->message);
        g_error_free(err);
    } else {
        g_free(c->pending_select);
        c->pending_select = g_strdup(name);
        column_add_file(c, f);
    }
    g_object_unref(f);
    g_free(name);
}

/* Asks for a new name and renames f; the renamed file, or NULL. */
static GFile *rename_file(Win *w, GFile *f)
{
    GFile *result = NULL;
    GFileInfo *info = g_file_query_info(f, "standard::display-name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    char *old = info ? g_strdup(g_file_info_get_display_name(info)) : g_file_get_basename(f);
    g_clear_object(&info);
    char *name = win_ask_name(w, "Rename", old, TRUE);
    if (name && strcmp(name, old)) {
        GFile *parent = g_file_get_parent(f), *exist = g_file_get_child(parent, name);
        if (g_file_query_exists(exist, NULL)) {
            char *msg = g_strdup_printf("“%s” already exists.", name);
            win_error(w, "Couldn't rename", msg);
            g_free(msg);
        } else {
            GError *err = NULL;
            GFile *nf = g_file_set_display_name(f, name, NULL, &err);
            if (!nf) {
                win_error_root(w, "Couldn't rename", err->message,
                               g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ? parent : NULL);
                g_error_free(err);
            } else {
                result = nf;
            }
        }
        g_object_unref(exist);
        g_object_unref(parent);
    }
    g_free(name);
    g_free(old);
    return result;
}

void win_rename(Win *w, Column *c)
{
    GFile *f;
    if (!column_selected_single(c, &f, NULL)) {
        g_clear_object(&f);
        return;
    }
    GFile *nf = rename_file(w, f);
    if (nf) {
        column_remove_file(c, f);
        char *name = g_file_get_basename(nf);
        g_free(c->pending_select);
        c->pending_select = name;
        column_add_file(c, nf);
        g_object_unref(nf);
    }
    g_object_unref(f);
}

/* the same for a file that isn't in a column (a thumbnail); folder monitors show the change */
void win_rename_file(Win *w, GFile *f)
{
    GFile *nf = rename_file(w, f);
    g_clear_object(&nf);
}

typedef struct {
    Win *w;
    GList *files;
    GPtrArray *errs;
    GFile *denied_in;     /* folder of the first "permission denied" */
} Del;

static gboolean delete_report(gpointer data)
{
    Del *d = data;
    if (d->errs->len) {
        GString *s = g_string_new(NULL);
        for (guint i = 0; i < d->errs->len && i < 20; i++)
            g_string_append_printf(s, "%s%s", i ? "\n" : "", (char *) g_ptr_array_index(d->errs, i));
        if (!d->w->dead)
            win_error_root(d->w, "Some items couldn't be deleted", s->str, d->denied_in);
        g_string_free(s, TRUE);
    } else {
        win_status(d->w, "%u item(s) deleted", g_list_length(d->files));
    }
    g_ptr_array_unref(d->errs);
    g_clear_object(&d->denied_in);
    g_list_free_full(d->files, g_object_unref);
    win_unref(d->w);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static gpointer delete_thread(gpointer data)
{
    Del *d = data;
    for (GList *l = d->files; l; l = l->next) {
        GError *err = NULL;
        if (!delete_tree(l->data, NULL, &err)) {
            char *b = g_file_get_basename(l->data);
            g_ptr_array_add(d->errs, g_strdup_printf("%s: %s", b, err ? err->message : "?"));
            if (!d->denied_in && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
                d->denied_in = g_file_get_parent(l->data);
            g_free(b);
        }
        g_clear_error(&err);
    }
    g_idle_add(delete_report, d);
    return NULL;
}

static void delete_files(Win *w, GList *files)
{
    Del *d = g_new0(Del, 1);
    d->w = win_ref(w);
    d->files = g_list_copy_deep(files, obj_copy, NULL);
    d->errs = g_ptr_array_new_with_free_func(g_free);
    g_thread_unref(g_thread_new("millerx-delete", delete_thread, d));
}

static char *some_names(GList *files, int max)
{
    GString *s = g_string_new(NULL);
    int i = 0;
    for (GList *l = files; l && i < max; l = l->next, i++) {
        char *b = g_file_get_basename(l->data);
        g_string_append_printf(s, "%s%s", i ? ", " : "", b);
        g_free(b);
    }
    if ((int) g_list_length(files) > max)
        g_string_append(s, " …");
    return g_string_free(s, FALSE);
}

void win_trash(Win *w, GList *files)
{
    if (!files)
        return;
    GList *failed = NULL;
    char *first_err = NULL;
    gboolean denied = FALSE;
    for (GList *l = files; l; l = l->next) {
        GError *err = NULL;
        if (!g_file_trash(l->data, NULL, &err)) {
            failed = g_list_append(failed, g_object_ref(l->data));
            if (!first_err)
                first_err = g_strdup(err->message);
            denied |= g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
            g_error_free(err);
        }
    }
    if (failed && denied && !is_root()) {
        char *names = some_names(failed, 5);
        char *msg = g_strdup_printf("%s: %s", names, first_err);
        GFile *parent = g_file_get_parent(failed->data);
        win_error_root(w, "Can't move to trash", msg, parent);
        g_clear_object(&parent);
        g_free(msg);
        g_free(names);
        g_list_free_full(failed, g_object_unref);
    } else if (failed) {
        char *names = some_names(failed, 5);
        char *msg = g_strdup_printf("%s: %s\n\nDelete permanently instead?", names, first_err);
        if (win_confirm(w, "Can't move to trash", msg, "Delete permanently"))
            delete_files(w, failed);
        g_free(msg);
        g_free(names);
        g_list_free_full(failed, g_object_unref);
    } else {
        win_status(w, "%u item(s) moved to trash", g_list_length(files));
    }
    g_free(first_err);
}

void win_delete_forever(Win *w, GList *files)
{
    if (!files)
        return;
    char *names = some_names(files, 5);
    char *title = g_strdup_printf("Delete %u item(s) permanently?", g_list_length(files));
    char *msg = g_strdup_printf("%s\n\nThis can't be undone.", names);
    if (win_confirm(w, title, msg, "Delete"))
        delete_files(w, files);
    g_free(msg);
    g_free(title);
    g_free(names);
}

/* ---- clipboard (Thunar-compatible x-special/gnome-copied-files + uri-list + text) ---------- */
static void clip_get_cb(GtkClipboard *cb, GtkSelectionData *data, guint info, gpointer user)
{
    (void) cb;
    Win *w = user;
    if (!w->clip)
        return;
    GString *s = g_string_new(NULL);
    int n = g_list_length(w->clip), i = 0;
    char **uris = g_new0(char *, n + 1);
    for (GList *l = w->clip; l; l = l->next)
        uris[i++] = g_file_get_uri(l->data);
    if (info == T_GNOME) {
        g_string_append(s, w->clip_cut ? "cut" : "copy");
        for (i = 0; uris[i]; i++)
            g_string_append_printf(s, "\n%s", uris[i]);
        gtk_selection_data_set(data, ATOM_GNOME, 8, (guchar *) s->str, s->len);
    } else if (info == T_URIS) {
        gtk_selection_data_set_uris(data, uris);
    } else {
        for (GList *l = w->clip; l; l = l->next) {
            char *p = file_path(l->data);
            g_string_append_printf(s, "%s%s", s->len ? "\n" : "", p);
            g_free(p);
        }
        gtk_selection_data_set_text(data, s->str, -1);
    }
    g_strfreev(uris);
    g_string_free(s, TRUE);
}

static void clip_clear_cb(GtkClipboard *cb, gpointer user)
{
    (void) cb;
    Win *w = user;
    g_list_free_full(w->clip, g_object_unref);
    w->clip = NULL;
    win_unref(w);
}

/* takes ownership of files */
void win_clip_set(Win *w, GList *files, gboolean cut)
{
    if (!files)
        return;
    static const GtkTargetEntry targets[] = {
        { (char *) "x-special/gnome-copied-files", 0, T_GNOME },
        { (char *) "text/uri-list", 0, T_URIS },
        { (char *) "UTF8_STRING", 0, T_TEXT },
        { (char *) "text/plain;charset=utf-8", 0, T_TEXT },
    };
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (gtk_clipboard_set_with_data(cb, targets, G_N_ELEMENTS(targets), clip_get_cb, clip_clear_cb,
                                    win_ref(w))) {
        /* clear_cb of an earlier owner ran inside set_with_data; now fill in ours */
        g_list_free_full(w->clip, g_object_unref);
        w->clip = files;
        w->clip_cut = cut;
        win_status(w, "%u item(s) %s", g_list_length(files), cut ? "cut" : "copied");
    } else {
        win_unref(w);
        g_list_free_full(files, g_object_unref);
    }
}

/* (files, cut) from our own clipboard, Thunar's, or any uri list */
static GList *clip_get(Win *w, gboolean *cut)
{
    *cut = FALSE;
    if (w->clip) {
        *cut = w->clip_cut;
        return g_list_copy_deep(w->clip, obj_copy, NULL);
    }
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    GList *out = NULL;
    GtkSelectionData *sd = gtk_clipboard_wait_for_contents(cb, ATOM_GNOME);
    if (sd && gtk_selection_data_get_length(sd) > 0) {
        char *txt = g_strndup((const char *) gtk_selection_data_get_data(sd), gtk_selection_data_get_length(sd));
        char **lines = g_strsplit(txt, "\n", -1);
        if (lines[0] && (!strcmp(lines[0], "copy") || !strcmp(lines[0], "cut"))) {
            *cut = !strcmp(lines[0], "cut");
            for (int i = 1; lines[i]; i++)
                if (*g_strstrip(lines[i]))
                    out = g_list_append(out, g_file_new_for_uri(lines[i]));
        }
        g_strfreev(lines);
        g_free(txt);
    }
    if (sd)
        gtk_selection_data_free(sd);
    if (out)
        return out;
    char **uris = gtk_clipboard_wait_for_uris(cb);
    for (int i = 0; uris && uris[i]; i++)
        out = g_list_append(out, g_file_new_for_uri(uris[i]));
    g_strfreev(uris);
    return out;
}

void win_paste_into(Win *w, GFile *folder)
{
    gboolean cut;
    GList *files = clip_get(w, &cut);
    if (!files) {
        win_status(w, "Nothing to paste");
        return;
    }
    job_run(w, cut, files, folder);
    g_list_free_full(files, g_object_unref);
    if (cut)
        gtk_clipboard_clear(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
}

/* ---- context menu --------------------------------------------------------------------- */
typedef struct {
    Win *w;
    Column *c;
    GList *files;
    GFile *folder;
    GAppInfo *app;
    int action;
} Act;

enum { A_OPEN_DIR, A_OPEN, A_OPEN_WITH, A_CUT, A_COPY, A_PASTE_INTO, A_PASTE, A_RENAME, A_TRASH,
       A_DELETE, A_NEW_FOLDER, A_TERMINAL, A_COPY_PATH, A_BOOKMARK, A_PROPERTIES, A_ROOT, A_SHOW,
       A_NEW_WINDOW, A_EXTRACT_HERE, A_EXTRACT_FOLDER, A_COMPRESS };

/* ---- archives ------------------------------------------------------------------------------ */
/* Asks for the archive's name and format, then packs the files (which share one folder) there. */
static void compress_dialog(Win *w, GList *files)
{
    static int last_format;          /* the format picked last time, this session */
    GFile *folder = g_file_get_parent(files->data);
    char *initial = files->next ? g_file_get_basename(folder) : g_file_get_basename(files->data);
    if (!files->next && !is_dir(files->data, FALSE)) {          /* "notes.txt" -> "notes" */
        const char *dot = strrchr(initial, '.');
        if (dot && dot != initial)
            initial[dot - initial] = 0;
    }
    GtkWidget *d = gtk_dialog_new_with_buttons("Compress", GTK_WINDOW(w->window), GTK_DIALOG_MODAL,
                                               "Cancel", GTK_RESPONSE_CANCEL, "Compress", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    g_object_set(grid, "margin", 12, NULL);
    char *what = files->next ? g_strdup_printf("%u items", g_list_length(files)) : g_file_get_basename(files->data);
    char *head = g_strdup_printf("Pack “%s” into an archive in the same folder:", what);
    GtkWidget *l = gtk_label_new(head);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 2, 1);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), initial);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 32);
    gtk_widget_set_hexpand(e, TRUE);
    gtk_grid_attach(GTK_GRID(grid), e, 0, 1, 1, 1);
    GtkWidget *fmt = gtk_combo_box_text_new();
    for (int i = 0; i < ARC_N_FORMATS; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fmt), ARC_FORMATS[i].ext);
    gtk_combo_box_set_active(GTK_COMBO_BOX(fmt), last_format);
    gtk_grid_attach(GTK_GRID(grid), fmt, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid);
    gtk_widget_show_all(d);
    gtk_widget_grab_focus(e);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
    int f = gtk_combo_box_get_active(GTK_COMBO_BOX(fmt));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_OK && *name && !strchr(name, '/') && f >= 0) {
        last_format = f;
        char *full = g_strconcat(name, ARC_FORMATS[f].ext, NULL);
        GFile *out = g_file_get_child(folder, full);
        if (g_file_query_exists(out, NULL)) {
            char *msg = g_strdup_printf("“%s” already exists. Pick another name.", full);
            win_error(w, "Couldn't compress", msg);
            g_free(msg);
        } else {
            job_compress(w, files, out, f);
        }
        g_object_unref(out);
        g_free(full);
    }
    g_free(name);
    g_free(head);
    g_free(what);
    g_free(initial);
    g_object_unref(folder);
}

/* every file in the list is an archive millerX can unpack */
static gboolean all_archives(GList *files)
{
    for (GList *l = files; l; l = l->next) {
        GFileInfo *fi = g_file_query_info(l->data, "standard::content-type,standard::type",
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
        gboolean yes = fi && g_file_info_get_file_type(fi) == G_FILE_TYPE_REGULAR
                       && g_file_info_get_content_type(fi) && is_archive_type(g_file_info_get_content_type(fi));
        g_clear_object(&fi);
        if (!yes)
            return FALSE;
    }
    return files != NULL;
}

static void act_free(gpointer data, GClosure *cl)
{
    (void) cl;
    Act *a = data;
    g_list_free_full(a->files, g_object_unref);
    g_clear_object(&a->folder);
    g_clear_object(&a->app);
    if (a->c)       /* NULL: a menu for one file outside the columns (a thumbnail) */
        column_unref(a->c);
    g_free(a);
}

static void act_run(GtkMenuItem *mi, Act *a)
{
    (void) mi;
    Win *w = a->w;
    Column *c = a->c;
    if (w->dead || (c && c->closed))
        return;
    switch (a->action) {
    case A_OPEN_DIR: win_focus_next(w, c); break;
    case A_OPEN: win_open_default(w, a->files->data); break;
    case A_OPEN_WITH: open_with(w, a->app, a->files); break;
    case A_CUT:
    case A_COPY:
        win_clip_set(w, g_list_copy_deep(a->files, obj_copy, NULL), a->action == A_CUT);
        break;
    case A_PASTE_INTO: win_paste_into(w, a->files->data); break;
    case A_PASTE: win_paste_into(w, c->gfile); break;
    case A_RENAME:
        if (c)
            win_rename(w, c);
        else
            win_rename_file(w, a->files->data);
        break;
    case A_SHOW: win_open_path(w, a->files->data); break;
    case A_TRASH: win_trash(w, a->files); break;
    case A_DELETE: win_delete_forever(w, a->files); break;
    case A_NEW_FOLDER: win_new_folder(w, c); break;
    case A_TERMINAL: terminal_here(w, a->folder); break;
    case A_COPY_PATH: {
        GString *s = g_string_new(NULL);
        for (GList *l = a->files; l; l = l->next) {
            char *p = file_path(l->data);
            g_string_append_printf(s, "%s%s", s->len ? "\n" : "", p);
            g_free(p);
        }
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s->str, -1);
        g_string_free(s, TRUE);
        break;
    }
    case A_BOOKMARK: places_bookmark_add(w, a->folder); break;
    case A_ROOT: win_open_as_root(w, a->folder); break;
    case A_NEW_WINDOW: win_new(w->app, a->folder); break;
    case A_EXTRACT_HERE:
    case A_EXTRACT_FOLDER: job_extract(w, a->files, a->action == A_EXTRACT_FOLDER); break;
    case A_COMPRESS: compress_dialog(w, a->files); break;
    case A_PROPERTIES:
        dialog_properties(w, a->files && (!a->files->next || !c) ? a->files->data : c->gfile);
        break;
    }
}

static GtkWidget *item(GtkWidget *menu, Win *w, Column *c, GList *files, GFile *folder, int action,
                       const char *label, const char *accel, gboolean sensitive)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    if (accel) {
        guint key;
        GdkModifierType mods;
        gtk_accelerator_parse(accel, &key, &mods);
        gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(mi))), key, mods);
    }
    gtk_widget_set_sensitive(mi, sensitive);
    Act *a = g_new0(Act, 1);
    a->w = w;
    a->c = c;
    if (c)
        column_ref(c);
    a->files = g_list_copy_deep(files, obj_copy, NULL);
    a->folder = folder ? g_object_ref(folder) : NULL;
    a->action = action;
    g_object_set_data(G_OBJECT(mi), "act", a);
    g_signal_connect_data(mi, "activate", G_CALLBACK(act_run), a, act_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    return mi;
}

static void sep(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static gboolean destroy_idle(gpointer menu)
{
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

/* "deactivate" comes before the chosen item's "activate": destroy later, not now */
static void menu_done(GtkMenuShell *menu, gpointer d)
{
    (void) d;
    g_idle_add(destroy_idle, menu);
}

static void on_hidden_toggled(GtkCheckMenuItem *mi, Win *w)
{
    (void) mi;
    win_toggle_hidden(w);
}

/* ---- per-folder sorting ------------------------------------------------------------ */
/* a folder's order changed: its columns resort, every column redraws its ⇅ marks, and a gallery
 * showing that folder is made again */
void app_sorting_changed(GtkApplication *app, const char *path)
{
    for (GList *l = gtk_application_get_windows(app); l; l = l->next) {
        Win *w = g_object_get_data(G_OBJECT(l->data), "millerx-win");
        if (!w || w->dead)
            continue;
        for (GList *k = w->columns; k; k = k->next) {
            Column *c = k->data;
            char *cp = g_file_get_path(c->gfile);
            if (!g_strcmp0(cp, path))
                column_apply_sort(c);
            g_free(cp);
            gtk_widget_queue_draw(c->view);
        }
        char *pp = w->preview->current ? g_file_get_path(w->preview->current) : NULL;
        if (!g_strcmp0(pp, path)) {
            GFile *f = g_object_ref(w->preview->current);
            preview_show_file(w->preview, f);
            g_object_unref(f);
        }
        g_free(pp);
    }
}

static void sort_pick(GtkCheckMenuItem *mi, Win *w)
{
    if (!gtk_check_menu_item_get_active(mi))
        return;                  /* radio items: only the newly chosen one counts */
    const char *path = g_object_get_data(G_OBJECT(mi), "path");
    FolderSort cur;
    sorting_get(path, &cur);
    sorting_set(path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "by")), cur.reverse);
    app_sorting_changed(w->app, path);
}

static void sort_reverse(GtkCheckMenuItem *mi, Win *w)
{
    const char *path = g_object_get_data(G_OBJECT(mi), "path");
    FolderSort cur;
    sorting_get(path, &cur);
    sorting_set(path, cur.by, gtk_check_menu_item_get_active(mi));
    app_sorting_changed(w->app, path);
}

static void sort_reset(GtkMenuItem *mi, Win *w)
{
    const char *path = g_object_get_data(G_OBJECT(mi), "path");
    sorting_clear(path);
    app_sorting_changed(w->app, path);
}

static void sort_menu(GtkWidget *menu, Win *w, GFile *folder)
{
    char *path = folder ? g_file_get_path(folder) : NULL;
    if (!path)
        return;
    FolderSort cur;
    gboolean custom = sorting_get(path, &cur);
    GtkWidget *sub = gtk_menu_new();
    static const char *labels[] = { "Name", "Date modified (newest first)", "Size (largest first)", "Type" };
    GSList *group = NULL;
    for (int b = 0; b < 4; b++) {
        GtkWidget *mi = gtk_radio_menu_item_new_with_label(group, labels[b]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(mi));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), (int) cur.by == b);
        g_object_set_data_full(G_OBJECT(mi), "path", g_strdup(path), g_free);
        g_object_set_data(G_OBJECT(mi), "by", GINT_TO_POINTER(b));
        g_signal_connect(mi, "toggled", G_CALLBACK(sort_pick), w);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
    }
    sep(sub);
    GtkWidget *rev = gtk_check_menu_item_new_with_label("Reversed");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(rev), cur.reverse);
    g_object_set_data_full(G_OBJECT(rev), "path", g_strdup(path), g_free);
    g_signal_connect(rev, "toggled", G_CALLBACK(sort_reverse), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), rev);
    sep(sub);
    GtkWidget *back = gtk_menu_item_new_with_label("Back to name");
    gtk_widget_set_sensitive(back, custom);
    g_object_set_data_full(G_OBJECT(back), "path", g_strdup(path), g_free);
    g_signal_connect(back, "activate", G_CALLBACK(sort_reset), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), back);
    char *base = g_file_get_basename(folder);
    char *l = g_strdup_printf("Sort “%s”", base);
    GtkWidget *top = gtk_menu_item_new_with_label(l);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    g_free(l);
    g_free(base);
    g_free(path);
}

void win_context_menu(Win *w, Column *c, GdkEventButton *ev)
{
    GList *files = column_selected_files(c);
    GFile *one = NULL;
    gboolean single_dir = FALSE;
    if (column_selected_single(c, &one, &single_dir))
        g_object_unref(one);
    else
        single_dir = FALSE;
    int n = g_list_length(files);
    GtkWidget *menu = gtk_menu_new();
    GFile *folder = single_dir ? files->data : c->gfile;

    if (files) {
        if (single_dir) {
            item(menu, w, c, files, NULL, A_OPEN_DIR, "Open", NULL, TRUE);
            item(menu, w, c, NULL, files->data, A_NEW_WINDOW, "Open in new window", NULL, TRUE);
        } else if (n == 1)
            item(menu, w, c, files, NULL, A_OPEN, "Open", "Return", TRUE);
        GFileInfo *info = g_file_query_info(files->data, "standard::content-type", G_FILE_QUERY_INFO_NONE,
                                            NULL, NULL);
        const char *ct = info ? g_file_info_get_content_type(info) : NULL;
        GList *apps = ct ? g_app_info_get_all_for_type(ct) : NULL;
        if (apps) {
            GtkWidget *sub = gtk_menu_new();
            for (GList *l = apps; l; l = l->next) {
                GtkWidget *mi = item(sub, w, c, files, NULL, A_OPEN_WITH,
                                     g_app_info_get_display_name(l->data), NULL, TRUE);
                Act *a = g_object_get_data(G_OBJECT(mi), "act");
                a->app = g_object_ref(l->data);
            }
            GtkWidget *ow = gtk_menu_item_new_with_label("Open with");
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(ow), sub);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), ow);
            g_list_free_full(apps, g_object_unref);
        }
        g_clear_object(&info);
        sep(menu);
        if (all_archives(files)) {
            item(menu, w, c, files, NULL, A_EXTRACT_HERE, "Extract here", NULL, TRUE);
            if (n == 1) {
                char *b = g_file_get_basename(files->data), *stem = arc_stem(b);
                char *l = g_strdup_printf("Extract to “%s/”", stem);
                item(menu, w, c, files, NULL, A_EXTRACT_FOLDER, l, NULL, TRUE);
                g_free(l);
                g_free(stem);
                g_free(b);
            } else {
                item(menu, w, c, files, NULL, A_EXTRACT_FOLDER, "Extract each to its own folder", NULL, TRUE);
            }
        }
        item(menu, w, c, files, NULL, A_COMPRESS, "Compress…", NULL, TRUE);
        sep(menu);
        item(menu, w, c, files, NULL, A_CUT, "Cut", "<Control>x", TRUE);
        item(menu, w, c, files, NULL, A_COPY, "Copy", "<Control>c", TRUE);
        if (single_dir) {   /* a highlighted folder gets the paste (Ctrl+V does the same) */
            char *b = g_file_get_basename(files->data);
            char *l = g_strdup_printf("Paste into “%s”", b);
            item(menu, w, c, files, NULL, A_PASTE_INTO, l, "<Control>v", TRUE);
            g_free(l);
            g_free(b);
        } else {
            item(menu, w, c, files, NULL, A_PASTE, "Paste", "<Control>v", TRUE);
        }
        sep(menu);
        item(menu, w, c, files, NULL, A_RENAME, "Rename…", "F2", n == 1);
        item(menu, w, c, files, NULL, A_TRASH, "Move to trash", "Delete", TRUE);
        item(menu, w, c, files, NULL, A_DELETE, "Delete permanently…", "<Shift>Delete", TRUE);
        sep(menu);
    } else {
        item(menu, w, c, NULL, NULL, A_PASTE, "Paste", "<Control>v", TRUE);
        sep(menu);
    }
    item(menu, w, c, NULL, NULL, A_NEW_FOLDER, "New folder…", "<Control><Shift>n", TRUE);
    sort_menu(menu, w, folder);       /* the highlighted folder, else this column's own */
    item(menu, w, c, NULL, folder, A_TERMINAL, "Open terminal here", NULL, TRUE);
    if (files)
        item(menu, w, c, files, NULL, A_COPY_PATH, "Copy path", NULL, TRUE);
    item(menu, w, c, NULL, folder, A_BOOKMARK, "Add to bookmarks", "<Control>d", TRUE);
    if (!is_root())
        item(menu, w, c, NULL, folder, A_ROOT, "Open as root…", NULL, TRUE);
    sep(menu);
    item(menu, w, c, files, NULL, A_PROPERTIES, "Properties…", "<Alt>Return", TRUE);
    sep(menu);
    GtkWidget *hid = gtk_check_menu_item_new_with_label("Show hidden files");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hid), w->show_hidden);
    g_signal_connect(hid, "toggled", G_CALLBACK(on_hidden_toggled), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), hid);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), c->view, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(menu_done), NULL);
    if (ev)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) ev);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), c->view, GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST, NULL);
    g_list_free_full(files, g_object_unref);
}

/* Menu for files that aren't rows in a column: thumbnails in the preview's gallery (one or several,
 * all from one folder). ev = the right-click, or NULL (menu key) to open it at the widget. */
void win_file_menu(Win *w, GList *files, GdkEventButton *ev, GtkWidget *attach)
{
    if (!files)
        return;
    GFile *f = files->data;
    gboolean one = !files->next;
    GFileInfo *info = g_file_query_info(f, "standard::content-type,standard::type", G_FILE_QUERY_INFO_NONE,
                                        NULL, NULL);
    gboolean isdir = info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    GtkWidget *menu = gtk_menu_new();
    if (one) {
        if (isdir)
            item(menu, w, NULL, files, NULL, A_SHOW, "Open", NULL, TRUE);
        else
            item(menu, w, NULL, files, NULL, A_OPEN, "Open", NULL, TRUE);
        const char *ct = info ? g_file_info_get_content_type(info) : NULL;
        GList *apps = ct && !isdir ? g_app_info_get_all_for_type(ct) : NULL;
        if (apps) {
            GtkWidget *sub = gtk_menu_new();
            for (GList *l = apps; l; l = l->next) {
                GtkWidget *mi = item(sub, w, NULL, files, NULL, A_OPEN_WITH,
                                     g_app_info_get_display_name(l->data), NULL, TRUE);
                Act *a = g_object_get_data(G_OBJECT(mi), "act");
                a->app = g_object_ref(l->data);
            }
            GtkWidget *ow = gtk_menu_item_new_with_label("Open with");
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(ow), sub);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), ow);
            g_list_free_full(apps, g_object_unref);
        }
        if (!isdir)
            item(menu, w, NULL, files, NULL, A_SHOW, "Show in column", NULL, TRUE);
        sep(menu);
    }
    g_clear_object(&info);
    if (all_archives(files)) {
        item(menu, w, NULL, files, NULL, A_EXTRACT_HERE, "Extract here", NULL, TRUE);
        if (one) {
            char *b = g_file_get_basename(f), *stem = arc_stem(b);
            char *l = g_strdup_printf("Extract to “%s/”", stem);
            item(menu, w, NULL, files, NULL, A_EXTRACT_FOLDER, l, NULL, TRUE);
            g_free(l);
            g_free(stem);
            g_free(b);
        } else {
            item(menu, w, NULL, files, NULL, A_EXTRACT_FOLDER, "Extract each to its own folder", NULL, TRUE);
        }
    }
    item(menu, w, NULL, files, NULL, A_COMPRESS, "Compress…", NULL, TRUE);
    sep(menu);
    item(menu, w, NULL, files, NULL, A_CUT, "Cut", "<Control>x", TRUE);
    item(menu, w, NULL, files, NULL, A_COPY, "Copy", "<Control>c", TRUE);
    sep(menu);
    item(menu, w, NULL, files, NULL, A_RENAME, "Rename…", "F2", one);
    item(menu, w, NULL, files, NULL, A_TRASH, "Move to trash", "Delete", TRUE);
    item(menu, w, NULL, files, NULL, A_DELETE, "Delete permanently…", "<Shift>Delete", TRUE);
    sep(menu);
    item(menu, w, NULL, files, NULL, A_COPY_PATH, one ? "Copy path" : "Copy paths", NULL, TRUE);
    item(menu, w, NULL, files, NULL, A_PROPERTIES, "Properties…", "<Alt>Return", one);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), attach, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(menu_done), NULL);
    if (ev)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) ev);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), attach, GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST, NULL);
}

/* a second window at the folder this one shows (Ctrl+N, Menu -> New window) */
static void new_window_here(Win *w)
{
    GFile *cur = win_current_folder(w);
    win_new(w->app, cur);
    g_object_unref(cur);
}

static void on_new_window(GtkMenuItem *mi, Win *w)
{
    (void) mi;
    new_window_here(w);
}

/* ---- keys -------------------------------------------------------------------------------- */
static gboolean on_key(GtkWidget *win, GdkEventKey *ev, Win *w)
{
    (void) win;
    gboolean ctrl = ev->state & GDK_CONTROL_MASK, alt = ev->state & GDK_MOD1_MASK;
    gboolean shift = ev->state & GDK_SHIFT_MASK;
    guint k = ev->keyval;
    if (ctrl && !shift && (k == GDK_KEY_n || k == GDK_KEY_N)) {
        new_window_here(w);
        return TRUE;
    }
    if (ctrl && (k == GDK_KEY_h || k == GDK_KEY_H)) {
        win_toggle_hidden(w);
        return TRUE;
    }
    if (ctrl && (k == GDK_KEY_l || k == GDK_KEY_L)) {
        gtk_widget_grab_focus(w->location);
        return TRUE;
    }
    if (alt && k == GDK_KEY_Up) {
        go_up(w);
        return TRUE;
    }
    if (alt && k == GDK_KEY_Home) {
        go_home(w);
        return TRUE;
    }
    if (alt && (k == GDK_KEY_slash || k == GDK_KEY_KP_Divide)) {
        go_root(w);
        return TRUE;
    }
    if (alt && (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter)) {
        Column *c = g_list_find(w->columns, w->focused) ? w->focused : NULL;
        GFile *f = NULL;
        if (c && column_selected_single(c, &f, NULL)) {
            dialog_properties(w, f);
        } else {
            GFile *cur = win_current_folder(w);
            dialog_properties(w, cur);
            g_object_unref(cur);
        }
        g_clear_object(&f);
        return TRUE;
    }
    if (ctrl && (k == GDK_KEY_d || k == GDK_KEY_D)) {
        GFile *cur = win_current_folder(w);
        places_bookmark_add(w, cur);
        g_object_unref(cur);
        return TRUE;
    }
    if (ctrl && k == GDK_KEY_comma) {
        dialog_settings(w);
        return TRUE;
    }
    if (k == GDK_KEY_BackSpace && !gtk_widget_has_focus(w->location)) {
        go_up(w);
        return TRUE;
    }
    if (ctrl && (k == GDK_KEY_q || k == GDK_KEY_Q || k == GDK_KEY_w || k == GDK_KEY_W)) {
        gtk_window_close(GTK_WINDOW(w->window));
        return TRUE;
    }
    return FALSE;
}

/* ---- construction -------------------------------------------------------------------------- */
static void on_destroy(GtkWidget *win, Win *w)
{
    (void) win;
    for (GList *l = w->jobs; l; l = l->next)
        job_cancel(l->data);
    w->dead = TRUE;
    for (GList *l = w->columns; l; l = l->next) {
        column_close(l->data);
        column_unref(l->data);
    }
    g_list_free(w->columns);
    w->columns = NULL;
    if (w->volumes)
        g_signal_handlers_disconnect_by_data(w->volumes, w);
    if (w->clip)
        gtk_clipboard_store(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    win_unref(w);
}

static GtkWidget *tool_button(const char *icon, const char *tip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

/* places menus are rebuilt BEFORE they pop up; filling them while already shown made GTK
 * open them at the old size, then resize and leave them scrolled */
static void places_clicked(GtkButton *b, Win *w)
{
    GtkWidget *menu = g_object_get_data(G_OBJECT(b), "bm") ? w->bm_menu : w->dev_menu;
    if (menu == w->bm_menu)
        places_fill_bookmarks(w);
    else
        places_fill_devices(w);
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST, NULL);
}

static GtkWidget *menu_button(GtkWidget *menu, const char *icon, const char *tip)
{
    GtkWidget *b = gtk_menu_button_new();
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(b), menu);
    gtk_container_add(GTK_CONTAINER(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

static void on_settings(GtkMenuItem *mi, Win *w) { (void) mi; dialog_settings(w); }
static void on_about(GtkMenuItem *mi, Win *w) { (void) mi; dialog_about(w); }
static void on_root_here(GtkMenuItem *mi, Win *w)
{
    (void) mi;
    GFile *cur = win_current_folder(w);
    win_open_as_root(w, cur);
    g_object_unref(cur);
}
static void on_up(GtkButton *b, Win *w) { (void) b; go_up(w); }
static void on_home(GtkButton *b, Win *w) { (void) b; go_home(w); }
static void on_root(GtkButton *b, Win *w) { (void) b; go_root(w); }
static void on_cancel_jobs(GtkButton *b, Win *w)
{
    (void) b;
    for (GList *l = w->jobs; l; l = l->next)
        job_cancel(l->data);
}

static void on_hadj_changed(GtkAdjustment *adj, Win *w)
{
    (void) adj;
    /* whenever the strip of columns changes width, keep the newest column in view */
    win_scroll_end(w);
}

Win *win_new(GtkApplication *app, GFile *start)
{
    if (!ATOM_GNOME) {
        ATOM_GNOME = gdk_atom_intern_static_string("x-special/gnome-copied-files");
        ATOM_URIS = gdk_atom_intern_static_string("text/uri-list");
    }
    Win *w = g_new0(Win, 1);
    w->ref = 1;
    w->app = app;
    w->show_hidden = LOOK.show_hidden;
    w->pv_start = -1;
    w->col_widths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    w->window = gtk_application_window_new(app);
    g_object_set_data(G_OBJECT(w->window), "millerx-win", w);
    gtk_window_set_title(GTK_WINDOW(w->window), "millerX");
    gtk_window_set_icon_name(GTK_WINDOW(w->window), "millerx");
    gtk_window_set_default_size(GTK_WINDOW(w->window), 1200, 700);
    gtk_window_set_position(GTK_WINDOW(w->window), GTK_WIN_POS_CENTER);   /* not wherever the WM likes */

    w->location = gtk_entry_new();
    gtk_widget_set_hexpand(w->location, TRUE);
    g_signal_connect(w->location, "activate", G_CALLBACK(on_location), w);
    GtkWidget *up = tool_button("go-up", "Parent folder (Backspace, Alt+Up)");
    g_signal_connect(up, "clicked", G_CALLBACK(on_up), w);
    GtkWidget *home = tool_button("go-home", "Home (Alt+Home)");
    g_signal_connect(home, "clicked", G_CALLBACK(on_home), w);
    GtkWidget *root = tool_button("computer", "File system / (Alt+/)");
    g_signal_connect(root, "clicked", G_CALLBACK(on_root), w);
    w->bm_menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(w->bm_menu), w->window, NULL);
    GtkWidget *bm = tool_button("user-bookmarks", "Bookmarks (Ctrl+D adds this folder)");
    g_object_set_data(G_OBJECT(bm), "bm", GINT_TO_POINTER(1));
    g_signal_connect(bm, "clicked", G_CALLBACK(places_clicked), w);
    w->dev_menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(w->dev_menu), w->window, NULL);
    GtkWidget *dev = tool_button("drive-removable-media", "Drives: mount, open, eject");
    g_signal_connect(dev, "clicked", G_CALLBACK(places_clicked), w);
    GtkWidget *main_menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label("New window");
    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(mi))), GDK_KEY_n, GDK_CONTROL_MASK);
    g_signal_connect(mi, "activate", G_CALLBACK(on_new_window), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), gtk_separator_menu_item_new());
    if (!is_root()) {
        mi = gtk_menu_item_new_with_label("Open this folder as root…");
        g_signal_connect(mi, "activate", G_CALLBACK(on_root_here), w);
        gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
        gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), gtk_separator_menu_item_new());
    }
    mi = gtk_menu_item_new_with_label("Settings…");
    g_signal_connect(mi, "activate", G_CALLBACK(on_settings), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
    mi = gtk_menu_item_new_with_label("About millerX");
    g_signal_connect(mi, "activate", G_CALLBACK(on_about), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
    gtk_widget_show_all(main_menu);
    GtkWidget *menu_btn = menu_button(main_menu, "open-menu", "Menu");

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_object_set(bar, "margin", 4, NULL);
    GtkWidget *btns[] = { up, home, root, bm, dev };
    for (int i = 0; i < 5; i++)
        gtk_box_pack_start(GTK_BOX(bar), btns[i], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), w->location, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bar), menu_btn, FALSE, FALSE, 0);

    w->colbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    w->hscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w->hscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    /* the columns take what they need; the preview gets the rest of the window */
    gtk_widget_set_hexpand(w->hscroll, FALSE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(w->hscroll), TRUE);
    gtk_container_add(GTK_CONTAINER(w->hscroll), w->colbox);
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(w->hscroll)), "changed",
                     G_CALLBACK(on_hadj_changed), w);
    w->preview = preview_new(w);
    w->pane_divider = divider_new(preview_drag, preview_reset, preview_drag_done, w, FALSE);
    GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(pane), w->hscroll, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pane), w->pane_divider, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pane), w->preview->box, TRUE, TRUE, 0);

    w->statusbar = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(w->statusbar), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->statusbar), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(w->statusbar, TRUE);
    w->progress = gtk_progress_bar_new();
    gtk_widget_set_valign(w->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(w->progress, 160, -1);
    w->cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(w->cancel_btn, "clicked", G_CALLBACK(on_cancel_jobs), w);
    GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_object_set(sbox, "margin", 4, NULL);
    gtk_box_pack_start(GTK_BOX(sbox), w->statusbar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sbox), w->progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sbox), w->cancel_btn, FALSE, FALSE, 0);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (is_root()) {
        GtkWidget *ib = gtk_info_bar_new();
        gtk_info_bar_set_message_type(GTK_INFO_BAR(ib), GTK_MESSAGE_ERROR);
        GtkWidget *l = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(l), "<b>You are root:</b> every change here is made as administrator. "
                                           "Be careful.");
        gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(ib))), l);
        gtk_box_pack_start(GTK_BOX(box), ib, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), pane, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), sbox, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(w->window), box);
    g_signal_connect(w->window, "key-press-event", G_CALLBACK(on_key), w);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_destroy), w);

    places_watch_volumes(w);

    gtk_widget_show_all(w->window);
    gtk_widget_hide(w->progress);
    gtk_widget_hide(w->cancel_btn);
    preview_clear(w->preview);
    win_open_path(w, start);
    return w;
}

/* settings dialog changed something: take it over and show it in every window */
void app_apply_look(GtkApplication *app, const Look *nl)
{
    gboolean resort = nl->folders_first != LOOK.folders_first;
    gboolean names = nl->gallery_names != LOOK.gallery_names;
    char *col = g_strdup(nl->divider_color), *hl = g_strdup(nl->highlight);
    gboolean hl_changed = g_strcmp0(nl->highlight, LOOK.highlight);
    g_free(LOOK.divider_color);
    g_free(LOOK.highlight);
    LOOK = *nl;
    LOOK.divider_color = col;
    LOOK.highlight = hl;
    if (hl_changed)
        highlight_apply();           /* live: the open windows change at once */
    for (GList *l = gtk_application_get_windows(app); l; l = l->next) {
        Win *w = g_object_get_data(G_OBJECT(l->data), "millerx-win");
        if (w && !w->dead) {
            win_look_changed(w, resort);
            if (hl_changed) {
                /* tree views keep the look of rows that were already selected: restyle each */
                gtk_widget_reset_style(w->window);
                for (GList *k = w->columns; k; k = k->next) {
                    Column *c = k->data;
                    gtk_widget_reset_style(c->view);
                    gtk_widget_queue_draw(c->view);
                }
                gtk_widget_reset_style(w->preview->grid);
                gtk_widget_queue_draw(w->window);
            }
            if (names && w->preview->current) {      /* gallery tiles with or without names */
                GFile *f = g_object_ref(w->preview->current);
                preview_show_file(w->preview, f);
                g_object_unref(f);
            }
        }
    }
}
