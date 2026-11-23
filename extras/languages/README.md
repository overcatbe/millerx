# Extra languages

Fun languages for millerX that don't come with the package. Download one (don't open it, just
save it), then in millerX: Settings → Language → "Your own…", pick the file and press
"Restart now".

| Language | What it is | Get it |
|---|---|---|
| `pirate` | Pirate speak, family-friendly: the trash is Davy Jones' Locker, delete is "Walk the plank", root is the cap'n | [pirate.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/pirate.po) |
| `pirate-salty` | Pirate speak for grown-ups: swears like a sailor, the trash is the Brig. **Not** family-friendly | [pirate-salty.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/pirate-salty.po) |
| `lolcat` | I CAN HAZ FILEZ: the trash is TEH LITTERBOX, root is BOSS CAT | [lolcat.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/lolcat.po) |

Or straight into place from a terminal (then pick it in Settings → Language):

```
mkdir -p ~/.local/share/millerx/languages
cd ~/.local/share/millerx/languages
wget https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/pirate.po   # or pirate-salty.po, lolcat.po
```

## Make your own

Copy `/usr/share/millerx/millerx.pot` (or one of the files here) to `NAME.po` and change the
`msgstr` lines. Lines you leave empty stay English. Keep `%s`, `%u`, `%d`, `%llu` and `\n`
exactly as they are: "Your own…" checks the file with `msgfmt` and tells you which line is
wrong.

Made a good one? Share it in [Discussions](https://github.com/overcatbe/millerx/discussions)
or send a pull request to add it here.
