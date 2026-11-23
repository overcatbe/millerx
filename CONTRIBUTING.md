# Contributing to millerX

Thanks for wanting to help! millerX is a lightweight but fully functional file manager, and it
wants to be the one for Miller columns on Linux, next to Thunar and Dolphin. Bug reports, ideas,
translations and code all help it get there. Here is how.

## Bugs

Open an [issue](https://github.com/overcatbe/millerx/issues) and include:

- the version (`millerx --version`) and your distribution
- what you did, what you expected, and what happened instead
- a screenshot if it's about how something looks
- for a crash: the last lines millerX printed when started from a terminal

## Ideas

Ideas are welcome, also as an issue. Please read [What millerX isn't](README.md#what-millerx-isnt)
first: some things are left out on purpose. A wish that fits millerX has a good chance; many
features in it started as a forum post.

## Translations

New languages, real or just for fun, go into [extras/languages](extras/languages/), not into
the package: millerX keeps the four it has (German, Greek, Wizard, Wienerisch) and stays small.
Anyone can use one with Settings → Language → "Your own…". To add a language (here `xx`, e.g.
`fr` or `pt_BR`):

1. Copy `po/millerx.pot` to `extras/languages/xx.po` and fill in the `msgstr` lines (Poedit or
   any text editor). Lines you leave empty stay English.
2. Keep `%s`, `%u`, `%d`, `%llu` and `\n` exactly as they are: same number, same order.
3. Check it: `msgfmt --check-format -o /dev/null extras/languages/xx.po` must print nothing.
4. Try it with "Your own…", add a line to the table in `extras/languages/README.md` and send a
   pull request.

Corrections to the four built-in languages (`po/`) are just as welcome.

## Code

millerX is C with GTK 3 and GIO, built with meson. Pull requests are welcome, but for anything
bigger than a fix, please open an issue first so we can talk about whether it fits. A few
things millerX cares about:

- small and fast: no new dependency without a good reason
- visible texts go through `_()` so they can be translated
- it has to build on Slackware 15.0 (GLib 2.70, GTK 3.24)

By sending a pull request you agree that your contribution is under the project's license,
GPL-2.0-or-later.
