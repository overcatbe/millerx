/* millerX — checking a compiled language file (.mo) before it is used.
 *
 * A translation is code: millerX hands many of them to printf-style functions, so a translation
 * with an extra %s reads arguments that were never passed (a crash), and %n writes to memory.
 * msgfmt --check-format only checks entries flagged c-format, and a .mo isn't checked by anyone,
 * so millerX checks the .mo itself, on import and before every use.
 *
 * The rule: for each msgid that is a printf format, every argument a translation uses must be in
 * the msgid at the same position, with the same kind (integer, floating, string, pointer) and the
 * same length modifier. Fewer arguments are fine (a plural's singular often drops its %d), so are
 * %n$ reorderings, %% and %m. Refused: %n, unknown or unfinished directives, gaps in numbered
 * arguments, numbered and plain mixed, numbered * widths. A msgid containing "% " is not a format
 * (the label "Follow the theme (20 % lighter / darker)"); no millerX format uses the space flag.
 *
 * The file itself is untrusted: every offset and length is checked against its size and every
 * string must end in its NUL. Reported by rizitis in GitHub issue #5. */
#include "millerx.h"
#include <string.h>

#define MAX_ARGS 32

typedef struct {
    int n;                      /* highest argument position used */
    char kind[MAX_ARGS + 1];    /* per position (1-based): 0 unused, 'i' 'f' 's' 'p' */
    char len[MAX_ARGS + 1];     /* length modifier: 0 none, 'H' hh, 'h', 'l', 'L' ll, 'D' long double, 'j' 'z' 't' */
    gboolean numbered, plain;
} Args;

static gboolean put(Args *a, int pos, char kind, char len, char **why)
{
    if (pos < 1 || pos > MAX_ARGS) {
        *why = g_strdup(_("too many arguments"));
        return FALSE;
    }
    if (a->kind[pos] && (a->kind[pos] != kind || a->len[pos] != len)) {
        *why = g_strdup_printf(_("argument %d is used as two different kinds"), pos);
        return FALSE;
    }
    a->kind[pos] = kind;
    a->len[pos] = len;
    if (pos > a->n)
        a->n = pos;
    return TRUE;
}

/* digits followed by '$': their value, and *p moved past the '$'; else 0 and *p unchanged */
static int dollar(const char **p)
{
    const char *q = *p;
    int v = 0;
    while (*q >= '0' && *q <= '9' && v < 10000)
        v = v * 10 + (*q++ - '0');
    if (q == *p || *q != '$')
        return 0;
    *p = q + 1;
    return v;
}

/* All the directives in S (one form, NUL-terminated). FALSE with *why on anything refused. */
static gboolean parse(const char *s, Args *a, char **why)
{
    memset(a, 0, sizeof *a);
    int next = 1;               /* the next plain argument */
    for (const char *p = s; *p; p++) {
        if (*p != '%')
            continue;
        p++;
        if (*p == '%')
            continue;
        int pos = dollar(&p);
        if (pos)
            a->numbered = TRUE;
        else
            a->plain = TRUE;
        while (*p && strchr("-+ #0'I", *p))
            p++;
        for (int part = 0; part < 2; part++) {          /* width, then .precision */
            if (part == 1) {
                if (*p != '.')
                    break;
                p++;
            }
            if (*p == '*') {
                p++;
                if (dollar(&p)) {
                    *why = g_strdup(_("a numbered * width"));
                    return FALSE;
                }
                if (!put(a, next++, 'i', 0, why))
                    return FALSE;
                a->plain = TRUE;
            } else {
                while (*p >= '0' && *p <= '9')
                    p++;
            }
        }
        char len = 0;
        switch (*p) {
        case 'h': p++; len = 'h'; if (*p == 'h') { p++; len = 'H'; } break;
        case 'l': p++; len = 'l'; if (*p == 'l') { p++; len = 'L'; } break;
        case 'q': p++; len = 'L'; break;
        case 'L': p++; len = 'D'; break;
        case 'j': p++; len = 'j'; break;
        case 'z': case 'Z': p++; len = 'z'; break;
        case 't': p++; len = 't'; break;
        }
        char kind;
        switch (*p) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'c':
            kind = 'i';
            break;
        case 'C':
            kind = 'i'; len = 'l';
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            kind = 'f';
            break;
        case 's':
            kind = 's';
            break;
        case 'S':
            kind = 's'; len = 'l';
            break;
        case 'p':
            kind = 'p';
            break;
        case 'm':
            if (pos) {
                *why = g_strdup_printf(_("a numbered %s"), "%m");
                return FALSE;
            }
            continue;           /* no argument */
        case 'n':
            *why = g_strdup_printf(_("%s is not allowed"), "%n");
            return FALSE;
        case '\0':
            *why = g_strdup(_("an unfinished directive at the end"));
            return FALSE;
        default:
            *why = g_strdup_printf(_("an unknown directive “%s%c”"), "%", *p);
            return FALSE;
        }
        if (len == 'D' && kind != 'f') {
            *why = g_strdup(_("an unknown directive"));
            return FALSE;
        }
        if (!put(a, pos ? pos : next++, kind, len, why))
            return FALSE;
    }
    if (a->numbered && a->plain) {
        *why = g_strdup(_("numbered and plain arguments mixed"));
        return FALSE;
    }
    for (int i = 1; i <= a->n; i++)
        if (!a->kind[i]) {
            *why = g_strdup_printf(_("argument %d is skipped"), i);
            return FALSE;
        }
    return TRUE;
}

/* one translation form against the msgid's arguments */
static gboolean form_ok(const Args *id, const char *form, char **why)
{
    Args t;
    if (!parse(form, &t, why))
        return FALSE;
    for (int i = 1; i <= t.n; i++) {
        if (i > id->n) {
            *why = g_strdup_printf(_("argument %d isn't in the original"), i);
            return FALSE;
        }
        if (t.kind[i] != id->kind[i] || t.len[i] != id->len[i]) {
            *why = g_strdup_printf(_("argument %d is of another kind than in the original"), i);
            return FALSE;
        }
    }
    return TRUE;
}

static guint32 rd(const guchar *d, gsize off, gboolean swap)
{
    guint32 v;
    memcpy(&v, d + off, 4);
    return swap ? GUINT32_SWAP_LE_BE(v) : v;
}

/* string I of the table at TAB: its start and length, NUL included in the file; FALSE if out of the file */
static gboolean str_at(const guchar *d, gsize size, guint32 tab, guint32 i, gboolean swap, const char **s, gsize *len)
{
    guint64 e = (guint64) tab + (guint64) i * 8;
    if (e + 8 > size)
        return FALSE;
    guint32 l = rd(d, e, swap), o = rd(d, e + 4, swap);
    if ((guint64) o + l + 1 > size || d[o + l] != '\0')
        return FALSE;
    *s = (const char *) d + o;
    *len = l;
    return TRUE;
}

gboolean mo_check(const guchar *d, gsize size, char **why)
{
    char *w = NULL;
    if (size < 28) {
        *why = g_strdup(_("not a language file (too short)"));
        return FALSE;
    }
    guint32 magic;
    memcpy(&magic, d, 4);
    gboolean swap;
    if (magic == 0x950412de)
        swap = FALSE;
    else if (magic == 0xde120495)
        swap = TRUE;
    else {
        *why = g_strdup(_("not a language file (.mo)"));
        return FALSE;
    }
    guint32 rev = rd(d, 4, swap), n = rd(d, 8, swap), otab = rd(d, 12, swap), ttab = rd(d, 16, swap);
    if (rev >> 16 > 1) {
        *why = g_strdup(_("a .mo version millerX doesn't know"));
        return FALSE;
    }
    if (rev >> 16 == 1 && (size < 48 || rd(d, 36, swap) != 0)) {
        *why = g_strdup(_("system-dependent strings, which millerX can't check"));
        return FALSE;
    }
    for (guint32 i = 0; i < n; i++) {
        const char *id, *tr;
        gsize idlen, trlen;
        if (!str_at(d, size, otab, i, swap, &id, &idlen) || !str_at(d, size, ttab, i, swap, &tr, &trlen)) {
            *why = g_strdup(_("the file is damaged"));
            return FALSE;
        }
        if (idlen == 0)
            continue;                                   /* the header */
        const char *plural = memchr(id, '\0', idlen);
        const char *fmt = plural ? plural + 1 : id;     /* every form is checked against msgid_plural */
        if (strstr(id, "% ") || (plural && strstr(fmt, "% ")))
            continue;                                   /* not a format, see above */
        Args a;
        if (!parse(fmt, &a, &w)) {                      /* not a format we know: nothing printf can use */
            g_free(w);
            w = NULL;
            continue;
        }
        for (const char *f = tr; f <= tr + trlen; f += strlen(f) + 1) {
            if (!form_ok(&a, f, &w)) {
                char *shown = g_strndup(id, 60);
                *why = g_strdup_printf(_("the translation of “%s”: %s"), shown, w);
                g_free(shown);
                g_free(w);
                return FALSE;
            }
        }
    }
    return TRUE;
}

gboolean mo_check_file(const char *path, char **why)
{
    char *data;
    gsize len;
    GError *e = NULL;
    if (!g_file_get_contents(path, &data, &len, &e)) {
        *why = g_strdup(e->message);
        g_error_free(e);
        return FALSE;
    }
    gboolean ok = mo_check((const guchar *) data, len, why);
    g_free(data);
    return ok;
}
