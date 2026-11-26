/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* The window: toolbar, the strip of columns, preview, status bar, actions, clipboard, menu. */
#include "millerx.h"
#include <string.h>
#include <unistd.h>
#include <gdk/gdkx.h>
#include <gio/gdesktopappinfo.h>

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
 * window, millerX never sees it. Needs millerX's polkit policy (installed as MILLERX_POLICY)
 * and a polkit authentication agent in the session. */
#define ROOT_POLICY MILLERX_POLICY

/* Is the policy where polkit looks? Newer polkit reads all of these (a copy in /etc wins, so a
 * changed one survives updates); older ones only /usr/share. Installed elsewhere (a build into
 * /usr/local) it is still found. */
static gboolean root_policy_found(void)
{
    static const char *const dirs[] = { "/etc/polkit-1/actions", "/run/polkit-1/actions",
                                        "/usr/local/share/polkit-1/actions", "/usr/share/polkit-1/actions" };
    if (g_file_test(ROOT_POLICY, G_FILE_TEST_EXISTS))
        return TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        char *p = g_build_filename(dirs[i], "org.mix.millerx.policy", NULL);
        gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);
        g_free(p);
        if (ok)
            return TRUE;
    }
    return FALSE;
}

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
            win_error(w, _("Can't open as root"),
                      _("No polkit authentication agent is running, so nothing can ask for the "
                      "root password.\n\nStart one in your session, for example polkit-gnome "
                      "(/usr/libexec/polkit-gnome-authentication-agent-1), lxpolkit, mate-polkit "
                      "or polkit-kde-agent, and try again."));
        } else if (code == 126) {
            win_status(w, _("Open as root: cancelled"));
        } else if (code == 127) {
            win_error(w, _("Can't open as root"), err_out && *err_out ? err_out : _("Not authorized."));
        } else if (code != 0 && err_out && strstr(err_out, "cannot open display")) {
            char *m = g_strdup_printf(_("The root window couldn't reach the display. "
                                        "Is millerX's polkit policy installed (%s)?"), ROOT_POLICY);
            win_error(w, _("Can't open as root"), m);
            g_free(m);
        } else {
            win_status(w, _("Root window closed"));
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
        win_error(w, _("Can't open as root"),
                  _("This needs polkit (the pkexec program), which isn't installed. "
                  "Install polkit and a polkit authentication agent, then try again."));
    } else if (!root_policy_found()) {
        char *m = g_strdup_printf(_("millerX's polkit policy isn't installed (%s). "
                                    "Install millerX as a package or with “ninja install”."), ROOT_POLICY);
        win_error(w, _("Can't open as root"), m);
        g_free(m);
    } else if (!path) {
        win_error(w, _("Can't open as root"), _("Only local folders can be opened as root."));
    } else {
        GError *err = NULL;
        GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                          &err, pkexec, exe, path, NULL);
        if (!p) {
            win_error(w, _("Can't open as root"), err->message);
            g_error_free(err);
        } else {
            win_status(w, _("Asking for the root password …"));
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
    if (!folder || is_root() || !LOOK.use_pkexec) {
        win_error(w, title, text);
        return;
    }
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                          GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), _("%s\n\nThis needs root rights."), text);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Close"), GTK_RESPONSE_CLOSE);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Open as root…"), GTK_RESPONSE_ACCEPT);
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
    gtk_dialog_add_button(GTK_DIALOG(d), _("Cancel"), GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), ok, GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_OK;
}

char *win_ask_name(Win *w, const char *title, const char *initial, gboolean select_stem)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), GTK_DIALOG_MODAL,
                                               _("Cancel"), GTK_RESPONSE_CANCEL, _("OK"), GTK_RESPONSE_OK, NULL);
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

/* The location bar's text. GVfs keeps trash and network addresses escaped (trash:///draft%20invoice.pdf):
 * decode them for reading, except what would change the meaning (% / ? #, control characters).
 * on_location reads it back with g_file_parse_name, which escapes again. */
static char *location_text(GFile *f)
{
    char *s = g_file_get_parse_name(f);
    if (!strstr(s, "://"))
        return s;
    GString *o = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        if (p[0] == '%' && g_ascii_isxdigit(p[1]) && g_ascii_isxdigit(p[2])) {
            int c = g_ascii_xdigit_value(p[1]) * 16 + g_ascii_xdigit_value(p[2]);
            if (c >= 0x20 && c != 0x7f && !strchr("%/?#", c)) {
                g_string_append_c(o, (char) c);
                p += 2;
                continue;
            }
        }
        g_string_append_c(o, *p);
    }
    if (!g_utf8_validate(o->str, -1, NULL)) {       /* not text after all: as it was */
        g_string_free(o, TRUE);
        return s;
    }
    g_free(s);
    return g_string_free(o, FALSE);
}

static void update_location(Win *w, GFile *f)
{
    if (file_is_computer(f)) {
        gtk_entry_set_text(GTK_ENTRY(w->location), "computer:///");
        gtk_window_set_title(GTK_WINDOW(w->window), is_root() ? _("ROOT: Computer") : _("Computer"));
        return;
    }
    char *p = file_path(f);
    char *shown = location_text(f);            /* "trash:///draft invoice.pdf", not "…draft%20invoice.pdf" */
    gtk_entry_set_text(GTK_ENTRY(w->location), shown);
    g_free(shown);
    /* the title is just the folder's name, like Thunar's: in the panel the full paths were all
     * cut to the same "/home/…" (the location bar has the whole path, the yellow M says millerX) */
    char *name;
    char *uri = g_file_get_uri(f);
    if (!strcmp(uri, "trash:///") || !strcmp(uri, "trash:"))
        name = g_strdup(_("Trash"));
    else {
        char *b = g_file_get_basename(f);
        name = b && strcmp(b, "/") ? g_filename_display_name(b) : g_strdup(p);
        g_free(b);
    }
    char *t = is_root() ? g_strdup_printf(_("ROOT: %s"), name) : g_strdup(name);
    gtk_window_set_title(GTK_WINDOW(w->window), t);
    g_free(t);
    g_free(name);
    g_free(uri);
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
    column_set_width(data, LOOK.column_width);
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
    search_close(w);                 /* going somewhere else ends a search */
    if (file_is_computer(f)) {
        win_open_computer(w);
        return;
    }
    GFile *keep = g_object_ref(f);     /* f may belong to a column we're about to drop */
    char *path = g_file_get_path(keep);
    clear_columns(w);
    GFile *up = !path && !is_dir(keep, TRUE) ? g_file_get_parent(keep) : NULL;
    if (up) {                          /* a file in the trash or on a server: its folder, it selected */
        GFileInfo *fi = g_file_query_info(keep, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
        GPtrArray *chain = g_ptr_array_new_with_free_func(g_free);
        if (fi)
            g_ptr_array_add(chain, g_strdup(g_file_info_get_display_name(fi)));
        add_column(w, up, chain->len ? chain : NULL);
        g_ptr_array_unref(chain);
        g_clear_object(&fi);
        g_object_unref(up);
    } else if (!path) {
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

/* the Computer column alone: disks, removable media, network */
void win_open_computer(Win *w)
{
    clear_columns(w);
    GFile *f = g_file_new_for_uri("computer:///");
    Column *c = add_column(w, f, NULL);
    g_object_unref(f);
    update_location(w, c->gfile);
    gtk_widget_grab_focus(c->view);
    win_status(w, _("Computer: click a disk to open it, right-click to eject"));
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
        win_status(w, ngettext("%d item selected", "%d items selected", n), n);
        g_list_free_full(files, g_object_unref);
        return;
    }
    GFile *f;
    gboolean isdir;
    column_selected_single(c, &f, &isdir);
    if (c->computer) {             /* a disk: its folders to the right, the disk in the preview */
        if (f) {
            update_location(w, f);
            add_column(w, f, NULL);
        } else {
            update_location(w, c->gfile);
        }
        computer_preview(c);
        g_clear_object(&f);
        idle_scroll_end(w);
        return;
    }
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
    win_open_computer(w);
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
    /* an address as the bar shows it (trash:///a b) is read back the same way */
    GFile *f = strstr(exp, "://") ? g_file_parse_name(exp) : g_file_new_for_commandline_arg(exp);
    if (file_is_computer(f) || g_file_query_exists(f, NULL))
        win_open_path(w, f);
    else
        win_status(w, _("Not found: %s"), t);
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
    win_status(w, w->show_hidden ? _("Hidden files shown") : _("Hidden files hidden"));
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
        /* columns not dragged by hand follow the column width setting */
        char *uri = g_file_get_uri(c->gfile);
        if (!g_hash_table_contains(w->col_widths, uri) && c->width != LOOK.column_width) {
            c->width = LOOK.column_width;
            gtk_widget_set_size_request(c->scroller, c->width, -1);
        }
        g_free(uri);
    }
    gtk_widget_set_size_request(w->pane_divider, dw, -1);
    if (w->root_mi) {                         /* "Open as root" switched off in the settings */
        gtk_widget_set_visible(w->root_mi, LOOK.use_pkexec);
        gtk_widget_set_visible(w->root_sep, LOOK.use_pkexec);
    }
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
/* a program started by us, with the session's language (see child_envp) */
static gboolean spawn_child(const char *dir, char **argv, GSpawnFlags flags, GError **err)
{
    char **env = child_envp();
    gboolean ok = g_spawn_async(dir, argv, env, flags, NULL, NULL, NULL, err);
    g_strfreev(env);
    return ok;
}

static void open_with(Win *w, GAppInfo *app, GList *files);

static void open_by_type(Win *w, GFile *f)
{
    if (LOOK.detect == DETECT_CONTENT) {       /* the program for what it really is, not its name */
        GFileInfo *fi = mx_query_info(f, "standard::content-type", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        const char *ct = fi ? g_file_info_get_content_type(fi) : NULL;
        GAppInfo *app = ct ? g_app_info_get_default_for_type(ct, FALSE) : NULL;
        g_clear_object(&fi);
        if (app) {
            GList *files = g_list_append(NULL, f);
            open_with(w, app, files);
            g_list_free(files);
            g_object_unref(app);
            return;
        }
    }
    char *uri = g_file_get_uri(f);
    GError *err = NULL;
    GAppLaunchContext *ctx = child_launch_context();
    gboolean ok = g_app_info_launch_default_for_uri(uri, ctx, &err);
    g_object_unref(ctx);
    if (!ok) {
        win_status(w, "%s", err->message);
        g_error_free(err);
    }
    g_free(uri);
}

static void run_in_terminal(Win *w, const char *path);
static void run_program(Win *w, const char *path);

/* What a file you may execute really is, by its first bytes. The x bit alone says little:
 * on FAT and NTFS sticks every file has it. */
enum { EXEC_NO, EXEC_PROGRAM, EXEC_SCRIPT };
static int exec_kind(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR) || access(path, X_OK))
        return EXEC_NO;
    unsigned char head[4] = { 0 };
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return EXEC_NO;
    size_t n = fread(head, 1, sizeof head, fp);
    fclose(fp);
    if (n >= 4 && !memcmp(head, "\177ELF", 4))
        return EXEC_PROGRAM;
    if (n >= 2 && head[0] == '#' && head[1] == '!')
        return EXEC_SCRIPT;
    return EXEC_NO;
}

/* a script: run it blindly, or read it first? */
static void ask_script(Win *w, const char *path)
{
    char *base = g_path_get_basename(path);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, _("“%s” is a script"), base);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             _("Run it, or open it to read what it does?"));
    gtk_dialog_add_button(GTK_DIALOG(d), _("Cancel"), 0);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Open"), 1);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Run in terminal"), 2);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Run"), 3);
    gtk_dialog_set_default_response(GTK_DIALOG(d), 1);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(base);
    if (r == 1) {
        GFile *f = g_file_new_for_path(path);
        open_by_type(w, f);
        g_object_unref(f);
    } else if (r == 2) {
        run_in_terminal(w, path);
    } else if (r == 3) {
        run_program(w, path);
    }
}

/* A program (ELF): never started without asking. Its name can say "holiday.jpg" and the x bit
 * says nothing (FAT sticks, archives), so the dialog says what it really is; Cancel is the default. */
static void ask_program(Win *w, const char *path)
{
    char *base = g_path_get_basename(path);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE, _("“%s” is a program"), base);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        _("Whatever its name says, this file is a program. It runs with your rights and can do "
          "anything you can. Run it only if you trust where it came from."));
    gtk_dialog_add_button(GTK_DIALOG(d), _("Cancel"), 0);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Run"), 1);
    gtk_dialog_set_default_response(GTK_DIALOG(d), 0);
    win_ref(w);                                 /* the window may be closed behind the dialog */
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(base);
    if (r == 1 && !w->dead)
        run_program(w, path);
    win_unref(w);
}

/* the file in the program for plain text, whatever its own type: to read a launcher */
static void open_as_text(Win *w, GFile *f)
{
    GAppInfo *app = g_app_info_get_default_for_type("text/plain", FALSE);
    if (!app) {
        win_status(w, "%s", _("No program for text files is set."));
        return;
    }
    GList *files = g_list_append(NULL, f);
    open_with(w, app, files);
    g_list_free(files);
    g_object_unref(app);
}

/* A launcher (.desktop) with the x bit: its Name= and Icon= can look like anything, and the x bit
 * survives extraction, so it says what it would run and asks. Open (as text) is the default. */
static void ask_launcher(Win *w, GFile *f, GDesktopAppInfo *app)
{
    char *base = g_file_get_basename(f);
    const char *name = g_app_info_get_name(G_APP_INFO(app));
    const char *cmd = g_app_info_get_commandline(G_APP_INFO(app));
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE, _("“%s” is a launcher"), base);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        _("It calls itself “%s” and runs:\n\n%s\n\nRun it only if you trust where it came from."),
        name ? name : "", cmd ? cmd : _("(no command given)"));
    gtk_dialog_add_button(GTK_DIALOG(d), _("Cancel"), 0);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Open as text"), 1);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Run"), 2);
    gtk_dialog_set_default_response(GTK_DIALOG(d), 1);
    win_ref(w);                                 /* the window may be closed behind the dialog */
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(base);
    if (!w->dead && r == 1)
        open_as_text(w, f);
    else if (!w->dead && r == 2)
        open_with(w, G_APP_INFO(app), NULL);
    win_unref(w);
}

/* A double-click: nothing runs without asking. A program or a launcher (.desktop with the x bit)
 * says what it is and asks, a script asks too; everything else opens in the program for its type. */
void win_open_default(Win *w, GFile *f)
{
    char *path = g_file_get_path(f);            /* NULL in sftp:// and other remote places */
    if (path && g_str_has_suffix(path, ".desktop") && !access(path, X_OK)) {
        /* only a launcher with the x bit, like Thunar: one you just downloaded opens as text */
        GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(path);
        if (app) {
            g_object_ref(f);                    /* the dialog's main loop may drop the row */
            ask_launcher(w, f, app);
            g_object_unref(f);
            g_object_unref(app);
            g_free(path);
            return;
        }
    }
    int kind = path ? exec_kind(path) : EXEC_NO;
    if (kind == EXEC_PROGRAM)
        ask_program(w, path);
    else if (kind == EXEC_SCRIPT)
        ask_script(w, path);
    else
        open_by_type(w, f);
    g_free(path);
}

static void open_with(Win *w, GAppInfo *app, GList *files)
{
    GError *err = NULL;
    GAppLaunchContext *ctx = child_launch_context();
    gboolean ok = g_app_info_launch(app, files, ctx, &err);
    g_object_unref(ctx);
    if (!ok) {
        win_status(w, "%s", err->message);
        g_error_free(err);
    }
}

/* Known terminals, how to tell them the folder, and how to have them run a command (NULL: we
 * don't know that one's way, so it isn't used for running). All of them are also started *in*
 * the folder, so the ones without a folder flag open there too. */
static const struct { const char *exe, *flag, *exec; } TERMINALS[] = {
    { "xfce4-terminal", "--working-directory=", "-x" },
    { "x-terminal-emulator", NULL, "-e" },    /* Debian's alternatives link */
    { "konsole", "--workdir", "-e" },
    { "gnome-terminal", "--working-directory=", "--" },
    { "kgx", "--working-directory=", NULL },  /* GNOME Console */
    { "ptyxis", "--working-directory=", NULL },
    { "mate-terminal", "--working-directory=", "-x" },
    { "lxterminal", "--working-directory=", NULL },
    { "qterminal", "-w", NULL },
    { "tilix", "--working-directory=", NULL },
    { "terminator", "--working-directory=", "-x" },
    { "alacritty", "--working-directory", "-e" },
    { "kitty", "--directory", "" },           /* "": the command simply follows */
    { "foot", "--working-directory=", "" },
    { "wezterm", NULL, NULL },
    { "urxvt", NULL, "-e" },
    { "st", NULL, "-e" },
    { "xterm", NULL, "-e" },
};

/* a program, started in its own folder */
static void run_program(Win *w, const char *path)
{
    char *dir = g_path_get_dirname(path);
    char *argv[] = { (char *) path, NULL };
    GError *err = NULL;
    if (!spawn_child(dir, argv, G_SPAWN_DEFAULT, &err)) {
        win_error(w, _("Couldn't run it"), err->message);
        g_error_free(err);
    }
    g_free(dir);
}

/* A script in a terminal, which then waits for Enter, so you can read what it printed.
 * $TERMINAL is taken to understand -e, like xterm. */
static void run_in_terminal(Win *w, const char *path)
{
    char *dir = g_path_get_dirname(path);
    const char *wait = "\"$0\"; echo; printf '[finished: press Enter]'; read x";
    gboolean ok = FALSE;
    const char *env = g_getenv("TERMINAL");
    if (env && *env) {
        char **tv = NULL;
        if (g_shell_parse_argv(env, NULL, &tv, NULL)) {
            GPtrArray *a = g_ptr_array_new();
            for (int i = 0; tv[i]; i++)
                g_ptr_array_add(a, tv[i]);
            const char *rest[] = { "-e", "/bin/sh", "-c", wait, path };
            for (guint i = 0; i < G_N_ELEMENTS(rest); i++)
                g_ptr_array_add(a, (char *) rest[i]);
            g_ptr_array_add(a, NULL);
            ok = spawn_child(dir, (char **) a->pdata, G_SPAWN_SEARCH_PATH, NULL);
            g_ptr_array_free(a, TRUE);
        }
        g_strfreev(tv);
    }
    for (guint i = 0; !ok && i < G_N_ELEMENTS(TERMINALS); i++) {
        if (!TERMINALS[i].exec)
            continue;
        char *exe = g_find_program_in_path(TERMINALS[i].exe);
        if (!exe)
            continue;
        GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(a, exe);
        const char *flag = TERMINALS[i].flag;
        if (flag && g_str_has_suffix(flag, "=")) {
            g_ptr_array_add(a, g_strconcat(flag, dir, NULL));
        } else if (flag) {
            g_ptr_array_add(a, g_strdup(flag));
            g_ptr_array_add(a, g_strdup(dir));
        }
        if (*TERMINALS[i].exec)
            g_ptr_array_add(a, g_strdup(TERMINALS[i].exec));
        const char *rest[] = { "/bin/sh", "-c", wait, path };
        for (guint k = 0; k < G_N_ELEMENTS(rest); k++)
            g_ptr_array_add(a, g_strdup(rest[k]));
        g_ptr_array_add(a, NULL);
        ok = spawn_child(dir, (char **) a->pdata, G_SPAWN_DEFAULT, NULL);
        g_ptr_array_free(a, TRUE);
    }
    if (!ok)
        win_error(w, _("No terminal found"),
                  _("Install a terminal (xterm, xfce4-terminal, konsole, …) or set the TERMINAL "
                  "environment variable to the one you use."));
    g_free(dir);
}

/* Open with → Check file type: a terminal in the file's folder runs `file` on it, so you see
 * what the file really is, whatever its name says. Its own function on purpose: run_in_terminal
 * (scripts) stays as it is; both use the same TERMINALS list. The name goes in as an argument
 * ("$0"), never into the shell text, so odd names can't do anything. */
static void check_file_type(Win *w, GFile *f)
{
    char *path = g_file_get_path(f);
    if (!path)
        return;
    char *dir = g_path_get_dirname(path), *name = g_path_get_basename(path);
    const char *script = "file -- \"$0\"; echo; printf '[finished: press Enter]'; read x";
    gboolean ok = FALSE;
    const char *env = g_getenv("TERMINAL");
    if (env && *env) {
        char **tv = NULL;
        if (g_shell_parse_argv(env, NULL, &tv, NULL)) {
            GPtrArray *a = g_ptr_array_new();
            for (int i = 0; tv[i]; i++)
                g_ptr_array_add(a, tv[i]);
            const char *rest[] = { "-e", "/bin/sh", "-c", script, name };
            for (guint i = 0; i < G_N_ELEMENTS(rest); i++)
                g_ptr_array_add(a, (char *) rest[i]);
            g_ptr_array_add(a, NULL);
            ok = spawn_child(dir, (char **) a->pdata, G_SPAWN_SEARCH_PATH, NULL);
            g_ptr_array_free(a, TRUE);
        }
        g_strfreev(tv);
    }
    for (guint i = 0; !ok && i < G_N_ELEMENTS(TERMINALS); i++) {
        if (!TERMINALS[i].exec)
            continue;
        char *exe = g_find_program_in_path(TERMINALS[i].exe);
        if (!exe)
            continue;
        GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(a, exe);
        const char *flag = TERMINALS[i].flag;
        if (flag && g_str_has_suffix(flag, "=")) {
            g_ptr_array_add(a, g_strconcat(flag, dir, NULL));
        } else if (flag) {
            g_ptr_array_add(a, g_strdup(flag));
            g_ptr_array_add(a, g_strdup(dir));
        }
        if (*TERMINALS[i].exec)
            g_ptr_array_add(a, g_strdup(TERMINALS[i].exec));
        const char *rest[] = { "/bin/sh", "-c", script, name };
        for (guint k = 0; k < G_N_ELEMENTS(rest); k++)
            g_ptr_array_add(a, g_strdup(rest[k]));
        g_ptr_array_add(a, NULL);
        ok = spawn_child(dir, (char **) a->pdata, G_SPAWN_DEFAULT, NULL);
        g_ptr_array_free(a, TRUE);
    }
    if (!ok)
        win_error(w, _("No terminal found"),
                  _("Install a terminal (xterm, xfce4-terminal, konsole, …) or set the TERMINAL "
                  "environment variable to the one you use."));
    g_free(name);
    g_free(dir);
    g_free(path);
}

/* Open with → Other application…: GTK's own chooser, every installed program; opens the file
 * once with the one picked (the default stays: that is "Set default") */
static void other_application(Win *w, GList *files)
{
    GtkWidget *d = gtk_app_chooser_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, files->data);
    gtk_app_chooser_widget_set_show_other(
        GTK_APP_CHOOSER_WIDGET(gtk_app_chooser_dialog_get_widget(GTK_APP_CHOOSER_DIALOG(d))), TRUE);
    GList *keep = g_list_copy_deep(files, obj_copy, NULL);
    GAppInfo *app = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK ? gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d))
                                                                     : NULL;
    gtk_widget_destroy(d);
    if (app && !w->dead)
        open_with(w, app, keep);
    g_clear_object(&app);
    g_list_free_full(keep, g_object_unref);
}

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
            ok = spawn_child(path, argv, G_SPAWN_SEARCH_PATH, NULL);
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
        ok = spawn_child(path, argv, G_SPAWN_DEFAULT, NULL);
        g_free(a1);
        g_free(a2);
        g_free(exe);
    }
    if (!ok)
        win_error(w, _("No terminal found"),
                  _("Install a terminal (xterm, xfce4-terminal, konsole, …) or set the TERMINAL "
                  "environment variable to the one you use."));
    g_free(path);
}

void win_new_folder(Win *w, Column *c)
{
    char *name = win_ask_name(w, _("New folder"), _("New Folder"), FALSE);
    if (!name)
        return;
    GFile *f = g_file_get_child(c->gfile, name);
    GError *err = NULL;
    if (!g_file_make_directory(f, NULL, &err)) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
            win_error_root(w, _("Couldn't create the folder"), err->message, c->gfile);
        else
            win_error(w, _("Couldn't create the folder"), err->message);
        g_error_free(err);
    } else {
        g_free(c->pending_select);
        c->pending_select = g_strdup(name);
        column_add_file(c, f);
    }
    g_object_unref(f);
    g_free(name);
}

/* an empty file, named by the user, in the column's folder */
void win_new_file(Win *w, Column *c)
{
    char *name = win_ask_name(w, _("New file"), _("New File"), FALSE);
    if (!name)
        return;
    GFile *f = g_file_get_child(c->gfile, name);
    GError *err = NULL;
    GFileOutputStream *out = g_file_create(f, G_FILE_CREATE_NONE, NULL, &err);
    if (!out) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
            win_error_root(w, _("Couldn't create the file"), err->message, c->gfile);
        else
            win_error(w, _("Couldn't create the file"), err->message);
        g_error_free(err);
    } else {
        g_output_stream_close(G_OUTPUT_STREAM(out), NULL, NULL);
        g_object_unref(out);
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
    char *name = win_ask_name(w, _("Rename"), old, TRUE);
    if (name && strcmp(name, old)) {
        GFile *parent = g_file_get_parent(f), *exist = g_file_get_child(parent, name);
        if (g_file_query_exists(exist, NULL)) {
            char *msg = g_strdup_printf(_("“%s” already exists."), name);
            win_error(w, _("Couldn't rename"), msg);
            g_free(msg);
        } else {
            GError *err = NULL;
            GFile *nf = g_file_set_display_name(f, name, NULL, &err);
            if (!nf) {
                win_error_root(w, _("Couldn't rename"), err->message,
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
            win_error_root(d->w, _("Some items couldn't be deleted"), s->str, d->denied_in);
        g_string_free(s, TRUE);
    } else {
        guint n = g_list_length(d->files);
        win_status(d->w, ngettext("%u item deleted", "%u items deleted", n), n);
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
        /* the trash takes a trashed folder only as a whole (it refuses its contents one by one) */
        gboolean ok = file_in_trash(l->data) ? g_file_delete(l->data, NULL, &err) : delete_tree(l->data, NULL, &err);
        if (!ok) {
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
        win_error_root(w, _("Can't move to trash"), msg, parent);
        g_clear_object(&parent);
        g_free(msg);
        g_free(names);
        g_list_free_full(failed, g_object_unref);
    } else if (failed) {
        char *names = some_names(failed, 5);
        char *msg = g_strdup_printf(_("%s: %s\n\nDelete permanently instead?"), names, first_err);
        if (win_confirm(w, _("Can't move to trash"), msg, _("Delete permanently")))
            delete_files(w, failed);
        g_free(msg);
        g_free(names);
        g_list_free_full(failed, g_object_unref);
    } else {
        guint n = g_list_length(files);
        win_status(w, ngettext("%u item moved to the trash", "%u items moved to the trash", n), n);
    }
    g_free(first_err);
}

void win_delete_forever(Win *w, GList *files)
{
    if (!files)
        return;
    char *names = some_names(files, 5);
    guint n = g_list_length(files);
    char *title = g_strdup_printf(ngettext("Delete %u item permanently?", "Delete %u items permanently?", n), n);
    char *msg = g_strdup_printf(_("%s\n\nThis can't be undone."), names);
    if (win_confirm(w, title, msg, _("Delete")))
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
        guint n = g_list_length(files);
        if (cut)
            win_status(w, ngettext("%u item cut", "%u items cut", n), n);
        else
            win_status(w, ngettext("%u item copied", "%u items copied", n), n);
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
        win_status(w, _("Nothing to paste"));
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
       A_DELETE, A_NEW_FOLDER, A_NEW_FILE, A_TERMINAL, A_COPY_PATH, A_BOOKMARK, A_PROPERTIES, A_ROOT, A_SHOW,
       A_NEW_WINDOW, A_EXTRACT_HERE, A_EXTRACT_TO, A_COMPRESS, A_SET_DEFAULT, A_CHECKSUMS, A_COMPARE,
       A_OTHER_APP, A_FILE_TYPE };

/* ---- archives ------------------------------------------------------------------------------ */
/* "Extract to…": a folder chooser, starting where the archives are; they all go into that folder */
static void extract_to_dialog(Win *w, GList *files)
{
    GList *archives = g_list_copy_deep(files, obj_copy, NULL);
    GtkWidget *d = gtk_file_chooser_dialog_new(_("Extract to"), GTK_WINDOW(w->window),
                                               GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("Cancel"),
                                               GTK_RESPONSE_CANCEL, _("Extract"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GFile *parent = g_file_get_parent(archives->data);
    if (parent)
        gtk_file_chooser_set_current_folder_file(GTK_FILE_CHOOSER(d), parent, NULL);
    g_clear_object(&parent);
    GFile *to = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d))
                                                                     : NULL;
    gtk_widget_destroy(d);
    if (to && !w->dead)
        job_extract(w, archives, to);
    g_clear_object(&to);
    g_list_free_full(archives, g_object_unref);
}

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
    GtkWidget *d = gtk_dialog_new_with_buttons(_("Compress"), GTK_WINDOW(w->window), GTK_DIALOG_MODAL,
                                               _("Cancel"), GTK_RESPONSE_CANCEL, _("Compress"), GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    g_object_set(grid, "margin", 12, NULL);
    char *what = files->next ? g_strdup_printf(ngettext("%u item", "%u items", g_list_length(files)), g_list_length(files)) : g_file_get_basename(files->data);
    char *head = g_strdup_printf(_("Pack “%s” into an archive in the same folder:"), what);
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
            char *msg = g_strdup_printf(_("“%s” already exists. Pick another name."), full);
            win_error(w, _("Couldn't compress"), msg);
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

/* the program a click opens this kind of file with, from now on: ~/.config/mimeapps.list, which
 * Thunar, file dialogs and the rest read too */
static void set_default_app(Win *w, GAppInfo *app, GFile *f)
{
    GFileInfo *fi = mx_query_info(f, "standard::content-type", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    const char *ct = fi ? g_file_info_get_content_type(fi) : NULL;
    GError *err = NULL;
    if (!ct)
        win_status(w, _("Couldn't tell what kind of file this is"));
    else if (!g_app_info_set_as_default_for_type(app, ct, &err)) {
        win_error(w, _("Couldn't set the default program"), err->message);
        g_error_free(err);
    } else {
        char *desc = g_content_type_get_description(ct);
        win_status(w, _("%s opens with %s now"), desc, g_app_info_get_display_name(app));
        g_free(desc);
    }
    g_clear_object(&fi);
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
    case A_OTHER_APP: other_application(w, a->files); break;
    case A_FILE_TYPE: check_file_type(w, a->files->data); break;
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
    case A_NEW_FILE: win_new_file(w, c); break;
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
    case A_EXTRACT_HERE: job_extract(w, a->files, NULL); break;
    case A_EXTRACT_TO: extract_to_dialog(w, a->files); break;
    case A_COMPRESS: compress_dialog(w, a->files); break;
    case A_SET_DEFAULT: set_default_app(w, a->app, a->files->data); break;
    case A_CHECKSUMS: dialog_checksums(w, a->files->data); break;
    case A_COMPARE: compare_files(w, a->files->data, a->files->next->data); break;
    case A_PROPERTIES:
        if (a->files && a->files->next)
            dialog_properties_multi(w, a->files);   /* several: one window for all of them */
        else
            dialog_properties(w, a->files ? a->files->data : c->gfile);
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
    static const char *labels[] = { N_("Name"), N_("Date modified (newest first)"), N_("Size (largest first)"),
                                    N_("Type") };
    GSList *group = NULL;
    for (int b = 0; b < 4; b++) {
        GtkWidget *mi = gtk_radio_menu_item_new_with_label(group, _(labels[b]));
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(mi));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), (int) cur.by == b);
        g_object_set_data_full(G_OBJECT(mi), "path", g_strdup(path), g_free);
        g_object_set_data(G_OBJECT(mi), "by", GINT_TO_POINTER(b));
        g_signal_connect(mi, "toggled", G_CALLBACK(sort_pick), w);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
    }
    sep(sub);
    GtkWidget *rev = gtk_check_menu_item_new_with_label(_("Reversed"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(rev), cur.reverse);
    g_object_set_data_full(G_OBJECT(rev), "path", g_strdup(path), g_free);
    g_signal_connect(rev, "toggled", G_CALLBACK(sort_reverse), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), rev);
    sep(sub);
    GtkWidget *back = gtk_menu_item_new_with_label(_("Back to name"));
    gtk_widget_set_sensitive(back, custom);
    g_object_set_data_full(G_OBJECT(back), "path", g_strdup(path), g_free);
    g_signal_connect(back, "activate", G_CALLBACK(sort_reset), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), back);
    char *base = g_file_get_basename(folder);
    char *l = g_strdup_printf(_("Sort “%s”"), base);
    GtkWidget *top = gtk_menu_item_new_with_label(l);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    g_free(l);
    g_free(base);
    g_free(path);
}

/* "Open with ▸" for files of type ct: each program, and for one file (can_default: not a folder,
 * that would be the default file manager) "Set default ▸" with the same programs, the default ticked */
static void open_with_menu(GtkWidget *menu, Win *w, Column *c, GList *files, const char *ct, gboolean can_default)
{
    GList *apps = ct ? g_app_info_get_all_for_type(ct) : NULL;
    /* one file (not a folder) on this computer: also Other application… and Check file type */
    gboolean one_file = ct && !files->next && g_strcmp0(ct, "inode/directory") && g_file_is_native(files->data);
    if (!apps && !one_file)
        return;
    GtkWidget *sub = gtk_menu_new();
    for (GList *l = apps; l; l = l->next) {
        GtkWidget *mi = item(sub, w, c, files, NULL, A_OPEN_WITH, g_app_info_get_display_name(l->data), NULL, TRUE);
        Act *a = g_object_get_data(G_OBJECT(mi), "act");
        a->app = g_object_ref(l->data);
    }
    if (can_default && !files->next) {
        GAppInfo *def = g_app_info_get_default_for_type(ct, FALSE);
        GtkWidget *dsub = gtk_menu_new();
        for (GList *l = apps; l; l = l->next) {
            GtkWidget *mi = gtk_check_menu_item_new_with_label(g_app_info_get_display_name(l->data));
            gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(mi), TRUE);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), def && g_app_info_equal(def, l->data));
            Act *a = g_new0(Act, 1);       /* like item(), for a tick item; connected after the tick is set */
            a->w = w;
            a->c = c;
            if (c)
                column_ref(c);
            a->files = g_list_copy_deep(files, obj_copy, NULL);
            a->app = g_object_ref(l->data);
            a->action = A_SET_DEFAULT;
            g_signal_connect_data(mi, "activate", G_CALLBACK(act_run), a, act_free, 0);
            gtk_menu_shell_append(GTK_MENU_SHELL(dsub), mi);
        }
        g_clear_object(&def);
        if (apps) {
            sep(sub);
            GtkWidget *sd = gtk_menu_item_new_with_label(_("Set default"));
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(sd), dsub);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), sd);
        } else {
            gtk_widget_destroy(dsub);
        }
    }
    if (one_file) {
        if (apps)
            sep(sub);
        item(sub, w, c, files, NULL, A_OTHER_APP, _("Other application…"), NULL, TRUE);
        char *fp = g_find_program_in_path("file");      /* not installed: no entry */
        if (fp)
            item(sub, w, c, files, NULL, A_FILE_TYPE, _("Check file type"), NULL, TRUE);
        g_free(fp);
    }
    GtkWidget *ow = gtk_menu_item_new_with_label(_("Open with"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(ow), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ow);
    g_list_free_full(apps, g_object_unref);
}

void win_context_menu(Win *w, Column *c, GdkEventButton *ev)
{
    if (c->computer) {
        computer_menu(w, c, ev);
        return;
    }
    if (c->trash) {
        trash_menu(w, c, ev);
        return;
    }
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
            item(menu, w, c, files, NULL, A_OPEN_DIR, _("Open"), NULL, TRUE);
            item(menu, w, c, NULL, files->data, A_NEW_WINDOW, _("Open in new window"), NULL, TRUE);
        } else if (n == 1)
            item(menu, w, c, files, NULL, A_OPEN, _("Open"), "Return", TRUE);
        GFileInfo *info = mx_query_info(files->data, "standard::content-type", G_FILE_QUERY_INFO_NONE,
                                            NULL, NULL);
        const char *ct = info ? g_file_info_get_content_type(info) : NULL;
        open_with_menu(menu, w, c, files, ct, !single_dir);
        g_clear_object(&info);
        sep(menu);
        if (all_archives(files)) {
            item(menu, w, c, files, NULL, A_EXTRACT_HERE, _("Extract here"), NULL, TRUE);
            item(menu, w, c, files, NULL, A_EXTRACT_TO, _("Extract to…"), NULL, TRUE);
        }
        item(menu, w, c, files, NULL, A_COMPRESS, _("Compress…"), NULL, TRUE);
        sep(menu);
        item(menu, w, c, files, NULL, A_CUT, _("Cut"), "<Control>x", TRUE);
        item(menu, w, c, files, NULL, A_COPY, _("Copy"), "<Control>c", TRUE);
        if (single_dir) {   /* a highlighted folder gets the paste (Ctrl+V does the same) */
            char *b = g_file_get_basename(files->data);
            char *l = g_strdup_printf(_("Paste into “%s”"), b);
            item(menu, w, c, files, NULL, A_PASTE_INTO, l, "<Control>v", TRUE);
            g_free(l);
            g_free(b);
        } else {
            item(menu, w, c, files, NULL, A_PASTE, _("Paste"), "<Control>v", TRUE);
        }
        sep(menu);
        item(menu, w, c, files, NULL, A_RENAME, _("Rename…"), "F2", n == 1);
        item(menu, w, c, files, NULL, A_TRASH, _("Move to trash"), "Delete", TRUE);
        item(menu, w, c, files, NULL, A_DELETE, _("Delete permanently…"), "<Shift>Delete", TRUE);
        sep(menu);
    } else {
        item(menu, w, c, NULL, NULL, A_PASTE, _("Paste"), "<Control>v", TRUE);
        sep(menu);
    }
    item(menu, w, c, NULL, NULL, A_NEW_FOLDER, _("New folder…"), "<Control><Shift>n", TRUE);
    item(menu, w, c, NULL, NULL, A_NEW_FILE, _("New file…"), NULL, TRUE);
    sort_menu(menu, w, folder);       /* the highlighted folder, else this column's own */
    item(menu, w, c, NULL, folder, A_TERMINAL, _("Open terminal here"), NULL, TRUE);
    if (files)
        item(menu, w, c, files, NULL, A_COPY_PATH, _("Copy path"), NULL, TRUE);
    if (n == 1 && !single_dir && !is_dir(files->data, TRUE))
        item(menu, w, c, files, NULL, A_CHECKSUMS, _("Checksums…"), NULL, TRUE);
    if (n == 2)
        item(menu, w, c, files, NULL, A_COMPARE, _("Compare"), NULL, TRUE);
    item(menu, w, c, NULL, folder, A_BOOKMARK, _("Add to bookmarks"), "<Control>d", TRUE);
    if (!is_root() && LOOK.use_pkexec)
        item(menu, w, c, NULL, folder, A_ROOT, _("Open as root…"), NULL, TRUE);
    sep(menu);
    item(menu, w, c, files, NULL, A_PROPERTIES, _("Properties…"), "<Alt>Return", TRUE);
    sep(menu);
    GtkWidget *hid = gtk_check_menu_item_new_with_label(_("Show hidden files"));
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
    GFileInfo *info = mx_query_info(f, "standard::content-type,standard::type", G_FILE_QUERY_INFO_NONE,
                                        NULL, NULL);
    gboolean isdir = info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    GtkWidget *menu = gtk_menu_new();
    if (one) {
        if (isdir)
            item(menu, w, NULL, files, NULL, A_SHOW, _("Open"), NULL, TRUE);
        else
            item(menu, w, NULL, files, NULL, A_OPEN, _("Open"), NULL, TRUE);
        const char *ct = info ? g_file_info_get_content_type(info) : NULL;
        open_with_menu(menu, w, NULL, files, isdir ? NULL : ct, TRUE);
        if (!isdir)
            item(menu, w, NULL, files, NULL, A_SHOW, _("Show in column"), NULL, TRUE);
        sep(menu);
    }
    g_clear_object(&info);
    if (all_archives(files)) {
        item(menu, w, NULL, files, NULL, A_EXTRACT_HERE, _("Extract here"), NULL, TRUE);
        item(menu, w, NULL, files, NULL, A_EXTRACT_TO, _("Extract to…"), NULL, TRUE);
    }
    item(menu, w, NULL, files, NULL, A_COMPRESS, _("Compress…"), NULL, TRUE);
    sep(menu);
    item(menu, w, NULL, files, NULL, A_CUT, _("Cut"), "<Control>x", TRUE);
    item(menu, w, NULL, files, NULL, A_COPY, _("Copy"), "<Control>c", TRUE);
    sep(menu);
    item(menu, w, NULL, files, NULL, A_RENAME, _("Rename…"), "F2", one);
    item(menu, w, NULL, files, NULL, A_TRASH, _("Move to trash"), "Delete", TRUE);
    item(menu, w, NULL, files, NULL, A_DELETE, _("Delete permanently…"), "<Shift>Delete", TRUE);
    sep(menu);
    item(menu, w, NULL, files, NULL, A_COPY_PATH, one ? _("Copy path") : _("Copy paths"), NULL, TRUE);
    if (one && !isdir)
        item(menu, w, NULL, files, NULL, A_CHECKSUMS, _("Checksums…"), NULL, TRUE);
    if (g_list_length(files) == 2)
        item(menu, w, NULL, files, NULL, A_COMPARE, _("Compare"), NULL, TRUE);
    item(menu, w, NULL, files, NULL, A_PROPERTIES, _("Properties…"), "<Alt>Return", one);
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
        GList *sel = c ? column_selected_files(c) : NULL;
        if (sel && sel->next) {
            dialog_properties_multi(w, sel);
        } else if (c && column_selected_single(c, &f, NULL)) {
            dialog_properties(w, f);
        } else {
            GFile *cur = win_current_folder(w);
            dialog_properties(w, cur);
            g_object_unref(cur);
        }
        g_clear_object(&f);
        g_list_free_full(sel, g_object_unref);
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
    if (ctrl && (k == GDK_KEY_f || k == GDK_KEY_F)) {
        search_open(w);
        return TRUE;
    }
    if (k == GDK_KEY_BackSpace && !GTK_IS_ENTRY(gtk_window_get_focus(GTK_WINDOW(w->window)))) {
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
    search_free(w);
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
    geom_save(&w->geom);             /* the last one closed is how the next start looks */
    win_unref(w);
}

/* ---- window place and size: remembered when closed, back the same at the next start ---- */
#define WIN_W 1400
#define WIN_H 800

/* moved or resized: note it, but not the maximized size (unmaximizing should give the old one) */
static gboolean on_configure(GtkWidget *win, GdkEventConfigure *e, Win *w)
{
    (void) e;
    GdkWindow *gw = gtk_widget_get_window(win);
    if (gw && gdk_window_get_state(gw) & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN |
                                           GDK_WINDOW_STATE_TILED))
        return FALSE;
    int width, height;
    gtk_window_get_size(GTK_WINDOW(win), &width, &height);
    if (width != w->geom.w || height != w->geom.h)
        w->geom_prev = w->geom;
    w->geom.w = width;
    w->geom.h = height;
    w->geom.has_size = TRUE;
    if (GDK_IS_X11_DISPLAY(gtk_widget_get_display(win))) {     /* Wayland doesn't tell, nor let us */
        gtk_window_get_position(GTK_WINDOW(win), &w->geom.x, &w->geom.y);
        w->geom.has_pos = TRUE;
    }
    return FALSE;
}

static gboolean on_window_state(GtkWidget *win, GdkEventWindowState *e, Win *w)
{
    gboolean max = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    if (max && !w->geom.max && w->geom_prev.has_size) {
        /* the window manager sometimes sends the maximized size before saying it maximized:
         * if what we noted last is that size, the normal one is the one before */
        int width, height;
        gtk_window_get_size(GTK_WINDOW(win), &width, &height);
        if (width == w->geom.w && height == w->geom.h)
            w->geom = w->geom_prev;
    }
    w->geom.max = max;
    return FALSE;
}

/* is the top of a window at x, y on some screen? (not after the monitor it was on is gone) */
static gboolean on_a_screen(GdkDisplay *d, int x, int y, int width)
{
    for (int i = 0; i < gdk_display_get_n_monitors(d); i++) {
        GdkRectangle r;
        gdk_monitor_get_workarea(gdk_display_get_monitor(d, i), &r);
        int cx = x + width / 2, cy = y + 10;
        if (cx >= r.x && cx < r.x + r.width && cy >= r.y && cy < r.y + r.height)
            return TRUE;
    }
    return FALSE;
}

/* the first window comes back as the last one was closed; new ones open centred, 1400 x 800
 * (at most 85 % of the screen) */
static void place_window(Win *w, gboolean first)
{
    GtkWindow *win = GTK_WINDOW(w->window);
    GdkDisplay *d = gtk_widget_get_display(w->window);
    GdkRectangle area = { 0, 0, WIN_W * 100 / 85, WIN_H * 100 / 85 };
    GdkMonitor *mon = gdk_display_get_primary_monitor(d);
    if (!mon)
        mon = gdk_display_get_monitor(d, 0);
    if (mon)
        gdk_monitor_get_workarea(mon, &area);
    Geom g;
    if (first)
        geom_load(&g);
    if (first && g.has_size) {
        gtk_window_set_default_size(win, g.w, g.h);
        if (g.has_pos && GDK_IS_X11_DISPLAY(d) && on_a_screen(d, g.x, g.y, g.w))
            gtk_window_move(win, g.x, g.y);
        else
            gtk_window_set_position(win, GTK_WIN_POS_CENTER);
        if (g.max)
            gtk_window_maximize(win);
        w->geom = w->geom_prev = g;
        return;
    }
    gtk_window_set_default_size(win, MIN(WIN_W, area.width * 85 / 100), MIN(WIN_H, area.height * 85 / 100));
    gtk_window_set_position(win, GTK_WIN_POS_CENTER);        /* not wherever the WM likes */
}

static GtkWidget *tool_button(const char *icon, const char *tip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

/* the bookmarks menu is rebuilt BEFORE it pops up; filling it while already shown made GTK
 * open it at the old size, then resize and leave it scrolled */
static void places_clicked(GtkButton *b, Win *w)
{
    (void) b;
    places_fill_bookmarks(w);
    gtk_menu_popup_at_widget(GTK_MENU(w->bm_menu), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST,
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

    gboolean first = gtk_application_get_windows(app) == NULL;
    w->window = gtk_application_window_new(app);
    g_object_set_data(G_OBJECT(w->window), "millerx-win", w);
    gtk_window_set_title(GTK_WINDOW(w->window), _("millerX"));
    gtk_window_set_icon_name(GTK_WINDOW(w->window), "millerx");
    place_window(w, first);
    g_signal_connect(w->window, "configure-event", G_CALLBACK(on_configure), w);
    g_signal_connect(w->window, "window-state-event", G_CALLBACK(on_window_state), w);

    w->location = gtk_entry_new();
    gtk_widget_set_hexpand(w->location, TRUE);
    g_signal_connect(w->location, "activate", G_CALLBACK(on_location), w);
    GtkWidget *up = tool_button("go-up", _("Parent folder (Backspace, Alt+Up)"));
    g_signal_connect(up, "clicked", G_CALLBACK(on_up), w);
    GtkWidget *home = tool_button("go-home", _("Home (Alt+Home)"));
    g_signal_connect(home, "clicked", G_CALLBACK(on_home), w);
    GtkWidget *root = tool_button("computer", _("Computer: disks, USB sticks, network (Alt+/)"));
    g_signal_connect(root, "clicked", G_CALLBACK(on_root), w);
    w->bm_menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(w->bm_menu), w->window, NULL);
    GtkWidget *bm = tool_button("user-bookmarks", _("Bookmarks (Ctrl+D adds this folder)"));
    g_object_set_data(G_OBJECT(bm), "bm", GINT_TO_POINTER(1));
    g_signal_connect(bm, "clicked", G_CALLBACK(places_clicked), w);
    GtkWidget *main_menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_label(_("New window"));
    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(mi))), GDK_KEY_n, GDK_CONTROL_MASK);
    g_signal_connect(mi, "activate", G_CALLBACK(on_new_window), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), gtk_separator_menu_item_new());
    if (!is_root()) {
        w->root_mi = mi = gtk_menu_item_new_with_label(_("Open this folder as root…"));
        g_signal_connect(mi, "activate", G_CALLBACK(on_root_here), w);
        gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
        w->root_sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), w->root_sep);
    }
    mi = gtk_menu_item_new_with_label(_("Search here…"));
    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(mi))), GDK_KEY_f, GDK_CONTROL_MASK);
    g_signal_connect_swapped(mi, "activate", G_CALLBACK(search_open), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
    mi = gtk_menu_item_new_with_label(_("Settings…"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_settings), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
    mi = gtk_menu_item_new_with_label(_("About millerX"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_about), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(main_menu), mi);
    gtk_widget_show_all(main_menu);
    GtkWidget *menu_btn = menu_button(main_menu, "open-menu", "Menu");

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_object_set(bar, "margin", 4, NULL);
    GtkWidget *btns[] = { up, home, root, bm };
    for (guint i = 0; i < G_N_ELEMENTS(btns); i++)
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
    gtk_box_pack_start(GTK_BOX(pane), w->preview->outer, TRUE, TRUE, 0);

    w->statusbar = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(w->statusbar), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->statusbar), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(w->statusbar, TRUE);
    w->progress = gtk_progress_bar_new();
    gtk_widget_set_valign(w->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(w->progress, 160, -1);
    w->cancel_btn = gtk_button_new_with_label(_("Cancel"));
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
        gtk_label_set_markup(GTK_LABEL(l), _("<b>You are root:</b> every change here is made as administrator. "
                                             "Be careful."));
        gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(ib))), l);
        gtk_box_pack_start(GTK_BOX(box), ib, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    search_build(w, box, pane);          /* its bar right under the toolbar */
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
    gboolean names = nl->detect != LOOK.detect || nl->gallery_names != LOOK.gallery_names || nl->thumbs != LOOK.thumbs ||
                     nl->use_ffmpeg != LOOK.use_ffmpeg || nl->use_pdftoppm != LOOK.use_pdftoppm ||
                     nl->use_blender != LOOK.use_blender || nl->thumb_size != LOOK.thumb_size;
    char *cmp = g_strdup(nl->compare && *nl->compare ? nl->compare : "auto");
    char *col = g_strdup(nl->divider_color), *hl = g_strdup(nl->highlight);
    char *lang = g_strdup(nl->language ? nl->language : "system");
    gboolean hl_changed = g_strcmp0(nl->highlight, LOOK.highlight);
    g_free(LOOK.divider_color);
    g_free(LOOK.highlight);
    g_free(LOOK.language);
    g_free(LOOK.compare);
    LOOK = *nl;                      /* the strings in nl aren't ours (entry text, old LOOK): own copies */
    LOOK.divider_color = col;
    LOOK.highlight = hl;
    LOOK.language = lang;
    LOOK.compare = cmp;
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

/* Settings → Language → Restart now: the same windows, at the same folders, in a new millerX that
 * reads the new language at its start; this one quits. Not while files are being copied. */
void app_restart(GtkApplication *app)
{
    GList *wins = g_list_reverse(g_list_copy(gtk_application_get_windows(app)));   /* oldest first */
    for (GList *l = wins; l; l = l->next) {
        Win *w = g_object_get_data(G_OBJECT(l->data), "millerx-win");
        if (w && !w->dead && w->jobs) {
            win_error(w, _("Not now"), _("Files are still being copied, moved or packed. Restart when that is done."));
            g_list_free(wins);
            return;
        }
    }
    look_save(NULL);
    /* the new millerX opens its first window where the last closed one was: that is the oldest
     * window here, which comes back first (a plain quit doesn't close windows, so save it now) */
    Win *first = wins ? g_object_get_data(G_OBJECT(wins->data), "millerx-win") : NULL;
    if (first && !first->dead)
        geom_save(&first->geom);
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe)
        exe = g_build_filename(MILLERX_BINDIR, "millerx", NULL);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, exe);
    for (GList *l = wins; l; l = l->next) {
        Win *w = g_object_get_data(G_OBJECT(l->data), "millerx-win");
        if (!w || w->dead)
            continue;
        GFile *f = win_current_folder(w);
        char *p = g_file_get_path(f);
        g_ptr_array_add(argv, p ? p : g_file_get_uri(f));
        g_object_unref(f);
    }
    g_ptr_array_add(argv, NULL);
    GError *err = NULL;
    if (spawn_child(NULL, (char **) argv->pdata, G_SPAWN_DEFAULT, &err)) {
        g_application_quit(G_APPLICATION(app));
    } else {
        Win *w = wins ? g_object_get_data(G_OBJECT(wins->data), "millerx-win") : NULL;
        if (w)
            win_error(w, _("Couldn't restart millerX"), err->message);
        g_error_free(err);
    }
    g_ptr_array_unref(argv);
    g_list_free(wins);
}
