/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Conflict question, properties, settings, about. */
#include "millerx.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- copy/move conflict ------------------------------------------------------------- */
Choice dialog_conflict(Win *w, GFile *src, GFile *dst, gboolean merge, gboolean *for_all)
{
    (void) src;
    char *base = g_file_get_basename(dst);
    GFile *parent = g_file_get_parent(dst);
    char *where = parent ? file_path(parent) : g_strdup("/");
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, "“%s” already exists", base);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "in %s.\n%s", where,
        merge ? "Merge the folders? Files with the same name will be replaced."
              : "Replace the existing file?");
    GtkWidget *allc = gtk_check_button_new_with_label("Do this for all conflicts");
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(d))), allc,
                       FALSE, FALSE, 0);
    gtk_widget_show(allc);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", 1);
    gtk_dialog_add_button(GTK_DIALOG(d), "Skip", 2);
    gtk_dialog_add_button(GTK_DIALOG(d), "Keep both", 3);
    gtk_dialog_add_button(GTK_DIALOG(d), merge ? "Merge" : "Replace", 4);
    gtk_dialog_set_default_response(GTK_DIALOG(d), 2);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    *for_all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(allc));
    gtk_widget_destroy(d);
    g_free(where);
    g_clear_object(&parent);
    g_free(base);
    switch (r) {
    case 2: return CH_SKIP;
    case 3: return CH_BOTH;
    case 4: return CH_REPLACE;
    default: return CH_CANCEL;
    }
}

/* ---- properties ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget *grid;
    int row;
} Rows;

static GtkWidget *prow(Rows *r, const char *key, const char *value, GtkWidget *widget)
{
    GtkWidget *k = gtk_label_new(key);
    gtk_label_set_xalign(GTK_LABEL(k), 1);
    gtk_label_set_yalign(GTK_LABEL(k), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
    GtkWidget *v = widget;
    if (!v) {
        v = gtk_label_new(value);
        gtk_label_set_xalign(GTK_LABEL(v), 0);
        gtk_label_set_selectable(GTK_LABEL(v), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(v), 50);
    }
    gtk_grid_attach(GTK_GRID(r->grid), k, 0, r->row, 1, 1);
    gtk_grid_attach(GTK_GRID(r->grid), v, 1, r->row, 1, 1);
    r->row++;
    return v;
}

typedef struct {
    GtkWidget *label;     /* ref */
    GCancellable *stop;   /* ref; cancelled when the dialog closes */
    char *path;
    guint64 total, files, dirs;
    gboolean running;
} Count;

static gboolean count_show(gpointer data)
{
    Count *c = data;
    if (!g_cancellable_is_cancelled(c->stop)) {
        char *sz = format_size_full(c->total);
        char *t = g_strdup_printf("%s\n%" G_GUINT64_FORMAT " files, %" G_GUINT64_FORMAT " folders%s",
                                  sz, c->files, c->dirs, c->running ? " …" : "");
        gtk_label_set_text(GTK_LABEL(c->label), t);
        g_free(t);
        g_free(sz);
    }
    g_object_unref(c->label);
    g_object_unref(c->stop);
    g_free(c->path);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void count_post(Count *c, gboolean running)
{
    Count *u = g_new0(Count, 1);
    *u = *c;
    u->label = g_object_ref(c->label);
    u->stop = g_object_ref(c->stop);
    u->path = NULL;
    u->running = running;
    g_idle_add(count_show, u);
}

static gpointer count_thread(gpointer data)
{
    Count *c = data;
    GPtrArray *stack = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(stack, g_strdup(c->path));
    gint64 last = g_get_monotonic_time();
    while (stack->len && !g_cancellable_is_cancelled(c->stop)) {
        char *dpath = g_ptr_array_steal_index(stack, stack->len - 1);
        GDir *d = g_dir_open(dpath, 0, NULL);
        if (d) {
            const char *n;
            while ((n = g_dir_read_name(d))) {
                char *p = g_build_filename(dpath, n, NULL);
                struct stat st;
                if (lstat(p, &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        g_ptr_array_add(stack, p);
                        c->dirs++;
                        continue;
                    }
                    c->total += st.st_size;
                    c->files++;
                }
                g_free(p);
            }
            g_dir_close(d);
        }
        g_free(dpath);
        if (g_get_monotonic_time() - last > 300000) {
            last = g_get_monotonic_time();
            count_post(c, TRUE);
        }
    }
    g_ptr_array_unref(stack);
    count_post(c, FALSE);
    g_object_unref(c->label);
    g_object_unref(c->stop);
    g_free(c->path);
    g_free(c);
    return NULL;
}

typedef struct {
    Win *w;
    char *path;
    gboolean mine, isdir;
    GtkWidget *octal;
    GtkWidget *boxes[9];
} Perm;

static void perm_free(Perm *p)
{
    g_free(p->path);
    g_free(p);
}

static void show_octal(Perm *p, mode_t m)
{
    char s[11];
    const char *rwx = "rwxrwxrwx";
    s[0] = p->isdir ? 'd' : '-';
    for (int i = 0; i < 9; i++)
        s[i + 1] = m & (1 << (8 - i)) ? rwx[i] : '-';
    s[10] = 0;
    char *t = g_strdup_printf("%s   %04o%s", s, (unsigned) (m & 07777), p->mine ? "" : "   (not yours, read only)");
    gtk_label_set_text(GTK_LABEL(p->octal), t);
    g_free(t);
}

static void perm_toggled(GtkToggleButton *cb, Perm *p)
{
    int bit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cb), "bit"));
    struct stat st;
    if (stat(p->path, &st) != 0)
        return;
    mode_t m = gtk_toggle_button_get_active(cb) ? (st.st_mode | bit) : (st.st_mode & ~bit);
    if (chmod(p->path, m & 07777) != 0) {
        win_error(p->w, "Couldn't change permissions", g_strerror(errno));
        g_signal_handlers_block_by_func(cb, perm_toggled, p);
        gtk_toggle_button_set_active(cb, !gtk_toggle_button_get_active(cb));
        g_signal_handlers_unblock_by_func(cb, perm_toggled, p);
        return;
    }
    if (stat(p->path, &st) == 0)
        show_octal(p, st.st_mode);
}

static void unref_notify(gpointer data, GClosure *cl)
{
    (void) cl;
    g_object_unref(data);
}

static void on_props_response(GtkDialog *d, int r, GCancellable *stop)
{
    (void) r;
    g_cancellable_cancel(stop);
    gtk_widget_destroy(GTK_WIDGET(d));
}

void dialog_properties(Win *w, GFile *f)
{
    GError *err = NULL;
    GFileInfo *info = g_file_query_info(f, "standard::*,time::*,unix::*,owner::*", G_FILE_QUERY_INFO_NONE,
                                        NULL, &err);
    if (!info) {
        win_error(w, "No properties", err->message);
        g_error_free(err);
        return;
    }
    GFileInfo *linfo = g_file_query_info(f, "standard::is-symlink,standard::symlink-target",
                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    gboolean isdir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    const char *dname = g_file_info_get_display_name(info);
    char *title = g_strdup_printf("Properties: %s", dname);
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), 0, "Close",
                                               GTK_RESPONSE_CLOSE, NULL);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(d), 460, -1);
    Rows r = { gtk_grid_new(), 1 };
    gtk_grid_set_column_spacing(GTK_GRID(r.grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(r.grid), 6);
    g_object_set(r.grid, "margin", 14, NULL);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), r.grid, TRUE, TRUE, 0);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GIcon *gi = g_file_info_get_icon(info);
    GtkWidget *icon = gi ? gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG) : gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    GtkWidget *name = gtk_label_new(NULL);
    char *mk = g_markup_printf_escaped("<b>%s</b>", dname);
    gtk_label_set_markup(GTK_LABEL(name), mk);
    g_free(mk);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_selectable(GTK_LABEL(name), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(name), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
    gtk_box_pack_start(GTK_BOX(head), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), name, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(r.grid), head, 0, 0, 2, 1);

    const char *ct = g_file_info_get_content_type(info);
    char *desc = ct ? g_content_type_get_description(ct) : g_strdup("?");
    char *type = ct ? g_strdup_printf("%s  (%s)", desc, ct) : g_strdup(desc);
    prow(&r, "Type", type, NULL);
    g_free(type);
    g_free(desc);
    GFile *parent = g_file_get_parent(f);
    char *loc = parent ? file_path(parent) : g_strdup("/");
    prow(&r, "Location", loc, NULL);
    g_free(loc);
    g_clear_object(&parent);
    if (linfo && g_file_info_get_is_symlink(linfo)) {
        const char *t = g_file_info_get_symlink_target(linfo);
        prow(&r, "Link to", t ? t : "?", NULL);
    }
    GCancellable *stop = g_cancellable_new();
    g_signal_connect_data(d, "response", G_CALLBACK(on_props_response), stop, unref_notify, 0);
    if (isdir) {
        GtkWidget *size_l = prow(&r, "Size", "counting …", NULL);
        char *path = g_file_get_path(f);
        if (path) {
            Count *c = g_new0(Count, 1);
            c->label = g_object_ref(size_l);
            c->stop = g_object_ref(stop);
            c->path = path;
            g_thread_unref(g_thread_new("millerx-count", count_thread, c));
        }
        GFileInfo *fs = g_file_query_filesystem_info(f, "filesystem::free,filesystem::size,filesystem::type",
                                                     NULL, NULL);
        if (fs) {
            char *free_s = g_format_size(g_file_info_get_attribute_uint64(fs, "filesystem::free"));
            char *tot_s = g_format_size(g_file_info_get_attribute_uint64(fs, "filesystem::size"));
            const char *fst = g_file_info_get_attribute_string(fs, "filesystem::type");
            char *t = fst ? g_strdup_printf("%s of %s  (%s)", free_s, tot_s, fst)
                          : g_strdup_printf("%s of %s", free_s, tot_s);
            prow(&r, "Free space", t, NULL);
            g_free(t);
            g_free(tot_s);
            g_free(free_s);
            g_object_unref(fs);
        }
    } else {
        char *s = format_size_full(g_file_info_get_size(info));
        prow(&r, "Size", s, NULL);
        g_free(s);
    }
    const char *tkeys[][2] = { { "time::modified", "Modified" }, { "time::access", "Accessed" },
                               { "time::created", "Created" } };
    for (int i = 0; i < 3; i++)
        if (g_file_info_has_attribute(info, tkeys[i][0])) {
            GDateTime *dt = g_date_time_new_from_unix_local(
                (gint64) g_file_info_get_attribute_uint64(info, tkeys[i][0]));
            char *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
            prow(&r, tkeys[i][1], s, NULL);
            g_free(s);
            g_date_time_unref(dt);
        }
    const char *ou = g_file_info_get_attribute_string(info, "owner::user");
    const char *og = g_file_info_get_attribute_string(info, "owner::group");
    char *uid = g_strdup_printf("%u", g_file_info_get_attribute_uint32(info, "unix::uid"));
    char *gid = g_strdup_printf("%u", g_file_info_get_attribute_uint32(info, "unix::gid"));
    char *owner = g_strdup_printf("%s : %s", ou ? ou : uid, og ? og : gid);
    prow(&r, "Owner", owner, NULL);
    g_free(owner);
    g_free(uid);
    g_free(gid);

    char *path = g_file_get_path(f);
    if (g_file_info_has_attribute(info, "unix::mode") && path) {
        guint32 mode = g_file_info_get_attribute_uint32(info, "unix::mode");
        Perm *p = g_new0(Perm, 1);
        p->w = w;
        p->path = g_strdup(path);
        p->isdir = isdir;
        p->mine = g_file_info_get_attribute_uint32(info, "unix::uid") == getuid() || getuid() == 0;
        GtkWidget *perm = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(perm), 10);
        gtk_grid_set_row_spacing(GTK_GRID(perm), 2);
        const char *heads[] = { "Read", "Write", isdir ? "Enter" : "Exec" };
        const char *who[] = { "Owner", "Group", "Others" };
        for (int c = 0; c < 3; c++)
            gtk_grid_attach(GTK_GRID(perm), gtk_label_new(heads[c]), c + 1, 0, 1, 1);
        for (int rr = 0; rr < 3; rr++) {
            GtkWidget *l = gtk_label_new(who[rr]);
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_grid_attach(GTK_GRID(perm), l, 0, rr + 1, 1, 1);
            for (int c = 0; c < 3; c++) {
                int bit = 1 << (8 - (rr * 3 + c));
                GtkWidget *cb = gtk_check_button_new();
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), mode & bit);
                gtk_widget_set_sensitive(cb, p->mine);
                gtk_widget_set_halign(cb, GTK_ALIGN_CENTER);
                g_object_set_data(G_OBJECT(cb), "bit", GINT_TO_POINTER(bit));
                g_signal_connect(cb, "toggled", G_CALLBACK(perm_toggled), p);
                gtk_grid_attach(GTK_GRID(perm), cb, c + 1, rr + 1, 1, 1);
            }
        }
        p->octal = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(p->octal), 0);
        show_octal(p, mode);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_pack_start(GTK_BOX(box), perm, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), p->octal, FALSE, FALSE, 0);
        prow(&r, "Permissions", NULL, box);
        g_object_set_data_full(G_OBJECT(d), "perm", p, (GDestroyNotify) perm_free);
    }
    g_free(path);
    gtk_widget_show_all(d);
    g_clear_object(&linfo);
    g_object_unref(info);
}

/* ---- settings --------------------------------------------------------------------------------- */
typedef struct {
    Win *w;
    GtkWidget *auto_rb, *own_rb, *colour, *width, *style, *pw, *ff, *sh;
} Set;

static void settings_changed(GtkWidget *any, Set *s)
{
    (void) any;
    gboolean own = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->own_rb));
    gtk_widget_set_sensitive(s->colour, own);
    Look nl = LOOK;
    char *col = NULL;
    if (own) {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(s->colour), &c);
        col = g_strdup_printf("#%02x%02x%02x", (int) (c.red * 255 + 0.5), (int) (c.green * 255 + 0.5),
                              (int) (c.blue * 255 + 0.5));
    }
    nl.divider_color = col ? col : (char *) "auto";
    nl.divider_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->width));
    nl.fade = !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->style)), "fade");
    nl.preview_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->pw));
    nl.folders_first = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->ff));
    nl.show_hidden = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->sh));
    app_apply_look(s->w->app, &nl);
    g_free(col);
}

static void settings_close(GtkDialog *d, int r, Set *s)
{
    (void) r;
    GError *err = NULL;
    if (!look_save(&err)) {
        win_error(s->w, "Couldn't save the settings", err->message);
        g_error_free(err);
    }
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void srow(GtkWidget *grid, int *r, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    gtk_grid_attach(GTK_GRID(grid), l, 0, *r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w, 1, *r, 1, 1);
    (*r)++;
}

static void section(GtkWidget *grid, int *r, const char *text)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_strdup_printf("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (*r)
        gtk_widget_set_margin_top(l, 6);
    gtk_grid_attach(GTK_GRID(grid), l, 0, *r, 2, 1);
    (*r)++;
}

void dialog_settings(Win *w)
{
    GtkWidget *d = gtk_dialog_new_with_buttons("millerX settings", GTK_WINDOW(w->window), 0, "Close",
                                               GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    g_object_set(grid, "margin", 14, NULL);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);
    Set *s = g_new0(Set, 1);
    s->w = w;
    g_object_set_data_full(G_OBJECT(d), "set", s, g_free);
    int r = 0;

    section(grid, &r, "Column dividers");
    s->auto_rb = gtk_radio_button_new_with_label(NULL, "Follow the theme (20 % lighter / darker)");
    s->own_rb = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s->auto_rb), "Own colour");
    s->colour = gtk_color_button_new();
    GdkRGBA rgba;
    if (strcmp(LOOK.divider_color, "auto") && gdk_rgba_parse(&rgba, LOOK.divider_color)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->own_rb), TRUE);
    } else {
        divider_rgba(w->pane_divider, &rgba);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->auto_rb), TRUE);
    }
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(s->colour), &rgba);
    gtk_widget_set_sensitive(s->colour, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->own_rb)));
    GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(cbox), s->own_rb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cbox), s->colour, FALSE, FALSE, 0);
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(vb), s->auto_rb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), cbox, FALSE, FALSE, 0);
    srow(grid, &r, "Colour", vb);
    s->width = gtk_spin_button_new_with_range(1, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->width), LOOK.divider_width);
    srow(grid, &r, "Width (px)", s->width);
    s->style = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->style), "solid", "Solid");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->style), "fade", "Fading at the ends");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->style), LOOK.fade ? "fade" : "solid");
    srow(grid, &r, "Style", s->style);
    section(grid, &r, "Preview");
    s->pw = gtk_spin_button_new_with_range(200, 900, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->pw), LOOK.preview_width);
    srow(grid, &r, "Width (px)", s->pw);
    section(grid, &r, "Browsing");
    s->ff = gtk_check_button_new_with_label("Folders first");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->ff), LOOK.folders_first);
    srow(grid, &r, "Sort by name", s->ff);
    s->sh = gtk_check_button_new_with_label("Show hidden files when millerX starts");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->sh), LOOK.show_hidden);
    srow(grid, &r, "Hidden files", s->sh);

    g_signal_connect(s->auto_rb, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->colour, "color-set", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->width, "value-changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->style, "changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->pw, "value-changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->ff, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->sh, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(d, "response", G_CALLBACK(settings_close), s);
    gtk_widget_show_all(d);
}

void dialog_about(Win *w)
{
    gtk_show_about_dialog(GTK_WINDOW(w->window), "program-name", "millerX", "version", MILLERX_VERSION,
                          "comments", "Files in Miller columns, from / to where you are.",
                          "logo-icon-name", "millerx", "modal", TRUE, NULL);
}
