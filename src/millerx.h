/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* millerX: a small Miller-column file browser (GTK 3, GIO).
 *
 * Columns of folders from / to the current folder, a preview of the selection on the right.
 * Open, open with, terminal here, copy/cut/paste (Thunar-compatible clipboard), rename,
 * trash, delete, new folder, drag & drop, bookmarks, drives. Columns follow changes on disk.
 */
#ifndef MILLERX_H
#define MILLERX_H

#include <gtk/gtk.h>

#ifndef MILLERX_VERSION
#define MILLERX_VERSION "0.5.4"
#endif

#define COL_WIDTH 230
#define COL_MIN 120
#define COL_MAX 1200
#define PREVIEW_DEFAULT 340
#define TEXT_PREVIEW_BYTES 8192
#define ARCHIVE_LIST_MAX 300

#define ATTRS "standard::name,standard::display-name,standard::type,standard::icon," \
              "standard::is-hidden,standard::is-backup,standard::size," \
              "standard::content-type,time::modified,unix::mode"

/* list store columns */
enum { C_ICON, C_NAME, C_ISDIR, C_FILE, C_SORT, C_N };

/* ---- settings (settings.c) --------------------------------------------- */
typedef struct {
    char *divider_color;    /* "auto" or a colour GdkRGBA can parse */
    int divider_width;      /* 1-12 */
    gboolean fade;          /* divider_style = fade | solid */
    int preview_width;      /* 200-900 */
    gboolean folders_first;
    gboolean show_hidden;   /* at start */
} Look;

extern Look LOOK;

void look_load(void);
gboolean look_save(GError **err);
char *sort_key(const char *name, gboolean isdir);
void divider_rgba(GtkWidget *w, GdkRGBA *out);
int divider_hit_width(void);

typedef void (*DragFn)(double dx, gpointer data);
typedef void (*PlainFn)(gpointer data);
GtkWidget *divider_new(DragFn drag, PlainFn reset, PlainFn done, gpointer data, gboolean line);

/* ---- helpers (util.c) ---------------------------------------------------- */
char *file_path(GFile *f);                       /* path, or uri for non-local files */
gboolean is_dir(GFile *f, gboolean follow);
gboolean device_of(GFile *f, dev_t *dev);
GFile *unique_child(GFile *folder, const char *name);
gboolean delete_tree(GFile *f, GCancellable *cancel, GError **err);
char *format_size_full(guint64 n);              /* "1.2 MB (1 234 567 bytes)" */
gboolean is_archive_type(const char *ct);
GtkWidget *menu_item_new(const char *label, GIcon *gicon, const char *icon_name);
gpointer obj_copy(gconstpointer src, gpointer data);   /* GCopyFunc for g_list_copy_deep */

/* ---- window (window.c) --------------------------------------------------- */
typedef struct Win Win;
typedef struct Column Column;
typedef struct Preview Preview;

struct Column {
    int ref;
    Win *win;
    GFile *gfile;
    GPtrArray *chain;          /* names still to select, one per column, when opening a path */
    char *pending_select;      /* select this name as soon as it shows up */
    gboolean loaded, closed;
    GtkListStore *store;
    GtkWidget *view, *scroller, *frame, *divider;
    GFileMonitor *monitor;
    GCancellable *cancel;
    int width;
    double drag_start;         /* width when a divider drag started, < 0 = not dragging */
    GString *typed;
    gint64 typed_at;
};

struct Preview {
    Win *win;
    GtkWidget *box, *image, *icon, *text, *text_scroll, *title, *details;
    guint token;
    int width;
    GFile *current;
};

struct Win {
    int ref;
    gboolean dead;
    GtkApplication *app;
    GtkWidget *window, *location, *colbox, *hscroll, *pane_divider;
    GtkWidget *statusbar, *progress, *cancel_btn;
    GtkWidget *bm_menu, *dev_menu;
    Preview *preview;
    GList *columns;            /* Column*, left to right */
    Column *focused;
    gboolean show_hidden;
    gboolean resizing;
    double pv_start;
    GHashTable *col_widths;    /* folder uri -> width the user dragged it to (this session) */
    GList *clip;               /* GFile* while we own the clipboard */
    gboolean clip_cut;
    GList *jobs;
    GVolumeMonitor *volumes;
};

Win *win_new(GtkApplication *app, GFile *start);
Win *win_ref(Win *w);
void win_unref(Win *w);
void win_status(Win *w, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void win_error(Win *w, const char *title, const char *text);
void win_error_root(Win *w, const char *title, const char *text, GFile *folder);
void win_open_as_root(Win *w, GFile *folder);
gboolean is_root(void);
gboolean win_confirm(Win *w, const char *title, const char *text, const char *ok);
char *win_ask_name(Win *w, const char *title, const char *initial, gboolean select_stem);
void win_open_path(Win *w, GFile *f);
void win_chain_finished(Win *w);
Column *win_last_column(Win *w);
void win_column_selected(Win *w, Column *c);
void win_focus_next(Win *w, Column *c);
void win_focus_prev(Win *w, Column *c);
gboolean win_scroll_end(gpointer w);
void win_reload_all(Win *w);
GFile *win_current_folder(Win *w);
GdkModifierType win_pointer_mask(Win *w);
void win_look_changed(Win *w, gboolean resort);
void win_context_menu(Win *w, Column *c, GdkEventButton *ev);
void win_open_default(Win *w, GFile *f);
void win_trash(Win *w, GList *files);
void win_delete_forever(Win *w, GList *files);
void win_new_folder(Win *w, Column *c);
void win_rename(Win *w, Column *c);
void win_clip_set(Win *w, GList *files, gboolean cut);
void win_paste_into(Win *w, GFile *folder);
void win_toggle_hidden(Win *w);
void app_apply_look(GtkApplication *app, const Look *nl);

/* ---- column (column.c) ----------------------------------------------------- */
Column *column_new(Win *w, GFile *gfile, GPtrArray *chain);
void column_ref(Column *c);
void column_unref(Column *c);
void column_close(Column *c);
void column_continue_chain(Column *c);
GList *column_selected_files(Column *c);      /* new list of refs; free with g_list_free_full(.., g_object_unref) */
gboolean column_selected_single(Column *c, GFile **f, gboolean *isdir);
int column_n_selected(Column *c);
void column_select_iter(Column *c, GtkTreeIter *it);
gboolean column_select_name(Column *c, const char *name);
void column_add_file(Column *c, GFile *f);
void column_remove_file(Column *c, GFile *f);
void column_set_width(Column *c, int w);

/* ---- preview (preview.c) --------------------------------------------------- */
Preview *preview_new(Win *w);
void preview_free(Preview *p);
void preview_clear(Preview *p);
void preview_show_file(Preview *p, GFile *f);
void preview_show_many(Preview *p, GList *files);

/* ---- jobs (job.c) ------------------------------------------------------------ */
typedef struct Job Job;
Job *job_run(Win *w, gboolean move, GList *sources, GFile *dest);
void job_cancel(Job *j);

/* ---- places (places.c) -------------------------------------------------------- */
void places_fill_bookmarks(Win *w);
void places_fill_devices(Win *w);
void places_bookmark_add(Win *w, GFile *folder);
void places_watch_volumes(Win *w);

/* ---- dialogs (dialogs.c) ------------------------------------------------------- */
typedef enum { CH_CANCEL, CH_SKIP, CH_BOTH, CH_REPLACE } Choice;
Choice dialog_conflict(Win *w, GFile *src, GFile *dst, gboolean merge, gboolean *for_all);
void dialog_properties(Win *w, GFile *f);
void dialog_settings(Win *w);
void dialog_about(Win *w);

#endif
