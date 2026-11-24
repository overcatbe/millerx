/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Conflict question, properties, settings, about. */
#include "millerx.h"
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
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
                                          GTK_BUTTONS_NONE, _("“%s” already exists"), base);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), _("in %s.\n%s"), where,
        merge ? _("Merge the folders? Files with the same name will be replaced.")
              : _("Replace the existing file?"));
    GtkWidget *allc = gtk_check_button_new_with_label(_("Do this for all conflicts"));
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(d))), allc,
                       FALSE, FALSE, 0);
    gtk_widget_show(allc);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Cancel"), 1);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Skip"), 2);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Keep both"), 3);
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
    char **roots;         /* several folders to count (Properties of a selection), or NULL: path */
    guint64 total, files, dirs;
    gboolean running;
} Count;

static gboolean count_show(gpointer data)
{
    Count *c = data;
    if (!g_cancellable_is_cancelled(c->stop)) {
        char *sz = format_size_full(c->total);
        char *nf = g_strdup_printf(ngettext("%llu file", "%llu files", c->files),
                                   (unsigned long long) c->files);
        char *nd = g_strdup_printf(ngettext("%llu folder", "%llu folders", c->dirs),
                                   (unsigned long long) c->dirs);
        char *t = g_strdup_printf("%s\n%s, %s%s", sz, nf, nd, c->running ? " …" : "");
        g_free(nf);
        g_free(nd);
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
    u->roots = NULL;
    u->running = running;
    g_idle_add(count_show, u);
}

static gpointer count_thread(gpointer data)
{
    Count *c = data;
    GPtrArray *stack = g_ptr_array_new_with_free_func(g_free);
    if (c->roots)
        for (char **r = c->roots; *r; r++)
            g_ptr_array_add(stack, g_strdup(*r));
    else
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
    g_strfreev(c->roots);
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
    char *t = g_strdup_printf("%s   %04o%s", s, (unsigned) (m & 07777), p->mine ? "" : _("   (not yours, read only)"));
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
        win_error(p->w, _("Couldn't change permissions"), g_strerror(errno));
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
        win_error(w, _("No properties"), err->message);
        g_error_free(err);
        return;
    }
    GFileInfo *linfo = g_file_query_info(f, "standard::is-symlink,standard::symlink-target",
                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    gboolean isdir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    const char *dname = g_file_info_get_display_name(info);
    char *title = g_strdup_printf(_("Properties: %s"), dname);
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), 0, _("Close"),
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
        prow(&r, _("Link to"), t ? t : "?", NULL);
    }
    GCancellable *stop = g_cancellable_new();
    g_signal_connect_data(d, "response", G_CALLBACK(on_props_response), stop, unref_notify, 0);
    if (isdir) {
        GtkWidget *size_l = prow(&r, _("Size"), _("counting …"), NULL);
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
            prow(&r, _("Free space"), t, NULL);
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

/* ---- compare two files ------------------------------------------------------------------------- */
static void compare_text(Win *w, const char *a, const char *b)
{
    gboolean dirs = g_file_test(a, G_FILE_TEST_IS_DIR) && g_file_test(b, G_FILE_TEST_IS_DIR);
    const char *argv[] = { "diff", dirs ? "-ru" : "-u", a, b, NULL };
    char *out = NULL, *err = NULL;
    int status = 0;
    GError *e = NULL;
    if (!g_spawn_sync(NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &status, &e)) {
        win_error(w, _("Couldn't compare"), e->message);
        g_error_free(e);
        return;
    }
    char *ta = g_path_get_basename(a), *tb = g_path_get_basename(b);
    char *title = g_strdup_printf(_("%s and %s"), ta, tb);
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), 0, _("Close"), GTK_RESPONSE_CLOSE, NULL);
    g_free(title);
    g_free(ta);
    g_free(tb);
    gtk_window_set_default_size(GTK_WINDOW(d), 760, 520);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    if (out && *out) {                  /* the differences, as diff writes them */
        GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
        GtkWidget *tv = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
        char *valid = g_utf8_make_valid(out, -1);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), valid, -1);
        g_free(valid);
        gtk_container_add(GTK_CONTAINER(sc), tv);
        gtk_box_pack_start(GTK_BOX(area), sc, TRUE, TRUE, 0);
    } else {
        GtkWidget *l = gtk_label_new(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? _("The two are the same.")
                                                                                   : (err && *err ? err : _("diff found nothing to show.")));
        g_object_set(l, "margin", 20, NULL);
        gtk_box_pack_start(GTK_BOX(area), l, TRUE, TRUE, 0);
        gtk_window_set_default_size(GTK_WINDOW(d), 360, -1);
    }
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
    g_free(out);
    g_free(err);
}

void compare_files(Win *w, GFile *fa, GFile *fb)
{
    char *a = g_file_get_path(fa), *b = g_file_get_path(fb);
    if (!a || !b) {
        win_error(w, _("Couldn't compare"), _("Only files on this computer can be compared."));
        g_free(a);
        g_free(b);
        return;
    }
    char **argv = NULL;
    const char *setting = LOOK.compare && *LOOK.compare ? LOOK.compare : "auto";
    if (strcmp(setting, "auto")) {      /* your own command: the two files at its end */
        int argc = 0;
        GError *e = NULL;
        if (!g_shell_parse_argv(setting, &argc, &argv, &e)) {
            win_error(w, _("Couldn't compare"), e->message);
            g_error_free(e);
            g_free(a);
            g_free(b);
            return;
        }
    } else {
        const char *tools[] = { "meld", "kdiff3", "kompare", "diffuse" };
        for (int i = 0; i < 4 && !argv; i++) {
            char *found = g_find_program_in_path(tools[i]);
            if (found) {
                argv = g_new0(char *, 2);
                argv[0] = found;
            }
        }
    }
    if (!argv) {                        /* none: the differences as text */
        compare_text(w, a, b);
        g_free(a);
        g_free(b);
        return;
    }
    guint n = g_strv_length(argv);
    argv = g_renew(char *, argv, n + 3);
    argv[n] = a;
    argv[n + 1] = b;
    argv[n + 2] = NULL;
    GError *e = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e)) {
        win_error(w, _("Couldn't compare"), e->message);
        g_error_free(e);
    }
    g_strfreev(argv);                   /* a and b with it */
}

/* ---- properties of several items: one window for all of them ---------------------------------- */
typedef struct {
    Win *w;
    GPtrArray *paths;       /* the selected items */
    GtkWidget *boxes[9];
} MultiPerm;

static void multi_free(MultiPerm *m)
{
    g_ptr_array_unref(m->paths);
    g_free(m);
}

/* each box: all have it (ticked), none (clear) or some ("mixed": inconsistent) */
static void multi_show(MultiPerm *m)
{
    for (int b = 0; b < 9; b++) {
        int bit = 1 << (8 - b), on = 0, off = 0;
        for (guint i = 0; i < m->paths->len; i++) {
            struct stat st;
            if (stat(g_ptr_array_index(m->paths, i), &st) == 0)
                (st.st_mode & bit) ? on++ : off++;
        }
        GtkToggleButton *cb = GTK_TOGGLE_BUTTON(m->boxes[b]);
        g_signal_handlers_block_matched(cb, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, m);
        gtk_toggle_button_set_inconsistent(cb, on && off);
        gtk_toggle_button_set_active(cb, on && !off);
        g_signal_handlers_unblock_matched(cb, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, m);
    }
}

/* a click sets the bit on every item (or takes it from every item), "mixed" or not */
static void multi_toggled(GtkToggleButton *cb, MultiPerm *m)
{
    int bit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cb), "bit"));
    gboolean set = gtk_toggle_button_get_active(cb) || gtk_toggle_button_get_inconsistent(cb);
    int failed = 0;
    const char *why = NULL;
    for (guint i = 0; i < m->paths->len; i++) {
        const char *path = g_ptr_array_index(m->paths, i);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        mode_t mode = set ? (st.st_mode | bit) : (st.st_mode & ~bit);
        if (chmod(path, mode & 07777) != 0) {
            failed++;
            why = g_strerror(errno);
        }
    }
    if (failed) {
        char *t = g_strdup_printf(ngettext("%d item wasn't changed: %s", "%d items weren't changed: %s", failed),
                                  failed, why);
        win_error(m->w, _("Couldn't change permissions"), t);
        g_free(t);
    }
    multi_show(m);                  /* as they really are now */
}

void dialog_properties_multi(Win *w, GList *files)
{
    guint n = g_list_length(files);
    MultiPerm *m = g_new0(MultiPerm, 1);
    m->w = w;
    m->paths = g_ptr_array_new_with_free_func(g_free);
    guint64 file_bytes = 0, nfiles = 0, ndirs = 0;
    GPtrArray *dirs = g_ptr_array_new();
    char *owner = NULL, *parent = NULL;
    gboolean same_owner = TRUE, same_parent = TRUE, all_mine = TRUE;
    GIcon *icon = NULL;
    for (GList *l = files; l; l = l->next) {
        char *path = g_file_get_path(l->data);
        if (!path)
            continue;
        GFileInfo *info = g_file_query_info(l->data, "standard::*,unix::*,owner::*", G_FILE_QUERY_INFO_NONE,
                                            NULL, NULL);
        if (!info) {
            g_free(path);
            continue;
        }
        gboolean isdir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        if (isdir) {
            ndirs++;
            g_ptr_array_add(dirs, g_strdup(path));
        } else {
            nfiles++;
            file_bytes += (guint64) g_file_info_get_size(info);
        }
        if (!icon && g_file_info_get_icon(info))
            icon = g_object_ref(g_file_info_get_icon(info));
        const char *ou = g_file_info_get_attribute_string(info, "owner::user");
        const char *og = g_file_info_get_attribute_string(info, "owner::group");
        char *o = g_strdup_printf("%s : %s", ou ? ou : "?", og ? og : "?");
        if (!owner)
            owner = o;
        else {
            same_owner &= !strcmp(owner, o);
            g_free(o);
        }
        all_mine &= g_file_info_get_attribute_uint32(info, "unix::uid") == getuid() || getuid() == 0;
        GFile *par = g_file_get_parent(l->data);
        char *pp = par ? file_path(par) : g_strdup("/");
        g_clear_object(&par);
        if (!parent)
            parent = pp;
        else {
            same_parent &= !strcmp(parent, pp);
            g_free(pp);
        }
        g_ptr_array_add(m->paths, path);
        g_object_unref(info);
    }
    char *title = g_strdup_printf(ngettext("Properties: %u item", "Properties: %u items", n), n);
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), 0, _("Close"), GTK_RESPONSE_CLOSE, NULL);
    g_free(title);
    gtk_window_set_default_size(GTK_WINDOW(d), 460, -1);
    Rows r = { gtk_grid_new(), 1 };
    gtk_grid_set_column_spacing(GTK_GRID(r.grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(r.grid), 6);
    g_object_set(r.grid, "margin", 14, NULL);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), r.grid, TRUE, TRUE, 0);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *img = icon ? gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DIALOG) : gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    g_clear_object(&icon);
    GtkWidget *name = gtk_label_new(NULL);
    char *count = g_strdup_printf(ngettext("%u item", "%u items", n), n);
    char *mk = g_markup_printf_escaped("<b>%s</b>", count);
    gtk_label_set_markup(GTK_LABEL(name), mk);
    g_free(mk);
    g_free(count);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_box_pack_start(GTK_BOX(head), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), name, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(r.grid), head, 0, 0, 2, 1);

    char *nf = g_strdup_printf(ngettext("%llu file", "%llu files", nfiles), (unsigned long long) nfiles);
    char *nd = g_strdup_printf(ngettext("%llu folder", "%llu folders", ndirs), (unsigned long long) ndirs);
    char *kind = g_strdup_printf("%s, %s", nf, nd);
    prow(&r, _("Kind"), kind, NULL);
    g_free(kind);
    g_free(nf);
    g_free(nd);
    if (parent)
        prow(&r, _("Location"), same_parent ? parent : _("several folders"), NULL);
    GCancellable *stop = g_cancellable_new();
    g_signal_connect_data(d, "response", G_CALLBACK(on_props_response), stop, unref_notify, 0);
    if (dirs->len) {                /* the folders' contents counted in the background, the files' added */
        GtkWidget *size_l = prow(&r, _("Size"), _("counting …"), NULL);
        Count *c = g_new0(Count, 1);
        c->label = g_object_ref(size_l);
        c->stop = g_object_ref(stop);
        g_ptr_array_add(dirs, NULL);
        c->roots = (char **) g_ptr_array_steal(dirs, NULL);
        c->total = file_bytes;
        c->files = nfiles;
        c->dirs = ndirs;
        g_thread_unref(g_thread_new("millerx-count", count_thread, c));
    } else {
        char *sz = format_size_full(file_bytes);
        prow(&r, _("Size"), sz, NULL);
        g_free(sz);
    }
    g_ptr_array_unref(dirs);
    prow(&r, _("Owner"), same_owner && owner ? owner : _("mixed"), NULL);

    /* permissions: one box per bit for all of them; a dash where they differ */
    GtkWidget *perm = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(perm), 10);
    gtk_grid_set_row_spacing(GTK_GRID(perm), 2);
    const char *x = !nfiles ? _("Enter") : !ndirs ? _("Exec") : _("Exec / Enter");
    const char *heads[] = { _("Read"), _("Write"), x };
    const char *who[] = { _("Owner"), _("Group"), _("Others") };
    for (int c = 0; c < 3; c++)
        gtk_grid_attach(GTK_GRID(perm), gtk_label_new(heads[c]), c + 1, 0, 1, 1);
    for (int rr = 0; rr < 3; rr++) {
        GtkWidget *l = gtk_label_new(who[rr]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(perm), l, 0, rr + 1, 1, 1);
        for (int c = 0; c < 3; c++) {
            int b = rr * 3 + c;
            GtkWidget *cb = gtk_check_button_new();
            gtk_widget_set_sensitive(cb, all_mine);
            gtk_widget_set_halign(cb, GTK_ALIGN_CENTER);
            g_object_set_data(G_OBJECT(cb), "bit", GINT_TO_POINTER(1 << (8 - b)));
            g_signal_connect(cb, "toggled", G_CALLBACK(multi_toggled), m);
            gtk_grid_attach(GTK_GRID(perm), cb, c + 1, rr + 1, 1, 1);
            m->boxes[b] = cb;
        }
    }
    multi_show(m);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(box), perm, FALSE, FALSE, 0);
    GtkWidget *note = gtk_label_new(all_mine ? _("A dash: some have it, some don't. A click sets it for all of them.")
                                             : _("Not all of them are yours: read only."));
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
    prow(&r, _("Permissions"), NULL, box);
    g_object_set_data_full(G_OBJECT(d), "multi", m, (GDestroyNotify) multi_free);
    g_free(owner);
    g_free(parent);
    gtk_widget_show_all(d);
}

/* ---- settings --------------------------------------------------------------------------------- */
typedef struct {
    Win *w;
    GtkWidget *auto_rb, *own_rb, *colour, *width, *style, *pw, *cw, *ff, *sh;
    GtkWidget *cache_size, *cache_clear, *cache_mb, *cache_days, *cache_auto;
    GtkWidget *sort_list, *names, *thumbs, *tsize, *lang, *lang_note;
    GtkWidget *use[4];         /* the optional helpers, when installed; NULL when not */
    GtkWidget *compare;        /* what compares two files */
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
    nl.language = (char *) gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->lang));   /* copied by app_apply_look */
    gtk_widget_set_visible(s->lang_note, g_strcmp0(nl.language, LANGUAGE_AT_START ? LANGUAGE_AT_START : "system"));
    nl.gallery_names = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->names));
    nl.compare = (char *) gtk_entry_get_text(GTK_ENTRY(s->compare));   /* copied by app_apply_look */
    gboolean *uses[] = { &nl.use_ffmpeg, &nl.use_pdftoppm, &nl.use_pkexec, &nl.use_blender };
    for (int i = 0; i < 4; i++)
        if (s->use[i])
            *uses[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->use[i]));
    const char *tid = gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->thumbs));
    nl.thumbs = !g_strcmp0(tid, "local") ? THUMBS_LOCAL : !g_strcmp0(tid, "never") ? THUMBS_NEVER : THUMBS_ALWAYS;
    const char *zid = gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->tsize));
    nl.thumb_size = !g_strcmp0(zid, "small") ? TSIZE_SMALL : !g_strcmp0(zid, "large") ? TSIZE_LARGE : TSIZE_MEDIUM;
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
        win_error(s->w, _("Couldn't save the settings"), err->message);
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

#ifdef ENABLE_NLS
/* "Your own…": pick a language file; it is checked, copied where millerX looks and chosen */
static void on_own_languages(GtkButton *b, Set *s)
{
    GtkWindow *top = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(b)));
    GtkWidget *d = gtk_file_chooser_dialog_new(_("Add a language file"), top, GTK_FILE_CHOOSER_ACTION_OPEN,
                                               _("Cancel"), GTK_RESPONSE_CANCEL, _("Add"), GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, _("Language files (.po, .mo)"));
    gtk_file_filter_add_pattern(f, "*.po");
    gtk_file_filter_add_pattern(f, "*.mo");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    const char *dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);   /* where a download lands */
    char *dl2 = dl ? g_strdup(dl) : g_build_filename(g_get_home_dir(), "Downloads", NULL);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d),
                                        g_file_test(dl2, G_FILE_TEST_IS_DIR) ? dl2 : g_get_home_dir());
    g_free(dl2);
    char *path = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT
                 ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)) : NULL;
    gtk_widget_destroy(d);
    if (!path)
        return;
    char *err = NULL, *name = own_language_add(path, &err);
    if (!name) {
        char *base = g_path_get_basename(path);
        GtkWidget *m = gtk_message_dialog_new(top, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                              _("Couldn't add “%s”"), base);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", err);
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        g_free(base);
    } else {
        char *id = g_strconcat("own:", name, NULL);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->lang), id)) {   /* new: into the list */
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->lang), id, name);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->lang), id);
        }
        g_free(id);
    }
    g_free(err);
    g_free(name);
    g_free(path);
}
#endif

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
    { "ffmpeg", N_("video frames, music covers, AVIF and HEIC pictures"),
      N_("videos and music show their icon, AVIF/HEIC too"), N_("ffmpeg (Slackware: l/ffmpeg)") },
    { "pdftoppm", N_("the first page of PDFs"),
      N_("PDFs show their icon"), N_("poppler (Debian/Ubuntu: poppler-utils)") },
    { "pkexec", N_("“Open as root”"),
      N_("no root window"), N_("polkit, plus an authentication agent") },
    { "blender-thumbnailer", N_("the pictures saved in Blender (.blend) files"),
      N_(".blend files show their icon"), N_("Blender (it comes with it; also found in /opt/blender*)") },
};

/* found: a tick box to use it or not; missing: what it would give and where to get it */
static void helper_rows(GtkWidget *grid, int *r, Set *s)
{
    const gboolean on[] = { LOOK.use_ffmpeg, LOOK.use_pdftoppm, LOOK.use_pkexec, LOOK.use_blender };
    G_STATIC_ASSERT(G_N_ELEMENTS(on) == G_N_ELEMENTS(HELPERS));
    for (guint i = 0; i < G_N_ELEMENTS(HELPERS); i++) {
        char *found = !strcmp(HELPERS[i].prog, "blender-thumbnailer") ? blender_thumbnailer()
                                                                       : g_find_program_in_path(HELPERS[i].prog);
        if (found) {
            s->use[i] = gtk_check_button_new_with_label(_(HELPERS[i].gives));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->use[i]), on[i]);
            if (strncmp(found, "/usr/", 5))          /* /opt/blender-4.2/…: say which one */
                gtk_widget_set_tooltip_text(s->use[i], found);
            srow(grid, r, HELPERS[i].prog, s->use[i]);
            g_free(found);
            continue;
        }
        char *m = g_markup_printf_escaped(_("✗ not installed: %s.\n<small>Install %s\nfor %s.</small>"),
                                      _(HELPERS[i].without), _(HELPERS[i].package), _(HELPERS[i].gives));
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
    char *t = removed ? g_strdup_printf(ngettext("%s in %u thumbnail (%u deleted)", "%s in %u thumbnails (%u deleted)",
                                                files), sz, files, removed)
                      : g_strdup_printf(ngettext("%s in %u thumbnail", "%s in %u thumbnails", files), sz, files);
    gtk_label_set_text(GTK_LABEL(l), t);
    g_free(t);
    g_free(sz);
    g_object_unref(l);
}

static void cache_clear_clicked(GtkButton *b, Set *s)
{
    (void) b;
    if (!win_confirm(s->w, _("Clear the thumbnail cache?"),
                     _("All thumbnails in ~/.cache/thumbnails are deleted. Thunar and file dialogs use them "
                     "too; everything is made again when it's needed."), _("Clear")))
        return;
    gtk_label_set_text(GTK_LABEL(s->cache_size), _("clearing…"));
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
        GtkWidget *l = gtk_label_new(_("None. Right-click a folder → Sort … to give it its own order."));
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
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Name"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Date (newest first)"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Size (largest first)"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Type"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), fs.by);
        GtkWidget *rev = gtk_check_button_new_with_label(_("Reversed"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rev), fs.reverse);
        GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(x, _("Sort by name again"));
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
    char *msg = add ? g_strdup_printf(_("%s goes into ~/.config/gtk-3.0/gtk.css, in a block marked as "
                                      "millerX's; the rest of that file stays as it is. GTK 3 programs "
                                      "use it the next time they start. You can remove it here again."),
                                      LOOK.highlight)
                    : g_strdup(_("millerX's block is taken out of ~/.config/gtk-3.0/gtk.css; the rest of "
                               "that file stays. GTK 3 programs use the theme's colour again the next "
                               "time they start."));
    if (win_confirm(s->w, add ? _("Use this highlight in all GTK programs?") : _("Remove it from all GTK programs?"),
                    msg, add ? _("Use it") : _("Remove"))) {
        GError *err = NULL;
        if (!highlight_system_set(add ? LOOK.highlight : NULL, &err)) {
            win_error(s->w, _("Couldn't write ~/.config/gtk-3.0/gtk.css"), err->message);
            g_error_free(err);
        }
        gtk_widget_set_sensitive(s->hl_unsys, highlight_system_has());
    }
    g_free(msg);
}

void dialog_settings(Win *w)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(_("millerX settings"), GTK_WINDOW(w->window), 0, _("Close"),
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
    GtkWidget *look = tab(nb, _("Look"), max_h);
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(cols), left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cols), right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(look), cols, FALSE, FALSE, 0);
    GtkWidget *thumbs_tab = tab(nb, _("Thumbnails"), max_h);
    GtkWidget *folders_tab = tab(nb, _("Folders"), max_h);
    GtkWidget *grid = NULL;
    Set *s = g_new0(Set, 1);
    s->w = w;
    g_object_set_data_full(G_OBJECT(d), "set", s, g_free);
    int r = 0;

    new_column(left);
    section(&grid, &r, _("Column dividers"));
    s->auto_rb = gtk_radio_button_new_with_label(NULL, _("Follow the theme (20 % lighter / darker)"));
    s->own_rb = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s->auto_rb), _("Own colour"));
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
    srow(grid, &r, _("Colour"), vb);
    s->width = gtk_spin_button_new_with_range(1, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->width), LOOK.divider_width);
    srow(grid, &r, _("Width (px)"), s->width);
    s->style = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->style), "solid", _("Solid"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->style), "fade", _("Fading at the ends"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->style), LOOK.fade ? "fade" : "solid");
    srow(grid, &r, _("Style"), s->style);
    section(&grid, &r, _("Highlight colour"));
    s->hl_auto = gtk_radio_button_new_with_label(NULL, _("Follow the theme"));
    s->hl_own = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s->hl_auto), _("Own colour"));
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
    srow(grid, &r, _("millerX"), hvb);
    s->hl_sys = gtk_button_new_with_label(_("Use for all GTK programs…"));
    s->hl_unsys = gtk_button_new_with_label(_("Remove from all"));
    gtk_widget_set_sensitive(s->hl_sys, hown);
    gtk_widget_set_sensitive(s->hl_unsys, highlight_system_has());
    GtkWidget *sysb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(sysb), s->hl_sys, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sysb), s->hl_unsys, FALSE, FALSE, 0);
    srow(grid, &r, _("System"), sysb);
    new_column(right);
    section(&grid, &r, _("Browsing"));
    s->cw = gtk_spin_button_new_with_range(COL_MIN, 600, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->cw), LOOK.column_width);
    gtk_widget_set_tooltip_text(s->cw, _("Where new columns start. A column you drag keeps its own width; "
                                       "double-click its divider to bring it back to this one."));
    srow(grid, &r, _("Column width (px)"), s->cw);
    s->ff = gtk_check_button_new_with_label(_("Folders first"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->ff), LOOK.folders_first);
    srow(grid, &r, _("Sort by name"), s->ff);
    s->sh = gtk_check_button_new_with_label(_("Show hidden files when millerX starts"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->sh), LOOK.show_hidden);
    srow(grid, &r, _("Hidden files"), s->sh);
    s->lang = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->lang), "system", _("System"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->lang), "en", _("English"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->lang), "wizard", _("Wizard (English, of a sort)"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->lang), "wienerisch", _("Wienerisch (Viennese slang)"));
#ifdef ENABLE_NLS
    GPtrArray *own = own_languages_list();          /* people's own files, under their file names */
    for (guint i = 0; i < own->len; i++) {
        char *id = g_strconcat("own:", (char *) own->pdata[i], NULL);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->lang), id, own->pdata[i]);
        g_free(id);
    }
    g_ptr_array_unref(own);
#endif
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->lang), LOOK.language ? LOOK.language : "system"))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->lang), "system");   /* its file is gone */
    gtk_widget_set_tooltip_text(s->lang, _("System: the language your computer is set to (English where there is "
                                           "no translation). Takes effect the next time millerX starts."));
#ifdef ENABLE_NLS
    GtkWidget *lrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(lrow), s->lang, FALSE, FALSE, 0);
    GtkWidget *ob = gtk_button_new_with_label(_("Your own…"));
    char *tip = g_strdup_printf(_("Add a language file (.po or .mo), for example one from millerX's extras "
                                  "on GitHub. To write your own, copy %s and fill in the msgstr lines; "
                                  "lines you leave empty stay English."),
                                MILLERX_POT);
    gtk_widget_set_tooltip_text(ob, tip);
    g_free(tip);
    g_signal_connect(ob, "clicked", G_CALLBACK(on_own_languages), s);
    gtk_box_pack_start(GTK_BOX(lrow), ob, FALSE, FALSE, 0);
    srow(grid, &r, _("Language"), lrow);
#else
    srow(grid, &r, _("Language"), s->lang);
#endif
    /* a language other than the one millerX started with: how to get it now */
    s->lang_note = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ln = gtk_label_new(_("Takes effect after a restart."));
    gtk_widget_set_opacity(ln, 0.8);
    GtkWidget *rb = gtk_button_new_with_label(_("Restart now"));
    g_signal_connect_swapped(rb, "clicked", G_CALLBACK(app_restart), s->w->app);
    gtk_box_pack_start(GTK_BOX(s->lang_note), ln, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->lang_note), rb, FALSE, FALSE, 0);
    srow(grid, &r, "", s->lang_note);
    section(&grid, &r, _("Preview"));
    s->pw = gtk_spin_button_new_with_range(200, 900, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->pw), LOOK.preview_width);
    srow(grid, &r, _("Minimum width (px)"), s->pw);   /* it takes the rest of the window */
    s->names = gtk_check_button_new_with_label(_("Show names under thumbnails"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->names), LOOK.gallery_names);
    srow(grid, &r, _("Gallery"), s->names);
    new_column(thumbs_tab);
    section(&grid, &r, _("Thumbnails"));
    s->thumbs = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->thumbs), "always", _("Always"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->thumbs), "local", _("Not in network folders"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->thumbs), "never", _("Never (plain icons)"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->thumbs), LOOK.thumbs == THUMBS_LOCAL ? "local"
                                                          : LOOK.thumbs == THUMBS_NEVER ? "never" : "always");
    gtk_widget_set_tooltip_text(s->thumbs, _("Pictures in the gallery under a folder. A file you click is always "
                                           "shown big. Network folders (NFS, Samba, sshfs) can be slow with them."));
    srow(grid, &r, _("Show"), s->thumbs);
    s->tsize = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->tsize), "small", _("Small (about 100 px)"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->tsize), "medium", _("Medium (about 200 px)"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->tsize), "large", _("Large (about 400 px)"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->tsize), LOOK.thumb_size == TSIZE_SMALL ? "small"
                                                         : LOOK.thumb_size == TSIZE_LARGE ? "large" : "medium");
    gtk_widget_set_tooltip_text(s->tsize, _("How big the gallery's pictures are. Large ones are made again the "
                                          "first time (bigger thumbnails, a bigger cache); a wide pane gets "
                                          "more of them per row, not bigger ones."));
    srow(grid, &r, _("Size"), s->tsize);
    section(&grid, &r, _("Cache (shared with Thunar)"));
    s->cache_size = gtk_label_new(_("measuring…"));
    gtk_label_set_xalign(GTK_LABEL(s->cache_size), 0);
    s->cache_clear = gtk_button_new_with_label(_("Clear…"));
    GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(sbox), s->cache_size, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sbox), s->cache_clear, FALSE, FALSE, 0);
    srow(grid, &r, _("Now"), sbox);
    s->cache_mb = gtk_spin_button_new_with_range(50, 100000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->cache_mb), LOOK.cache_max_mb);
    srow(grid, &r, _("Keep at most (MB)"), s->cache_mb);
    s->cache_days = gtk_spin_button_new_with_range(7, 3650, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->cache_days), LOOK.cache_days);
    srow(grid, &r, _("Forget unused after (days)"), s->cache_days);
    s->cache_auto = gtk_check_button_new_with_label(_("Once a day when millerX starts"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->cache_auto), LOOK.cache_auto);
    srow(grid, &r, _("Clean up"), s->cache_auto);
    thumbcache_measure(cache_measured, g_object_ref(s->cache_size));
    section(&grid, &r, _("Optional helpers (millerX works without them; untick to not use one)"));
    helper_rows(grid, &r, s);
    s->compare = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(s->compare), LOOK.compare ? LOOK.compare : "auto");
    gtk_widget_set_tooltip_text(s->compare, _("Two files selected → Compare. \"auto\": meld, kdiff3, kompare or diffuse, "
                                              "whichever is there; none: the differences as text. Or your own command "
                                              "(the two files are added at the end)."));
    srow(grid, &r, _("Compare with"), s->compare);
    new_column(folders_tab);
    section(&grid, &r, _("Folders sorted their own way (all others: by name)"));
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
    g_signal_connect(s->compare, "changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->thumbs, "changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->tsize, "changed", G_CALLBACK(settings_changed), s);
    g_signal_connect(s->lang, "changed", G_CALLBACK(settings_changed), s);
    for (int i = 0; i < 4; i++)
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
    gtk_widget_set_visible(s->lang_note, g_strcmp0(LOOK.language, LANGUAGE_AT_START ? LANGUAGE_AT_START : "system"));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), LAST_TAB);
    g_signal_connect(nb, "switch-page", G_CALLBACK(on_tab), NULL);
}

void dialog_about(Win *w)
{
    gtk_show_about_dialog(GTK_WINDOW(w->window), "program-name", "millerX", "version", MILLERX_VERSION,
                          "comments", _("Files in Miller columns, from / to where you are."),
                          "logo-icon-name", "millerx", "modal", TRUE, NULL);
}

/* ---- checksums of one file: MD5, SHA-1, SHA-256 in one read, on a worker thread ------------------ */
static const struct { GChecksumType type; const char *name; } SUMS[] = {
    { G_CHECKSUM_MD5, "MD5" }, { G_CHECKSUM_SHA1, "SHA-1" }, { G_CHECKSUM_SHA256, "SHA-256" },
};
#define N_SUMS G_N_ELEMENTS(SUMS)

typedef struct {
    int ref;
    gboolean dead;                 /* the window was closed */
    GFile *file;
    guint64 size;
    gint permille;                 /* read so far, 0-1000 (written by the worker) */
    gboolean ready;                /* hex[] is filled (set on the main thread, after the worker) */
    GCancellable *cancel;
    char *hex[N_SUMS];
    GtkWidget *dialog, *bar, *value[N_SUMS], *copy[N_SUMS], *entry, *verdict;
    guint tick;
} Ck;

static Ck *ck_ref(Ck *k)
{
    k->ref++;
    return k;
}

static void ck_unref(gpointer data)
{
    Ck *k = data;
    if (--k->ref)
        return;
    g_object_unref(k->file);
    g_object_unref(k->cancel);
    for (guint i = 0; i < N_SUMS; i++)
        g_free(k->hex[i]);
    g_free(k);
}

static void ck_work(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    Ck *k = data;
    GError *err = NULL;
    GFileInputStream *in = g_file_read(k->file, cancel, &err);
    if (!in) {
        g_task_return_error(task, err);
        return;
    }
    GChecksum *cs[N_SUMS];
    for (guint i = 0; i < N_SUMS; i++)
        cs[i] = g_checksum_new(SUMS[i].type);
    guchar *buf = g_malloc(1 << 20);
    guint64 done = 0;
    gssize n;
    while ((n = g_input_stream_read(G_INPUT_STREAM(in), buf, 1 << 20, cancel, &err)) > 0) {
        for (guint i = 0; i < N_SUMS; i++)
            g_checksum_update(cs[i], buf, n);
        done += n;
        if (k->size)
            g_atomic_int_set(&k->permille, (gint) MIN(1000, done * 1000 / k->size));
    }
    g_free(buf);
    g_object_unref(in);
    if (n == 0)                    /* the worker owns hex[] until it returns; the window reads it after */
        for (guint i = 0; i < N_SUMS; i++)
            k->hex[i] = g_strdup(g_checksum_get_string(cs[i]));
    for (guint i = 0; i < N_SUMS; i++)
        g_checksum_free(cs[i]);
    if (n < 0)
        g_task_return_error(task, err);
    else
        g_task_return_boolean(task, TRUE);
}

/* a pasted checksum: spaces, a trailing file name ("sha256sum" output) and case don't matter */
static void ck_compare(GtkEditable *e, Ck *k)
{
    char *t = g_ascii_strdown(gtk_entry_get_text(GTK_ENTRY(k->entry)), -1);
    g_strstrip(t);
    char *sp = strpbrk(t, " \t");
    if (sp)
        *sp = 0;
    const char *say = "";
    char *own = NULL;
    if (*t && k->ready) {
        say = _("✗ matches none of them");
        for (guint i = 0; i < N_SUMS; i++)
            if (!strcmp(t, k->hex[i])) {
                own = g_strdup_printf(_("✓ matches %s"), SUMS[i].name);
                break;
            }
    } else if (*t)
        say = _("…when the file is read");
    gtk_label_set_text(GTK_LABEL(k->verdict), own ? own : say);
    g_free(own);
    g_free(t);
}

static void ck_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Ck *k = data;
    GError *err = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), &err);
    if (!k->dead) {
        if (k->tick) {
            g_source_remove(k->tick);
            k->tick = 0;
        }
        gtk_widget_hide(k->bar);
        for (guint i = 0; i < N_SUMS; i++) {
            gtk_label_set_text(GTK_LABEL(k->value[i]), ok ? k->hex[i] : "—");
            gtk_widget_set_sensitive(k->copy[i], ok);
        }
        k->ready = ok;
        if (!ok) {
            char *m = g_strdup_printf(_("✗ couldn't read the file: %s"), err->message);
            gtk_label_set_text(GTK_LABEL(k->verdict), m);
            g_free(m);
        } else
            ck_compare(NULL, k);
    }
    g_clear_error(&err);
    ck_unref(k);
}

static gboolean ck_progress(gpointer data)
{
    Ck *k = data;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(k->bar), g_atomic_int_get(&k->permille) / 1000.0);
    return G_SOURCE_CONTINUE;
}

static void ck_copy(GtkButton *b, Ck *k)
{
    for (guint i = 0; i < N_SUMS; i++)
        if (GTK_WIDGET(b) == k->copy[i] && k->ready)
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), k->hex[i], -1);
}

static void ck_closed(GtkWidget *d, Ck *k)
{
    k->dead = TRUE;
    if (k->tick)
        g_source_remove(k->tick);
    k->tick = 0;
    g_cancellable_cancel(k->cancel);   /* a big ISO stops being read */
    ck_unref(k);
}

void dialog_checksums(Win *w, GFile *f)
{
    GFileInfo *fi = g_file_query_info(f, "standard::display-name,standard::size", G_FILE_QUERY_INFO_NONE,
                                      NULL, NULL);
    Ck *k = g_new0(Ck, 1);
    k->ref = 1;
    k->file = g_object_ref(f);
    k->size = fi ? (guint64) g_file_info_get_size(fi) : 0;
    k->cancel = g_cancellable_new();
    char *title = g_strdup_printf(_("Checksums: %s"), fi ? g_file_info_get_display_name(fi) : "");
    g_clear_object(&fi);
    k->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                            _("_Close"), GTK_RESPONSE_CLOSE, NULL);
    g_free(title);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    g_object_set(grid, "margin", 14, NULL);
    int r = 0;
    k->bar = gtk_progress_bar_new();
    gtk_grid_attach(GTK_GRID(grid), k->bar, 0, r++, 3, 1);
    for (guint i = 0; i < N_SUMS; i++) {
        GtkWidget *l = gtk_label_new(SUMS[i].name);
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        k->value[i] = gtk_label_new(_("reading…"));
        gtk_label_set_xalign(GTK_LABEL(k->value[i]), 0);
        gtk_label_set_selectable(GTK_LABEL(k->value[i]), TRUE);
        gtk_label_set_width_chars(GTK_LABEL(k->value[i]), 64);       /* a SHA-256 fits on one line */
        PangoAttrList *at = pango_attr_list_new();
        pango_attr_list_insert(at, pango_attr_family_new("monospace"));
        gtk_label_set_attributes(GTK_LABEL(k->value[i]), at);
        pango_attr_list_unref(at);
        k->copy[i] = gtk_button_new_with_label(_("Copy"));
        gtk_widget_set_sensitive(k->copy[i], FALSE);
        g_signal_connect(k->copy[i], "clicked", G_CALLBACK(ck_copy), k);
        gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), k->value[i], 1, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), k->copy[i], 2, r++, 1, 1);
    }
    GtkWidget *cl = gtk_label_new(_("Compare"));
    gtk_label_set_xalign(GTK_LABEL(cl), 1);
    k->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(k->entry), _("paste the checksum you were given"));
    gtk_grid_attach(GTK_GRID(grid), cl, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), k->entry, 1, r++, 2, 1);
    k->verdict = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(k->verdict), 0);
    gtk_grid_attach(GTK_GRID(grid), k->verdict, 1, r++, 2, 1);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(k->dialog))), grid);
    g_signal_connect(k->entry, "changed", G_CALLBACK(ck_compare), k);
    g_signal_connect(k->dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(k->dialog, "destroy", G_CALLBACK(ck_closed), k);
    gtk_widget_show_all(k->dialog);
    k->tick = g_timeout_add(200, ck_progress, k);
    GTask *t = g_task_new(NULL, k->cancel, ck_done, ck_ref(k));
    g_task_set_task_data(t, k, NULL);
    g_task_run_in_thread(t, ck_work);
    g_object_unref(t);
}
