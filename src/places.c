/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Bookmarks (the GTK file, shared with Thunar and the file dialogs) and drives. */
#include "millerx.h"
#include <mntent.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

/* ---- bookmarks --------------------------------------------------------------------------- */
typedef struct {
    char *uri, *label;
} Mark;

static char *bookmarks_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
}

static void mark_free(Mark *m)
{
    g_free(m->uri);
    g_free(m->label);
    g_free(m);
}

static GPtrArray *bookmarks_read(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify) mark_free);
    char *path = bookmarks_file(), *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (!*line)
                continue;
            Mark *m = g_new0(Mark, 1);
            char *sp = strchr(line, ' ');
            m->uri = sp ? g_strndup(line, sp - line) : g_strdup(line);
            m->label = sp ? g_strdup(sp + 1) : g_strdup("");
            g_ptr_array_add(a, m);
        }
        g_strfreev(lines);
    }
    g_free(data);
    g_free(path);
    return a;
}

static void bookmarks_write(Win *w, GPtrArray *a)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < a->len; i++) {
        Mark *m = g_ptr_array_index(a, i);
        g_string_append_printf(s, *m->label ? "%s %s\n" : "%s%s\n", m->uri, m->label);
    }
    char *path = bookmarks_file(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    GError *err = NULL;
    if (!g_file_set_contents(path, s->str, s->len, &err)) {   /* writes a temp file, then renames */
        win_error(w, _("Couldn't save the bookmarks"), err->message);
        g_error_free(err);
    }
    g_free(dir);
    g_free(path);
    g_string_free(s, TRUE);
}

void places_bookmark_add(Win *w, GFile *folder)
{
    char *uri = g_file_get_uri(folder), *p = file_path(folder);
    GPtrArray *a = bookmarks_read();
    gboolean have = FALSE;
    for (guint i = 0; i < a->len && !have; i++)
        have = !strcmp(((Mark *) g_ptr_array_index(a, i))->uri, uri);
    if (have) {
        win_status(w, _("Already bookmarked: %s"), p);
    } else {
        Mark *m = g_new0(Mark, 1);
        m->uri = g_strdup(uri);
        m->label = g_strdup("");
        g_ptr_array_add(a, m);
        bookmarks_write(w, a);
        win_status(w, _("Bookmarked: %s"), p);
    }
    g_ptr_array_unref(a);
    g_free(p);
    g_free(uri);
}

static void bookmark_remove(Win *w, const char *uri)
{
    GPtrArray *a = bookmarks_read();
    for (guint i = 0; i < a->len; i++)
        if (!strcmp(((Mark *) g_ptr_array_index(a, i))->uri, uri)) {
            g_ptr_array_remove_index(a, i);
            break;
        }
    bookmarks_write(w, a);
    g_ptr_array_unref(a);
    GFile *f = g_file_new_for_uri(uri);
    char *p = file_path(f);
    win_status(w, _("Bookmark removed: %s"), p);
    g_free(p);
    g_object_unref(f);
}

/* menu items carry what they need as object data */
/* the trash opens in a window of its own: close it and you are back where you were */
static void on_open_trash(GtkMenuItem *mi, Win *w)
{
    win_new(w->app, g_object_get_data(G_OBJECT(mi), "file"));
}

static void on_open_bookmark(GtkMenuItem *mi, Win *w)
{
    GFile *f = g_object_get_data(G_OBJECT(mi), "file");
    char *path = g_file_get_path(f);
    if (path && !g_file_query_exists(f, NULL))
        win_status(w, _("Not found: %s"), path);
    else
        win_open_path(w, f);
    g_free(path);
}

static void on_add_here(GtkMenuItem *mi, Win *w)
{
    places_bookmark_add(w, g_object_get_data(G_OBJECT(mi), "file"));
}

static void on_remove_here(GtkMenuItem *mi, Win *w)
{
    GFile *f = g_object_get_data(G_OBJECT(mi), "file");
    char *uri = g_file_get_uri(f);
    bookmark_remove(w, uri);
    g_free(uri);
}

/* the small ✕ at the end of a bookmark removes it and keeps the menu open */
static gboolean on_mark_release(GtkWidget *mi, GdkEventButton *ev, Win *w)
{
    GtkWidget *x = g_object_get_data(G_OBJECT(mi), "x");
    int xx, yy;
    if (ev->button != 1 || !gtk_widget_translate_coordinates(mi, x, (int) ev->x, (int) ev->y, &xx, &yy))
        return FALSE;
    GtkAllocation a;
    gtk_widget_get_allocation(x, &a);
    if (xx < -6 || xx >= a.width + 6) /* a little slack around the icon */
        return FALSE;
    GtkWidget *menu = gtk_widget_get_parent(mi);
    bookmark_remove(w, g_object_get_data(G_OBJECT(mi), "uri"));
    gtk_widget_destroy(mi);
    /* the last bookmark gone: show the placeholder like a fresh fill would */
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    gboolean any = FALSE;
    for (GList *l = kids; l; l = l->next)
        any |= g_object_get_data(G_OBJECT(l->data), "uri") != NULL;
    if (!any) {
        GtkWidget *none = menu_item_new(_("No bookmarks yet"), NULL, NULL);
        gtk_widget_set_sensitive(none, FALSE);
        gtk_menu_shell_prepend(GTK_MENU_SHELL(menu), none);
        gtk_widget_show_all(none);
    }
    g_list_free(kids);
    return TRUE;
}

static void clear_menu(GtkWidget *m)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(m));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
}

static GtkWidget *add(GtkWidget *menu, GtkWidget *mi, GFile *f, GCallback cb, Win *w)
{
    if (f)
        g_object_set_data_full(G_OBJECT(mi), "file", g_object_ref(f), g_object_unref);
    if (cb)
        g_signal_connect(mi, "activate", cb, w);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    return mi;
}

void places_fill_bookmarks(Win *w)
{
    GtkWidget *m = w->bm_menu;
    clear_menu(m);
    GPtrArray *a = bookmarks_read();
    GFile *cur = win_current_folder(w);
    char *cur_uri = g_file_get_uri(cur);
    gboolean cur_marked = FALSE;
    for (guint i = 0; i < a->len; i++) {
        Mark *mk = g_ptr_array_index(a, i);
        GFile *f = g_file_new_for_uri(mk->uri);
        char *path = g_file_get_path(f);
        char *name = *mk->label ? g_strdup(mk->label) : path ? g_file_get_basename(f) : g_strdup(mk->uri);
        gboolean exists = !path || g_file_query_exists(f, NULL);
        GtkWidget *mi = menu_item_new(name, NULL, exists ? "folder" : "dialog-warning");
        GtkWidget *x = gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_margin_start(x, 18);
        gtk_box_pack_end(GTK_BOX(gtk_bin_get_child(GTK_BIN(mi))), x, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(mi), "x", x);
        g_object_set_data_full(G_OBJECT(mi), "uri", g_strdup(mk->uri), g_free);
        g_signal_connect(mi, "button-release-event", G_CALLBACK(on_mark_release), w);
        char *tip = file_path(f);
        gtk_widget_set_tooltip_text(mi, tip);
        add(m, mi, f, G_CALLBACK(on_open_bookmark), w);
        if (!strcmp(mk->uri, cur_uri))
            cur_marked = TRUE;
        g_free(tip);
        g_free(name);
        g_free(path);
        g_object_unref(f);
    }
    if (!a->len) {
        GtkWidget *mi = menu_item_new(_("No bookmarks yet"), NULL, NULL);
        gtk_widget_set_sensitive(mi, FALSE);
        add(m, mi, NULL, NULL, w);
    }
    if (!file_in_trash(cur)) {          /* the trash has its own line below */
        gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
        char *base = g_file_get_basename(cur);
        char *label = g_strdup_printf(cur_marked ? _("Remove “%s”") : _("Bookmark “%s”"), base ? base : "/");
        add(m, menu_item_new(label, NULL, cur_marked ? "list-remove" : "bookmark-new"), cur,
            cur_marked ? G_CALLBACK(on_remove_here) : G_CALLBACK(on_add_here), w);
        g_free(label);
        g_free(base);
    }
    /* the trash last, always there (no ✕); its icon says whether there is anything in it */
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    GFile *trash = g_file_new_for_uri("trash:///");
    GFileInfo *ti = g_file_query_info(trash, "trash::item-count", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    guint32 tn = ti ? g_file_info_get_attribute_uint32(ti, "trash::item-count") : 0;
    g_clear_object(&ti);
    GtkWidget *tmi = menu_item_new(_("Trash"), NULL, tn ? "user-trash-full" : "user-trash");
    char *ttip = tn ? g_strdup_printf(ngettext("%u item", "%u items", tn), tn) : g_strdup(_("empty"));
    gtk_widget_set_tooltip_text(tmi, ttip);
    g_free(ttip);
    add(m, tmi, trash, G_CALLBACK(on_open_trash), w);
    g_object_unref(trash);
    gtk_widget_show_all(m);
    g_free(cur_uri);
    g_object_unref(cur);
    g_ptr_array_unref(a);
}

/* ---- drives -------------------------------------------------------------------------------- */
typedef struct {
    Win *w;
    char *name;
    gboolean eject;
} Op;

static void op_free(Op *o)
{
    win_unref(o->w);
    g_free(o->name);
    g_free(o);
}

/* ---- the Computer column: disks, removable media, network ------------------------------------ */
gboolean file_is_computer(GFile *f)
{
    return f && g_file_has_uri_scheme(f, "computer");
}

/* everything really mounted from a disk or a network share (also through /etc/fstab, which GIO
 * keeps to itself as "system"); not boot partitions, images or pseudo file systems */
static gboolean is_disk_mount(const struct mntent *e, gboolean *net)
{
    static const char *NET[] = { "nfs", "nfs4", "cifs", "smb3", "fuse.sshfs", NULL };
    *net = FALSE;
    for (int i = 0; NET[i]; i++)
        if (!strcmp(e->mnt_type, NET[i]))
            *net = TRUE;
    if (!*net && strncmp(e->mnt_fsname, "/dev/", 5))
        return FALSE;                                   /* proc, sysfs, tmpfs, overlay … */
    if (!strncmp(e->mnt_fsname, "/dev/loop", 9) || !strcmp(e->mnt_type, "swap") ||
        !strcmp(e->mnt_type, "squashfs") || !strcmp(e->mnt_type, "iso9660") ||
        !strcmp(e->mnt_type, "udf"))
        return FALSE;                                   /* images and CDs: GIO lists those */
    static const char *HIDE[] = { "/boot", "/boot/efi", "/efi", "/recovery", NULL };
    for (int i = 0; HIDE[i]; i++)
        if (!strcmp(e->mnt_dir, HIDE[i]))
            return FALSE;
    static const char *UNDER[] = { "/proc/", "/sys/", "/dev/", "/snap/", "/var/lib/", NULL };
    for (int i = 0; UNDER[i]; i++)
        if (g_str_has_prefix(e->mnt_dir, UNDER[i]))
            return FALSE;
    return TRUE;
}

/* is path on a network share (NFS, Samba, sshfs …)? From the mount table, so a dead share
 * can't hang us the way asking the share itself would */
gboolean path_on_network(const char *path)
{
    static const char *NET[] = { "nfs", "nfs4", "cifs", "smb3", "smbfs", "fuse.sshfs", "fuse.rclone",
                                 "9p", "afs", "ceph", "glusterfs", "davfs", "fuse.gvfsd-fuse", NULL };
    FILE *f = setmntent("/proc/self/mounts", "r");
    if (!f)
        return FALSE;
    size_t best = 0;
    gboolean net = FALSE;
    struct mntent *e;
    while ((e = getmntent(f))) {
        size_t n = strlen(e->mnt_dir);
        gboolean under = !strcmp(e->mnt_dir, "/") ||
                         (!strncmp(path, e->mnt_dir, n) && (path[n] == '/' || path[n] == '\0'));
        if (!under || n < best)
            continue;
        best = n;                                   /* the deepest mount holding path decides */
        net = FALSE;
        for (int i = 0; NET[i]; i++)
            if (!strcmp(e->mnt_type, NET[i]))
                net = TRUE;
    }
    endmntent(f);
    return net;
}

/* is path a mount point of a device (/dev/…)? Those are udisks2's or the mount table's rows */
static gboolean is_dev_mount(const char *path)
{
    FILE *f = path ? setmntent("/proc/self/mounts", "r") : NULL;
    if (!f)
        return FALSE;
    gboolean r = FALSE;
    struct mntent *e;
    while (!r && (e = getmntent(f)))
        r = !strcmp(e->mnt_dir, path) && !strncmp(e->mnt_fsname, "/dev/", 5);
    endmntent(f);
    return r;
}

/* one row; obj (a GMount or GVolume) is kept in the column, its index in C_SIZE; the kind in
 * C_MTIME; "where\ndevice\nfile system" in C_EXT for the preview; C_SORT keeps the groups in order */
static void add_row(Column *c, int group, int kind, const char *name, GIcon *icon, GFile *root,
                    GObject *obj, const char *info)
{
    guint64 idx = 0;
    if (obj) {
        g_ptr_array_add(c->objs, g_object_ref(obj));
        idx = c->objs->len - 1;
    }
    char *key = g_strdup_printf("%d%04d", group, gtk_tree_model_iter_n_children(GTK_TREE_MODEL(c->store), NULL));
    GtkTreeIter it;
    gtk_list_store_insert_with_values(c->store, &it, -1, C_ICON, icon, C_NAME, name,
                                      C_ISDIR, root != NULL, C_FILE, root, C_SORT, key,
                                      C_MTIME, (gint64) kind, C_SIZE, idx, C_EXT, info ? info : "", -1);
    g_free(key);
}

static void fill_disks(Column *c, GHashTable *gio_paths, gboolean want_net)
{
    FILE *f = setmntent("/proc/self/mounts", "r");
    if (!f)
        return;
    GHashTable *done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    struct mntent *e;
    while ((e = getmntent(f))) {
        gboolean net;
        if (!is_disk_mount(e, &net) || net != want_net || g_hash_table_contains(gio_paths, e->mnt_dir) ||
            g_hash_table_contains(done, e->mnt_dir))
            continue;
        g_hash_table_add(done, g_strdup(e->mnt_dir));
        GFile *root = g_file_new_for_path(e->mnt_dir);
        char *name = !strcmp(e->mnt_dir, "/") ? g_strdup(_("File System")) : g_path_get_basename(e->mnt_dir);
        char *info = g_strdup_printf("%s\n%s\n%s", e->mnt_dir, e->mnt_fsname, e->mnt_type);
        GIcon *icon = g_themed_icon_new(net ? "folder-remote" : "drive-harddisk");
        add_row(c, net ? 3 : 1, K_DISK, name, icon, root, NULL, info);
        g_object_unref(icon);
        g_free(info);
        g_free(name);
        g_object_unref(root);
    }
    endmntent(f);
    g_hash_table_unref(done);
}

void computer_fill(Column *c)
{
    Win *w = c->win;
    gtk_list_store_clear(c->store);
    g_ptr_array_set_size(c->objs, 0);
    /* sticks, disks, cards and discs: from udisks2 when it is there; then GIO keeps only what has
     * no device (phones, cameras, network places). Without udisks2 GIO lists them all, as before. */
    gboolean ud = ud_ready();
    GPtrArray *rows = ud ? ud_rows() : NULL;
    GHashTable *gio_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *all = g_volume_monitor_get_mounts(w->volumes);
    for (guint i = 0; rows && i < rows->len; i++) {
        UdRow *r = g_ptr_array_index(rows, i);
        if (r->mount)
            g_hash_table_add(gio_paths, g_strdup(r->mount));
    }
    for (GList *l = all; !ud && l; l = l->next) {
        GFile *root = g_mount_get_root(l->data);
        char *p = g_file_get_path(root);
        if (p)
            g_hash_table_add(gio_paths, p);
        g_object_unref(root);
    }
    fill_disks(c, gio_paths, FALSE);
    g_hash_table_unref(gio_paths);
    for (guint i = 0; rows && i < rows->len; i++) {
        UdRow *r = g_ptr_array_index(rows, i);
        int kind = r->state == UD_MOUNTED ? K_UD_MOUNTED : r->state == UD_LOCKED ? K_UD_LOCKED : K_UD_UNMOUNTED;
        GFile *root = r->mount ? g_file_new_for_path(r->mount) : NULL;
        char *info = g_strdup_printf("%s\n%s\n%s", r->mount ? r->mount : "", r->dev ? r->dev : "", r->fs ? r->fs : "");
        add_row(c, r->system ? 1 : 2, kind, r->name, r->icon, root, r->obj, info);
        g_free(info);
        g_clear_object(&root);
    }
    if (rows)
        g_ptr_array_unref(rows);

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *vols = g_volume_monitor_get_volumes(w->volumes);
    for (GList *l = vols; l; l = l->next) {
        GVolume *v = l->data;
        GMount *mnt = g_volume_get_mount(v);
        char *name = g_volume_get_name(v);
        GIcon *icon = g_volume_get_icon(v);
        char *dev = g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        if (ud && dev) {
            /* a device: udisks2's row */
        } else if (mnt) {
            GFile *root = g_mount_get_root(mnt);
            g_hash_table_add(seen, g_file_get_uri(root));
            char *where = file_path(root);
            char *info = g_strdup_printf("%s\n%s\n", where, dev ? dev : "");
            add_row(c, 2, K_MOUNT, name, icon, root, G_OBJECT(mnt), info);
            g_free(info);
            g_free(where);
            g_object_unref(root);
        } else if (g_volume_can_mount(v)) {
            char *info = g_strdup_printf("\n%s\n", dev ? dev : "");
            add_row(c, 2, K_VOLUME, name, icon, NULL, G_OBJECT(v), info);
            g_free(info);
        }
        g_free(dev);
        g_clear_object(&icon);
        g_clear_object(&mnt);
        g_free(name);
    }
    g_list_free_full(vols, g_object_unref);
    /* mounts without a volume: network places (sftp://, smb://), phones … */
    for (GList *l = all; l; l = l->next) {
        GMount *mnt = l->data;
        GVolume *v = g_mount_get_volume(mnt);
        GFile *root = g_mount_get_root(mnt);
        char *uri = g_file_get_uri(root);
        char *native = ud ? g_file_get_path(root) : NULL;
        gboolean device = native && is_dev_mount(native);   /* udisks2's (or the mount table's) */
        g_free(native);
        if (!v && !device && !g_mount_is_shadowed(mnt) && !g_hash_table_contains(seen, uri)) {
            char *name = g_mount_get_name(mnt);
            GIcon *icon = g_mount_get_icon(mnt);
            char *where = file_path(root);
            char *info = g_strdup_printf("%s\n\n", where);
            add_row(c, g_file_is_native(root) ? 2 : 3, K_MOUNT, name, icon, root, G_OBJECT(mnt), info);
            g_free(info);
            g_free(where);
            g_clear_object(&icon);
            g_free(name);
        }
        g_free(uri);
        g_object_unref(root);
        g_clear_object(&v);
    }
    g_list_free_full(all, g_object_unref);
    g_hash_table_unref(seen);
    GHashTable *none = g_hash_table_new(g_str_hash, g_str_equal);
    fill_disks(c, none, TRUE);                          /* NFS / Samba / sshfs from fstab */
    g_hash_table_unref(none);
    GIcon *plus = g_themed_icon_new("network-server");
    add_row(c, 9, K_CONNECT, _("Connect to server…"), plus, NULL, NULL, NULL);
    g_object_unref(plus);
}

static Column *computer_column(Win *w)
{
    Column *c = w->columns ? w->columns->data : NULL;
    return c && c->computer ? c : NULL;
}

/* the disks changed (a stick came or went): list them again, keeping what is open */
/* what a row is, not what it is called: two sticks named "8.0 GB Volume" are two rows, and after a
 * refresh the selection must come back to the same one, or Safe to remove would hit the other */
static char *row_key(Column *c, GtkTreeIter *it)
{
    gint64 kind;
    guint64 idx;
    char *name = NULL;
    GFile *root = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(c->store), it, C_MTIME, &kind, C_SIZE, &idx, C_NAME, &name, C_FILE, &root, -1);
    gboolean has_obj = kind == K_MOUNT || kind == K_VOLUME || kind == K_UD_MOUNTED || kind == K_UD_UNMOUNTED ||
                       kind == K_UD_LOCKED;
    gpointer obj = has_obj && idx < c->objs->len ? g_ptr_array_index(c->objs, idx) : NULL;
    char *uri = root ? g_file_get_uri(root) : NULL;
    /* a stick's row changes kind when it is mounted: its object stays, so the object decides */
    char *key = obj ? g_strdup_printf("o%p", obj) : g_strdup_printf("%" G_GINT64_FORMAT "|%s|%s", kind, uri ? uri : "",
                                                                    name ? name : "");
    g_free(uri);
    g_clear_object(&root);
    g_free(name);
    return key;
}

void computer_refresh(Win *w)
{
    Column *c = w->dead ? NULL : computer_column(w);
    if (!c)
        return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
    GtkTreeModel *m;
    GtkTreeIter it;
    char *was = NULL;
    if (gtk_tree_selection_get_selected(sel, &m, &it))
        was = row_key(c, &it);
    g_signal_handlers_block_matched(sel, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, c);
    computer_fill(c);
    gboolean found = FALSE;
    gboolean ok = was && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(c->store), &it);
    while (ok && !found) {
        char *n = row_key(c, &it);
        if (!g_strcmp0(n, was)) {
            gtk_tree_selection_select_iter(sel, &it);
            found = TRUE;
        }
        g_free(n);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(c->store), &it);
    }
    g_signal_handlers_unblock_matched(sel, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, c);
    if (was && !found)
        win_column_selected(w, c);                      /* what was open is gone */
    g_free(was);
}

/* the selected row: its kind and object (no new ref) */
static int selected_row(Column *c, GtkTreeIter *it, GObject **obj, char **info, char **name)
{
    GtkTreeModel *m;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), &m, it))
        return -1;
    gint64 kind;
    guint64 idx;
    gtk_tree_model_get(m, it, C_MTIME, &kind, C_SIZE, &idx, C_EXT, info, C_NAME, name, -1);
    gboolean has_obj = kind == K_MOUNT || kind == K_VOLUME || kind == K_UD_MOUNTED || kind == K_UD_UNMOUNTED ||
                       kind == K_UD_LOCKED;
    *obj = has_obj && idx < c->objs->len ? g_ptr_array_index(c->objs, idx) : NULL;
    return (int) kind;
}

/* the preview for the selected row: the disk, not its contents (no counting a whole disk) */
void computer_preview(Column *c)
{
    GtkTreeIter it;
    GObject *obj;
    char *info = NULL, *name = NULL;
    int kind = selected_row(c, &it, &obj, &info, &name);
    if (kind < 0)
        return;
    GIcon *icon;
    GFile *root;
    gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, C_ICON, &icon, C_FILE, &root, -1);
    char **f = g_strsplit(info ? info : "", "\n", 3);
    const char *where = f[0] ? f[0] : "", *dev = f[0] && f[1] ? f[1] : "", *fs = f[0] && f[1] && f[2] ? f[2] : "";
    const char *note = kind == K_VOLUME || kind == K_UD_UNMOUNTED ? _("Not mounted: click to mount it.")
                     : kind == K_UD_LOCKED ? _("Encrypted: click to unlock it.")
                     : kind == K_CONNECT ? _("sftp://, smb://, ftp://, dav:// … through GVfs. Click to enter an address.")
                     : NULL;
    preview_show_disk(c->win->preview, name, icon, root, where, dev, fs, note);
    g_strfreev(f);
    g_clear_object(&icon);
    g_clear_object(&root);
    g_free(info);
    g_free(name);
}

static gboolean computer_select_root(Win *w, GFile *root);

static void connect_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Win *w = data;
    GError *err = NULL;
    if (!g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err) &&
        !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
        if (!w->dead && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            char *u = g_file_get_uri(G_FILE(src));
            char *t = g_strdup_printf(_("Couldn't connect to %s"), u);
            win_error(w, t, err->message);
            g_free(t);
            g_free(u);
        }
        g_clear_error(&err);
        win_unref(w);
        return;
    }
    g_clear_error(&err);
    if (!w->dead) {
        win_status(w, " ");
        GMount *mnt = g_file_find_enclosing_mount(G_FILE(src), NULL, NULL);
        GFile *root = mnt ? g_mount_get_root(mnt) : NULL;
        if (!root || !computer_select_root(w, root))
            win_open_path(w, G_FILE(src));
        g_clear_object(&root);
        g_clear_object(&mnt);
    }
    win_unref(w);
}

static void connect_to_server(Win *w)
{
    char *addr = win_ask_name(w, _("Connect to server"), "sftp://", FALSE);
    if (!addr)
        return;
    g_strstrip(addr);
    GFile *f = g_file_new_for_uri(addr);
    if (!g_file_has_uri_scheme(f, "sftp") && !g_file_has_uri_scheme(f, "smb") &&
        !g_file_has_uri_scheme(f, "ftp") && !g_file_has_uri_scheme(f, "ftps") &&
        !g_file_has_uri_scheme(f, "dav") && !g_file_has_uri_scheme(f, "davs") &&
        !g_file_has_uri_scheme(f, "nfs") && !g_file_has_uri_scheme(f, "afp")) {
        win_error(w, _("Not a server address"), _("Use sftp://, smb://, ftp://, dav:// or nfs:// in front of the server."));
    } else {
        win_status(w, _("Connecting to %s …"), addr);
        GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(w->window));
        g_file_mount_enclosing_volume(f, G_MOUNT_MOUNT_NONE, op, NULL, connect_done, win_ref(w));
        g_object_unref(op);
    }
    g_object_unref(f);
    g_free(addr);
}

static void mount_volume(Win *w, GVolume *v);
static void mount_ud(Win *w, GObject *dev, gboolean new_window);

/* a click or Enter on a row that isn't a folder yet: mount it, or connect */
void computer_activate(Column *c)
{
    GtkTreeIter it;
    GObject *obj;
    char *info = NULL, *name = NULL;
    int kind = selected_row(c, &it, &obj, &info, &name);
    if (kind == K_VOLUME && obj)
        mount_volume(c->win, G_VOLUME(obj));
    else if ((kind == K_UD_UNMOUNTED || kind == K_UD_LOCKED) && obj)
        mount_ud(c->win, obj, FALSE);
    else if (kind == K_CONNECT)
        connect_to_server(c->win);
    g_free(info);
    g_free(name);
}

/* step out of a drive that is going away, so nothing of ours keeps it busy */
static void leave_root(Win *w, GFile *root)
{
    if (w->dead)
        return;
    gboolean inside = FALSE;
    for (GList *l = w->columns; l && !inside; l = l->next) {
        Column *c = l->data;
        inside = !c->computer && (g_file_equal(c->gfile, root) || g_file_has_prefix(c->gfile, root));
    }
    if (inside && computer_column(w)) {
        win_open_computer(w);
    } else if (inside) {
        GFile *h = g_file_new_for_path(g_get_home_dir());
        win_open_path(w, h);
        g_object_unref(h);
    }
}

static void leave_mount(Win *w, GMount *mnt)
{
    GFile *root = g_mount_get_root(mnt);
    leave_root(w, root);
    g_object_unref(root);
}

/* in the Computer column, select (and so open) the row of root; FALSE if it isn't there */
static gboolean computer_select_root(Win *w, GFile *root)
{
    Column *c = computer_column(w);
    if (!c)
        return FALSE;
    computer_refresh(w);
    GtkTreeModel *m = GTK_TREE_MODEL(c->store);
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        GFile *f;
        gtk_tree_model_get(m, &it, C_FILE, &f, -1);
        gboolean eq = f && g_file_equal(f, root);
        g_clear_object(&f);
        if (eq) {
            column_select_iter(c, &it);
            return TRUE;
        }
        ok = gtk_tree_model_iter_next(m, &it);
    }
    return FALSE;
}

static void mounted(GObject *src, GAsyncResult *res, gpointer data)
{
    Op *o = data;
    GVolume *v = G_VOLUME(src);
    GError *err = NULL;
    if (!g_volume_mount_finish(v, res, &err)) {
        if (!o->w->dead && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            char *t = g_strdup_printf(_("Couldn't mount %s"), o->name);
            win_error(o->w, t, err->message);
            win_status(o->w, " ");
            g_free(t);
        }
        g_error_free(err);
    } else if (!o->w->dead) {
        GMount *mnt = g_volume_get_mount(v);
        if (mnt) {
            GFile *root = g_mount_get_root(mnt);
            win_status(o->w, _("Mounted: %s"), o->name);
            if (!computer_select_root(o->w, root))
                win_open_path(o->w, root);
            g_object_unref(root);
            g_object_unref(mnt);
        }
    }
    op_free(o);
}

static void mount_volume(Win *w, GVolume *v)
{
    Op *o = g_new0(Op, 1);
    o->w = win_ref(w);
    o->name = g_volume_get_name(v);
    win_status(w, _("Mounting %s …"), o->name);
    GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(w->window));
    g_volume_mount(v, G_MOUNT_MOUNT_NONE, op, NULL, mounted, o);
    g_object_unref(op);
}

static void unmounted(GObject *src, GAsyncResult *res, gpointer data)
{
    Op *o = data;
    GError *err = NULL;
    gboolean ok = o->eject ? g_mount_eject_with_operation_finish(G_MOUNT(src), res, &err)
                           : g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err);
    if (!o->w->dead) {
        if (!ok) {
            char *t = g_strdup_printf(o->eject ? _("Couldn't eject %s") : _("Couldn't unmount %s"), o->name);
            win_error(o->w, t, err->message);
            win_status(o->w, " ");
            g_free(t);
        } else if (o->eject) {
            win_status(o->w, _("%s: safe to remove"), o->name);
        } else {
            win_status(o->w, _("%s unmounted"), o->name);
        }
        computer_refresh(o->w);
    }
    g_clear_error(&err);
    op_free(o);
}

static void unmount(Win *w, GMount *mnt)
{
    Op *o = g_new0(Op, 1);
    o->w = win_ref(w);
    o->name = g_mount_get_name(mnt);
    o->eject = g_mount_can_eject(mnt);
    leave_mount(w, mnt);
    win_status(w, "%s %s …", o->eject ? "Ejecting" : "Unmounting", o->name);
    GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(w->window));
    if (o->eject)
        g_mount_eject_with_operation(mnt, G_MOUNT_UNMOUNT_NONE, op, NULL, unmounted, o);
    else
        g_mount_unmount_with_operation(mnt, G_MOUNT_UNMOUNT_NONE, op, NULL, unmounted, o);
    g_object_unref(op);
}

/* ---- udisks2: mount, unmount, safe to remove ---- */
static GtkApplication *APP;

typedef struct {
    char *name;
    gboolean flag;                  /* mounting: open a new window there; unmounting: remove */
} UdCtx;

static UdCtx *ctx_new(GObject *dev, gboolean flag)
{
    UdCtx *x = g_new0(UdCtx, 1);
    x->name = ud_name(dev);
    x->flag = flag;
    return x;
}

static void ctx_free(UdCtx *x)
{
    g_free(x->name);
    g_free(x);
}

static void ud_mounted(Win *w, const char *path, const char *err, gpointer data)
{
    UdCtx *x = data;
    if (!w->dead) {
        if (err && *err) {
            char *t = g_strdup_printf(_("Couldn't mount %s"), x->name);
            win_error(w, t, err);
            g_free(t);
            win_status(w, " ");
        } else if (err) {
            win_status(w, " ");                         /* the passphrase was cancelled */
        } else if (path) {
            win_status(w, _("Mounted %s at %s"), x->name, path);
            GFile *root = g_file_new_for_path(path);
            if (x->flag)
                win_new(w->app, root);
            else if (!computer_select_root(w, root))
                win_open_path(w, root);
            g_object_unref(root);
        }
    }
    ctx_free(x);
}

static void mount_ud(Win *w, GObject *dev, gboolean new_window)
{
    UdCtx *x = ctx_new(dev, new_window);
    win_status(w, _("Mounting %s …"), x->name);
    ud_mount(w, dev, ud_mounted, x);
}

static void ud_unmounted(Win *w, const char *path, const char *err, gpointer data)
{
    UdCtx *x = data;
    if (!w->dead) {
        if (err && *err) {
            char *t = g_strdup_printf(x->flag ? _("Couldn't make %s safe to remove") : _("Couldn't unmount %s"), x->name);
            win_error(w, t, err);
            g_free(t);
            win_status(w, " ");
        } else {
            win_status(w, x->flag ? _("%s: safe to remove") : _("%s unmounted"), x->name);
        }
        computer_refresh(w);
    }
    ctx_free(x);
}

static void unmount_ud(Win *w, GObject *dev, gboolean remove)
{
    /* every window steps out first, so millerX itself doesn't keep it busy */
    GPtrArray *mps = ud_mounts_on_drive(dev, remove);
    for (guint i = 0; i < mps->len; i++) {
        GFile *root = g_file_new_for_path(g_ptr_array_index(mps, i));
        for (GList *l = gtk_application_get_windows(w->app); l; l = l->next) {
            Win *o = g_object_get_data(G_OBJECT(l->data), "millerx-win");
            if (o)
                leave_root(o, root);
        }
        g_object_unref(root);
    }
    UdCtx *x = ctx_new(dev, remove);
    /* unmounting writes out what is still in memory: say so, on a slow stick it takes a while */
    if (mps->len)
        win_status(w, _("Writing what is left to %s and unmounting …"), x->name);
    else
        win_status(w, _("Switching %s off …"), x->name);
    g_ptr_array_unref(mps);
    ud_unmount(w, dev, remove, ud_unmounted, x);
}

/* the row of dev in the Computer column, selected; FALSE if it isn't there */
static gboolean computer_select_dev(Win *w, GObject *dev)
{
    Column *c = computer_column(w);
    if (!c)
        return FALSE;
    GtkTreeModel *m = GTK_TREE_MODEL(c->store);
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        gint64 kind;
        guint64 idx;
        gtk_tree_model_get(m, &it, C_MTIME, &kind, C_SIZE, &idx, -1);
        if ((kind == K_UD_MOUNTED || kind == K_UD_UNMOUNTED || kind == K_UD_LOCKED) && idx < c->objs->len &&
            g_ptr_array_index(c->objs, idx) == dev) {
            column_select_iter(c, &it);
            return TRUE;
        }
        ok = gtk_tree_model_iter_next(m, &it);
    }
    return FALSE;
}

/* ---- a stick plugged in: tell (Automount off), or mount it and open a window (on) ---- */
static int LOCK_FD = -1;

/* with several millerX running, only one of them reacts (the first to take the lock) */
static gboolean devices_ours(void)
{
    if (LOCK_FD >= 0)
        return TRUE;
    char *p = g_build_filename(g_get_user_runtime_dir(), "millerx-devices.lock", NULL);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    g_free(p);
    if (fd < 0)
        return TRUE;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return FALSE;
    }
    LOCK_FD = fd;
    return TRUE;
}

static Win *active_win(void)
{
    if (!APP)
        return NULL;
    GtkWindow *aw = gtk_application_get_active_window(APP);
    Win *w = aw ? g_object_get_data(G_OBJECT(aw), "millerx-win") : NULL;
    for (GList *l = gtk_application_get_windows(APP); !w && l; l = l->next)
        w = g_object_get_data(G_OBJECT(l->data), "millerx-win");
    return w && !w->dead ? w : NULL;
}

static void arrived_clicked(Win *w, int button, gpointer data)
{
    GObject *dev = data;
    if (button == 2) {                              /* Show: its row in the Computer column */
        win_open_computer(w);
        computer_select_dev(w, dev);
        return;
    }
    char *mp = ud_state(dev) == UD_MOUNTED ? ud_mount_point(dev) : NULL;
    if (mp) {                                       /* Open */
        GFile *root = g_file_new_for_path(mp);
        win_open_path(w, root);
        g_object_unref(root);
        g_free(mp);
    } else {
        mount_ud(w, dev, FALSE);                    /* Mount / Unlock… */
    }
}

void places_device_arrived(GObject *dev)
{
    Win *w = active_win();
    if (!w || is_root() || !devices_ours())
        return;
    int st = ud_state(dev);
    if (LOOK.automount && st == UD_UNMOUNTED && ud_auto(dev)) {
        mount_ud(w, dev, TRUE);
        return;
    }
    char *name = ud_name(dev), *what = ud_describe(dev), *t;
    const char *b1;
    if (st == UD_MOUNTED) {
        char *mp = ud_mount_point(dev);
        t = g_strdup_printf(_("%s (%s) is mounted at %s"), name, what, mp ? mp : "?");
        g_free(mp);
        b1 = _("Open");
    } else if (st == UD_LOCKED) {
        t = g_strdup_printf(_("%s plugged in (%s). It is encrypted."), name, what);
        b1 = _("Unlock…");
    } else {
        t = g_strdup_printf(_("%s plugged in (%s)."), name, what);
        b1 = _("Mount");
    }
    win_notice(w, g_dbus_object_get_object_path(G_DBUS_OBJECT(dev)), t, b1, _("Show"), arrived_clicked,
               g_object_ref(dev), g_object_unref);
    g_free(t);
    g_free(name);
    g_free(what);
}

void places_device_gone(const char *path)
{
    for (GList *l = APP ? gtk_application_get_windows(APP) : NULL; l; l = l->next) {
        Win *w = g_object_get_data(G_OBJECT(l->data), "millerx-win");
        if (w && !w->dead)
            win_notice_close(w, path);
    }
}

void places_devices_changed(void)
{
    for (GList *l = APP ? gtk_application_get_windows(APP) : NULL; l; l = l->next) {
        Win *w = g_object_get_data(G_OBJECT(l->data), "millerx-win");
        if (w && !w->dead)
            computer_refresh(w);
    }
}

/* ---- right-click in the Computer column ---- */
enum { CM_OPEN_NEW, CM_MOUNT, CM_UNMOUNT, CM_CONNECT, CM_REFRESH, CM_UD_MOUNT, CM_UD_UNMOUNT, CM_UD_REMOVE };

static void cm_run(GtkMenuItem *mi, Win *w)
{
    int what = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "what"));
    GObject *obj = g_object_get_data(G_OBJECT(mi), "obj");
    GFile *root = g_object_get_data(G_OBJECT(mi), "root");
    switch (what) {
    case CM_OPEN_NEW: if (root) win_new(w->app, root); break;
    case CM_MOUNT: if (obj) mount_volume(w, G_VOLUME(obj)); break;
    case CM_UNMOUNT: if (obj) unmount(w, G_MOUNT(obj)); break;
    case CM_CONNECT: connect_to_server(w); break;
    case CM_REFRESH: computer_refresh(w); break;
    case CM_UD_MOUNT: if (obj) mount_ud(w, obj, FALSE); break;
    case CM_UD_UNMOUNT: if (obj) unmount_ud(w, obj, FALSE); break;
    case CM_UD_REMOVE: if (obj) unmount_ud(w, obj, TRUE); break;
    }
}

static void cm_item(GtkWidget *menu, Win *w, const char *label, int what, GObject *obj, GFile *root,
                    gboolean on)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(mi), "what", GINT_TO_POINTER(what));
    if (obj)
        g_object_set_data_full(G_OBJECT(mi), "obj", g_object_ref(obj), g_object_unref);
    if (root)
        g_object_set_data_full(G_OBJECT(mi), "root", g_object_ref(root), g_object_unref);
    gtk_widget_set_sensitive(mi, on);
    g_signal_connect(mi, "activate", G_CALLBACK(cm_run), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static gboolean cm_destroy(gpointer menu)
{
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void cm_done(GtkMenuShell *menu, gpointer d)
{
    g_idle_add(cm_destroy, menu);       /* the chosen item is activated after this */
}

void computer_menu(Win *w, Column *c, GdkEventButton *ev)
{
    GtkWidget *menu = gtk_menu_new();
    GtkTreeIter it;
    GObject *obj;
    char *info = NULL, *name = NULL;
    int kind = selected_row(c, &it, &obj, &info, &name);
    GFile *root = NULL;
    if (kind >= 0)
        gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, C_FILE, &root, -1);
    if (root)
        cm_item(menu, w, _("Open in new window"), CM_OPEN_NEW, NULL, root, TRUE);
    if (kind == K_VOLUME)
        cm_item(menu, w, "Mount", CM_MOUNT, obj, NULL, obj != NULL);
    if (kind == K_MOUNT && obj) {
        gboolean ej = g_mount_can_eject(G_MOUNT(obj));
        cm_item(menu, w, ej ? "Eject" : "Unmount", CM_UNMOUNT, obj, NULL, ej || g_mount_can_unmount(G_MOUNT(obj)));
    }
    if (kind == K_UD_UNMOUNTED)
        cm_item(menu, w, _("Mount"), CM_UD_MOUNT, obj, NULL, obj != NULL);
    if (kind == K_UD_LOCKED)
        cm_item(menu, w, _("Unlock…"), CM_UD_MOUNT, obj, NULL, obj != NULL);
    if (kind == K_UD_MOUNTED)
        cm_item(menu, w, _("Unmount"), CM_UD_UNMOUNT, obj, NULL, obj != NULL);
    if ((kind == K_UD_MOUNTED || kind == K_UD_UNMOUNTED || kind == K_UD_LOCKED) && obj && ud_removable(obj))
        cm_item(menu, w, _("Safe to remove"), CM_UD_REMOVE, obj, NULL, TRUE);
    if (kind >= 0)
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    cm_item(menu, w, _("Connect to server…"), CM_CONNECT, NULL, NULL, TRUE);
    cm_item(menu, w, _("Refresh (F5)"), CM_REFRESH, NULL, NULL, TRUE);
    g_clear_object(&root);
    g_free(info);
    g_free(name);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w->window, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(cm_done), NULL);
    gtk_widget_show_all(menu);
    if (ev)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) ev);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), c->view, GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST, NULL);
}

/* ---- the disks coming and going ---- */
static void on_mount_added(GVolumeMonitor *vm, GMount *mnt, Win *w)
{
    (void) vm;
    if (ud_ready()) {                       /* a device: udisks2 tells about it */
        GFile *root = g_mount_get_root(mnt);
        char *p = g_file_get_path(root);
        gboolean device = p && is_dev_mount(p);
        g_free(p);
        g_object_unref(root);
        if (device)
            return;
    }
    char *n = g_mount_get_name(mnt);
    win_status(w, _("Mounted: %s"), n);
    g_free(n);
}

static void on_volume_added(GVolumeMonitor *vm, GVolume *v, Win *w)
{
    (void) vm;
    char *dev = g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
    gboolean device = dev != NULL;
    g_free(dev);
    if (device && ud_ready())               /* a device: udisks2 tells about it */
        return;
    char *n = g_volume_get_name(v);
    win_status(w, _("Drive connected: %s (drive button to open)"), n);
    g_free(n);
}

static void on_changed_any(GVolumeMonitor *vm, gpointer obj, Win *w)
{
    computer_refresh(w);
}

static void on_mount_gone(GVolumeMonitor *vm, GMount *mnt, Win *w)
{
    (void) vm;
    leave_mount(w, mnt);
}

void places_watch_volumes(Win *w)
{
    APP = w->app;
    w->volumes = g_volume_monitor_get();
    g_signal_connect(w->volumes, "mount-added", G_CALLBACK(on_mount_added), w);
    g_signal_connect(w->volumes, "volume-added", G_CALLBACK(on_volume_added), w);
    g_signal_connect(w->volumes, "mount-pre-unmount", G_CALLBACK(on_mount_gone), w);
    g_signal_connect(w->volumes, "mount-removed", G_CALLBACK(on_mount_gone), w);
    const char *any[] = { "mount-added", "mount-removed", "mount-changed", "volume-added",
                          "volume-removed", "volume-changed" };
    for (guint i = 0; i < G_N_ELEMENTS(any); i++)
        g_signal_connect_after(w->volumes, any[i], G_CALLBACK(on_changed_any), w);
}
