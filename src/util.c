/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Small helpers around GIO. */
#include "millerx.h"
#include <string.h>
#include <sys/stat.h>

char *file_path(GFile *f)
{
    char *p = g_file_get_path(f);
    return p ? p : g_file_get_uri(f);
}

gboolean is_dir(GFile *f, gboolean follow)
{
    return g_file_query_file_type(f, follow ? G_FILE_QUERY_INFO_NONE
                                            : G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL)
           == G_FILE_TYPE_DIRECTORY;
}

gboolean device_of(GFile *f, dev_t *dev)
{
    char *p = g_file_get_path(f);
    struct stat st;
    gboolean ok = p && stat(p, &st) == 0;
    if (ok)
        *dev = st.st_dev;
    g_free(p);
    return ok;
}

/* name (copy).ext, name (copy 2).ext, ... that doesn't exist yet in folder */
GFile *unique_child(GFile *folder, const char *name)
{
    const char *dot = strrchr(name, '.');
    char *base, *ext;
    if (!dot || dot == name) {          /* no extension, or a dotfile like .bashrc */
        base = g_strdup(name);
        ext = g_strdup("");
    } else {
        base = g_strndup(name, dot - name);
        ext = g_strdup(dot);
    }
    for (int n = 1;; n++) {
        char *cand = n == 1 ? g_strdup_printf(_("%s (copy)%s"), base, ext)
                            : g_strdup_printf(_("%s (copy %d)%s"), base, n, ext);
        GFile *child = g_file_get_child(folder, cand);
        g_free(cand);
        if (!g_file_query_exists(child, NULL)) {
            g_free(base);
            g_free(ext);
            return child;
        }
        g_object_unref(child);
    }
}

gboolean delete_tree(GFile *f, GCancellable *cancel, GError **err)
{
    if (is_dir(f, FALSE)) {
        GFileEnumerator *en = g_file_enumerate_children(f, "standard::name",
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, err);
        if (!en)
            return FALSE;
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, cancel, err))) {
            GFile *child = g_file_get_child(f, g_file_info_get_name(info));
            gboolean ok = delete_tree(child, cancel, err);
            g_object_unref(child);
            g_object_unref(info);
            if (!ok) {
                g_object_unref(en);
                return FALSE;
            }
        }
        g_object_unref(en);
        if (err && *err)
            return FALSE;
    }
    return g_file_delete(f, cancel, err);
}

char *format_size_full(guint64 n)
{
    char *h = g_format_size(n);
    /* thousands separated by spaces: 1 234 567 */
    char digits[32];
    g_snprintf(digits, sizeof digits, "%" G_GUINT64_FORMAT, n);
    GString *s = g_string_new(NULL);
    int len = strlen(digits);
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0)
            g_string_append_c(s, ' ');
        g_string_append_c(s, digits[i]);
    }
    char *r = g_strdup_printf(_("%s (%s bytes)"), h, s->str);
    g_string_free(s, TRUE);
    g_free(h);
    return r;
}

static const char *ARCHIVES[] = {
    "application/zip", "application/x-tar", "application/x-compressed-tar",
    "application/x-bzip-compressed-tar", "application/x-xz-compressed-tar",
    "application/x-zstd-compressed-tar", "application/java-archive",
    "application/vnd.comicbook+zip", "application/epub+zip", "application/x-7z-compressed",
    "application/vnd.rar", "application/x-rar", "application/x-cpio", NULL
};

gboolean is_archive_type(const char *ct)
{
    for (int i = 0; ARCHIVES[i]; i++)
        if (g_content_type_is_a(ct, ARCHIVES[i]))
            return TRUE;
    return FALSE;
}

/* a menu item with a small icon in front */
GtkWidget *menu_item_new(const char *label, GIcon *gicon, const char *icon_name)
{
    GtkWidget *mi = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (gicon || icon_name) {
        GtkWidget *img = gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_MENU)
                               : gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
        gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    }
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(mi), box);
    return mi;
}

gpointer obj_copy(gconstpointer src, gpointer data)
{
    (void) data;
    return g_object_ref((gpointer) src);
}
