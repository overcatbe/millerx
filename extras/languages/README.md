# More languages

Every language beyond English, German and Greek lives here, real ones and fun ones alike, and
doesn't come with the package. millerX stays small: only German and Greek are installed
(Wizard and Wienerisch came with it until 0.8.8); everything else is a download for whoever
wants it.

Download one (don't open it, just save it), then in millerX: Settings → Language →
"Your own…", pick the file and press "Restart now".

### Languages

None yet. French, Spanish, Polish, Slovak…? [Make one](#make-your-own) and send it in!

### Just for fun

| Language | What it is | Get it |
|---|---|---|
| `wizard` | English, of a sort: Move to trash is "Banish to the Void", delete is "Destroy forever", bookmarks are sacred texts | [wizard.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/wizard.po) |
| `wienerisch` | Viennese slang: root is "da Kaiser", items are "Dinga" | [wienerisch.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/wienerisch.po) |
| `pirate` | Pirate speak, family-friendly: the trash is Davy Jones' Locker, delete is "Walk the plank", root is the cap'n | [pirate.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/pirate.po) |
| `pirate-salty` | Pirate speak for grown-ups: swears like a sailor, the trash is the Brig. **Not** family-friendly | [pirate-salty.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/pirate-salty.po) |
| `lolcat` | I CAN HAZ FILEZ: the trash is TEH LITTERBOX, root is BOSS CAT | [lolcat.po](https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/lolcat.po) |

Or straight into place from a terminal (then pick it in Settings → Language):

```
mkdir -p ~/.local/share/millerx/languages
cd ~/.local/share/millerx/languages
wget https://raw.githubusercontent.com/overcatbe/millerx/main/extras/languages/pirate.po   # or wizard.po, wienerisch.po, pirate-salty.po, lolcat.po
```

## Make your own

Copy `/usr/share/millerx/millerx.pot` (or one of the files here) to `NAME.po` and change the
`msgstr` lines. Lines you leave empty stay English. Keep `%s`, `%u`, `%d`, `%llu` and `\n`
exactly as they are: "Your own…" checks the file with `msgfmt` and tells you which line is
wrong.

Send it as a pull request to add it here (or post it in
[Discussions](https://github.com/overcatbe/millerx/discussions) if you don't use git). Name a real
language by its code (`fr.po`, `pt_BR.po`), a fun one however you like.
