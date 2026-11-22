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
    GtkWidget *auto_rb, *own_rb, *colour, *width, *style, *pw, *cw, *ff, *sh;
    GtkWidget *cache_size, *cache_clear, *cache_mb, *cache_days, *cache_auto;
    GtkWidget *sort_list, *names, *thumbs;
    GtkWidget *use[3];         /* the optional helpers, when installed; NULL when not */
    GtkWidget *hl_auto, *hl_own, *hl_colour, *hl_sys, *hl_unsys;
} Set;

static void fill_sort_list(Set *s);

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
    gboolean hown = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->hl_own));
    gtk_widget_set_sensitive(s->hl_colour, hown);
    gtk_widget_set_sensitive(s->hl_sys, hown);
    char *hcol = NULL;
    if (hown) {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(s->hl_colour), &c);
        hcol = g_strdup_printf("#%02x%02x%02x", (int) (c.red * 255 + 0.5), (int) (c.green * 255 + 0.5),
                               (int) (c.blue * 255 + 0.5));
    }
    nl.highlight = hcol ? hcol : (char *) "auto";
    nl.divider_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->width));
    nl.fade = !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->style)), "fade");
    nl.preview_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->pw));
    nl.column_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->cw));
    nl.folders_first = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->ff));
    nl.show_hidden = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->sh));
    nl.gallery_names = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->names));
    gboolean *uses[] = { &nl.use_ffmpeg, &nl.use_pdftoppm, &nl.use_pkexec };
    for (int i = 0; i < 3; i++)
        if (s->use[i])
            *uses[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->use[i]));
    const char *tid = gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->thumbs));
    nl.thumbs = !g_strcmp0(tid, "local") ? THUMBS_LOCAL : !g_strcmp0(tid, "never") ? THUMBS_NEVER : THUMBS_ALWAYS;
    nl.cache_max_mb = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->cache_mb));
    nl.cache_days = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->cache_days));
    nl.cache_auto = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->cache_auto));
    app_apply_look(s->w->app, &nl);
    g_free(col);
    g_free(hcol);
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

/* While the settings are built: the box the section frames go into, and a size group that keeps
 * the labels of all sections equally wide, so the frames line up. */
static GtkWidget *SECTIONS;
static GtkSizeGroup *LABELS;
static int LAST_TAB;                   /* the settings open where they were left (this run) */

/* the labels of a new column line up with each other, not with the other column */
static void new_column(GtkWidget *col)
{
    SECTIONS = col;
    g_clear_object(&LABELS);           /* the labels keep the old group alive */
    LABELS = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
}

/* a tab: its sections in a box that scrolls if the screen is too small */
static GtkWidget *tab(GtkWidget *nb, const char *title, int max_h)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    g_object_set(box, "margin", 14, NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), max_h);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll, gtk_label_new(title));
    return box;
}

static void on_tab(GtkNotebook *nb, GtkWidget *page, guint n, gpointer d)
{
    LAST_TAB = (int) n;
}

static void srow(GtkWidget *grid, int *r, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    if (LABELS)
        gtk_size_group_add_widget(LABELS, l);
    gtk_grid_attach(GTK_GRID(grid), l, 0, *r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w, 1, *r, 1, 1);
    (*r)++;
}

/* a new section: a frame with the title on it, holding its own grid (which *grid becomes) */
static void section(GtkWidget **grid, int *r, const char *text)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_label_widget(GTK_FRAME(frame), l);
    gtk_frame_set_label_align(GTK_FRAME(frame), 0, 0.5);
    gtk_widget_set_margin_start(l, 8);          /* every title at the same spot, long or short */
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    g_object_set(g, "margin", 10, NULL);
    gtk_container_add(GTK_CONTAINER(frame), g);
    gtk_box_pack_start(GTK_BOX(SECTIONS), frame, FALSE, FALSE, 0);
    *grid = g;
    *r = 0;
}

/* Optional programs millerX uses if they are there: what each one gives, and what to install
 * if it is missing. Looked up when the settings open, so a fresh install shows up next time. */
static const struct { const char *prog, *gives, *without, *package; } HELPERS[] = {
    { "ffmpeg", "video frames, music covers, AVIF and HEIC pictures",
      "videos and music show their icon, AVIF/HEIC too", "ffmpeg (Slackware: l/ffmpeg)" },
    { "pdftoppm", "the first page of PDFs",
      "PDFs show their icon", "poppler (Debian/Ubuntu: poppler-utils)" },
    { "pkexec", "“Open as root”",
      "no root window", "polkit, plus an authentication agent" },
};

/* found: a tick box to use it or not; missing: what it would give and where to get it */
static void helper_rows(GtkWidget *grid, int *r, Set *s)
{
    const gboolean on[] = { LOOK.use_ffmpeg, LOOK.use_pdftoppm, LOOK.use_pkexec };
    for (guint i = 0; i < G_N_ELEMENTS(HELPERS); i++) {
        char *found = g_find_program_in_path(HELPERS[i].prog);
        if (found) {
            s->use[i] = gtk_check_button_new_with_label(HELPERS[i].gives);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->use[i]), on[i]);
            srow(grid, r, HELPERS[i].prog, s->use[i]);
            g_free(found);
            continue;
        }
        char *m = g_markup_printf_escaped("✗ not installed: %s.\n<small>Install %s\nfor %s.</small>",
                                      HELPERS[i].without, HELPERS[i].package, HELPERS[i].gives);
        GtkWidget *l = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(l), m);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        /* no wrapping: wrapped labels make GTK guess the dialog far too tall */
        gtk_widget_set_opacity(l, 0.85);
        srow(grid, r, HELPERS[i].prog, l);
        g_free(m);
        g_free(found);
    }
}

/* the size label is ours (a reference) even if the dialog was closed meanwhile */
static void cache_measured(guint64 bytes, guint files, guint removed, gpointer data)
{
    GtkWidget *l = data;
    char *sz = g_format_size(bytes);
    char *t = removed ? g_strdup_printf("%s in %u thumbnails (%u deleted)", sz, files, removed)
                      : g_strdup_printf("%s in %u thumbnails", sz, files);
    gtk_label_set_text(GTK_LABEL(l), t);
    g_free(t);
    g_free(sz);
    g_object_unref(l);
}

static void cache_clear_clicked(GtkButton *b, Set *s)
{
    (void) b;
    if (!win_confirm(s->w, "Clear the thumbnail cache?",
                     "All thumbnails in ~/.cache/thumbnails are deleted. Thunar and file dialogs use them "
                     "too; everything is made again when it's needed.", "Clear"))
        return;
    gtk_label_set_text(GTK_LABEL(s->cache_size), "clearing…");
    thumbcache_clear(cache_measured, g_object_ref(s->cache_size));
}

/* ---- folders sorted their own way: one row each, no table ------------------------------------ */
static void sortrow_changed(GtkWidget *any, Set *s)
{
    GtkWidget *row = g_object_get_data(G_OBJECT(any), "row");
    const char *path = g_object_get_data(G_OBJECT(row), "path");
    GtkWidget *combo = g_object_get_data(G_OBJECT(row), "combo"), *rev = g_object_get_data(G_OBJECT(row), "rev");
    sorting_set(path, gtk_combo_box_get_active(GTK_COMBO_BOX(combo)),
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rev)));
    app_sorting_changed(s->w->app, path);
    if (!sorting_get(path, NULL))       /* set back to plain name order: the row goes */
        fill_sort_list(s);
}

static void sortrow_remove(GtkButton *b, Set *s)
{
    GtkWidget *row = g_object_get_data(G_OBJECT(b), "row");
    char *path = g_strdup(g_object_get_data(G_OBJECT(row), "path"));
    sorting_clear(path);
    app_sorting_changed(s->w->app, path);
    g_free(path);
    fill_sort_list(s);
}

static void fill_sort_list(Set *s)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(s->sort_list));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    GList *paths = sorting_paths();
    if (!paths) {
        GtkWidget *l = gtk_label_new("None. Right-click a folder → Sort … to give it its own order.");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_opacity(l, 0.7);
        gtk_box_pack_start(GTK_BOX(s->sort_list), l, FALSE, FALSE, 0);
    }
    for (GList *p = paths; p; p = p->next) {
        FolderSort fs;
        sorting_get(p->data, &fs);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(p->data), g_free);
        GtkWidget *name = gtk_label_new(p->data);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_widget_set_tooltip_text(name, p->data);
        GtkWidget *combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Name");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Date (newest first)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Size (largest first)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Type");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), fs.by);
        GtkWidget *rev = gtk_check_button_new_with_label("Reversed");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rev), fs.reverse);
        GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(x, "Sort by name again");
        g_object_set_data(G_OBJECT(row), "combo", combo);
        g_object_set_data(G_OBJECT(row), "rev", rev);
        g_object_set_data(G_OBJECT(combo), "row", row);
        g_object_set_data(G_OBJECT(rev), "row", row);
        g_object_set_data(G_OBJECT(x), "row", row);
        g_signal_connect(combo, "changed", G_CALLBACK(sortrow_changed), s);
        g_signal_connect(rev, "toggled", G_CALLBACK(sortrow_changed), s);
        g_signal_connect(x, "clicked", G_CALLBACK(sortrow_remove), s);
        gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), rev, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), x, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(s->sort_list), row, FALSE, FALSE, 0);
    }
    g_list_free_full(paths, g_free);
    gtk_widget_show_all(s->sort_list);
}

/* ---- highlight swatches: one click and the window shows it --------------------------------- */
static const char *SWATCHES[] = { "#d4ff02", "#4c566a", "#6c5a9c", "#2a9d8f", "#e9a23b", "#d1495b",
                                  "#5b8c5a", "#d16ba5", "#8a8a8a" };

static void swatch_clicked(GtkButton *b, Set *s)
{
    GdkRGBA c;
    gdk_rgba_parse(&c, g_object_get_data(G_OBJECT(b), "colour"));
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(s->hl_colour), &c);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->hl_own)))
        settings_changed(NULL, s);
    else
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->hl_own), TRUE);   /* calls it via "toggled" */
}

static GtkWidget *swatches(Set *s)
{
    static GtkCssProvider *css;
    GString *rules = NULL;
    if (!css) {
        css = gtk_css_provider_new();
        rules = g_string_new(".millerx-swatch { min-width: 18px; min-height: 18px; padding: 0; "
                             "border-radius: 3px; }\n");
    }
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (guint i = 0; i < G_N_ELEMENTS(SWATCHES); i++) {
        GtkWidget *b = gtk_button_new();
        char cls[24];
        g_snprintf(cls, sizeof cls, "millerx-swatch-%u", i);
        GtkStyleContext *sc = gtk_widget_get_style_context(b);
        gtk_style_context_add_class(sc, "millerx-swatch");
        gtk_style_context_add_class(sc, cls);
        gtk_style_context_add_provider(sc, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        if (rules)
            g_string_append_printf(rules, ".%s { background: %s; }\n", cls, SWATCHES[i]);
        g_object_set_data(G_OBJECT(b), "colour", (gpointer) SWATCHES[i]);
        gtk_widget_set_tooltip_text(b, SWATCHES[i]);
        g_signal_connect(b, "clicked", G_CALLBACK(swatch_clicked), s);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    }
    if (rules) {
        gtk_css_provider_load_from_data(css, rules->str, -1, NULL);
        g_string_free(rules, TRUE);
    }
    return box;
}

/* ---- highlight for every GTK 3 program ------------------------------------------------- */
static void hl_system_clicked(GtkButton *b, Set *s)
{
    gboolean add = GTK_WIDGET(b) == s->hl_sys;
    char *msg = add ? g_strdup_printf("%s goes into ~/.config/gtk-3.0/gtk.css, in a block marked as "
                                      "millerX's; the rest of that file stays as it is. GTK 3 programs "
                                      "use it the next time they start. You can remove it here again.",
                                      LOOK.highlight)
                    : g_strdup("millerX's block is taken out of ~/.config/gtk-3.0/gtk.css; the rest of "
                               "that file stays. GTK 3 programs use the theme's colour again the next "
                               "time they start.");
    if (win_confirm(s->w, add ? "Use this highlight in all GTK programs?" : "Remove it from all GTK programs?",
                    msg, add ? "Use it" : "Remove")) {
        GError *err = NULL;
        if (!highlight_system_set(add ? LOOK.highlight : NULL, &err)) {
            win_error(s->w, "Couldn't write ~/.config/gtk-3.0/gtk.css", err->message);
            g_error_free(err);
        }
        gtk_widget_set_sensitive(s->hl_unsys, highlight_system_has());
    }
    g_free(msg);
}

void dialog_settings(Win *w)
{
    GtkWidget *d = gtk_dialog_new_with_buttons("millerX settings", GTK_WINDOW(w->window), 0, "Close",
                                               GTK_RESPONSE_CLOSE, NULL);
    /* three tabs: Look (two columns), Thumbnails, Folders; every section a framed box */
    GdkRectangle area = { 0, 0, 0, 900 };
    GdkMonitor *mon = gdk_display_get_monitor_at_window(gtk_widget_get_display(w->window),
                                                        gtk_widget_get_window(w->window));
    if (mon)
        gdk_monitor_get_workarea(mon, &area);
    int max_h = MAX(300, area.height - 160);
    GtkWidget *nb = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), nb, TRUE, TRUE, 0);
    GtkWidget *look = tab(nb, "Look", max_h);
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(cols), left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cols), right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(look), cols, FALSE, FALSE, 0);
    GtkWidget *thumbs_tab = tab(nb, "Thumbnails", max_h);
    GtkWidget *folders_tab = tab(nb, "Folders", max_h);
    GtkWidget *grid = NULL;
    Set *s = g_new0(Set, 1);
    s->w = w;
    g_object_set_data_full(G_OBJECT(d), "set", s, g_free);
    int r = 0;

    new_column(left);
    section(&grid, &r, "Column dividers");
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
    section(&grid, &r, "Highlight colour");
    s->hl_auto = gtk_radio_button_new_with_label(NULL, "Follow the theme");
    s->hl_own = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s->hl_auto), "Own colour");
    s->hl_colour = gtk_color_button_new();
    GdkRGBA hrgba;
    gboolean hown = strcmp(LOOK.highlight, "auto") && gdk_rgba_parse(&hrgba, LOOK.highlight);
    if (!hown)
        gdk_rgba_parse(&hrgba, "#d4ff02");
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(s->hl_colour), &hrgba);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hown ? s->hl_own : s->hl_auto), TRUE);
    gtk_widget_set_sensitive(s->hl_colour, hown);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(hbox), s->hl_own, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), s->hl_colour, FALSE, FALSE, 0);
    GtkWidget *hvb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(hvb), s->hl_auto, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hvb), hbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hvb), swatches(s), FALSE, FALSE, 0);
    srow(grid, &r, "millerX", hvb);
    s->hl_sys = gtk_button_new_with_label("Use for all GTK programs…");
    s->hl_unsys = gtk_button_new_with_label("Remove from all");
    gtk_widget_set_sensitive(s->hl_sys, hown);
    gtk_widget_set_sensitive(s->hl_unsys, highlight_system_has());
    GtkWidget *sysb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(sysb), s->hl_sys, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sysb), s->hl_unsys, FALSE, FALSE, 0);
    srow(grid, &r, "System", sysb);
    new_column(right);
    section(&grid, &r, "Browsing");
    s->cw = gtk_spin_button_new_with_range(COL_MIN, 600, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->cw), LOOK.column_width);
    gtk_widget_set_tooltip_text(s->cw, "Where new columns start. A column you drag keeps its own width; "
                                       "double-click its divider to bring it back to this one.");
    srow(grid, &r, "Column width (px)", s->cw);
    s->ff = gtk_check_button_new_with_label("Folders first");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->ff), LOOK.folders_first);
    srow(grid, &r, "Sort by name", s->ff);
    s->sh = gtk_check_button_new_with_label("Show hidden files when millerX starts");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->sh), LOOK.show_hidden);
    srow(grid, &r, "Hidden files", s->sh);
    section(&grid, &r, "Preview");
    s->pw = gtk_spin_button_new_with_range(200, 900, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->pw), LOOK.preview_width);
    srow(grid, &r, "Minimum width (px)", s->pw);   /* it takes the rest of the window */
    s->names = gtk_check_button_new_with_label("Show names under thumbnails");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->names), LOOK.gallery_names);
    srow(grid, &r, "Gallery", s->names);
    new_column(thumbs_tab);
    section(&grid, &r, "Thumbnails");
    s->thumbs = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->thumbs), "always", "Always");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->thumbs), "local", "Not in network folders");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->thumbs), "never", "Never (plain icons)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->thumbs), LOOK.thumbs == THUMBS_LOCAL ? "local"
                                                          : LOOK.thumbs == THUMBS_NEVER ? "never" : "always");
    gtk_widget_set_tooltip_text(s->thumbs, "Pictures in the gallery under a folder. A file you click is always "
                                           "shown big. Network folders (NFS, Samba, sshfs) can be slow with them.");
    srow(grid, &r, "Show", s->thumbs);
    section(&grid, &r, "Cache (shared with Thunar)");
    s->cache_size = gtk_label_new("measuring…");
    gtk_label_set_xalign(GTK_LABEL(s->cache_size), 0);
    s->cache_clear = gtk_button_new_with_label("Clear…");
    GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(sbox), s->cache_size, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sbox), s->cache_clear, FALSE, FALSE, 0);
    srow(grid, &r, "Now", sbox);
    s->cache_mb = gtk_spin_button_new_with_range(50, 100000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->cache_mb), LOOK.cache_max_mb);
    srow(grid, &r, "Keep at most (MB)", s->cache_mb);
    s->cache_days = gtk_spin_button_new_with_range(7, 3650, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->cache_days), LOOK.cache_days);
    srow(grid, &r, "Forget unused after (days)", s->cache_days);
    s->cache_auto = gtk_check_button_new_with_label("Once a day when millerX starts");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->cache_auto), LOOK.cache_auto);
    srow(grid, &r, "Clean up", s->cache_auto);
    thumbcache_measure(cache_measured, g_object_ref(s->cache_size));
    section(&grid, &r, "Optional helpers (millerX works without them; untick to not use one)");
    helper_rows(grid, &r, s);
    new_column(folders_tab);
    section(&grid, &r, "Folders sorted their own way (all others: by name)");
    s->sort_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *sl_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sl_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sl_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sl_scroll), MAX(220, max_h - 120));
    gtk_container_add(GTK_CONTAINER(sl_scroll), s->sort_list);
    gtk_grid_attach(GTK_GRID(grid), sl_scroll, 0, r++, 2, 1);
    fill_sort_list(s);

    g_signal_connect(s->auto_rb, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->colour, "color-set", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->width, "value-changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->style, "changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->pw, "value-changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->cw, "value-changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->ff, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->sh, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->names, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->thumbs, "changed", G_CALLBACK(settings_changed), s);
    for (int i = 0; i < 3; i++)
        if (s->use[i])
            g_signal_connect(s->use[i], "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->hl_auto, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->hl_colour, "color-set", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->hl_sys, "clicked", G_CALLBACK(hl_system_clicked), s);
    g_signal_connect(s->hl_unsys, "clicked", G_CALLBACK(hl_system_clicked), s);
    g_signal_connect(s->cache_mb, "value-changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->cache_days, "value-changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->cache_auto, "toggled", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->cache_clear, "clicked", G_CALLBACK(cache_clear_clicked), s);
    g_signal_connect(d, "response", G_CALLBACK(settings_close), s);
    SECTIONS = NULL;
    g_clear_object(&LABELS);           /* the labels keep the group alive */
    gtk_widget_show_all(d);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), LAST_TAB);
    g_signal_connect(nb, "switch-page", G_CALLBACK(on_tab), NULL);
}

void dialog_about(Win *w)
{
    gtk_show_about_dialog(GTK_WINDOW(w->window), "program-name", "millerX", "version", MILLERX_VERSION,
                          "comments", "Files in Miller columns, from / to where you are.",
                          "logo-icon-name", "millerx", "modal", TRUE, NULL);
}
