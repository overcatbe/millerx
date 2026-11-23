# millerX

A small, native file browser with **Miller columns**: every folder from `/` down to
where you are sits in its own column, and the selected file is previewed on the right.
Like a mind map of your disk. Written in C with GTK 3 and GIO.

> **New in 0.8.0:** the preview's name is a title across the top.
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

![millerX](docs/screenshot.png?v=074)

## What it does

- **Columns from `/`** to the current folder; ← → and the mouse move through them,
  the newest column always stays in view.
- **Preview** in the rest of the window: pictures (WebP, AVIF and HEIC too) fitted to the pane, a video's
  frame, a song's cover, the first
  page of PDFs, text, the contents of archives (zip, tar, 7z, rar, …), folder sizes counted in the background.
- **Gallery**: a folder's pictures and PDFs as thumbnails under its details. Opens at once
  even with thousands of files, loads only what is on screen, and right-click gives each
  thumbnail its own menu (open, show in column, rename, trash, …). Select several with
  Ctrl/Shift+click, drag them out or copy them like rows. Videos show a frame, music its cover. Names under the
  thumbnails if you like (one line, cut in the middle so the extension stays). Three sizes: small,
  medium, large. Blender files show the picture saved in them.
- **Shared thumbnail cache** (`~/.cache/thumbnails`, like Thunar and the file dialogs),
  cleaned up automatically: files that are gone, unused for 180 days, above 1 GB.
- **Sorting per folder**: everything is sorted by name, so your eye learns where things are.
  A folder can have its own order (date, size, type, reversed), shown by a bar on top of
  its column and a ⇅ next to its name.
- **Archives**: right-click → Extract here / Extract to "name/", or Compress… to zip, tar.gz,
  tar.xz, tar.zst or 7z. In the background, with Cancel; hostile archives can't write outside the
  folder.
- **Resizable columns**: drag a divider for long names, double-click for the default width.
- **More windows**: Ctrl+N, Menu → New window, or middle-click a folder. Copy, paste and
  drag & drop work between them, e.g. one window above the other to compare two folders.
- **Open, Open with, Open terminal here** (your `$TERMINAL`, or the first of 17 known terminals).
  Open with → Set default picks the program a click opens that kind of file with (saved in
  `~/.config/mimeapps.list`, so Thunar and the file dialogs follow it too).
- **Checksums** (right-click → Checksums…): MD5, SHA-1 and SHA-256 in one read, a Copy button each,
  and a field to paste the checksum you were given: ✓ or ✗.
- **Cut, copy, paste**, compatible with Thunar's clipboard. A highlighted folder receives the paste.
- **Drag & drop**: same file system moves, otherwise copies; Ctrl copies, Shift moves.
- **Rename, new folder, move to trash, delete permanently**, with a conflict dialog
  (skip, keep both, replace or merge, "do this for all").
- **Bookmarks**, shared with Thunar and the GTK file dialogs. The **Trash** is always the last
  line of the menu. It opens in a window of its own (close it and you are back) with the trash of
  every disk, sorted by name or with its buttons by size, newest or oldest deleted; the preview says where each item came from and when it was deleted, right-click puts items
  back, deletes them for good or empties the trash.
- **Search** (Ctrl+F): names with the typed text in, in this folder and every folder below it,
  upper/lower case alike, nearest first. Pick one to see it in the preview, Enter opens it in the
  columns, Escape goes back. Hidden files only when they are shown.
- **Computer column** (the computer button, Alt+/): all your disks with free space and a fill bar,
  USB sticks (one click mounts, right-click ejects), network shares and GVfs places (sftp://,
  smb://), and "Connect to server…". A disk opens to the right like any folder.
- **Type to jump**: type the first letters of a name; the same letter again goes to the next match.
- **Properties** with editable permissions, free space and folder totals.
- **Open as root** through polkit (`pkexec`): the password is asked by your polkit agent,
  millerX never sees it. A root window shows a red warning bar.
- **Highlight colour** of your own, chosen live in the settings, for millerX or for every
  GTK 3 program (a marked block in `~/.config/gtk-3.0/gtk.css`, removable again).
- **Languages:** English, German and Greek, following your system; Settings → Language can
  also pick English anyway, or one of two joke languages: **Wizard** ("Banish to the Void",
  "Thou art not worthy") and **Wienerisch** (Viennese slang, root is "da Kaiser").
  The Greek isn't from a native speaker, so corrections are very welcome. Translators welcome
  too: copy `po/millerx.pot` to `po/xx.po`, translate, send a pull request. Only the language in
  use is ever loaded; each one is about 30 KB on disk.
- Columns follow changes on disk. Names sort case-insensitive and naturally (file2 before
  file10), folders first (optional).

![The preview fits a picture into the rest of the window](docs/preview.png?v=074)

![The Computer column: disks, USB sticks and network, each with its free space](docs/computer.png?v=074)

What changed in each version: [CHANGELOG](CHANGELOG.md).

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
| Alt+Enter | properties |
| Ctrl+N | new window at this folder |
| Ctrl+, | settings |
| F5 | reload |
| Ctrl+Q, Ctrl+W | close |

Middle-click a folder (or right-click → Open in new window) to open it in a second window.

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

Programs it uses **if they are installed**, nothing breaks without them (Settings → Optional
helpers shows which ones it found):

| For | Program | Without it |
| --- | --- | --- |
| PDF previews | `pdftoppm` (poppler) | the PDF's icon |
| AVIF, HEIC and other pictures, video frames, music covers | `ffmpeg` | the file's icon |
| Open as root | `pkexec` + a polkit agent | no root window |
| Blender files' pictures | `blender-thumbnailer` (comes with Blender; also found in `/opt/blender`, else the newest `/opt/blender-*`) | the .blend icon |
| USB sticks, Connect to server | gvfs / udisks2 | only disks already mounted |
| Open terminal here | any terminal (`$TERMINAL` or one of 17 known) | a message saying none was found |

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

[browse]
folders_first = true
show_hidden = false

[thumbnails]
show = local             # local (none in network folders) | always | never
size = medium            # small | medium | large (about 100, 200, 400 px; at least 4, 3, 2 per row)
max_mb = 1024            # cache size to shrink to, least used first
max_days = 180           # forget thumbnails unused this long
auto_clean = true        # once a day when millerX starts

[helpers]                # installed helpers can be switched off
ffmpeg = true            # video frames, music covers, AVIF/HEIC
pdftoppm = true          # PDF pages
pkexec = true            # "Open as root"
blender = true           # pictures saved in .blend files

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
list them too, to change or remove.

![Settings](docs/settings.png?v=074)

## Bugs and wishes

Please open an [issue](https://github.com/overcatbe/millerx/issues). How to report a bug, suggest
an idea or add a translation: [CONTRIBUTING](CONTRIBUTING.md).

## License

GPL-2.0-or-later, see [COPYING](COPYING).

Forks are welcome. The name "millerX" and the logo aren't part of the license: a changed
version needs another name and logo, see [NOTICE](NOTICE).
