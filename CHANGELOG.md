# Changelog

## 0.6.4 (2026-11-21)

- **New window**: Ctrl+N or Menu → New window opens a second window at the same folder;
  right-click a folder → *Open in new window*, or middle-click it. Handy for comparing two
  folders. (Idea from a LinuxQuestions user who asked for tabs.)

## 0.6.3 (2026-11-21)

- **Builds on Slackware 15.0 again** (and anything else with GLib 2.70 or newer).
  0.6.2 used thumbnail names that only came with GLib 2.76. Now millerX finds the
  cached thumbnails itself, the way the freedesktop standard describes, so it works the
  same everywhere. Thanks to random_guy and Paulo2 on LinuxQuestions for the report.

## 0.6.2 (2026-11-20)

- **Your own highlight colour** (Settings → Highlight colour). Click a swatch or pick any
  colour and the window shows it at once; the text on it turns dark or light by itself.
  "Follow the theme" brings back the theme's colour.
- **For all GTK programs too**, if you like: *Use for all GTK programs…* writes the colour
  into `~/.config/gtk-3.0/gtk.css`, in a block marked as millerX's. Everything else in that
  file stays as it is, and *Remove from all* takes only that block out again.
  Programs pick it up the next time they start.

## 0.6.1 (2026-11-20)

- **AVIF, HEIC and other pictures** that gdk-pixbuf can't read are shown through
  **ffmpeg**, if it is installed, in the preview and in the gallery.
- **Names under thumbnails** (Settings → Gallery): one line under each tile, cut in the
  middle so the extension stays visible (`a-very-lo…nal-v2.jpg`). Off by default.

## 0.6 (2026-11-20)

### The preview pane

- **Takes the rest of the window.** The columns get what they need, the preview gets
  everything else and follows the window when you resize it. The width in the settings
  (and dragging the pane's edge) is now its *minimum*.
- **New layout:** the file or folder icon on the left, name and details on the right,
  a line, then the picture, text or gallery underneath.
- **One picture always fits** the pane, and is never enlarged beyond its own size.
- **WebP** pictures work everywhere, decoded with libwebp (many systems' gdk-pixbuf has
  no WebP loader). Animated WebP still shows its icon.

![The preview](docs/preview.png)

### The gallery (a folder's contents)

- **More columns on wide panes** instead of ever bigger tiles (around 200 px each).
- **Opens at once, even with thousands of files:** every tile is created right away as an
  icon, and pictures are loaded only for what is on screen plus one page ahead,
  four at a time. Tiles you scroll past quickly are skipped and loaded when they come back.
  Up to 5000 files per folder.
- **PDFs show their first page.**
- **Right-click a thumbnail** for its own menu: open, open with, show in column, cut, copy,
  rename, move to trash, delete permanently, copy path, properties.
  Delete, Shift+Delete, F2, the Menu key and Alt+Enter work on a selected thumbnail too.
- **Follows its folder:** deleted and renamed files change in place, the gallery doesn't
  jump back to the top.
- **"Show in column"** selects the file in its column and scrolls it into view.

### Thumbnails

- Thumbnails are **saved to the shared cache** (`~/.cache/thumbnails`, the freedesktop
  standard), so the second visit is instant, and Thunar, file dialogs and millerX all
  use each other's thumbnails.
- **The cache is kept in check**, once a day when millerX starts: thumbnails of files that
  are gone (not of files on a drive that is just unplugged), thumbnails unused for 180 days,
  then the least used ones above 1 GB. All three can be changed in the settings, which
  also show the cache's size and have a **Clear…** button.

### Sorting per folder

Everything stays **sorted by name**, so your eye learns where things are. A folder that
should be different gets its own order: right-click it → **Sort "…"** → name, date modified
(newest first), size (largest first) or type, optionally reversed.

- A thin bar on top of its column says how it is sorted; its ✕ goes back to name.
- A faint ⇅ next to the folder's name in the column on its left.
- The gallery uses the same order.
- The settings list every folder with its own order, to change or remove it.

![A folder sorted by size](docs/sorting.png)

### Smaller things

- Opening millerX with a path scrolls to that row once the columns are laid out.
- Settings: the preview's width is labelled "Minimum width".

## 0.5.6 (2026-11-19)

- Pictures in the preview as big as the pane allows; small ones keep their size.
- Folders show their contents as a thumbnail grid under the details
  (background loading, reuses existing thumbnails; double-click opens).

## 0.5.5 (2026-11-19)

- Bookmarks and drives menus open at full size right away (no resize, no scrolling up).
- A small ✕ next to each bookmark removes it.

## 0.5.4 (2026-11-19)

- First public release.
