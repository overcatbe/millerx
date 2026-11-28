/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 overcatbe */
/* The highlight (selection) colour: the theme's, or your own, for millerX, and if you want it
 * for every GTK 3 program through a marked block in ~/.config/gtk-3.0/gtk.css. */
#include "millerx.h"
#include <stdlib.h>
#include <string.h>

#define MARK_START "/* millerX highlight: start (edit the colour in millerX's settings) */"
#define MARK_END "/* millerX highlight: end */"

/* readable text on bg: dark on light colours, white on dark ones */
static void text_for(const GdkRGBA *bg, char out[8])
{
    double l = 0.2126 * bg->red + 0.7152 * bg->green + 0.0722 * bg->blue;
    g_strlcpy(out, l > 0.55 ? "#111111" : "#ffffff", 8);
}

/* The CSS for one colour. Named colours for themes built on them (Breeze, …), plain rules for
 * the ones that write their colours out (Adwaita). */
static char *css_for(const char *colour)
{
    GdkRGBA bg;
    if (!colour || !gdk_rgba_parse(&bg, colour))
        return NULL;
    char fg[8];
    text_for(&bg, fg);
    char *hex = g_strdup_printf("#%02x%02x%02x", (int) (bg.red * 255 + .5), (int) (bg.green * 255 + .5),
                                (int) (bg.blue * 255 + .5));
    char *css = g_strdup_printf(
        "@define-color theme_selected_bg_color_breeze %1$s;\n"
        "@define-color theme_selected_bg_color %1$s;\n"
        "@define-color theme_selected_fg_color_breeze %2$s;\n"
        "@define-color theme_selected_fg_color %2$s;\n"
        "treeview.view:selected, .view:selected, flowboxchild:selected, list row:selected,\n"
        "iconview:selected, selection {\n"
        "  background-color: %1$s;\n"
        "  background-image: none;\n"
        "  color: %2$s;\n"
        "}\n", hex, fg);
    g_free(hex);
    return css;
}

/* millerX's own windows: on start and whenever the setting changes */
void highlight_apply(void)
{
    static GtkCssProvider *prov;
    GdkScreen *scr = gdk_screen_get_default();
    if (!scr)
        return;
    /* a new provider each time: reloading the attached one doesn't restyle open windows */
    if (prov) {
        gtk_style_context_remove_provider_for_screen(scr, GTK_STYLE_PROVIDER(prov));
        g_clear_object(&prov);
    }
    char *css = strcmp(LOOK.highlight, "auto") ? css_for(LOOK.highlight) : NULL;
    if (css) {
        prov = gtk_css_provider_new();
        gtk_css_provider_load_from_data(prov, css, -1, NULL);
        gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(prov),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_free(css);
}

/* ---- every GTK 3 program: a marked block in ~/.config/gtk-3.0/gtk.css ------------------------ */
static char *gtk_css_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtk-3.0", "gtk.css", NULL);
}

/* the file without our block (and its blank line), everything else exactly as it was */
static char *without_block(const char *text)
{
    const char *a = text ? strstr(text, MARK_START) : NULL;
    const char *b = a ? strstr(a, MARK_END) : NULL;
    if (!a || !b)
        return g_strdup(text ? text : "");
    b += strlen(MARK_END);
    while (*b == '\n')
        b++;
    GString *s = g_string_new_len(text, a - text);
    g_string_append(s, b);
    return g_string_free(s, FALSE);
}

gboolean highlight_system_has(void)
{
    char *path = gtk_css_path(), *text = NULL;
    gboolean has = g_file_get_contents(path, &text, NULL, NULL) && strstr(text, MARK_START);
    g_free(text);
    g_free(path);
    return has;
}

/* colour NULL removes the block; the rest of the file is kept */
gboolean highlight_system_set(const char *colour, GError **err)
{
    char *path = gtk_css_path(), *text = NULL;
    GError *re = NULL;
    /* a gtk.css that is there but can't be read: never written over (all of it would be gone) */
    if (!g_file_get_contents(path, &text, NULL, &re) && !g_error_matches(re, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_propagate_error(err, re);
        g_free(path);
        return FALSE;
    }
    g_clear_error(&re);
    /* a link (a dotfiles repository): written where it points, the link stays */
    char *real = realpath(path, NULL);
    if (real) {
        g_free(path);
        path = g_strdup(real);
        free(real);
    }
    char *rest = without_block(text);
    GString *out = g_string_new(NULL);
    char *css = colour ? css_for(colour) : NULL;
    if (css)
        g_string_append_printf(out, "%s\n%s%s\n\n", MARK_START, css, MARK_END);
    g_string_append(out, rest);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    gboolean ok = TRUE;
    if (out->len || text)       /* nothing to write and no file: leave it that way */
        ok = g_file_set_contents(path, out->str, -1, err);
    g_free(dir);
    g_free(css);
    g_string_free(out, TRUE);
    g_free(rest);
    g_free(text);
    g_free(path);
    return ok;
}
