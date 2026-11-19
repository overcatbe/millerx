/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* millerX entry point. */
#include "millerx.h"
#include <string.h>

static void on_startup(GApplication *app, gpointer d)
{
    (void) app; (void) d;
    look_load();
    gtk_window_set_default_icon_name("millerx");
}

static void on_activate(GApplication *app, gpointer d)
{
    (void) d;
    GFile *home = g_file_new_for_path(g_get_home_dir());
    win_new(GTK_APPLICATION(app), home);
    g_object_unref(home);
}

static void on_open(GApplication *app, GFile **files, int n, const char *hint, gpointer d)
{
    (void) hint; (void) d;
    for (int i = 0; i < n; i++)
        win_new(GTK_APPLICATION(app), files[i]);
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))) {
        g_print("millerX %s\n", MILLERX_VERSION);
        return 0;
    }
    GtkApplication *app = gtk_application_new("org.mix.MillerX",
                                              G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
