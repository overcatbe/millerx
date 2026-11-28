# millerX

A small, native file browser with **Miller columns**: every folder from `/` down to
where you are sits in its own column, and the selected file is previewed on the right.
Like a mind map of your disk. Written in C with GTK 3 and GIO.

> **New in 0.9.0:** **millerX Unleashed is a setting now**: Settings → Unleash →
> **Clicks and drags**, Classic (right-click selects, as always; also the touch-screen way) or
> Unleashed (right-click and drag only hold, folders open under a drag) · **USB sticks without
> GVfs**, straight from udisks2, with a notice when one is plugged in and **Safe to remove**;
> **Automount** off by default (thanks, SCerovec) · **"Restart now"** after an update · the copy
> bar counts **bytes** (thanks, kgha) · clearer **"You may not read it"** messages (thanks, brobr).
>
> **0.8.8:** **Trusted programs**: the "is a program" question has a **Trusted** button,
> and what you trusted runs without asking while it stays the same file; what your distribution
> installed doesn't ask at all (thanks, SCerovec) · **middle-click a file** to open it · the
> status line counts right after a multi-select and shows "28 items, 3 selected" · **Esc**
> unselects · **Git actions** in the right-click menu, with a small Commit window, and green/red
> names (thanks, random_guy) · Settings got a **Programs** and a **Tools** tab.
>
> **0.8.6:** security fixes from rizitis' review: a program or launcher **asks before it
> runs** · language files are checked · no previews as root · hardened build · optional
> **Recognise files by content** (thanks, SCerovec) · after a move or trash millerX stays on the
> parent folder. See [Security in 0.8.6](#security-in-086).
>
> **0.8.5:** fixes a **crash in Settings** (since 0.8.3) · **Extract to…** picks any folder
> to unpack into · Open with → **Other application…** (every installed program) and **Check file
> type** (what the file really is, whatever its name says) · Greek complete again.
>
> **0.8.4:** gallery icons **fill their tiles** · **Tiny** thumbnails (about 64 px) ·
> **links** in italics with the link emblem · **New file** in the right-click menu · **Ctrl+Shift**
> when dropping makes links.
>
> **0.8.3:** the gallery shows **files only** (no folder tiles) · a folder's size and
> gallery wait until its column is filled · **Properties of several items** in one window, mixed
> permissions shown with a dash · **drag with the right button** (or Alt when dropping): Copy,
> Move or **Link here** · **Compare** two files (meld, kdiff3, kompare, diffuse or your own tool).
>
> **0.8.2:** **your own languages**: Settings → Language → "Your own…" adds a language
> file · extra fun ones to download: [pirate, pirate-salty and LOLCAT](extras/languages/) · a busy
> folder no longer recounts its size over and over.
>
> **0.8.1:** Greek revised by a native speaker (thanks, rizitis!) · the preview no longer
> stretches when millerX starts in a folder.
>
> **0.8.0:** the preview's name is a title across the top.
>
> **0.7.9:** the window resizes freely again (the height no longer jumps) · window titles
> show the folder's name, so windows can be told apart in the panel.
>
> **0.7.7–0.7.8:** languages: German and Greek, plus two joke languages, **Wizard** and
> **Wienerisch** (Settings → Language, with "Restart now") · a clicked thumbnail lights up in its
> column · `man millerx`.
>
> **0.7.6:** search (Ctrl+F) · a Trash window · "Show in folder" · Checksums… · Blender pictures ·
> thumbnail sizes · Open with → Set default.
>
> All the details: [CHANGELOG](CHANGELOG.md).

### Miller columns

Miller columns, also called *cascading lists*, show a tree one level per column:
pick a folder and its contents open in the next column to the right, while every
level above stays visible. You always see where you are and how you got there,
and moving up or sideways is a single click or arrow key.

The technique is credited to **Mark S. Miller**, who came up with it in 1980 at
Yale University; it was later used at Project Xanadu, Datapoint and NeXT.
millerX carries his name.

![millerX: from the home folder through the columns into a gallery of holiday photos and an album](docs/tour.gif?v=082)

## What it does

- **Columns from `/`** to the current folder; ← → and the mouse move through them,
  the newest column always stays in view.
- **Preview** in the rest of the window: pictures (WebP, AVIF and HEIC too) fitted to the pane, a video's
  frame, a song's cover, the first
  page of PDFs, text, the contents of archives (zip, tar, 7z, rar, …), folder sizes counted in the background.
- **Gallery**: a folder's files as thumbnails under its details (files only: subfolders stay in
  the columns, so a folder full of folders just shows its details). Opens at once
  even with thousands of files, loads only what is on screen, and right-click gives each
  thumbnail its own menu (open, show in column, rename, trash, …). Select several with
  Ctrl/Shift+click, drag them out or copy them like rows. Videos show a frame, music its cover. Names under the
  thumbnails if you like (one line, cut in the middle so the extension stays). Four sizes: tiny,
  small, medium, large. Blender files show the picture saved in them.
- **Shared thumbnail cache** (`~/.cache/thumbnails`, like Thunar and the file dialogs),
  cleaned up automatically: files that are gone, unused for 180 days, above 1 GB.
- **Sorting per folder**: everything is sorted by name, so your eye learns where things are.
  A folder can have its own order (date, size, type, reversed), shown by a bar on top of
  its column and a ⇅ next to its name.
- **Archives**: right-click → Extract here (loose files get a folder of their own) / Extract
  to… (pick a folder), or Compress… to zip, tar.gz, tar.xz, tar.zst or 7z. In the background,
  with Cancel; hostile archives can't write outside the folder.
- **Resizable columns**: drag a divider for long names, double-click for the default width.
- **More windows**: Ctrl+N, Menu → New window, or middle-click a folder (a middle-click on a
  file opens it, like a double-click). Copy, paste and
  drag & drop work between them, e.g. one window above the other to compare two folders.
- **Open, Open with, Open terminal here** (your `$TERMINAL`, or the first of 17 known terminals).
  Open with → Set default picks the program a click opens that kind of file with (saved in
  `~/.config/mimeapps.list`, so Thunar and the file dialogs follow it too). Open with → Other
  application… offers every installed program, and Check file type shows in a terminal what
  `file` says the file really is, whatever its name says.
- **Git** (optional, off by default: Settings → Tools → Git): inside a repository the
  right-click menu gets **Git ▸** Status, Log and Diff (in your terminal), Add, **Commit…** (a
  small window: the message, and exactly which files go in) and Revert… (asks first).
  A second switch, **Colour file names by their Git state**, makes new files green and changed
  ones red (a folder shows what's in it). millerX asks git about the folder you look at only,
  and stores nothing itself; a repository whose own config has filters or includes, which could
  make git run programs, stays uncoloured.
  Git actions run git as usual: in a repository you don't trust, its own settings (pager, diff
  tool, hooks, filters) can run programs, as with git in a terminal. Revert gives a changed file its
  committed version back; a file that was only added stays and is just taken out of Git again.
- **Checksums** (right-click → Checksums…): MD5, SHA-1 and SHA-256 in one read, a Copy button each,
  and a field to paste the checksum you were given: ✓ or ✗.
- **Cut, copy, paste**, compatible with Thunar's clipboard. A highlighted folder receives the paste.
- **Drag & drop**: same file system moves, otherwise copies; Ctrl copies, Shift moves, Ctrl+Shift
  links. Drag with the right button (or hold Alt when dropping) and millerX asks: Copy here, Move
  here or Link here.
- **Compare** two files: select two, right-click → Compare. It opens meld, kdiff3, kompare or
  diffuse, whichever is installed, or your own tool (Settings → Programs → Compare with);
  with none of them it shows the differences itself (`diff -u`).
- **Links** show in italics with the link emblem, in the columns and the gallery.
- **Rename, new folder, new file, move to trash, delete permanently**, with a conflict dialog
  (skip, keep both, replace or merge, "do this for all").
- **Bookmarks**, shared with Thunar and the GTK file dialogs. The **Trash** is always the last
  line of the menu. It opens in a window of its own (close it and you are back) with the trash of
  every disk, sorted by name or with its buttons by size, newest or oldest deleted; the preview says where each item came from and when it was deleted, right-click puts items
  back, deletes them for good or empties the trash.
- **Search** (Ctrl+F): names with the typed text in, in this folder and every folder below it,
  upper/lower case alike, nearest first. Pick one to see it in the preview, Enter opens it in the
  columns, Escape goes back. Hidden files only when they are shown.
- **Computer column** (the computer button, Alt+/): all your disks with free space and a fill bar,
  USB sticks, cards and discs straight from udisks2 (one click mounts, encrypted ones ask for their
  passphrase; right-click: Unmount, Safe to remove), network shares and GVfs places (sftp://,
  smb://), and "Connect to server…". A disk opens to the right like any folder.
- **A stick plugged in** gets a notice: its name, device, file system and size, with Mount and
  Show; you see where it was mounted, and "Safe to remove" writes out what is left, unmounts it and
  switches it off. Or Settings → Tools → **Automount** (off by default): it mounts itself and opens
  in a new window.
- **Type to jump**: type the first letters of a name; the same letter again goes to the next match.
- **Properties** with editable permissions, free space and folder totals. Several items selected:
  one window for all of them; a box with a dash means some have that permission, a click sets it
  for all.
- **Open as root** through polkit (`pkexec`): the password is asked by your polkit agent,
  millerX never sees it. A root window shows a red warning bar.
- **Highlight colour** of your own, chosen live in the settings, for millerX or for every
  GTK 3 program (a marked block in `~/.config/gtk-3.0/gtk.css`, removable again).
- **Languages:** English, German and Greek, following your system; Settings → Language can
  also pick English anyway. The joke languages **Wizard** ("Banish to the Void", "Thou art not
  worthy") and **Wienerisch** (Viennese slang, root is "da Kaiser") are downloads in extras now.
  The Greek is looked after by rizitis, a native speaker. Only the language in use is ever
  loaded; each one is about 30 KB on disk.
- **More languages:** every new language, real or just for fun, is a download in
  [extras/languages](extras/languages/) (pirate, LOLCAT, …), so millerX doesn't grow with each
  one: German and Greek come with it. Settings → Language → "Your own…" picks the file, checks it and
  adds it to the list. Or write your own: copy `/usr/share/millerx/millerx.pot` to `NAME.po` and
  fill in the `msgstr` lines (empty ones stay English). Translators welcome: see
  [CONTRIBUTING](CONTRIBUTING.md#translations).
- Columns follow changes on disk. Names sort case-insensitive and naturally (file2 before
  file10), folders first (optional).

![The preview fits a picture into the rest of the window](docs/preview.png?v=082)

![The gallery: a folder's pictures under its details; the picked one lights up in the column too](docs/gallery.png?v=082)

![Search (Ctrl+F): every name with "morning" in it, here and below](docs/search.png?v=082)

![The Trash in its own window: sorted by newest, with where each item came from](docs/trash.png?v=083)

![The Computer column: disks, USB sticks and network, each with its free space](docs/computer.png?v=082)

The languages: Wizard (from extras), German and Greek.

<p>
<img src="docs/wizard.png?v=082" width="32%" alt="Wizard: Banish to the Void, Read the runes, Add to sacred texts">
<img src="docs/german.png?v=082" width="32%" alt="German: the right-click menu of an archive">
<img src="docs/greek.png?v=082" width="32%" alt="Greek: the settings">
</p>

What changed in each version: [CHANGELOG](CHANGELOG.md).

## Security in 0.8.6

**Thanks to rizitis** for a thorough security review ([#5](https://github.com/overcatbe/millerx/issues/5)).
Everything he found is fixed in 0.8.6:

- A double-click on a program asks first (Cancel is the default), whatever the file is called.
- A launcher (`.desktop`) shows the command it would run and asks first (Open as text is the default).
- Language files are checked before use; a broken one can't crash millerX any more.
- The root window shows no previews (no pictures, video, PDFs or archive contents read as root).
- Folders extracted from archives get normal permissions.
- Compare runs in the background and can be stopped.
- Built with hardening (PIE, full RELRO, stack protector, FORTIFY).
- What a file really is, whatever its name: right-click → Open with → Check file type.

**Thanks to SCerovec** for the idea of recognising files by their content. Other file managers don't
do this, so it's off by default, for those who want to be extra careful:

- Settings → Programs → Recognise files → **By content, when you open a file**. millerX then reads
  the start of the file you select or open: the preview, Properties, Open with and a double-click
  go by what it really is (a "holiday.jpg" that is text opens in the text editor). The icons in
  the lists stay by name, nothing is read in the background. Not a virus scanner.

Tested on 2026-11-26 with a small program that only creates a marker file:

| | "holiday.jpg" that is a program | a program with no extension |
|---|---|---|
| **Thunar 4.20.10** | picture icon, opens in the image viewer | runs it at once |
| **Dolphin 26.08.3** | picture icon, opens in the image viewer | asks, Cancel is the default |
| **millerX 0.8.5** | picture icon, **ran it** | ran it |
| **millerX 0.8.6** | picture icon, asks first (by content: the preview says "Executable") | asks, Cancel is the default |

Nautilus wasn't tested.

### Trusted programs (0.8.8)

> *"A sentience of impending doom that always follows reckless foolishness."*
> SCerovec, on why millerX asks first

The question before a program, script or launcher runs has a **Trusted** button (SCerovec's
wish): it runs it, and from then on it runs without asking, **as long as it is the same file**.
millerX remembers its SHA-256 fingerprint; an update or another file put there under the same
name is asked about again ("it has changed since you trusted it"). Settings → Programs lists
what you trusted, ✕ forgets one.

What your distribution installed doesn't ask at all: **Trust system programs (owned by root)**,
on by default, lets a program, script or launcher run when it and every folder above it belong
to root and only root can change them (/usr, /opt…). Whoever can change those owns the machine
already, so a question there protects nothing, and Kate doesn't ask again after every update.
Your own files (your home, downloads, USB sticks) still ask. Only Linux's own disk file systems
count (ext4, XFS, Btrfs and friends): a FAT or NTFS stick, an ISO or a network share mounted by
root shows every file as root's, and those still ask. A link counts as root's only when the link
and its folder are root's too, so a "README" link to /usr/bin/make in a download asks.

The same place has **Ask before running programs, scripts and launchers**, on by default;
switched off, everything runs at once without a question, also a "holiday.jpg" that is really
a program.

## What millerX isn't

millerX is a lightweight but fully functional file manager, built around Miller columns. To stay
fast and clear, it leaves some things out on purpose:

- **No tabs.** Open a second window instead (Ctrl+N or middle-click a folder); copy, paste and
  drag & drop work between windows.
- **Nothing running in the background.** No daemon, no service, no indexer. When you close
  millerX, it's gone.
- **No custom actions** (your own commands in the right-click menu), perhaps later.
- **No feature just because another file manager has it.** Ideas are welcome, but millerX goes
  its own way instead of copying Dolphin or Nautilus.
- **No new dependency without a good reason.** Optional helpers (ffmpeg, pdftoppm, Blender)
  stay optional: without them millerX shows an icon instead of a picture.

## Keys

| Key | Action |
| --- | --- |
| ← → | previous / next column |
| Backspace, Alt+↑ | up one level |
| Alt+Home, Alt+/ | home, file system root |
| Enter | open |
| F2 | rename |
| Delete, Shift+Delete | move to trash, delete permanently |
| Ctrl+C, Ctrl+X, Ctrl+V | copy, cut, paste |
| Ctrl+Shift+N | new folder |
| Ctrl+D | bookmark this folder |
| Ctrl+H | show hidden files |
| Ctrl+L | location bar |
| Ctrl+F | search here (Escape: back to the columns) |
| Esc | unselect everything in this column |
| Alt+Enter | properties |
| Ctrl+N | new window at this folder |
| Ctrl+, | settings |
| F5 | reload |
| Ctrl+Q, Ctrl+W | close |

Middle-click a folder (or right-click → Open in new window) to open it in a second window;
middle-click a file to open it.

**Clicks and drags** (Settings → Unleash) decides what a right-click and a drag do:
- **Classic** (the default): a right-click selects what it is on, and a folder opens to the right;
  a drag carries the selection. What you select is what you process, and it is the way for touch
  screens, where a long press is the right-click.
- **Unleashed**: a right-click on something not selected only holds it (bold, underlined); its
  menu acts on it and the columns stay where they are. A drag carries what you picked up without
  selecting it (the old selection stays), and a folder held still under a drag for a moment opens,
  so you can go down and drop inside. This was millerX Unleashed until 0.8.8; its settings, sort
  orders and trusted programs are taken over the first time, if millerX had none of its own.

In the gallery: Ctrl/Shift+click selects several thumbnails, Ctrl+A all. Enter opens one in its
column; Delete / Shift+Delete, Ctrl+C / Ctrl+X, F2, the Menu key and Alt+Enter work as in a column.

## What it needs

One program of about 170 KB. Everything it links is already on any desktop that runs
GTK 3 programs:

```
millerx
├── GTK 3            the window, columns and dialogs
│   └── GLib/GIO, Pango, Cairo, gdk-pixbuf   (come with GTK)
├── libarchive       looking into, unpacking and packing archives
└── libwebp          WebP pictures (optional: left out if it isn't there when building)
```

Programs it uses **if they are installed**, nothing breaks without them (Settings → Thumbnails →
Optional helpers shows which ones it found):

| For | Program | Without it |
| --- | --- | --- |
| PDF previews | `pdftoppm` (poppler) | the PDF's icon |
| AVIF, HEIC and other pictures, video frames, music covers | `ffmpeg` | the file's icon |
| Open as root | `pkexec` + a polkit agent | no root window |
| Compare two files | `meld`, `kdiff3`, `kompare` or `diffuse` (or your own, in Settings) | the differences as text (`diff -u`) |
| Blender files' pictures | `blender-thumbnailer` (comes with Blender; also found in `/opt/blender`, else the newest `/opt/blender-*`) | the .blend icon |
| USB sticks, cards, discs | udisks2 (gvfs when udisks2 isn't running) | only disks already mounted |
| Connect to server | gvfs | no network places |
| Open terminal here, Git's Status / Log / Diff | any terminal (`$TERMINAL` or one of 17 known) | a message saying none was found |
| Git actions (off by default) | `git` | no Git menu |

No Qt, no KDE or GNOME libraries, no Python, no thumbnailer service, nothing running in
the background. It works with any window manager or desktop on X11.

## Install

Packages for **Slackware**, **Debian/Ubuntu** and **Arch** are attached to every
[release](https://github.com/overcatbe/millerx/releases).

- **Slackware**: the ready-built `millerx-*_SBo.txz` (built on 15.0, runs on -current too):
  `installpkg millerx-*.txz` as root. Or build it yourself: download the `.SlackBuild.tar.gz`
  and the source tarball, then as root:
  `./millerx.SlackBuild && installpkg /tmp/millerx-*_SBo.tgz`
- **Debian / Ubuntu**: `sudo apt install ./millerx_*.deb`
- **Arch**: `sudo pacman -U millerx-*.pkg.tar.zst`

### Build from source (any distro)

Needs GTK 3.22+, GLib/GIO 2.70+, libarchive, meson 0.60+ and ninja (on Slackware 15.0: with the
official patches, which bring meson 1.11). Optional: libwebp (WebP pictures;
found automatically when building), `pdftoppm` (poppler) for PDF previews, `ffmpeg` for
AVIF, HEIC and other pictures gdk-pixbuf can't read, polkit plus an
authentication agent for "Open as root".

```sh
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
```

The translations are built when `msgfmt` (gettext) is there, otherwise millerX is simply English.
To leave them out on purpose: `meson setup build --prefix=/usr -Dnls=disabled`.

## Settings

Menu → Settings (Ctrl+,), or `~/.config/millerx/millerx.conf`:

```ini
[look]
divider_color = auto     # auto = theme background 20 % lighter (dark) / darker (light), or #rrggbb
divider_width = 2        # 1-12
divider_style = solid    # solid | fade
preview_width = 340      # 200-900, the least the preview gets; it takes the rest
gallery_names = true     # a line with the name under each thumbnail
highlight_color = auto   # auto = the theme's, or #rrggbb
column_width = 230       # 120-600, where new folder columns start
recognise = name         # name (like other file managers) | content (the file you select or open)

[browse]
folders_first = true
show_hidden = false
clicks = classic         # classic (right-click selects) | unleashed (right-click and drag only hold)
ask_programs = true      # a program, script or launcher asks before it runs (what you trusted doesn't)
trust_system = true      # ...except root's own (owned by root, in folders only root can change)

[devices]
automount = false        # a stick plugged in mounts itself and opens a window (off: a notice says what came)

[updates]
notice = true            # "millerX was updated · Restart now" after a package upgrade

[thumbnails]
show = local             # local (none in network folders) | always | never
size = medium            # tiny | small | medium | large (about 64, 100, 200, 400 px; at least 6, 4, 3, 2 per row)
max_mb = 1024            # cache size to shrink to, least used first
max_days = 180           # forget thumbnails unused this long
auto_clean = true        # once a day when millerX starts

[helpers]                # installed helpers can be switched off
ffmpeg = true            # video frames, music covers, AVIF/HEIC
pdftoppm = true          # PDF pages
pkexec = true            # "Open as root"
blender = true           # pictures saved in .blend files
git = false              # Git actions in the right-click menu, inside a repository
git_colours = false      # ...and file names coloured by their Git state
compare = auto           # Compare: auto (meld, kdiff3, kompare, diffuse) or your own command

[window]                 # written by millerX: where the last closed window was
```

**"Show in folder" from a browser:** while millerX runs, it answers it (D-Bus
`org.freedesktop.FileManager1`), unless another file manager already does, such as Thunar's
daemon. To have millerX started for it when none is running, put this into
`~/.local/share/dbus-1/services/org.freedesktop.FileManager1.service`:

```ini
[D-BUS Service]
Name=org.freedesktop.FileManager1
Exec=/usr/bin/millerx --dbus
```

Folders with their own sorting are kept in `~/.config/millerx/sorting.conf`, one per line
(`date`, `size`, `type` or `name`, a tab, `1` if reversed, a tab, the folder). The settings
list them too, to change or remove. Trusted programs are in `~/.config/millerx/trusted` (size,
SHA-256 and path, one per line); Settings → Programs lists them.

![Settings](docs/settings.png?v=082)

## Testers

millerX is tested by the people of the
[LinuxQuestions thread](https://www.linuxquestions.org/questions/slackware-14/miller-columns-file-manager-4175768497/):
01micko, brobr, chrisretusn, dchmelik, FTIO, gmgf, gus3, kgha, Paulo2, pm_a_cup_of_tea,
random_guy, rizitis and SCerovec. Thank you!

## Bugs and wishes

Please open an [issue](https://github.com/overcatbe/millerx/issues). How to report a bug, suggest
an idea or add a translation: [CONTRIBUTING](CONTRIBUTING.md).

## License

GPL-2.0-or-later, see [COPYING](COPYING).

Forks are welcome. The name "millerX" and the logo aren't part of the license: a changed
version needs another name and logo, see [NOTICE](NOTICE).
