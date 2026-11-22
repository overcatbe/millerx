/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Bookmarks (the GTK file, shared with Thunar and the file dialogs) and drives. */
#include "millerx.h"
#include <mntent.h>
#include <string.h>

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
        win_error(w, "Couldn't save the bookmarks", err->message);
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
        win_status(w, "Already bookmarked: %s", p);
    } else {
        Mark *m = g_new0(Mark, 1);
        m->uri = g_strdup(uri);
        m->label = g_strdup("");
        g_ptr_array_add(a, m);
        bookmarks_write(w, a);
        win_status(w, "Bookmarked: %s", p);
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
    win_status(w, "Bookmark removed: %s", p);
    g_free(p);
    g_object_unref(f);
}

/* menu items carry what they need as object data */
static void on_open_bookmark(GtkMenuItem *mi, Win *w)
{
    GFile *f = g_object_get_data(G_OBJECT(mi), "file");
    char *path = g_file_get_path(f);
    if (path && !g_file_query_exists(f, NULL))
        win_status(w, "Not found: %s", path);
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
    if (GTK_IS_SEPARATOR_MENU_ITEM(kids->data)) {
        GtkWidget *none = menu_item_new("No bookmarks yet", NULL, NULL);
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
        GtkWidget *mi = menu_item_new("No bookmarks yet", NULL, NULL);
        gtk_widget_set_sensitive(mi, FALSE);
        add(m, mi, NULL, NULL, w);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    char *base = g_file_get_basename(cur);
    char *label = g_strdup_printf(cur_marked ? "Remove “%s”" : "Bookmark “%s”", base ? base : "/");
    add(m, menu_item_new(label, NULL, cur_marked ? "list-remove" : "bookmark-new"), cur,
        cur_marked ? G_CALLBACK(on_remove_here) : G_CALLBACK(on_add_here), w);
    gtk_widget_show_all(m);
    g_free(label);
    g_free(base);
    g_free(cur_uri);
    g_object_unref(cur);
    g_ptr_array_unref(a);
}

/* ---- drives -------------------------------------------------------------------------------- */
/* step out of a drive that is going away, so nothing of ours keeps it busy */
static void leave_mount(Win *w, GMount *mnt)
{
    if (w->dead)
        return;
    GFile *root = g_mount_get_root(mnt);
    gboolean inside = FALSE;
    for (GList *l = w->columns; l && !inside; l = l->next) {
        Column *c = l->data;
        inside = g_file_equal(c->gfile, root) || g_file_has_prefix(c->gfile, root);
    }
    g_object_unref(root);
    if (inside) {
        GFile *h = g_file_new_for_path(g_get_home_dir());
        win_open_path(w, h);
        g_object_unref(h);
    }
}

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

static void mounted(GObject *src, GAsyncResult *res, gpointer data)
{
    Op *o = data;
    GVolume *v = G_VOLUME(src);
    GError *err = NULL;
    if (!g_volume_mount_finish(v, res, &err)) {
        if (!o->w->dead) {
            char *t = g_strdup_printf("Couldn't mount %s", o->name);
            win_error(o->w, t, err->message);
            win_status(o->w, " ");
            g_free(t);
        }
        g_error_free(err);
    } else if (!o->w->dead) {
        GMount *mnt = g_volume_get_mount(v);
        if (mnt) {
            GFile *root = g_mount_get_root(mnt);
            win_open_path(o->w, root);
            g_object_unref(root);
            g_object_unref(mnt);
        }
    }
    op_free(o);
}

static void on_mount_volume(GtkMenuItem *mi, Win *w)
{
    GVolume *v = g_object_get_data(G_OBJECT(mi), "volume");
    Op *o = g_new0(Op, 1);
    o->w = win_ref(w);
    o->name = g_volume_get_name(v);
    win_status(w, "Mounting %s …", o->name);
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
            char *t = g_strdup_printf("Couldn't %s %s", o->eject ? "eject" : "unmount", o->name);
            win_error(o->w, t, err->message);
            win_status(o->w, " ");
            g_free(t);
        } else if (o->eject) {
            win_status(o->w, "%s: safe to remove", o->name);
        } else {
            win_status(o->w, "%s unmounted", o->name);
        }
    }
    g_clear_error(&err);
    op_free(o);
}

static void on_unmount(GtkMenuItem *mi, Win *w)
{
    GMount *mnt = g_object_get_data(G_OBJECT(mi), "mount");
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

static void add_mount(Win *w, GtkWidget *m, GMount *mnt, const char *name, GIcon *icon)
{
    GFile *root = g_mount_get_root(mnt);
    GtkWidget *mi = menu_item_new(name, icon, NULL);
    char *tip = file_path(root);
    gtk_widget_set_tooltip_text(mi, tip);
    g_free(tip);
    add(m, mi, root, G_CALLBACK(on_open_bookmark), w);
    g_object_unref(root);

    gboolean eject = g_mount_can_eject(mnt);
    char *mname = g_mount_get_name(mnt);
    char *label = g_strdup_printf("%s “%s”", eject ? "Eject" : "Unmount", mname);
    GtkWidget *off = menu_item_new(label, NULL, "media-eject");
    gtk_widget_set_margin_start(off, 20);
    gtk_widget_set_sensitive(off, eject || g_mount_can_unmount(mnt));
    g_object_set_data_full(G_OBJECT(off), "mount", g_object_ref(mnt), g_object_unref);
    add(m, off, NULL, G_CALLBACK(on_unmount), w);
    g_free(label);
    g_free(mname);
}

/* ---- the disks themselves: everything really mounted (also through /etc/fstab, which GIO keeps
 * to itself as "system"), with its free space ---- */
static gboolean is_disk_mount(const struct mntent *e)
{
    static const char *NET[] = { "nfs", "nfs4", "cifs", "smb3", "fuse.sshfs", NULL };
    gboolean net = FALSE;
    for (int i = 0; NET[i]; i++)
        if (!strcmp(e->mnt_type, NET[i]))
            net = TRUE;
    if (!net && strncmp(e->mnt_fsname, "/dev/", 5))
        return FALSE;                                   /* proc, sysfs, tmpfs, overlay … */
    if (!strncmp(e->mnt_fsname, "/dev/loop", 9) || !strcmp(e->mnt_type, "swap") ||
        !strcmp(e->mnt_type, "squashfs") || !strcmp(e->mnt_type, "iso9660") ||
        !strcmp(e->mnt_type, "udf"))
        return FALSE;                                   /* images and CDs: under removable */
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

/* "File System (/)  312.4 GB free of 937.0 GB"; local disks only, a dead network share mustn't hang */
static int add_disks(Win *w, GtkWidget *m, GHashTable *gio_paths)
{
    FILE *f = setmntent("/proc/self/mounts", "r");
    if (!f)
        return 0;
    int n = 0;
    GHashTable *done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    struct mntent *e;
    while ((e = getmntent(f))) {
        if (!is_disk_mount(e) || g_hash_table_contains(gio_paths, e->mnt_dir) ||
            g_hash_table_contains(done, e->mnt_dir))
            continue;
        g_hash_table_add(done, g_strdup(e->mnt_dir));
        gboolean net = strncmp(e->mnt_fsname, "/dev/", 5) != 0;
        GFile *root = g_file_new_for_path(e->mnt_dir);
        char *base = g_path_get_basename(e->mnt_dir);
        char *name = !strcmp(e->mnt_dir, "/") ? g_strdup("File System (/)")
                                               : g_strdup_printf("%s (%s)", base, e->mnt_dir);
        char *label = NULL;
        if (!net) {
            GFileInfo *fi = g_file_query_filesystem_info(root, G_FILE_ATTRIBUTE_FILESYSTEM_FREE ","
                                                         G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, NULL, NULL);
            if (fi && g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
                char *fr = g_format_size(g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
                char *sz = g_format_size(g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));
                label = g_strdup_printf("%s   %s free of %s", name, fr, sz);
                g_free(fr);
                g_free(sz);
            }
            g_clear_object(&fi);
        }
        GtkWidget *mi = menu_item_new(label ? label : name, NULL, net ? "folder-remote" : "drive-harddisk");
        char *tip = g_strdup_printf("%s (%s)", e->mnt_fsname, e->mnt_type);
        gtk_widget_set_tooltip_text(mi, tip);
        add(m, mi, root, G_CALLBACK(on_open_bookmark), w);
        g_free(tip);
        g_free(label);
        g_free(name);
        g_free(base);
        g_object_unref(root);
        n++;
    }
    endmntent(f);
    g_hash_table_unref(done);
    return n;
}

void places_fill_devices(Win *w)
{
    GtkWidget *m = w->dev_menu;
    clear_menu(m);
    int n = 0;
    /* what GIO shows itself (sticks, phones, network places) comes below, with mount and eject */
    GHashTable *gio_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *all = g_volume_monitor_get_mounts(w->volumes);
    for (GList *l = all; l; l = l->next) {
        GFile *root = g_mount_get_root(l->data);
        char *p = g_file_get_path(root);
        if (p)
            g_hash_table_add(gio_paths, p);
        g_object_unref(root);
    }
    g_list_free_full(all, g_object_unref);
    int disks = add_disks(w, m, gio_paths);
    g_hash_table_unref(gio_paths);
    if (disks)
        gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *vols = g_volume_monitor_get_volumes(w->volumes);
    for (GList *l = vols; l; l = l->next) {
        GVolume *v = l->data;
        GMount *mnt = g_volume_get_mount(v);
        char *name = g_volume_get_name(v);
        GIcon *icon = g_volume_get_icon(v);
        if (mnt) {
            GFile *root = g_mount_get_root(mnt);
            g_hash_table_add(seen, g_file_get_uri(root));
            g_object_unref(root);
            add_mount(w, m, mnt, name, icon);
            g_object_unref(mnt);
            n++;
        } else if (g_volume_can_mount(v)) {
            char *label = g_strdup_printf("%s (not mounted)", name);
            GtkWidget *mi = menu_item_new(label, icon, NULL);
            g_object_set_data_full(G_OBJECT(mi), "volume", g_object_ref(v), g_object_unref);
            add(m, mi, NULL, G_CALLBACK(on_mount_volume), w);
            g_free(label);
            n++;
        }
        g_clear_object(&icon);
        g_free(name);
    }
    g_list_free_full(vols, g_object_unref);
    GList *mounts = g_volume_monitor_get_mounts(w->volumes);
    for (GList *l = mounts; l; l = l->next) {
        GMount *mnt = l->data;
        GVolume *v = g_mount_get_volume(mnt);
        GFile *root = g_mount_get_root(mnt);
        char *uri = g_file_get_uri(root);
        if (!v && !g_mount_is_shadowed(mnt) && !g_hash_table_contains(seen, uri)) {
            char *name = g_mount_get_name(mnt);
            GIcon *icon = g_mount_get_icon(mnt);
            add_mount(w, m, mnt, name, icon);
            g_clear_object(&icon);
            g_free(name);
            n++;
        }
        g_free(uri);
        g_object_unref(root);
        g_clear_object(&v);
    }
    g_list_free_full(mounts, g_object_unref);
    g_hash_table_unref(seen);
    if (!n) {
        GtkWidget *mi = menu_item_new("No USB drives or other removable media", NULL, NULL);
        gtk_widget_set_sensitive(mi, FALSE);
        add(m, mi, NULL, NULL, w);
    }
    gtk_widget_show_all(m);
}

static void on_mount_added(GVolumeMonitor *vm, GMount *mnt, Win *w)
{
    (void) vm;
    char *n = g_mount_get_name(mnt);
    win_status(w, "Mounted: %s", n);
    g_free(n);
}

static void on_volume_added(GVolumeMonitor *vm, GVolume *v, Win *w)
{
    (void) vm;
    char *n = g_volume_get_name(v);
    win_status(w, "Drive connected: %s (drive button to open)", n);
    g_free(n);
}

static void on_mount_gone(GVolumeMonitor *vm, GMount *mnt, Win *w)
{
    (void) vm;
    leave_mount(w, mnt);
}

void places_watch_volumes(Win *w)
{
    w->volumes = g_volume_monitor_get();
    g_signal_connect(w->volumes, "mount-added", G_CALLBACK(on_mount_added), w);
    g_signal_connect(w->volumes, "volume-added", G_CALLBACK(on_volume_added), w);
    g_signal_connect(w->volumes, "mount-pre-unmount", G_CALLBACK(on_mount_gone), w);
    g_signal_connect(w->volumes, "mount-removed", G_CALLBACK(on_mount_gone), w);
}
