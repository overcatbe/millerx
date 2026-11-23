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

/* translations: _() for a text, N_() where it is only listed (translated where it is shown),
 * ngettext() for "1 item" / "3 items". Without them (no msgfmt at build time) English only. */
#ifdef ENABLE_NLS
#include <glib/gi18n.h>
#else
#define _(s) (s)
#define N_(s) (s)
#define ngettext(one, many, n) ((n) == 1 ? (one) : (many))
#endif

#ifndef MILLERX_VERSION
#define MILLERX_VERSION "0.7"
#endif

enum { THUMBS_ALWAYS, THUMBS_LOCAL, THUMBS_NEVER };
enum { TSIZE_SMALL, TSIZE_MEDIUM, TSIZE_LARGE };    /* gallery tiles: about 100, 200, 400 px */
#define COL_DEFAULT 230      /* the start width of a column, unless set in Settings */
#define COL_MIN 120
#define COL_MAX 1200
#define PREVIEW_DEFAULT 340
#define TEXT_PREVIEW_BYTES 8192
#define ARCHIVE_LIST_MAX 300

#define ATTRS "standard::name,standard::display-name,standard::type,standard::icon," \
              "standard::is-hidden,standard::is-backup,standard::size," \
              "standard::content-type,time::modified,unix::mode,trash::deletion-date"

/* list store columns */
enum { C_ICON, C_NAME, C_ISDIR, C_FILE, C_SORT, C_MTIME, C_SIZE, C_EXT, C_N };

/* ---- settings (settings.c) --------------------------------------------- */
typedef struct {
    char *divider_color;    /* "auto" or a colour GdkRGBA can parse */
    int divider_width;      /* 1-12 */
    gboolean fade;          /* divider_style = fade | solid */
    int preview_width;      /* 200-900 */
    gboolean folders_first;
    gboolean show_hidden;   /* at start */
    gboolean gallery_names; /* a line with the name under each thumbnail */
    char *highlight;        /* selection colour: "auto" (the theme's) or #rrggbb */
    int cache_max_mb;       /* thumbnail cache: shrink to this, least used first */
    int cache_days;         /* forget thumbnails unused this long */
    gboolean cache_auto;    /* clean once a day when millerX starts */
    int column_width;       /* start width of folder columns, 120-600 */
    int thumbs;             /* gallery thumbnails: THUMBS_ALWAYS, _LOCAL (not on network folders), _NEVER */
    gboolean use_ffmpeg;    /* optional helpers, when installed: switched off here */
    gboolean use_pdftoppm;
    gboolean use_pkexec;    /* off: no "Open as root" anywhere */
    gboolean use_blender;   /* blender-thumbnailer: the picture saved inside .blend files */
    int thumb_size;         /* TSIZE_SMALL, _MEDIUM, _LARGE */
    char *language;         /* "system", "en", "wizard", "wienerisch" or "own:NAME": read once at start */
} Look;

extern Look LOOK;

void look_load(void);
char *look_language(void);             /* the [look] language setting, before GTK starts */
char *own_languages_dir(void);         /* ~/.local/share/millerx/languages: people's own NAME.po / NAME.mo */
GPtrArray *own_languages_list(void);   /* the NAMEs found there, sorted */
char *own_language_prepare(const char *name);   /* a folder to bind the text domain to, or NULL */
char *own_language_add(const char *path, char **err);   /* checked and copied there: its NAME */
extern char *LANGUAGE_AT_START;        /* the language this run started with */
char **child_envp(void);               /* our environment, LANGUAGE as the session had it */
GAppLaunchContext *child_launch_context(void);
void app_restart(GtkApplication *app); /* same windows again in a new process (new language) */
gboolean look_save(GError **err);

/* where the last closed window was ([window] in millerx.conf); x/y only on X11 */
typedef struct {
    int x, y, w, h;
    gboolean has_pos, has_size, max;
} Geom;
void geom_load(Geom *g);
void geom_save(const Geom *g);
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
/* ---- per-folder sorting (sorting.c) ------------------------------------------------ */
typedef enum { SORT_NAME, SORT_DATE, SORT_SIZE, SORT_TYPE } SortBy;
typedef struct {
    SortBy by;
    gboolean reverse;       /* against the natural direction (date/size: newest/largest first) */
} FolderSort;
void sorting_load(void);
gboolean sorting_get(const char *path, FolderSort *out);     /* TRUE: the folder has its own */
void sorting_set(const char *path, SortBy by, gboolean reverse);
void sorting_clear(const char *path);
GList *sorting_paths(void);                                  /* new strings, sorted */
char *sorting_describe(FolderSort s);
char *sorting_ext(const char *name);
int sorting_compare(FolderSort s, gboolean da, gboolean db, const char *ka, const char *kb,
                    gint64 ma, gint64 mb, guint64 sa, guint64 sb, const char *ea, const char *eb);

typedef struct Win Win;
typedef struct Column Column;
typedef struct Preview Preview;
typedef struct Search Search;

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
    FolderSort sort;           /* this folder's own order, if custom */
    gboolean custom;
    GtkWidget *sortbar, *sortlabel;
    gboolean computer;         /* the Computer column: disks, removable media, network */
    gboolean trash;            /* the trash or a folder in it: put back, delete for good */
    gboolean trash_root;       /* trash:/// itself: its dates are deletion dates, its own sort button */
    GPtrArray *objs;           /* its GMounts / GVolumes, by row (C_SIZE) */
};

struct Preview {
    Win *win;
    GtkWidget *outer;                /* scrolls the whole pane when the window is shorter than it */
    GtkWidget *box, *image, *icon, *text, *text_scroll, *title, *details;
    GtkWidget *grid_scroll, *grid;   /* a folder's contents as thumbnails, under its details */
    GtkWidget *header, *sep;         /* icon left + name and details right; a line under it */
    GtkWidget *image_scroll;         /* the picture's area: takes the room left, never grows the window */
    char *image_path;                /* picture or PDF shown now, NULL if none */
    int image_kind;                  /* MEDIA_*: how the picture for image_path is made */
    int image_w, image_h;            /* the area size it was drawn for */
    guint image_timer, resize_timer;
    int grid_w;                      /* pane width the thumbnails were made for */
    int cols, tile;                  /* tiles per row, tile size in px */
    GThreadPool *pool;               /* makes the tiles' pictures */
    gint vis_lo, vis_hi;             /* tile indexes on screen + one page ahead (read by the workers) */
    guint vis_idle, add_idle;
    GPtrArray *pending;              /* folder entries still to become tiles */
    guint pending_pos;
    char *pending_dir;
    guint grid_timer;
    gboolean grid_icons;       /* this grid shows plain icons (thumbnails off here) */
    guint grid_token;          /* the grid's own: reloading it must not stop the size count */
    GFileMonitor *grid_monitor;
    guint grid_refresh;
    GtkWidget *dir_size_l, *dir_count_l;   /* the shown folder's Size and Items rows (refs) */
    guint sizes_gen;                 /* only the newest count may write the rows */
    gboolean header_narrow;          /* icon above the details (narrow pane) instead of beside */
    guint header_idle;
    char *drag_path;                 /* thumbnail pressed with button 1, until it drags or lets go */
    GList *drag_files;               /* GFile*: what a running drag carries */
    int press_x, press_y;
    gboolean press_keep;             /* the press kept a multi-selection (for dragging it) */
    guint token;
    int width;
    GFile *current;
};

struct Win {
    int ref;
    gboolean dead;
    GtkWidget *root_mi, *root_sep;   /* "Open this folder as root…" in the main menu */
    Geom geom;              /* its normal (unmaximized) place and size, kept up to date */
    Geom geom_prev;         /* the one before: maximizing may report the big size first */
    GtkApplication *app;
    GtkWidget *window, *location, *colbox, *hscroll, *pane_divider;
    GtkWidget *statusbar, *progress, *cancel_btn;
    GtkWidget *bm_menu;
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
    Search *search;            /* Ctrl+F: its bar and result list, hidden when not searching */
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
void app_sorting_changed(GtkApplication *app, const char *path);
void win_context_menu(Win *w, Column *c, GdkEventButton *ev);
void win_file_menu(Win *w, GList *files, GdkEventButton *ev, GtkWidget *attach);
void win_rename_file(Win *w, GFile *f);
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
void column_apply_sort(Column *c);
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
void column_mark_files(Column *c, GList *files);   /* select rows quietly (the gallery's picks) */

/* ---- preview (preview.c) --------------------------------------------------- */
Preview *preview_new(Win *w);
void preview_free(Preview *p);
void preview_clear(Preview *p);
void preview_show_file(Preview *p, GFile *f);
void preview_show_many(Preview *p, GList *files);
GtkWidget *disk_bar(double used);
char *blender_thumbnailer(void);   /* PATH, /opt/blender, newest /opt/blender-*; NULL if none */
void preview_show_disk(Preview *p, const char *name, GIcon *icon, GFile *root, const char *where,
                       const char *device, const char *fs, const char *note);

/* ---- jobs (job.c) ------------------------------------------------------------ */
typedef struct Job Job;
Job *job_run(Win *w, gboolean move, GList *sources, GFile *dest);
Job *job_extract(Win *w, GList *archives, gboolean into_folder);
Job *job_compress(Win *w, GList *sources, GFile *out, int format);
void job_cancel(Job *j);

/* archive.c: packing and unpacking with libarchive (worker thread) */
typedef struct { const char *name, *ext; } ArcFormat;
extern const ArcFormat ARC_FORMATS[];
extern const int ARC_N_FORMATS;
typedef void (*ArcProgress)(gpointer data, const char *name, double frac);
char *arc_stem(const char *name);
gboolean arc_extract(const char *archive, const char *dir, GCancellable *cancel, ArcProgress cb,
                     gpointer data, GError **err);
gboolean arc_create(const char *out, int format, char **paths, const char *base, GCancellable *cancel,
                    ArcProgress cb, gpointer data, GError **err);

/* ---- places (places.c) -------------------------------------------------------- */
void places_fill_bookmarks(Win *w);
gboolean path_on_network(const char *path);
/* the Computer column (places.c); its rows' kind is in C_MTIME */
enum { K_DISK, K_MOUNT, K_VOLUME, K_CONNECT };
gboolean file_is_computer(GFile *f);
void computer_fill(Column *c);
void computer_refresh(Win *w);
void computer_preview(Column *c);
void computer_activate(Column *c);
void computer_menu(Win *w, Column *c, GdkEventButton *ev);
void win_open_computer(Win *w);
void places_bookmark_add(Win *w, GFile *folder);
void places_watch_volumes(Win *w);

/* ---- search (search.c) ---------------------------------------------------------------------------- */
void search_build(Win *w, GtkWidget *bar_box, GtkWidget *pane);
void search_open(Win *w);
void search_close(Win *w);
void search_free(Win *w);

/* ---- the trash (trash.c) ------------------------------------------------------------------------ */
gboolean file_in_trash(GFile *f);
gboolean trash_top(GFile *f);
void trash_menu(Win *w, Column *c, GdkEventButton *ev);
void win_restore(Win *w, GList *files);
void win_empty_trash(Win *w);

/* ---- highlight colour (highlight.c) ------------------------------------------------ */
void highlight_apply(void);
gboolean highlight_system_has(void);
gboolean highlight_system_set(const char *colour, GError **err);    /* NULL removes */

/* ---- thumbnail cache (thumbcache.c) ------------------------------------------------ */
typedef void (*ThumbcacheDone)(guint64 bytes, guint files, guint removed, gpointer data);
void thumbcache_measure(ThumbcacheDone done, gpointer data);
void thumbcache_clean(ThumbcacheDone done, gpointer data);
void thumbcache_clear(ThumbcacheDone done, gpointer data);
void thumbcache_auto_clean(void);

/* ---- dialogs (dialogs.c) ------------------------------------------------------- */
typedef enum { CH_CANCEL, CH_SKIP, CH_BOTH, CH_REPLACE } Choice;
Choice dialog_conflict(Win *w, GFile *src, GFile *dst, gboolean merge, gboolean *for_all);
void dialog_properties(Win *w, GFile *f);
void dialog_settings(Win *w);
void dialog_about(Win *w);
void dialog_checksums(Win *w, GFile *f);

#endif
