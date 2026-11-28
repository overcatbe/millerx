/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* Sticks, disks, SD cards and discs straight from udisks2 (the system bus), without GVfs.
 *
 * udisks2 is what GVfs itself asks; talking to it directly keeps the Computer column working on
 * desktops without GVfs (or with a broken session bus), and gives us what we need for doing it
 * "the stick-shift way": what arrived, mount it, where it went, unmount (which writes out what is
 * left), lock, power off, safe to remove. Without udisks2 (no udisksd) the Computer column falls
 * back to GIO as before.
 *
 * Shown here: every device with a file system or an encrypted (LUKS) container that udisks2
 * doesn't ask us to hide, except the system's own devices while they are mounted (those come
 * from the mount table, places.c). An unlocked container is shown as its cleartext device. */
#include "millerx.h"
#include <string.h>

#define UD "org.freedesktop.UDisks2"
#define UD_BLOCK UD ".Block"
#define UD_FS UD ".Filesystem"
#define UD_CRYPT UD ".Encrypted"
#define UD_DRIVE UD ".Drive"
#define UD_LOOP UD ".Loop"

static GDBusObjectManager *MGR;
static GHashTable *KNOWN;           /* object paths already seen as devices: not "arrived" again */
static gint64 SETTLE_UNTIL;         /* udisks2 (re)starting lists everything anew: no notices */
static guint REFRESH;

/* ---- properties --------------------------------------------------------------------------------- */
static GVariant *prop(GDBusObject *o, const char *iface, const char *name)
{
    GDBusInterface *i = o ? g_dbus_object_get_interface(o, iface) : NULL;
    if (!i)
        return NULL;
    GVariant *v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(i), name);
    g_object_unref(i);
    return v;
}

static gboolean has(GDBusObject *o, const char *iface)
{
    GDBusInterface *i = o ? g_dbus_object_get_interface(o, iface) : NULL;
    if (i)
        g_object_unref(i);
    return i != NULL;
}

static gboolean prop_b(GDBusObject *o, const char *iface, const char *name)
{
    GVariant *v = prop(o, iface, name);
    gboolean b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    if (v)
        g_variant_unref(v);
    return b;
}

static guint64 prop_t(GDBusObject *o, const char *iface, const char *name)
{
    GVariant *v = prop(o, iface, name);
    guint64 t = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64) ? g_variant_get_uint64(v) : 0;
    if (v)
        g_variant_unref(v);
    return t;
}

/* a string, object path or byte string property; NULL when empty (or "/", no object) */
static char *prop_s(GDBusObject *o, const char *iface, const char *name)
{
    GVariant *v = prop(o, iface, name);
    char *s = NULL;
    if (v && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
        s = g_variant_dup_string(v, NULL);
    else if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING))
        s = g_variant_dup_bytestring(v, NULL);
    if (v)
        g_variant_unref(v);
    if (s && (!*s || !strcmp(s, "/"))) {
        g_free(s);
        s = NULL;
    }
    return s;
}

static GDBusObject *object_at(const char *path)
{
    return MGR && path ? g_dbus_object_manager_get_object(MGR, path) : NULL;
}

/* where the file system is mounted (the first place), or NULL */
static char *mount_point(GDBusObject *o)
{
    GVariant *v = prop(o, UD_FS, "MountPoints");
    char *r = NULL;
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE("aay")) && g_variant_n_children(v) > 0) {
        GVariant *c = g_variant_get_child_value(v, 0);
        r = g_variant_dup_bytestring(c, NULL);
        g_variant_unref(c);
    }
    if (v)
        g_variant_unref(v);
    if (r && !*r) {
        g_free(r);
        r = NULL;
    }
    return r;
}

/* the container an unlocked (cleartext) device comes from, or NULL (a new ref) */
static GDBusObject *backing_of(GDBusObject *o)
{
    char *b = prop_s(o, UD_BLOCK, "CryptoBackingDevice");
    GDBusObject *r = object_at(b);
    g_free(b);
    return r;
}

/* the drive's object path (through the container for an unlocked device), or NULL */
static char *drive_of(GDBusObject *o)
{
    char *d = prop_s(o, UD_BLOCK, "Drive");
    if (!d) {
        GDBusObject *b = backing_of(o);
        if (b) {
            d = prop_s(b, UD_BLOCK, "Drive");
            g_object_unref(b);
        }
    }
    return d;
}

static gboolean is_system(GDBusObject *o)
{
    GDBusObject *b = backing_of(o);
    gboolean sys = prop_b(b ? b : o, UD_BLOCK, "HintSystem");
    if (b)
        g_object_unref(b);
    return sys;
}

int ud_state(GObject *obj)
{
    GDBusObject *o = G_DBUS_OBJECT(obj);
    if (!has(o, UD_BLOCK) || prop_b(o, UD_BLOCK, "HintIgnore"))
        return UD_SKIP;
    if (has(o, UD_CRYPT)) {
        char *clear = prop_s(o, UD_CRYPT, "CleartextDevice");
        g_free(clear);
        return clear ? UD_SKIP : UD_LOCKED;           /* unlocked: its cleartext device is shown */
    }
    if (!has(o, UD_FS))
        return UD_SKIP;                               /* a whole disk, swap, a partition table … */
    char *m = mount_point(o);
    g_free(m);
    return m ? UD_MOUNTED : UD_UNMOUNTED;
}

/* ---- what a row shows -------------------------------------------------------------------------- */
static GIcon *device_icon(GDBusObject *o)
{
    char *hint = prop_s(o, UD_BLOCK, "HintIconName");
    if (hint) {
        GIcon *i = g_themed_icon_new_with_default_fallbacks(hint);
        g_free(hint);
        return i;
    }
    char *d = drive_of(o);
    GDBusObject *drive = object_at(d);
    g_free(d);
    const char *name = "drive-harddisk";
    char *bus = drive ? prop_s(drive, UD_DRIVE, "ConnectionBus") : NULL;
    char *dev = prop_s(o, UD_BLOCK, "Device");
    if (drive && prop_b(drive, UD_DRIVE, "Optical"))
        name = "media-optical";
    else if (dev && g_str_has_prefix(dev, "/dev/mmcblk"))
        name = "media-flash";
    else if (!g_strcmp0(bus, "usb"))
        name = "drive-removable-media-usb";           /* falls back to drive-removable-media */
    else if (drive && prop_b(drive, UD_DRIVE, "Removable"))
        name = "drive-removable-media";
    else if (has(o, UD_LOOP))
        name = "media-optical";                       /* a mounted image: the disc icon, as GIO did */
    g_free(bus);
    g_free(dev);
    if (drive)
        g_object_unref(drive);
    return g_themed_icon_new_with_default_fallbacks(name);
}

char *ud_name(GObject *obj)
{
    GDBusObject *o = G_DBUS_OBJECT(obj);
    char *label = prop_s(o, UD_BLOCK, "IdLabel");
    if (!label)
        label = prop_s(o, UD_BLOCK, "HintName");
    if (label)
        return label;
    char *size = g_format_size(prop_t(o, UD_BLOCK, "Size"));
    char *n = ud_state(obj) == UD_LOCKED ? g_strdup_printf(_("%s Encrypted"), size)
                                          : g_strdup_printf(_("%s Volume"), size);
    g_free(size);
    return n;
}

/* the device as people know it: /dev/sdb1, also for the unlocked container (not /dev/dm-0) */
static char *device_name(GDBusObject *o)
{
    GDBusObject *b = backing_of(o);
    char *d = prop_s(b ? b : o, UD_BLOCK, "PreferredDevice");
    if (!d)
        d = prop_s(b ? b : o, UD_BLOCK, "Device");
    if (b)
        g_object_unref(b);
    return d;
}

static void row_free(gpointer p)
{
    UdRow *r = p;
    g_object_unref(r->obj);
    g_clear_object(&r->icon);
    g_free(r->name);
    g_free(r->dev);
    g_free(r->fs);
    g_free(r->mount);
    g_free(r);
}

static int row_cmp(gconstpointer a, gconstpointer b)
{
    const UdRow *x = *(UdRow *const *) a, *y = *(UdRow *const *) b;
    return g_strcmp0(x->dev, y->dev);
}

gboolean ud_ready(void)
{
    char *owner = MGR ? g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(MGR)) : NULL;
    g_free(owner);
    return owner != NULL;
}

GPtrArray *ud_rows(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(row_free);
    if (!ud_ready())
        return a;
    GList *all = g_dbus_object_manager_get_objects(MGR);
    for (GList *l = all; l; l = l->next) {
        GDBusObject *o = l->data;
        int st = ud_state(G_OBJECT(o));
        if (st == UD_SKIP)
            continue;
        gboolean sys = is_system(o);
        if (sys && st == UD_MOUNTED)
            continue;                                 /* the system's own disks: from the mount table */
        UdRow *r = g_new0(UdRow, 1);
        r->obj = g_object_ref(G_OBJECT(o));
        r->state = st;
        r->system = sys;
        r->name = ud_name(G_OBJECT(o));
        r->icon = device_icon(o);
        r->dev = device_name(o);
        char *fs = prop_s(o, UD_BLOCK, "IdType");
        GDBusObject *b = backing_of(o);
        r->fs = b ? g_strdup_printf(_("%s, encrypted"), fs ? fs : "?") : g_strdup(fs ? fs : "");
        g_free(fs);
        if (b)
            g_object_unref(b);
        r->mount = st == UD_MOUNTED ? mount_point(o) : NULL;
        g_ptr_array_add(a, r);
    }
    g_list_free_full(all, g_object_unref);
    g_ptr_array_sort(a, row_cmp);
    return a;
}

char *ud_describe(GObject *obj)
{
    GDBusObject *o = G_DBUS_OBJECT(obj);
    char *dev = device_name(o), *fs = prop_s(o, UD_BLOCK, "IdType");
    char *size = g_format_size(prop_t(o, UD_BLOCK, "Size"));
    char *s = g_strdup_printf("%s, %s, %s", dev ? dev : "?", fs ? fs : "?", size);
    g_free(dev);
    g_free(fs);
    g_free(size);
    return s;
}

char *ud_mount_point(GObject *obj)
{
    return mount_point(G_DBUS_OBJECT(obj));
}

gboolean ud_auto(GObject *obj)
{
    return prop_b(G_DBUS_OBJECT(obj), UD_BLOCK, "HintAuto");
}

/* does anything on that drive hold the running system (Linux started from a USB disk)? Its other
 * partitions are listed, but "safe to remove" would unmount the system's too */
static gboolean drive_holds_system(const char *drive)
{
    static const char *const SYS[] = { "/", "/usr", "/var", "/home", "/boot", "/boot/efi", "/opt", "/tmp" };
    gboolean r = FALSE;
    GList *all = g_dbus_object_manager_get_objects(MGR);
    for (GList *l = all; l && !r; l = l->next) {
        char *d = has(l->data, UD_FS) ? drive_of(l->data) : NULL;
        if (d && !strcmp(d, drive)) {
            GVariant *v = prop(l->data, UD_FS, "MountPoints");
            for (gsize i = 0; v && g_variant_is_of_type(v, G_VARIANT_TYPE("aay")) && i < g_variant_n_children(v) && !r; i++) {
                GVariant *c = g_variant_get_child_value(v, i);
                const char *mp = g_variant_get_bytestring(c);
                for (gsize k = 0; k < G_N_ELEMENTS(SYS) && !r; k++)
                    r = !strcmp(mp, SYS[k]);
                g_variant_unref(c);
            }
            if (v)
                g_variant_unref(v);
        }
        g_free(d);
    }
    g_list_free_full(all, g_object_unref);
    return r;
}

/* can it be taken away: a stick, a card, a disc, a USB disk, a mounted image; never the drive
 * the system runs from */
gboolean ud_removable(GObject *obj)
{
    GDBusObject *o = G_DBUS_OBJECT(obj);
    if (has(o, UD_LOOP))
        return TRUE;
    char *d = drive_of(o);
    if (!d || drive_holds_system(d)) {
        g_free(d);
        return FALSE;
    }
    GDBusObject *drive = object_at(d);
    g_free(d);
    if (!drive)
        return FALSE;
    char *bus = prop_s(drive, UD_DRIVE, "ConnectionBus");
    gboolean r = prop_b(drive, UD_DRIVE, "Removable") || prop_b(drive, UD_DRIVE, "MediaRemovable") ||
                 prop_b(drive, UD_DRIVE, "Ejectable") || prop_b(drive, UD_DRIVE, "CanPowerOff") ||
                 !g_strcmp0(bus, "usb");
    g_free(bus);
    g_object_unref(drive);
    return r;
}

/* ---- calls --------------------------------------------------------------------------------------- */
static GVariant *no_options(void)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "auth.no_user_interaction", g_variant_new_boolean(FALSE));
    return g_variant_builder_end(&b);
}

/* udisks2's error, said the way people understand it */
static char *explain(GError *e)
{
    char *remote = g_dbus_error_get_remote_error(e);
    g_dbus_error_strip_remote_error(e);
    char *s;
    if (!g_strcmp0(remote, UD ".Error.DeviceBusy"))
        s = g_strdup(_("Something still uses it (a program, or a terminal open in it). Close that and try again."));
    else if (!g_strcmp0(remote, UD ".Error.NotAuthorized") || !g_strcmp0(remote, UD ".Error.NotAuthorizedCanObtain") ||
             !g_strcmp0(remote, UD ".Error.NotAuthorizedDismissed"))
        s = g_strdup_printf(_("Not allowed: %s"), e->message);
    else
        s = g_strdup(e->message);
    g_free(remote);
    return s;
}

static gboolean error_is(GError *e, const char *name)
{
    char *remote = e ? g_dbus_error_get_remote_error(e) : NULL;
    gboolean r = !g_strcmp0(remote, name);
    g_free(remote);
    return r;
}

typedef struct {
    char *path, *iface, *method;
    GVariant *params;               /* floating until sent */
} Step;

typedef struct {
    Win *w;
    UdDone done;
    gpointer data;
    GPtrArray *steps;               /* Step*, run one after the other */
    guint next;
    char *clear;                    /* mounting: the device to mount (after unlocking) */
    int tries;
    char *result;
} Op;

static void step_free(gpointer p)
{
    Step *s = p;
    g_free(s->path);
    g_free(s->iface);
    g_free(s->method);
    if (s->params)
        g_variant_unref(g_variant_ref_sink(s->params));
    g_free(s);
}

static void add_step(Op *op, const char *path, const char *iface, const char *method, GVariant *params)
{
    Step *s = g_new0(Step, 1);
    s->path = g_strdup(path);
    s->iface = g_strdup(iface);
    s->method = g_strdup(method);
    s->params = params;
    g_ptr_array_add(op->steps, s);
}

static Op *op_new(Win *w, UdDone done, gpointer data)
{
    Op *op = g_new0(Op, 1);
    op->w = win_ref(w);
    op->done = done;
    op->data = data;
    op->steps = g_ptr_array_new_with_free_func(step_free);
    return op;
}

static void op_finish(Op *op, const char *err)
{
    if (op->done)
        op->done(op->w, err ? NULL : op->result, err, op->data);
    win_unref(op->w);
    g_ptr_array_unref(op->steps);
    g_free(op->clear);
    g_free(op->result);
    g_free(op);
}

static GDBusConnection *bus(void)
{
    return g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(MGR));
}

static void run_steps(Op *op);

static void step_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Op *op = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (v)
        g_variant_unref(v);
    /* already done by somebody else: fine */
    if (e && (error_is(e, UD ".Error.NotMounted") || error_is(e, UD ".Error.NotLocked") ||
              error_is(e, UD ".Error.AlreadyUnmounting")))
        g_clear_error(&e);
    if (e) {
        char *m = explain(e);
        g_error_free(e);
        op_finish(op, m);
        g_free(m);
        return;
    }
    run_steps(op);
}

static void run_steps(Op *op)
{
    if (op->next >= op->steps->len) {
        op_finish(op, NULL);
        return;
    }
    Step *s = g_ptr_array_index(op->steps, op->next++);
    GVariant *p = s->params;
    s->params = NULL;
    /* unmounting writes out what is still in memory: on a slow stick that takes minutes, so no
     * time limit (the same for a password prompt from polkit) */
    g_dbus_connection_call(bus(), UD, s->path, s->iface, s->method, p, NULL,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, NULL, step_done, op);
}

/* ---- mounting (and unlocking first) ---- */
static void mount_clear(Op *op);

static gboolean mount_retry(gpointer data)
{
    mount_clear(data);
    return G_SOURCE_REMOVE;
}

static void mounted(GObject *src, GAsyncResult *res, gpointer data)
{
    Op *op = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (v) {
        g_variant_get(v, "(s)", &op->result);
        g_variant_unref(v);
        op_finish(op, NULL);
        return;
    }
    if (error_is(e, UD ".Error.AlreadyMounted")) {
        g_error_free(e);
        GDBusObject *o = object_at(op->clear);
        op->result = o ? mount_point(o) : NULL;
        if (o)
            g_object_unref(o);
        op_finish(op, NULL);
        return;
    }
    /* just unlocked: udisks2 may not know the file system on it yet; ask again a few times */
    if ((g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
         g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
         g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT)) && op->tries++ < 25) {
        g_error_free(e);
        g_timeout_add(200, mount_retry, op);
        return;
    }
    char *m = explain(e);
    g_error_free(e);
    op_finish(op, m);
    g_free(m);
}

static void mount_clear(Op *op)
{
    g_dbus_connection_call(bus(), UD, op->clear, UD_FS, "Mount", g_variant_new("(@a{sv})", no_options()),
                           G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
                           NULL, mounted, op);
}

static void unlocked(GObject *src, GAsyncResult *res, gpointer data)
{
    Op *op = data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!v) {
        char *m = error_is(e, UD ".Error.Failed") && strstr(e->message, "passphrase")
                  ? g_strdup(_("Wrong passphrase.")) : explain(e);
        g_error_free(e);
        op_finish(op, m);
        g_free(m);
        return;
    }
    g_variant_get(v, "(o)", &op->clear);
    g_variant_unref(v);
    if (KNOWN)
        g_hash_table_add(KNOWN, g_strdup(op->clear));   /* ours: not a new stick */
    mount_clear(op);
}

/* the passphrase of an encrypted stick; NULL: cancelled. Wipe it after use. */
static char *ask_passphrase(Win *w, const char *name)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(_("Unlock"), GTK_WINDOW(w->window),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               _("Cancel"), GTK_RESPONSE_CANCEL, _("Unlock"), GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    g_object_set(box, "margin", 12, "spacing", 8, NULL);
    char *t = g_strdup_printf(_("%s is encrypted. Its passphrase:"), name);
    GtkWidget *l = gtk_label_new(t);
    g_free(t);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
    gtk_widget_show_all(d);
    char *pass = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
        pass = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
    gtk_entry_set_text(GTK_ENTRY(e), "");
    gtk_widget_destroy(d);
    return pass;
}

void ud_mount(Win *w, GObject *obj, UdDone done, gpointer data)
{
    GDBusObject *o = G_DBUS_OBJECT(obj);
    Op *op = op_new(w, done, data);
    int st = ud_state(obj);
    if (!MGR || st == UD_SKIP) {
        op_finish(op, _("It isn't there any more."));
        return;
    }
    if (st == UD_MOUNTED) {
        op->result = mount_point(o);
        op_finish(op, NULL);
        return;
    }
    if (st == UD_UNMOUNTED) {
        op->clear = g_strdup(g_dbus_object_get_object_path(o));
        mount_clear(op);
        return;
    }
    /* the passphrase window waits for you, and meanwhile the stick may be pulled out and its
     * object freed: go on by its path, and only if udisks2 still has it */
    char *path = g_strdup(g_dbus_object_get_object_path(o));
    char *name = ud_name(obj);
    char *pass = ask_passphrase(w, name);
    g_free(name);
    if (!pass) {
        g_free(path);
        op_finish(op, "");                 /* cancelled: nothing to say */
        return;
    }
    GDBusObject *still = object_at(path);
    if (!still || ud_state(G_OBJECT(still)) != UD_LOCKED) {
        explicit_bzero(pass, strlen(pass));
        g_free(pass);
        g_free(path);
        if (still)
            g_object_unref(still);
        op_finish(op, _("It isn't there any more."));
        return;
    }
    g_object_unref(still);
    GVariant *p = g_variant_new("(s@a{sv})", pass, no_options());
    explicit_bzero(pass, strlen(pass));
    g_free(pass);
    g_dbus_connection_call(bus(), UD, path, UD_CRYPT, "Unlock", p,
                           G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
                           NULL, unlocked, op);
    g_free(path);
}

/* ---- unmounting, and making it safe to remove ---- */
static GVariant *empty_options(void)
{
    return g_variant_new("(@a{sv})", no_options());
}

static void add_unmount(Op *op, GDBusObject *o)
{
    char *m = has(o, UD_FS) ? mount_point(o) : NULL;
    if (m)
        add_step(op, g_dbus_object_get_object_path(o), UD_FS, "Unmount", empty_options());
    g_free(m);
}

void ud_unmount(Win *w, GObject *obj, gboolean remove, UdDone done, gpointer data)
{
    GDBusObject *o = G_DBUS_OBJECT(obj);
    Op *op = op_new(w, done, data);
    if (!MGR) {
        op_finish(op, _("It isn't there any more."));
        return;
    }
    if (!remove) {
        add_unmount(op, o);
        GDBusObject *b = backing_of(o);       /* an encrypted stick: locked again */
        if (b) {
            add_step(op, g_dbus_object_get_object_path(b), UD_CRYPT, "Lock", empty_options());
            g_object_unref(b);
        }
        run_steps(op);
        return;
    }
    /* everything on the same drive: unmount all of it, lock what was unlocked, then power it off */
    char *drive = drive_of(o);
    if (drive && drive_holds_system(drive)) {
        g_free(drive);
        op_finish(op, _("The system runs from this disk: it can't be switched off."));
        return;
    }
    GList *all = g_dbus_object_manager_get_objects(MGR);
    if (drive) {
        for (GList *l = all; l; l = l->next) {
            char *d = has(l->data, UD_BLOCK) ? drive_of(l->data) : NULL;
            if (!g_strcmp0(d, drive))
                add_unmount(op, l->data);
            g_free(d);
        }
        for (GList *l = all; l; l = l->next) {
            char *d = has(l->data, UD_CRYPT) ? prop_s(l->data, UD_BLOCK, "Drive") : NULL;
            char *clear = d ? prop_s(l->data, UD_CRYPT, "CleartextDevice") : NULL;
            if (clear && !g_strcmp0(d, drive))
                add_step(op, g_dbus_object_get_object_path(l->data), UD_CRYPT, "Lock", empty_options());
            g_free(clear);
            g_free(d);
        }
        GDBusObject *dr = object_at(drive);
        if (dr && prop_b(dr, UD_DRIVE, "CanPowerOff"))
            add_step(op, drive, UD_DRIVE, "PowerOff", empty_options());
        else if (dr && prop_b(dr, UD_DRIVE, "Ejectable"))
            add_step(op, drive, UD_DRIVE, "Eject", empty_options());
        if (dr)
            g_object_unref(dr);
    } else {                                  /* a mounted image: unmount, then let go of the loop device */
        add_unmount(op, o);
        GDBusObject *b = backing_of(o);
        GDBusObject *loop = b ? b : g_object_ref(o);
        if (b)
            add_step(op, g_dbus_object_get_object_path(b), UD_CRYPT, "Lock", empty_options());
        if (has(loop, UD_LOOP))
            add_step(op, g_dbus_object_get_object_path(loop), UD_LOOP, "Delete", empty_options());
        g_object_unref(loop);
    }
    g_free(drive);
    g_list_free_full(all, g_object_unref);
    run_steps(op);
}

/* every place the steps of a removal will unmount (so the windows can step out first) */
GPtrArray *ud_mounts_on_drive(GObject *obj, gboolean remove)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    GDBusObject *o = G_DBUS_OBJECT(obj);
    char *drive = remove ? drive_of(o) : NULL;
    if (!drive || !MGR) {
        char *m = mount_point(o);
        if (m)
            g_ptr_array_add(a, m);
        g_free(drive);
        return a;
    }
    GList *all = g_dbus_object_manager_get_objects(MGR);
    for (GList *l = all; l; l = l->next) {
        char *d = has(l->data, UD_FS) ? drive_of(l->data) : NULL;
        char *m = !g_strcmp0(d, drive) ? mount_point(l->data) : NULL;
        if (m)
            g_ptr_array_add(a, m);
        g_free(d);
    }
    g_list_free_full(all, g_object_unref);
    g_free(drive);
    return a;
}

/* ---- watching: devices coming and going --------------------------------------------------------- */
static gboolean refresh_now(gpointer d)
{
    REFRESH = 0;
    places_devices_changed();
    return G_SOURCE_REMOVE;
}

static void schedule_refresh(void)
{
    if (!REFRESH)
        REFRESH = g_timeout_add(150, refresh_now, NULL);
}

/* seen for the first time: a stick (or card, disc …) just arrived. Only a new device or a new
 * file system on one (a disc put in) is news; a change of state (locked, unmounted) is not. */
static void check(GDBusObject *o, gboolean announce)
{
    int st = ud_state(G_OBJECT(o));
    if (st == UD_SKIP)
        return;
    const char *path = g_dbus_object_get_object_path(o);
    if (g_hash_table_contains(KNOWN, path))
        return;
    g_hash_table_add(KNOWN, g_strdup(path));
    if (!announce || g_get_monotonic_time() < SETTLE_UNTIL || is_system(o))
        return;
    GDBusObject *b = backing_of(o);           /* somebody unlocked a stick we already know */
    if (b) {
        g_object_unref(b);
        return;
    }
    places_device_arrived(G_OBJECT(o));
}

static void on_object_added(GDBusObjectManager *m, GDBusObject *o, gpointer d)
{
    check(o, TRUE);
    schedule_refresh();
}

static void on_interface_added(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, gpointer d)
{
    check(o, TRUE);                           /* a disc put in: its file system shows up later */
    schedule_refresh();
}

static void on_object_removed(GDBusObjectManager *m, GDBusObject *o, gpointer d)
{
    const char *path = g_dbus_object_get_object_path(o);
    places_device_gone(path);
    g_hash_table_remove(KNOWN, path);
    schedule_refresh();
}

static void on_interface_removed(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, gpointer d)
{
    if (ud_state(G_OBJECT(o)) == UD_SKIP) {   /* a disc taken out */
        places_device_gone(g_dbus_object_get_object_path(o));
        g_hash_table_remove(KNOWN, g_dbus_object_get_object_path(o));
    }
    schedule_refresh();
}

static void on_props(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *p, GVariant *changed,
                     const char *const *inval, gpointer d)
{
    check(G_DBUS_OBJECT(o), FALSE);           /* locked again, say: known now, but no news */
    schedule_refresh();
}

static void on_owner(GObject *m, GParamSpec *ps, gpointer d)
{
    /* udisks2 (re)started: it lists every device again, none of them is new */
    g_hash_table_remove_all(KNOWN);
    SETTLE_UNTIL = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    schedule_refresh();
}

static void manager_ready(GObject *src, GAsyncResult *res, gpointer d)
{
    GError *e = NULL;
    MGR = g_dbus_object_manager_client_new_for_bus_finish(res, &e);
    if (!MGR) {
        g_message("udisks2: %s; the Computer column uses GIO", e->message);
        g_error_free(e);
        return;
    }
    GList *all = g_dbus_object_manager_get_objects(MGR);
    for (GList *l = all; l; l = l->next)
        check(l->data, FALSE);                /* there before us: not "arrived" */
    g_list_free_full(all, g_object_unref);
    g_signal_connect(MGR, "object-added", G_CALLBACK(on_object_added), NULL);
    g_signal_connect(MGR, "object-removed", G_CALLBACK(on_object_removed), NULL);
    g_signal_connect(MGR, "interface-added", G_CALLBACK(on_interface_added), NULL);
    g_signal_connect(MGR, "interface-removed", G_CALLBACK(on_interface_removed), NULL);
    g_signal_connect(MGR, "interface-proxy-properties-changed", G_CALLBACK(on_props), NULL);
    g_signal_connect(MGR, "notify::name-owner", G_CALLBACK(on_owner), NULL);
    places_devices_changed();                 /* open Computer columns: from udisks2 now */
}

void ud_start(GtkApplication *app)
{
    if (MGR || KNOWN)
        return;
    (void) app;
    KNOWN = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, UD,
                                             "/org/freedesktop/UDisks2", NULL, NULL, NULL, NULL, manager_ready, NULL);
}
