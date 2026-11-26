/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Small helpers around GIO. */
#include "millerx.h"
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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

/* ---- Recognise files by content (SCerovec's idea, LinuxQuestions) -------------------------------
 * Settings → Folders → Recognise files. By name (the default) millerX is like other file managers:
 * GIO goes by the name and only looks inside when the name says nothing. By content, the file you
 * select or open is read too (its first 4 KB, once): where the content is of another *family* than
 * the name says (a "holiday.jpg" that is a program, text or an archive; a "notes.txt" that is a
 * picture), the content wins in the preview, Properties, Open with and on a double-click. Inside a
 * family the name stays: magic guesses between text kinds are weak, office files are zips inside.
 * Lists (columns, gallery, search) stay by name: nothing is read in the background. Not a virus
 * scanner. */
enum { F_OTHER, F_EXEC, F_TEXT, F_MEDIA, F_DOC, F_ARCHIVE };

static int family(const char *ct)
{
    static const char *const EXE[] = { "application/x-executable", "application/x-sharedlib",
        "application/x-pie-executable", "application/x-ms-dos-executable",
        "application/vnd.microsoft.portable-executable", NULL };
    for (int i = 0; EXE[i]; i++)
        if (g_content_type_is_a(ct, EXE[i]))
            return F_EXEC;
    if (g_content_type_is_a(ct, "text/plain"))
        return F_TEXT;
    /* pictures that are text inside (XBM and XPM are C code, PNM can be ASCII) */
    if (g_content_type_is_a(ct, "image/x-xbitmap") || g_content_type_is_a(ct, "image/x-xpixmap")
        || g_content_type_is_a(ct, "image/x-portable-anymap"))
        return F_TEXT;
    if (g_str_has_prefix(ct, "image/") || g_str_has_prefix(ct, "audio/") || g_str_has_prefix(ct, "video/"))
        return F_MEDIA;
    if (g_content_type_is_a(ct, "application/pdf") || g_content_type_is_a(ct, "application/postscript")
        || g_str_has_prefix(ct, "application/vnd.oasis.opendocument") || g_str_has_prefix(ct, "application/vnd.openxmlformats")
        || g_str_has_prefix(ct, "application/vnd.ms-") || g_content_type_is_a(ct, "application/msword")
        || g_content_type_is_a(ct, "application/rtf") || g_content_type_is_a(ct, "application/epub+zip"))
        return F_DOC;
    if (is_archive_type(ct))
        return F_ARCHIVE;
    return F_OTHER;
}

/* the content's type where it is of another family than NAME_CT, else NULL */
static char *type_by_content(const char *path, const char *name_ct)
{
    if (!path || !name_ct || g_content_type_is_unknown(name_ct))
        return NULL;                    /* no name type: GIO looked inside already */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    char buf[4096];
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode))
        n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0)
        return NULL;
    gboolean unsure = FALSE;
    char *g = g_content_type_guess(NULL, (const guchar *) buf, (gsize) n, &unsure);
    int nf = family(name_ct), cf = family(g);
    gboolean flip = !unsure && !g_content_type_is_unknown(g) && cf != nf && !g_content_type_is_a(name_ct, g)
                    && (cf == F_EXEC                        /* a program is always shown as one */
                        || ((nf == F_MEDIA || nf == F_DOC || nf == F_ARCHIVE || nf == F_TEXT)
                            && cf != F_OTHER && !(nf == F_DOC && cf == F_ARCHIVE)));
    if (!flip) {
        g_free(g);
        return NULL;
    }
    return g;
}

GFileInfo *mx_query_info(GFile *f, const char *attrs, GFileQueryInfoFlags flags, GCancellable *cancel, GError **err)
{
    GFileInfo *info = g_file_query_info(f, attrs, flags, cancel, err);
    const char *ct = info && LOOK.detect == DETECT_CONTENT ? g_file_info_get_content_type(info) : NULL;
    if (!ct)
        return info;
    char *path = g_file_get_path(f);
    char *real = type_by_content(path, ct);
    g_free(path);
    if (real) {
        g_file_info_set_content_type(info, real);
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_ICON)) {
            GIcon *ic = g_content_type_get_icon(real);
            g_file_info_set_icon(info, ic);
            g_object_unref(ic);
        }
        g_free(real);
    }
    return info;
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

/* Programs we start get the language the session had, not the one chosen in millerX's settings
 * (main() keeps it in MILLERX_LANG0). */
char **child_envp(void)
{
    char **env = g_get_environ();
    const char *orig = g_environ_getenv(env, "MILLERX_LANG0");
    if (orig && !strncmp(orig, "set:", 4))
        env = g_environ_setenv(env, "LANGUAGE", orig + 4, TRUE);
    else if (orig)
        env = g_environ_unsetenv(env, "LANGUAGE");
    return g_environ_unsetenv(env, "MILLERX_LANG0");
}

GAppLaunchContext *child_launch_context(void)
{
    GAppLaunchContext *ctx = g_app_launch_context_new();
    const char *orig = g_getenv("MILLERX_LANG0");
    if (orig && !strncmp(orig, "set:", 4))
        g_app_launch_context_setenv(ctx, "LANGUAGE", orig + 4);
    else if (orig)
        g_app_launch_context_unsetenv(ctx, "LANGUAGE");
    g_app_launch_context_unsetenv(ctx, "MILLERX_LANG0");
    return ctx;
}

/* a symlink's icon: its target's icon with the theme's link emblem in the corner */
GIcon *link_icon(GIcon *icon)
{
    GIcon *mark = g_themed_icon_new("emblem-symbolic-link");
    GEmblem *em = g_emblem_new(mark);
    GIcon *out = g_emblemed_icon_new(icon, em);
    g_object_unref(em);
    g_object_unref(mark);
    return out;
}
