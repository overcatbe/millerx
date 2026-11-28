/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Git actions (random_guy's wish; Settings → Tools → Git, off by default). Inside a
 * repository the right-click menu gets "Git ▸": Status, Log and Diff in your terminal (reading is
 * what a terminal is good at), Add at once, Commit… in a small window that shows what goes in,
 * Revert… after asking. git runs with an argument list, never through a shell with file names in
 * it; the message goes to git in a file (-F), so quotes and new lines can't do anything. */
#include "millerx.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>

enum { GIT_STATUS, GIT_LOG, GIT_DIFF, GIT_ADD, GIT_COMMIT, GIT_REVERT };
static gboolean has_dotgit(const char *dir);

typedef struct {
    int ref;                    /* the menu holds one; an action running holds one (its dialog's loop
                                 * may let the menu go first) */
    Win *w;
    char *top;                  /* the repository's top folder */
    GPtrArray *paths;           /* what was clicked (absolute paths), or the folder */
} GitCtx;

static void ctx_free(gpointer p)
{
    GitCtx *g = p;
    if (--g->ref > 0)
        return;
    win_unref(g->w);
    g_free(g->top);
    g_ptr_array_unref(g->paths);
    g_free(g);
}

/* git -C top args…; its output and errors (new strings) when asked for; TRUE when it exited 0 */
static gboolean git_run(const char *top, const char *const *args, char **out, char **err)
{
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, (char *) "git");
    /* file names are names, never patterns: "a[1].txt" must not also mean "a1.txt" (a Revert would
     * throw away that one's changes too); and no fsmonitor program from the repository's config */
    g_ptr_array_add(a, (char *) "--literal-pathspecs");
    g_ptr_array_add(a, (char *) "-c");
    g_ptr_array_add(a, (char *) "core.fsmonitor=false");
    g_ptr_array_add(a, (char *) "-C");
    g_ptr_array_add(a, (char *) top);
    for (int i = 0; args[i]; i++)
        g_ptr_array_add(a, (char *) args[i]);
    g_ptr_array_add(a, NULL);
    int status = 0;
    char *o = NULL, *e = NULL;
    gboolean ok = g_spawn_sync(NULL, (char **) a->pdata, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, &o, &e, &status, NULL) && g_spawn_check_wait_status(status, NULL);
    g_ptr_array_free(a, TRUE);
    if (out)
        *out = o;
    else
        g_free(o);
    if (err)
        *err = e;
    else
        g_free(e);
    return ok;
}

/* the repository a folder is in (a new string), or NULL */
static char *git_top(const char *dir)
{
    const char *args[] = { "rev-parse", "--show-toplevel", NULL };
    char *out = NULL;
    if (!git_run(dir, args, &out, NULL)) {
        g_free(out);
        return NULL;
    }
    size_t len = strlen(out);           /* only the line's end: a folder name may end in a space */
    while (len && out[len - 1] == '\n')
        out[--len] = '\0';
    if (!*out) {
        g_free(out);
        return NULL;
    }
    return out;
}

/* "git <what> -- paths…" in the terminal, waiting for Enter at the end */
static void in_terminal(GitCtx *g, const char *const *what)
{
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, (char *) "millerx-git");     /* $0 */
    for (int i = 0; what[i]; i++)
        g_ptr_array_add(a, (char *) what[i]);
    g_ptr_array_add(a, (char *) "--");
    for (guint i = 0; i < g->paths->len; i++)
        g_ptr_array_add(a, g->paths->pdata[i]);
    g_ptr_array_add(a, NULL);
    win_terminal_run(g->w, g->top, "git --literal-pathspecs -c core.fsmonitor=false \"$@\"; echo; "
                     "printf '[finished: press Enter]'; read x",
                     (const char *const *) a->pdata);
    g_ptr_array_free(a, TRUE);
}

/* git with what + "--" + the paths, its error shown if it fails; TRUE when it worked */
static gboolean on_paths(GitCtx *g, const char *const *what, const char *fail_title)
{
    GPtrArray *a = g_ptr_array_new();
    for (int i = 0; what[i]; i++)
        g_ptr_array_add(a, (char *) what[i]);
    g_ptr_array_add(a, (char *) "--");
    for (guint i = 0; i < g->paths->len; i++)
        g_ptr_array_add(a, g->paths->pdata[i]);
    g_ptr_array_add(a, NULL);
    char *err = NULL;
    gboolean ok = git_run(g->top, (const char *const *) a->pdata, NULL, &err);
    g_ptr_array_free(a, TRUE);
    if (!ok)
        win_error(g->w, fail_title, err && *err ? g_strstrip(err) : _("git didn't say why."));
    g_free(err);
    return ok;
}

/* ---- Commit…: what goes in, the message, Commit ------------------------------------------------ */
typedef struct {
    GtkWidget *dialog, *text, *also, *commit;
    int staged, others;
} CommitWin;

static void commit_check(CommitWin *cw)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(cw->text));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char *t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    gboolean any = cw->staged > 0 || (cw->also && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cw->also)));
    gtk_widget_set_sensitive(cw->commit, any && *g_strstrip(t));
    g_free(t);
}

static void commit_changed(GtkWidget *any, CommitWin *cw)
{
    (void) any;
    commit_check(cw);
}

/* git status --porcelain -z: what is added ("M src/x.c"), and how many other changes there are */
static GString *read_status(const char *top, int *staged, int *others)
{
    const char *args[] = { "status", "--porcelain=v1", "-z", NULL };
    char *out = NULL;
    GString *list = g_string_new(NULL);
    *staged = *others = 0;
    if (git_run(top, args, &out, NULL) && out) {
        for (char *p = out; *p; p += strlen(p) + 1) {
            if (strlen(p) < 4)
                continue;
            char x = p[0], y = p[1];
            const char *name = p + 3;
            if (x == 'R' || x == 'C' || y == 'R' || y == 'C')   /* a rename: the old name follows */
                p += strlen(p) + 1;
            if (x != ' ' && x != '?' && x != '!') {
                (*staged)++;
                g_string_append_printf(list, "%c  %s\n", x, name);
            }
            if ((y != ' ' && y != '!') || x == '?')
                (*others)++;
        }
    }
    g_free(out);
    if (list->len)
        g_string_truncate(list, list->len - 1);
    return list;
}

static void do_commit(GitCtx *g)
{
    CommitWin cw = { 0 };
    GString *list = read_status(g->top, &cw.staged, &cw.others);
    if (!cw.staged && !cw.others) {
        win_status(g->w, "%s", _("Nothing to commit: no changes in this repository."));
        g_string_free(list, TRUE);
        return;
    }
    const char *bargs[] = { "symbolic-ref", "--short", "-q", "HEAD", NULL };
    char *branch = NULL;
    if (git_run(g->top, bargs, &branch, NULL))
        g_strstrip(branch);
    char *repo = g_path_get_basename(g->top);

    cw.dialog = gtk_dialog_new_with_buttons(_("Commit"), GTK_WINDOW(g->w->window), GTK_DIALOG_MODAL,
                                            _("Cancel"), GTK_RESPONSE_CANCEL, NULL);
    cw.commit = gtk_dialog_add_button(GTK_DIALOG(cw.dialog), _("Commit"), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_object_set(box, "margin", 12, NULL);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(cw.dialog))), box, TRUE, TRUE, 0);

    char *head = branch && *branch ? g_markup_printf_escaped(_("<b>Commit to “%s”</b> (branch %s)"), repo, branch)
                                   : g_markup_printf_escaped(_("<b>Commit to “%s”</b>"), repo);
    GtkWidget *hl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hl), head);
    gtk_label_set_xalign(GTK_LABEL(hl), 0);
    gtk_box_pack_start(GTK_BOX(box), hl, FALSE, FALSE, 0);

    GtkWidget *hint = gtk_label_new(_("The first line is the title; a blank line, then the details if you like."));
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_set_opacity(hint, 0.7);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    cw.text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(cw.text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(cw.text), TRUE);
    GtkWidget *ts = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(ts), GTK_SHADOW_IN);
    gtk_widget_set_size_request(ts, 520, 110);
    gtk_container_add(GTK_CONTAINER(ts), cw.text);
    gtk_box_pack_start(GTK_BOX(box), ts, TRUE, TRUE, 0);

    GtkWidget *gl = gtk_label_new(cw.staged ? _("Goes in:") : _("Nothing added yet."));
    gtk_label_set_xalign(GTK_LABEL(gl), 0);
    gtk_box_pack_start(GTK_BOX(box), gl, FALSE, FALSE, 0);
    if (cw.staged) {
        GtkWidget *files = gtk_label_new(list->str);
        gtk_label_set_xalign(GTK_LABEL(files), 0);
        gtk_label_set_selectable(GTK_LABEL(files), TRUE);
        PangoAttrList *at = pango_attr_list_new();
        pango_attr_list_insert(at, pango_attr_family_new("monospace"));
        gtk_label_set_attributes(GTK_LABEL(files), at);
        pango_attr_list_unref(at);
        GtkWidget *fs = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fs), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(fs), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(fs), 200);
        gtk_container_add(GTK_CONTAINER(fs), files);
        gtk_box_pack_start(GTK_BOX(box), fs, FALSE, FALSE, 0);
    }
    if (cw.others) {
        char *l = g_strdup_printf(ngettext("Also add the other changed file (%d)",
                                           "Also add the other changed files (%d)", cw.others), cw.others);
        cw.also = gtk_check_button_new_with_label(l);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cw.also), cw.staged == 0);
        gtk_widget_set_tooltip_text(cw.also, _("Changed, new and deleted files that aren't added yet go in too "
                                               "(git add -A). Files your .gitignore names stay out."));
        g_signal_connect(cw.also, "toggled", G_CALLBACK(commit_changed), &cw);
        gtk_box_pack_start(GTK_BOX(box), cw.also, FALSE, FALSE, 0);
        g_free(l);
    }
    g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(cw.text)), "changed", G_CALLBACK(commit_changed), &cw);
    commit_check(&cw);
    gtk_widget_show_all(cw.dialog);
    gtk_widget_grab_focus(cw.text);

    int r = gtk_dialog_run(GTK_DIALOG(cw.dialog));
    char *msg = NULL;
    gboolean also = cw.also && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cw.also));
    if (r == GTK_RESPONSE_OK) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(cw.text));
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(b, &s, &e);
        msg = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    }
    gtk_widget_destroy(cw.dialog);
    g_free(head);
    g_free(repo);
    g_free(branch);
    g_string_free(list, TRUE);
    if (!msg || g->w->dead) {
        g_free(msg);
        return;
    }

    char *err = NULL;
    const char *add[] = { "add", "-A", NULL };
    if (also && !git_run(g->top, add, NULL, &err)) {
        win_error(g->w, _("Couldn't add the changed files"), err && *err ? g_strstrip(err) : _("git didn't say why."));
        g_free(err);
        g_free(msg);
        return;
    }
    g_free(err);
    err = NULL;
    char *tmp = NULL;
    int fd = g_file_open_tmp("millerx-commit-XXXXXX", &tmp, NULL);
    gboolean written = fd >= 0 && write(fd, msg, strlen(msg)) == (ssize_t) strlen(msg);
    if (fd >= 0)
        close(fd);
    if (!written) {
        win_error(g->w, _("Couldn't commit"), _("The message couldn't be written to a temporary file."));
    } else {
        const char *args[] = { "commit", "-F", tmp, NULL };
        char *out = NULL;
        if (git_run(g->top, args, &out, &err)) {
            char *first = g_strdup(msg);
            char *nl = strchr(g_strstrip(first), '\n');
            if (nl)
                *nl = '\0';
            win_status(g->w, _("Committed: %s"), first);
            g_free(first);
        } else {
            /* git says why: no name set, a hook said no, nothing to commit … */
            const char *why = err && *g_strstrip(err) ? err : out && *g_strstrip(out) ? out : _("git didn't say why.");
            win_error(g->w, _("Couldn't commit"), why);
        }
        g_free(out);
    }
    if (tmp) {
        g_unlink(tmp);
        g_free(tmp);
    }
    g_free(err);
    g_free(msg);
}

/* ---- Revert… ----------------------------------------------------------------------------------- */
/* "git restore --source=HEAD" deletes a file that is only added (it isn't in HEAD): those are first
 * just taken out of Git again (restore --staged), so they are untracked and the restore leaves them
 * alone; then everything else gets its committed version back. */
static void do_revert(GitCtx *g)
{
    GPtrArray *a = g_ptr_array_new();
    const char *ask[] = { "diff", "--cached", "--name-only", "--diff-filter=A", "-z", "--" };
    for (guint i = 0; i < G_N_ELEMENTS(ask); i++)
        g_ptr_array_add(a, (char *) ask[i]);
    for (guint i = 0; i < g->paths->len; i++)
        g_ptr_array_add(a, g->paths->pdata[i]);
    g_ptr_array_add(a, NULL);
    char *out = NULL;
    GPtrArray *added = g_ptr_array_new_with_free_func(g_free);    /* relative to the top */
    if (git_run(g->top, (const char *const *) a->pdata, &out, NULL) && out) {
        /* git_run gives a string: walk the NUL-separated names up to the real end */
        for (char *p = out; *p; p += strlen(p) + 1)
            g_ptr_array_add(added, g_strdup(p));
    }
    g_free(out);
    g_ptr_array_free(a, TRUE);
    gboolean ok = TRUE;
    if (added->len) {
        a = g_ptr_array_new();
        g_ptr_array_add(a, (char *) "restore");
        g_ptr_array_add(a, (char *) "--staged");
        g_ptr_array_add(a, (char *) "--");
        for (guint i = 0; i < added->len; i++)
            g_ptr_array_add(a, added->pdata[i]);
        g_ptr_array_add(a, NULL);
        char *err = NULL;
        ok = git_run(g->top, (const char *const *) a->pdata, NULL, &err);
        g_ptr_array_free(a, TRUE);
        if (!ok)
            win_error(g->w, _("Couldn't revert"), err && *err ? g_strstrip(err) : _("git didn't say why."));
        g_free(err);
    }
    /* what's left to restore: the clicked paths that weren't just such an added file */
    GPtrArray *rest = g_ptr_array_new_with_free_func(g_free);
    size_t tl = strlen(g->top);
    for (guint i = 0; ok && i < g->paths->len; i++) {
        const char *p = g->paths->pdata[i];
        gboolean was_added = FALSE;
        if (!strncmp(p, g->top, tl) && p[tl] == '/')
            for (guint k = 0; k < added->len && !was_added; k++)
                was_added = !strcmp(p + tl + 1, added->pdata[k]);
        if (!was_added)
            g_ptr_array_add(rest, g_strdup(p));
    }
    if (ok && rest->len) {
        GPtrArray *keep = g->paths;
        g->paths = rest;
        const char *r[] = { "restore", "--source=HEAD", "--staged", "--worktree", NULL };
        ok = on_paths(g, r, _("Couldn't revert"));
        g->paths = keep;
    }
    if (ok)
        win_status(g->w, "%s", _("Reverted to the last commit"));
    g_ptr_array_unref(rest);
    g_ptr_array_unref(added);
}

/* ---- the menu ---------------------------------------------------------------------------------- */
static void git_activate(GtkMenuItem *mi, gpointer data)
{
    (void) data;
    GitCtx *g = g_object_get_data(G_OBJECT(mi), "git");
    int what = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "git-what"));
    if (!g || g->w->dead)
        return;
    g->ref++;
    switch (what) {
    case GIT_STATUS: {
        const char *a[] = { "status", NULL };
        in_terminal(g, a);
        break;
    }
    case GIT_LOG: {
        const char *a[] = { "log", NULL };
        in_terminal(g, a);
        break;
    }
    case GIT_DIFF: {
        const char *a[] = { "diff", "HEAD", NULL };     /* added and not added, both */
        in_terminal(g, a);
        break;
    }
    case GIT_ADD: {
        const char *a[] = { "add", NULL };
        if (on_paths(g, a, _("Couldn't add to Git")))
            win_status(g->w, ngettext("Added to Git: %u item", "Added to Git: %u items", g->paths->len),
                       g->paths->len);
        break;
    }
    case GIT_COMMIT:
        do_commit(g);
        break;
    case GIT_REVERT: {
        char *one = g_path_get_basename(g->paths->pdata[0]);
        char *t = g->paths->len == 1
            ? g_strdup_printf(_("Your changes to “%s” are thrown away: Git puts back the last committed "
                                "version. This can't be undone."), one)
            : g_strdup_printf(ngettext("Your changes to %u item are thrown away: Git puts back the last "
                                       "committed version. This can't be undone.",
                                       "Your changes to %u items are thrown away: Git puts back the last "
                                       "committed version. This can't be undone.", g->paths->len),
                              g->paths->len);
        char *t2 = g_strconcat(t, "\n\n", _("A file that was only added, never committed, stays as it is: it "
                                             "is just taken out of Git again."), NULL);
        gboolean yes = win_confirm(g->w, _("Revert to the last commit?"), t2, _("Revert"));
        g_free(t2);
        g_free(t);
        g_free(one);
        if (yes && !g->w->dead)
            do_revert(g);
        break;
    }
    }
    ctx_free(g);
}

static void git_item(GtkWidget *sub, GitCtx *g, int what, const char *label)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(mi), "git", g);
    g_object_set_data(G_OBJECT(mi), "git-what", GINT_TO_POINTER(what));
    g_signal_connect(mi, "activate", G_CALLBACK(git_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
}

void git_menu_add(GtkWidget *menu, Win *w, GList *files, GFile *folder)
{
    if (!LOOK.use_git || !folder)
        return;
    char *dir = g_file_get_path(folder);
    char *real = dir ? realpath(dir, NULL) : NULL;
    /* no .git above it: not a repository, and git isn't even started (a right-click in a slow
     * network folder doesn't wait for it) */
    char *top = real && has_dotgit(real) ? git_top(dir) : NULL;
    free(real);
    if (!top) {
        g_free(dir);
        return;
    }
    GitCtx *g = g_new0(GitCtx, 1);
    g->ref = 1;
    g->w = win_ref(w);
    g->top = top;
    g->paths = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = files; l; l = l->next) {
        char *p = g_file_get_path(l->data);
        if (p)
            g_ptr_array_add(g->paths, p);
    }
    if (!g->paths->len)
        g_ptr_array_add(g->paths, g_strdup(dir));
    g_free(dir);

    GtkWidget *sub = gtk_menu_new();
    git_item(sub, g, GIT_STATUS, _("Status"));
    git_item(sub, g, GIT_LOG, _("Log"));
    git_item(sub, g, GIT_DIFF, _("Diff"));
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
    git_item(sub, g, GIT_ADD, _("Add"));
    git_item(sub, g, GIT_COMMIT, _("Commit…"));
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
    git_item(sub, g, GIT_REVERT, _("Revert…"));
    GtkWidget *mi = gtk_menu_item_new_with_label(_("Git"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), sub);
    g_object_set_data_full(G_OBJECT(mi), "git-ctx", g, ctx_free);   /* goes with the menu */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

/* ---- colours in the columns (Settings → Tools → Git, off by default) ----------------------------
 * Green: new (not in Git yet, or only added); red: changed since the last commit; a folder takes the
 * colour of what's inside it. git keeps the state itself (.git/index: each file's size, time and
 * hash), so millerX stores nothing: it asks "git status" about the shown folder only (fast also in
 * a big repository), keeps the answer in the column's rows, and asks again a moment after something
 * changes there, or after .git/index changes (a commit).
 * git status runs by itself here, not on a click, so a repository's own config must not be able to
 * run programs through it. git is asked which repository it really uses (no guessing: symlinked
 * folders, worktrees), bare repositories found by searching are refused, and a repository whose
 * config has filters, includes or fsmonitor (or a zero byte, which hides things from a text search)
 * gets no colours. status itself runs with fsmonitor and hooks off and without submodules (they have
 * configs of their own). Never as root. The review that found the holes this closes: 0.8.8. */

static const char *const GIT_SAFE[] = { "git", "--no-optional-locks", "--literal-pathspecs", "-c", "safe.bareRepository=explicit",
                                        "-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null", NULL };

static gboolean colours_on(void)
{
    return LOOK.use_git && LOOK.git_colours && !is_root();
}

/* a .git somewhere above dir (or dir itself)? Cheap, runs nothing: most folders aren't in a
 * repository, and then git isn't started at all */
static gboolean has_dotgit(const char *dir)
{
    char *d = g_strdup(dir);
    gboolean found = FALSE;
    while (!found) {
        char *g = g_build_filename(d, ".git", NULL);
        found = g_file_test(g, G_FILE_TEST_EXISTS);
        g_free(g);
        char *up = g_path_get_dirname(d);
        gboolean last = !strcmp(up, d);
        g_free(d);
        d = up;
        if (last)
            break;
    }
    g_free(d);
    return found;
}

/* one config file asks git to run something during status, or pulls in a file that could. Read
 * whole: a zero byte would end a text search early while git reads on, so it counts as risky. */
static gboolean risky_config(const char *file)
{
    char *text = NULL;
    gsize len = 0;
    if (!g_file_get_contents(file, &text, &len, NULL))
        return FALSE;
    gboolean risky = memchr(text, '\0', len) != NULL;
    if (!risky) {
        char *low = g_ascii_strdown(text, (gssize) len);
        risky = strstr(low, "[filter") || strstr(low, "[include") || strstr(low, "fsmonitor");
        g_free(low);
    }
    g_free(text);
    return risky;
}

/* the git dir git itself uses: its config, a worktree's own, and the main repository's */
static gboolean gitdir_risky(const char *gitdir)
{
    const char *names[] = { "config", "config.worktree" };
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        char *f = g_build_filename(gitdir, names[i], NULL);
        gboolean r = risky_config(f);
        g_free(f);
        if (r)
            return TRUE;
    }
    char *cd = g_build_filename(gitdir, "commondir", NULL), *common = NULL;
    gboolean risky = FALSE;
    if (g_file_get_contents(cd, &common, NULL, NULL)) {
        g_strstrip(common);
        char *main = g_path_is_absolute(common) ? g_strdup(common) : g_build_filename(gitdir, common, NULL);
        char *f = g_build_filename(main, "config", NULL);
        risky = risky_config(f);
        g_free(f);
        g_free(main);
    }
    g_free(common);
    g_free(cd);
    return risky;
}

/* git GIT_SAFE -C dir args… ; its output (bytes, may hold NULs) when it exited 0 */
static GBytes *git_safe(const char *dir, const char *const *args)
{
    GPtrArray *a = g_ptr_array_new();
    for (int i = 0; GIT_SAFE[i]; i++)
        g_ptr_array_add(a, (char *) GIT_SAFE[i]);
    g_ptr_array_add(a, (char *) "-C");
    g_ptr_array_add(a, (char *) dir);
    for (int i = 0; args[i]; i++)
        g_ptr_array_add(a, (char *) args[i]);
    g_ptr_array_add(a, NULL);
    GSubprocess *sp = g_subprocess_newv((const char *const *) a->pdata,
                                        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    g_ptr_array_free(a, TRUE);
    GBytes *out = NULL;
    if (sp && (!g_subprocess_communicate(sp, NULL, NULL, &out, NULL, NULL)
               || !g_subprocess_get_if_exited(sp) || g_subprocess_get_exit_status(sp)))
        g_clear_pointer(&out, g_bytes_unref);
    g_clear_object(&sp);
    return out;
}

typedef struct {
    char *dir;                  /* the column's folder, its real path */
    GHashTable *states;         /* result: name → GIT_NEW / GIT_CHANGED */
    gboolean all_new;           /* the folder itself is new (inside an untracked folder) */
    char *gitdir;               /* result: the git dir used, to watch its index */
} StatusJob;

static void job_free(gpointer p)
{
    StatusJob *j = p;
    g_free(j->dir);
    g_free(j->gitdir);
    if (j->states)
        g_hash_table_unref(j->states);
    g_free(j);
}

/* worker thread: which repository, is it safe, what's its status here */
static void status_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    (void) src;
    (void) cancel;
    StatusJob *j = data;
    const char *where[] = { "rev-parse", "--absolute-git-dir", "--show-toplevel", NULL };
    GBytes *b = git_safe(j->dir, where);
    if (!b) {
        g_task_return_boolean(task, FALSE);
        return;
    }
    char *text = g_strndup(g_bytes_get_data(b, NULL), g_bytes_get_size(b));
    g_bytes_unref(b);
    char **lines = g_strsplit(g_strstrip(text), "\n", 3);
    g_free(text);
    if (g_strv_length(lines) != 2 || gitdir_risky(lines[0])) {   /* a bare repository has no top: no colours */
        g_strfreev(lines);
        g_task_return_boolean(task, FALSE);
        return;
    }
    j->gitdir = g_strdup(lines[0]);
    char *top = g_strdup(lines[1]);
    g_strfreev(lines);
    /* the folder inside the repository: "" or "src/ui/" (both real paths, so no symlink mix-up) */
    size_t tl = strlen(top);
    char *prefix = NULL;
    if (!strcmp(j->dir, top))
        prefix = g_strdup("");
    else if (!strncmp(j->dir, top, tl) && j->dir[tl] == '/')
        prefix = g_strconcat(j->dir + tl + 1, "/", NULL);
    g_free(top);
    if (!prefix) {
        g_task_return_boolean(task, FALSE);
        return;
    }
    const char *st[] = { "status", "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=all",
                         "--", ".", NULL };
    GBytes *out = git_safe(j->dir, st);
    if (!out) {
        g_free(prefix);
        g_task_return_boolean(task, FALSE);
        return;
    }
    /* porcelain v1, -z: "XY path\0", a rename or copy (in X or Y) adds "old\0"; paths from the top */
    j->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gsize len = 0;
    const char *p = g_bytes_get_data(out, &len), *end = p + len;
    size_t pl = strlen(prefix);
    while (p < end) {
        const char *z = memchr(p, '\0', end - p);
        if (!z || z - p < 4)
            break;
        char x = p[0], y = p[1];
        const char *path = p + 3;
        size_t n = z - path;
        p = z + 1;
        if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {     /* skip the old name */
            const char *z2 = memchr(p, '\0', end - p);
            p = z2 ? z2 + 1 : end;
        }
        if (x == '!')
            continue;
        /* "?? src/new/" and we are in src/new/sub/: everything here is new */
        if (x == '?' && n && path[n - 1] == '/' && n <= pl && !strncmp(path, prefix, n)) {
            j->all_new = TRUE;
            continue;
        }
        if (n <= pl || strncmp(path, prefix, pl))
            continue;
        const char *rest = path + pl;
        const char *slash = memchr(rest, '/', z - rest);
        char *name = g_strndup(rest, slash ? (size_t) (slash - rest) : (size_t) (z - rest));
        int s = (x == '?' || (x == 'A' && y != 'M' && y != 'D')) ? GIT_NEW : GIT_CHANGED;
        int had = GPOINTER_TO_INT(g_hash_table_lookup(j->states, name));
        if (*name && s > had)                                   /* a folder: changed wins over new */
            g_hash_table_replace(j->states, name, GINT_TO_POINTER(s));
        else
            g_free(name);
    }
    g_bytes_unref(out);
    g_free(prefix);
    g_task_return_boolean(task, TRUE);
}

static void set_all(Column *c, GHashTable *states, gboolean all_new)
{
    GtkTreeModel *m = GTK_TREE_MODEL(c->store);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        GFile *f = NULL;
        int old = GIT_CLEAN, now = GIT_CLEAN;
        gtk_tree_model_get(m, &it, C_FILE, &f, C_GIT, &old, -1);
        if (all_new) {
            now = GIT_NEW;
        } else if (states && f) {
            char *base = g_file_get_basename(f);
            now = GPOINTER_TO_INT(g_hash_table_lookup(states, base));
            g_free(base);
        }
        if (now != old)
            gtk_list_store_set(c->store, &it, C_GIT, now, -1);
        g_clear_object(&f);
    }
}

static void on_index(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer data)
{
    (void) m;
    (void) f;
    (void) o;
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED
        || ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN)
        git_colours_schedule(data);
}

static void status_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src;
    Column *c = data;
    StatusJob *j = g_task_get_task_data(G_TASK(res));
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    c->git_busy = FALSE;
    if (!c->closed && colours_on()) {
        set_all(c, ok ? j->states : NULL, ok && j->all_new);
        if (ok && !c->git_index && j->gitdir) {
            char *ix = g_build_filename(j->gitdir, "index", NULL);
            GFile *gf = g_file_new_for_path(ix);
            c->git_index = g_file_monitor_file(gf, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
            if (c->git_index)
                g_signal_connect(c->git_index, "changed", G_CALLBACK(on_index), c);
            g_object_unref(gf);
            g_free(ix);
        }
        if (c->git_again) {
            c->git_again = FALSE;
            git_colours_schedule(c);
        }
    }
    column_unref(c);
}

static gboolean colours_run(gpointer data)
{
    Column *c = data;
    c->git_timer = 0;
    if (c->closed || c->computer || c->trash || !colours_on())
        return G_SOURCE_REMOVE;
    if (c->git_busy) {
        c->git_again = TRUE;
        return G_SOURCE_REMOVE;
    }
    char *path = g_file_get_path(c->gfile);
    char *dir = path ? realpath(path, NULL) : NULL;
    g_free(path);
    if (!dir || !has_dotgit(dir)) {             /* not in a repository: never coloured, nothing to do */
        free(dir);
        return G_SOURCE_REMOVE;
    }
    StatusJob *j = g_new0(StatusJob, 1);
    j->dir = g_strdup(dir);
    free(dir);
    column_ref(c);
    c->git_busy = TRUE;
    GTask *task = g_task_new(NULL, NULL, status_done, c);
    g_task_set_task_data(task, j, job_free);
    g_task_run_in_thread(task, status_thread);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

void git_colours_schedule(Column *c)
{
    if (c->closed)
        return;
    if (!colours_on()) {
        git_colours_stop(c);
        return;
    }
    if (!c->git_timer) {            /* many changes at once: one git status */
        column_ref(c);
        c->git_timer = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 300, colours_run, c,
                                          (GDestroyNotify) column_unref);
    }
}

void git_colours_stop(Column *c)
{
    if (c->git_timer) {
        g_source_remove(c->git_timer);
        c->git_timer = 0;
    }
    if (c->git_index) {
        g_signal_handlers_disconnect_by_data(c->git_index, c);
        g_file_monitor_cancel(c->git_index);
        g_clear_object(&c->git_index);
    }
    c->git_again = FALSE;
    if (!c->closed && c->store)
        set_all(c, NULL, FALSE);
}
