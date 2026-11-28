/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Folders sorted their own way. Everything else is sorted by name: the eye learns where things are,
 * so other orders are the exception, per folder, and always shown as such.
 * Kept in ~/.config/millerx/sorting.conf, one line per folder: by <tab> reversed <tab> path */
#include "millerx.h"
#include <string.h>

static GHashTable *SORTS;      /* path -> FolderSort* */
static const char *BY_NAMES[] = { "name", "date", "size", "type" };

static char *sort_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "millerx", "sorting.conf", NULL);
}

static void ensure(void)
{
    if (!SORTS)
        SORTS = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

void sorting_load(void)
{
    ensure();
    g_hash_table_remove_all(SORTS);
    char *file = sort_file(), *text = NULL;
    if (g_file_get_contents(file, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i] || lines[i][0] == '#')
                continue;
            char **f = g_strsplit(lines[i], "\t", 3);
            if (f[0] && f[1] && f[2] && f[2][0] == '/') {
                for (int b = 0; b < (int) G_N_ELEMENTS(BY_NAMES); b++)
                    if (!strcmp(f[0], BY_NAMES[b])) {
                        FolderSort *s = g_new0(FolderSort, 1);
                        s->by = b;
                        s->reverse = !strcmp(f[1], "1");
                        g_hash_table_replace(SORTS, g_strdup(f[2]), s);
                    }
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(text);
    g_free(file);
}

static void save(void)
{
    GString *out = g_string_new("# millerX: folders sorted their own way (all others: by name)\n"
                                "# by (name, date, size, type) <tab> reversed (0/1) <tab> folder\n");
    GList *paths = sorting_paths();
    for (GList *l = paths; l; l = l->next) {
        FolderSort *s = g_hash_table_lookup(SORTS, l->data);
        /* one line per folder: a name with a line break in it would end its line early, and what
         * follows would read as a rule for another folder. Such a folder keeps its order until
         * millerX quits, not in the file. */
        if (strpbrk(l->data, "\n\r"))
            continue;
        g_string_append_printf(out, "%s\t%d\t%s\n", BY_NAMES[s->by], s->reverse ? 1 : 0, (char *) l->data);
    }
    g_list_free_full(paths, g_free);
    char *file = sort_file(), *dir = g_path_get_dirname(file);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(file, out->str, -1, NULL);
    g_free(dir);
    g_free(file);
    g_string_free(out, TRUE);
}

gboolean sorting_get(const char *path, FolderSort *out)
{
    FolderSort *s = path && SORTS ? g_hash_table_lookup(SORTS, path) : NULL;
    if (out)
        *out = s ? *s : (FolderSort) { SORT_NAME, FALSE };
    return s != NULL;
}

void sorting_set(const char *path, SortBy by, gboolean reverse)
{
    ensure();
    if (by == SORT_NAME && !reverse) {      /* that's the normal order: nothing to keep */
        sorting_clear(path);
        return;
    }
    FolderSort *s = g_new0(FolderSort, 1);
    s->by = by;
    s->reverse = reverse;
    g_hash_table_replace(SORTS, g_strdup(path), s);
    save();
}

void sorting_clear(const char *path)
{
    ensure();
    if (g_hash_table_remove(SORTS, path))
        save();
}

GList *sorting_paths(void)
{
    ensure();
    GList *keys = g_hash_table_get_keys(SORTS), *copy = NULL;
    for (GList *l = keys; l; l = l->next)
        copy = g_list_prepend(copy, g_strdup(l->data));
    g_list_free(keys);
    return g_list_sort(copy, (GCompareFunc) g_strcmp0);
}

char *sorting_describe(FolderSort s)
{
    switch (s.by) {
    case SORT_DATE: return g_strdup(s.reverse ? _("date, oldest first") : _("date, newest first"));
    case SORT_SIZE: return g_strdup(s.reverse ? _("size, smallest first") : _("size, largest first"));
    case SORT_TYPE: return g_strdup(s.reverse ? _("type, Z to A") : _("type"));
    default: return g_strdup(s.reverse ? _("name, Z to A") : _("name"));
    }
}

/* the extension, case-folded, for sorting by type ("" for none and for .hidden files) */
char *sorting_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name && dot[1] ? g_utf8_casefold(dot + 1, -1) : g_strdup("");
}

/* Compares two entries the way a folder with sort s wants: folders first (if set), then the field
 * in its natural direction (newest, largest first), reversed if asked, then by name. */
int sorting_compare(FolderSort s, gboolean da, gboolean db, const char *ka, const char *kb,
                    gint64 ma, gint64 mb, guint64 sa, guint64 sb, const char *ea, const char *eb)
{
    if (LOOK.folders_first && da != db)
        return da ? -1 : 1;
    int r = 0;
    switch (s.by) {
    case SORT_DATE: r = ma > mb ? -1 : ma < mb; break;
    case SORT_SIZE: r = sa > sb ? -1 : sa < sb; break;
    case SORT_TYPE: r = g_strcmp0(ea, eb); break;
    default: r = g_strcmp0(ka, kb); break;
    }
    if (s.reverse)
        r = -r;
    return r ? r : g_strcmp0(ka, kb);
}
