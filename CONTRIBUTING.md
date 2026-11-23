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

Translations live in `po/`. To add a language (here `xx`, e.g. `fr` or `pt_BR`):

1. Copy `po/millerx.pot` to `po/xx.po` and fill in the `msgstr` lines (Poedit or any text editor).
2. Keep `%s`, `%u`, `%d`, `%llu` and `\n` exactly as they are: same number, same order.
3. Check it: `msgfmt --check -c -o /dev/null po/xx.po` must print nothing.
4. Add `xx` to `po/LINGUAS` and send a pull request.

Corrections to an existing language are just as welcome.

Just for fun? A joke language (pirate, LOLCAT, …) doesn't need to go into `po/`: anyone can add one
with Settings → Language → "Your own…". Send it as a pull request to
[extras/languages](extras/languages/) so others can download it.

## Code

millerX is C with GTK 3 and GIO, built with meson. Pull requests are welcome, but for anything
bigger than a fix, please open an issue first so we can talk about whether it fits. A few
things millerX cares about:

- small and fast: no new dependency without a good reason
- visible texts go through `_()` so they can be translated
- it has to build on Slackware 15.0 (GLib 2.70, GTK 3.24)

By sending a pull request you agree that your contribution is under the project's license,
GPL-2.0-or-later.
