/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* millerX entry point. */
#include "millerx.h"
#include <locale.h>
#include <stdlib.h>
#include <string.h>

/* ---- org.freedesktop.FileManager1: what browsers call for "Show in folder" ----------------------
 * Taken while millerX runs and nobody else has it (Thunar's daemon, Dolphin, …); we never push
 * another file manager away, and let one take it from us. Who answers when no file manager is
 * running is the system's choice (a D-Bus .service file), not ours. */
static const char FM1_XML[] =
    "<node><interface name='org.freedesktop.FileManager1'>"
    "<method name='ShowFolders'><arg type='as' name='URIs' direction='in'/>"
    "<arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItems'><arg type='as' name='URIs' direction='in'/>"
    "<arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItemProperties'><arg type='as' name='URIs' direction='in'/>"
    "<arg type='s' name='StartupId' direction='in'/></method>"
    "</interface></node>";

/* started by D-Bus for "Show in folder" (millerx --dbus): no window of our own, only the asked one */
char *LANGUAGE_AT_START;
static gboolean DBUS_ONLY;
static guint DBUS_WAIT;

static void dbus_release(GApplication *app)
{
    if (DBUS_WAIT) {
        g_source_remove(DBUS_WAIT);
        DBUS_WAIT = 0;
        g_application_release(app);   /* from now on it lives as long as its windows */
    }
}

static gboolean dbus_timeout(gpointer app)
{
    DBUS_WAIT = 0;
    g_application_release(app);       /* nobody asked after all: quit */
    return G_SOURCE_REMOVE;
}

static void fm1_call(GDBusConnection *con, const char *sender, const char *path, const char *iface,
                     const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer app)
{
    GVariantIter *uris;
    const char *sid, *uri;
    g_variant_get(params, "(as&s)", &uris, &sid);
    gboolean props = !strcmp(method, "ShowItemProperties");
    while (g_variant_iter_next(uris, "&s", &uri)) {
        /* ShowFolders: the folder itself; ShowItems: its folder, the item highlighted */
        GFile *f = g_file_new_for_commandline_arg(uri);
        Win *w = win_new(GTK_APPLICATION(app), f);
        if (*sid)
            gtk_window_set_startup_id(GTK_WINDOW(w->window), sid);
        gtk_window_present(GTK_WINDOW(w->window));
        if (props)
            dialog_properties(w, f);
        g_object_unref(f);
    }
    g_variant_iter_free(uris);
    g_dbus_method_invocation_return_value(inv, NULL);
    dbus_release(app);
}

static const GDBusInterfaceVTable FM1_VTABLE = { fm1_call, NULL, NULL, { 0 } };

static void fm1_bus(GDBusConnection *con, const char *name, gpointer app)
{
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(FM1_XML, NULL);
    if (info) {
        g_dbus_connection_register_object(con, "/org/freedesktop/FileManager1", info->interfaces[0],
                                          &FM1_VTABLE, app, NULL, NULL);
        g_dbus_node_info_unref(info);
    }
}

static void on_startup(GApplication *app, gpointer d)
{
    (void) d;
    /* queued: with several millerX running, the next one takes over when this one quits */
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.FileManager1", G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                   fm1_bus, NULL, NULL, app, NULL);
    look_load();
    sorting_load();
    thumbcache_auto_clean();
    highlight_apply();
    gtk_window_set_default_icon_name("millerx");
    ud_start(GTK_APPLICATION(app));        /* sticks and disks from udisks2 (GIO if it isn't there) */
    app_watch_update(GTK_APPLICATION(app));
}

static void on_activate(GApplication *app, gpointer d)
{
    (void) d;
    if (DBUS_ONLY) {
        DBUS_ONLY = FALSE;
        g_application_hold(app);
        DBUS_WAIT = g_timeout_add_seconds(30, dbus_timeout, app);
        return;
    }
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
    /* Settings → Language: English anyway, the wizard's English or Viennese; "system" = the system's.
     * LANGUAGE also turns GTK's own words (buttons, the colour picker) the same way. */
    /* LANGUAGE as the session had it, kept for the programs we start (and for a restart, which
     * must not inherit the choice of this run) */
    const char *orig = getenv("MILLERX_LANG0");
    if (orig) {
        if (!strncmp(orig, "set:", 4))
            setenv("LANGUAGE", orig + 4, 1);
        else
            unsetenv("LANGUAGE");
    } else {
        const char *l = getenv("LANGUAGE");
        char *v = l ? g_strconcat("set:", l, NULL) : g_strdup("unset");
        setenv("MILLERX_LANG0", v, 1);
        g_free(v);
    }
    char *lang = look_language();
    LANGUAGE_AT_START = g_strdup(lang);
    char *localedir = NULL;                         /* an own language file: bound instead of ours */
#ifdef ENABLE_NLS
    if (g_str_has_prefix(lang, "own:")) {
        localedir = own_language_prepare(lang + 4);
        if (localedir)
            setenv("LANGUAGE", "en@own:en", 1);
        else {                                       /* gone or broken: the system's language */
            g_free(lang);
            lang = g_strdup("system");
        }
    }
#endif
    if (!strcmp(lang, "en"))
        setenv("LANGUAGE", "en", 1);
    else if (!strcmp(lang, "wizard"))
        setenv("LANGUAGE", "en@wizard:en", 1);
    else if (!strcmp(lang, "wienerisch"))           /* Viennese; what it lacks (GTK's own words) in German */
        setenv("LANGUAGE", "de@wienerisch:de", 1);
    gtk_disable_setlocale();                    /* we set it here; GTK would undo the switch below */
    setlocale(LC_ALL, "");
    if (strcmp(lang, "system")) {
        /* With messages in the C locale LANGUAGE is ignored, and glibc does that for C.UTF-8 too
         * (rizitis, #5): the choices above need a real locale. Only the messages switch; dates,
         * numbers and sorting stay as the session has them. */
        const char *m = setlocale(LC_MESSAGES, NULL);
        if (m && (!strcmp(m, "C") || !strcmp(m, "POSIX") || !g_ascii_strncasecmp(m, "C.", 2))) {
            static const char *const real[] = { "en_US.UTF-8", "en_GB.UTF-8", "C.UTF-8" };
            for (guint i = 0; i < G_N_ELEMENTS(real) && !setlocale(LC_MESSAGES, real[i]); i++)
                ;
        }
    }
    g_free(lang);
    (void) localedir;
#ifdef ENABLE_NLS
    bindtextdomain(GETTEXT_PACKAGE, localedir ? localedir : LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
#endif
    if (argc > 1 && !strcmp(argv[1], "--dbus")) {   /* not a file to open: ours */
        DBUS_ONLY = TRUE;
        argv[1] = argv[0];
        argv++;
        argc--;
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
