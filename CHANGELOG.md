# Changelog

## 0.9.0 (not released yet)

One millerX again, wishes from the LinuxQuestions thread, and sticks without GVfs. (0.8.9 was
never released: it is all in here.)

- **A new settings tab, Unleash**, for how millerX behaves: Clicks and drags, and Updates.
- **millerX Unleashed is a setting now.** Settings → **Unleash** → **Clicks and drags**:
  - **Classic** (the default): a right-click selects, as millerX always did. What you select is
    what you process, and it is the way for touch screens. Thanks, SCerovec!
  - **Unleashed**: a right-click on something not selected only holds it (bold, underlined), its
    menu acts on it and the columns stay put; a drag carries what you picked up without selecting
    it; a folder held still under a drag opens. Thanks, kgha and brobr, who chose it!
  - Coming from millerX Unleashed: its settings, sort orders and trusted programs are taken over
    the first time millerX starts (if millerX had none of its own), with Unleashed chosen.
    millerX Unleashed itself ends with 0.8.8.

- **Wizard and Wienerisch moved to extras/languages**, next to pirate and LOLCAT: millerX installs
  German and Greek. Who used one: Settings → Language → "Your own…" adds it back (the file is in
  extras/languages); until then millerX speaks the system's language.
- **A review of all of millerX before 0.9.0** found these, all fixed:
  - **Language files:** a file could fake a plural form and so get a translation past the check
    that reads more `%s` than millerX passes (a crash). Every form is now checked against what
    millerX itself asks for.
  - **Trust system programs:** a program on an ext4 stick made by root counted as the system's
    and ran without the question. Only the system's own disk counts now (`/`, `/usr`, `/opt` …),
    never anything under /run, /media or /mnt.
  - **Git:** names with `[`, `*` or `?` were patterns to git, so Revert on `a[1].txt` also threw
    away the changes to `a1.txt`. Names are names now (`--literal-pathspecs`); and no fsmonitor
    program from a repository's config runs on Status, Diff, Add or Commit.
  - **Archives:** every Slackware package (and every `tar -C dir .`) said "1 item with an unsafe
    name was left out", though nothing was; and with that note, extracting over an existing folder
    with Merge did nothing. Both right now.
  - **Packing:** a file that couldn't be read to the end (a dead network share) or shrank while it
    was packed made a broken archive without a word. Now it says so and stops.
  - **Put back from the trash with Replace:** the old item was deleted first; if putting back then
    failed, both were gone. Now the old one waits aside and comes back if anything fails.
  - **Sticks:** after a refresh the Computer column picked its row back by name, so with two sticks
    called the same "Safe to remove" could hit the other one; restarting udisks2 announced (or, with
    Automount, mounted) every stick again.
  - **Restart now** waited for copies but not for a permanent delete, which it cut off halfway; and
    what was cut or copied is kept for the new millerX.
  - **Crashes:** Settings or Properties used after their main window was closed; the held item's
    menu (Unleashed) after its column went; the gallery after its window closed; a pulled-out
    encrypted stick while its passphrase was asked.
  - **The gallery** no longer opens FIFOs or devices named like pictures (four of them in /tmp
    stopped every thumbnail in that window).
  - **x-terminal-emulator** pointing to lxterminal or qterminal is passed over for running
    things, like those themselves.
  - **gtk.css** that couldn't be read is never written over (Use for all GTK programs), and a
    linked one stays a link. "By content" never opens a device file to look inside.
  - Coming from Unleashed: own language files come along, and the first start already speaks the
    chosen language.
- **USB sticks, cards and discs straight from udisks2**, no GVfs needed. The Computer column
  lists them with their name, device and file system; a click mounts, an encrypted one asks for
  its passphrase (wiped from memory after use). Right-click: Mount, Unlock…, Unmount and
  **Safe to remove**, which unmounts everything on that stick, locks what was unlocked and
  switches it off; the status line says it is writing out what is left, since that can take a
  while on a slow stick. Never offered for the disk the system runs from. Without udisks2 the
  Computer column works through GIO as before.
- **A stick plugged in gets a notice**: "SANDISK plugged in (/dev/sdb1, vfat, 32 GB)" with Mount
  and Show, then where it was mounted. Plug in, mount, copy, Safe to remove, walk away: the
  stick-shift way. Thanks, SCerovec!
- **Automount** (Settings → Tools → Devices, off by default): a stick mounts itself and opens in a
  new window. Encrypted ones always wait for you.
- **"millerX was updated · Restart now."** After a package upgrade the running millerX notices its
  own program was replaced and offers a restart; your windows come back where they were. New
  windows of an old millerX say so too; Settings → Unleash → Updates switches the bar off.
  Thanks, SCerovec!
- **The copy bar counts bytes**: it counts what to copy first, then "Copying name (2/5) · 1.2 GB
  of 4.0 GB", so one big file moves the bar too (it used to move only between files). A move on
  the same disk is still a quick rename, with nothing to count. Thanks, kgha!
- **"You may not read it"** instead of GLib's "Cannot retrieve attribute standard::type" in a
  normal window too: "/root belongs to root. Copy it in a millerX window opened as root", "…
  belongs to <user>", or "… is yours, but its permissions shut you out". Thanks, brobr!
- **`$TERMINAL` set to lxterminal, qterminal or tilix**: they still open folders, but running a
  script or a Git command uses the next terminal, because they split the command line their own
  way and a file name must never go through that.
- **Translations:** the texts about sticks and disks (udisks2) were never collected for
  translation; German and Greek have them now, and all 0.8.9 texts.

## 0.8.8 (2026-11-28)

Wishes from the LinuxQuestions thread. (There is no millerX 0.8.7: that number went to millerX
Unleashed's fix, and both are 0.8.8 now.)

- **Trusted programs.** The question before a program, script or launcher runs has a **Trusted**
  button: it runs it, and from then on without asking, as long as it is the same file (its
  SHA-256 is remembered; changed, it asks again and says so). Settings → Programs lists them,
  ✕ forgets one, and an open Settings window shows a newly trusted one at once. Thanks, SCerovec!
- **Trust system programs (owned by root)**, on by default: what your distribution installed
  (owned by root, in folders only root can change, on Linux's own disk file systems) runs without
  asking, also after updates. Your own files still ask, and so do sticks, ISOs and network shares
  mounted by root, and links whose own folder isn't root's.
- **Ask before running programs, scripts and launchers**, a setting in the same place, on by
  default. Off, everything runs at once without a question.
- **Settings has two new tabs.** **Programs**: Recognise files, Run (the switches above and the
  trusted list) and Compare with. **Tools**: parts of millerX that are off until you switch them
  on; Git is the first.
- **Middle-click a file to open it**, the same as a double-click (questions included). A folder
  still opens in a new window. Thanks, SCerovec!
- **The status line counts right.** After a multi-select it no longer keeps "3 items selected"
  when you click one item; several selected reads "/boot: 28 items, 3 selected". Thanks,
  random_guy!
- **Esc unselects** everything in the column; the columns to the right close and the status line
  shows the folder's count again. Thanks, random_guy!
- **Git actions** (a tool, off by default: Settings → Tools → Git). Inside a
  repository the right-click menu gets **Git ▸** Status, Log and Diff (in your terminal), Add,
  Commit… and Revert…. **Commit…** is a small window: the message (empty, the first line is the
  title) and exactly which files go in, plus "Also add the other changed files". Revert asks
  first, and keeps a file that was only added (it is just taken out of Git again). git runs with an argument list and the message goes in a file, so no file name or
  message can do anything odd. Thanks, random_guy!
- **Git colours** (a second switch there, off by default): new files green, changed ones red, a
  folder shows what's in it. millerX asks git about the shown folder only and again a moment after
  a change or a commit; a repository whose own config has filters or includes stays uncoloured,
  so browsing into a strange repository can't make git run anything. Thanks, random_guy!
- **Your `$TERMINAL` is understood.** Running a script, Check file type and Git's Status, Log and
  Diff used xterm's `-e` for every `$TERMINAL`; xfce4-terminal (`-x`), gnome-terminal, GNOME
  Console and Ptyxis (`--`), kitty and foot (the command just follows) and WezTerm (`start --`)
  now get their own way, from their manuals. One function does it for all three now.
- **Clearer messages in a root window** (thanks, brobr). Trash there goes to root's own trash
  (/root/.local/share/Trash), and the status line now says so; where root has no trash at all
  (/home, /boot, /var or another disk: GLib refuses), the question explains it before offering to
  delete. A copy GLib stops with "Cannot retrieve attribute standard::type" now says why: the file
  is on a mount only its own user may open (sshfs, a phone, a network share).
- German and Greek for all of it.
- README: a list of the testers. Thank you all!

## 0.8.6 (2026-11-26)

Security fixes from a review by rizitis (issue #5). Nothing here was ever a problem if you only
open files you trust. Most of it guards against the old Windows trick of a "holiday.jpg" that is
really a program. Edge cases, but now millerX asks before anything runs. Thanks, rizitis!

- **A program asks before it runs.** A double-click on a program (whatever its name, e.g. a
  "holiday.jpg" from a USB stick or an archive) shows a dialog first; Cancel is the default.
- **A launcher shows what it runs.** A double-click on a `.desktop` file with the x bit shows its
  name and the command it would run: Cancel / Open as text (the default) / Run.
- **Language files are checked** before use, on import and at every start: a translation may only
  use the `%s`, `%d`… of the original. A broken one could crash millerX; now it is refused and
  English is used.
- **The root window shows no previews:** no pictures, video, PDFs or archive contents are read as
  root. Text still shows.
- **Folders extracted from archives** (several items) get normal permissions instead of 0700.
- **Compare** without a diff tool installed runs in the background and can be stopped with Close;
  two big folders no longer freeze the window.
- **Hardened build:** PIE, full RELRO, stack protector and FORTIFY, for every package.
- **Recognise files by content** (Settings → Folders, off by default): millerX also reads the start
  of the file you select or open, and where the content says something else than the name (a
  program, text, a picture, an archive), the content wins in the preview, Properties, Open with
  and on a double-click. Other file managers go by the name; the icons in the lists stay by name
  here too. Not a virus scanner. Thanks, SCerovec!
- **Fixed:** after moving or trashing the folder a column showed, millerX opened the next folder
  on its own. It now stays on the parent folder.
- **Fixed:** your own language (and Wizard, Wienerisch) was ignored with `LANG=C.UTF-8`.

## 0.8.5 (2026-11-26)

- **Fixed a crash in Settings:** after changing a setting, closing Settings and changing another
  one, millerX could abort (double free), and the Compare box showed garbage. Since 0.8.3.
  Thanks, random_guy and SCerovec!
- **Extract to…** picks any folder to unpack into. It replaces "Extract to "name/"", which did
  the same as Extract here nearly always; Extract here stays as it was (one folder or file on
  top lands next to the archive, loose files get a folder of their own). Thanks, kgha, brobr and
  SCerovec!
- **Open with → Other application…** opens GTK's program chooser with every installed program,
  for files whose program doesn't register for their type. Opens the file once; the default
  stays (that is Set default).
- **Open with → Check file type** runs `file` on it in a terminal: a "photo.jpg" that is really
  text says so. Only there when `file` is installed. Thanks, gus3!
- **Greek** is complete again: the strings that came with 0.8.3 (Compare, the drop menu,
  Properties of several items) were missing.

## 0.8.4 (2026-11-25)

- **Gallery icons fill their tiles.** Files without a picture (text, code, …) showed their type
  icon at half the tile's size, which left a big gap between icon and name. Thanks, random_guy!
- **Tiny thumbnails:** Settings → Thumbnails → Size → **Tiny**, about 64 px, at least 6 per row:
  a big folder of photos at a glance.
- **Links look like links:** a symbolic link's name is in italics and its icon carries the link
  emblem, in the columns and in the gallery. Thanks, random_guy!
- **New file:** right click → **New file…**, right under New folder: asks for a name and makes
  an empty file, e.g. a notes.md in the folder you just made. Thanks, random_guy!
- **Ctrl+Shift when dropping makes links**, like Ctrl copies and Shift moves. The right-button
  drag (or Alt) still asks, the classic way. Thanks, SCerovec!

## 0.8.3 (2026-11-24)

- **The gallery shows files only.** Folder tiles are gone from it: a folder is never an end
  point, every folder tile looked the same, and they pushed the files you're looking for further
  down (that was also the big gap between names and pictures). A folder full of folders now shows
  its details and no gallery; a mixed one shows just its files. In a folder with many subfolders
  all the gallery's room goes to files. Thanks, brobr!
- **Columns first:** a folder's size count and its gallery now wait until its column is filled,
  so in a folder full of folders the names come at once instead of sharing the disk with the
  rest.
- **Properties of several items:** select a few files and folders, right click → Properties (or
  Alt+Return): one window for all of them, with how many files and folders, their size counted
  together, and the permissions: a box is ticked if all have it, empty if none has, and shows a
  dash if some have it; a click sets it for all of them. Before, it showed the folder they're in
  (hence "Enter" instead of "Exec"). Thanks, LQ!
- **Drag with the right button, and millerX asks:** Copy here, Move here or **Link here** (a
  symbolic link in the target folder). Holding Alt while dropping asks too, also for drags from
  other programs. The left button does what it always did; a right click without moving still
  opens the menu (now when you let go).
- **Compare two files:** select two, right click → **Compare**. It opens your comparison program:
  Settings → Optional helpers → **Compare with**: `auto` takes meld, kdiff3, kompare or diffuse,
  whichever is there, or put your own command (the two files go at its end). With none of them,
  millerX shows the differences itself (`diff -u`, or `diff -ru` for two folders).

## 0.8.2 (2026-11-23)

- **Your own languages:** Settings → Language → **"Your own…"** picks a language file (`.po` or
  `.mo`), checks it and adds it to the list; "Restart now" and millerX speaks it. Make one from
  the template that now comes with millerX (`/usr/share/millerx/millerx.pot`): fill in the
  `msgstr` lines, empty ones stay English. A file with a mistake (a missing `%s`) is turned down
  with the line that's wrong, so it can't crash millerX. The files live in
  `~/.local/share/millerx/languages/`.
- **Extra languages to download** in [extras/languages](extras/languages/), not part of the
  package, so millerX doesn't grow with every new one (the four it has stay): **pirate** (family-friendly: Davy Jones' Locker, walk the plank), **pirate-salty**
  (swears like a sailor, not for the kids) and **lolcat** (TEH LITTERBOX, NOM NOM NAO).
- **No more endless counting:** a folder shown in the preview counted its size and items again
  after every change in it, so during `slackpkg upgrade-all` it ran du over and over. It now
  counts once when the folder is opened, and again on F5. Thanks, SCerovec, for finding it!
- **Greek:** two more fixes by rizitis ("στο " in search results, "Show in column"), and the new
  texts.
- **Wizard, Wienerisch, German:** the new texts too.

## 0.8.1 (2026-11-23)

- **Greek translation revised by rizitis**, a native speaker. Thank you!
- **Preview fixed when millerX starts in a folder:** the details slid down to the middle and the
  gallery was missing (the details were measured as if each line were one letter wide).
- **Wizard:** bookmarks are "sacred texts" now ("Mark for return" sounded like getting something
  back from the trash).
- **German:** the space after "in" in search results was missing.

## 0.8.0 (2026-11-23)

- **The preview's name is a title across the top**, over the icon and the details: it lines up
  with them, and long file names get the whole width.

## 0.7.9 (2026-11-23)

- **Resizing fixed:** the window couldn't be made lower than a certain height, and pulling its
  width made the height jump: the details in the preview wrapped at the narrowest width and set
  the window's minimum height (up to 1400 px in a full folder). The preview now scrolls on its
  own when the window is lower than its contents; the window can be any size.

- **Window titles show the folder's name** ("Downloads", "Trash"), like Thunar, so windows can be
  told apart in the panel; the full path is in the location bar.

## 0.7.8 (2026-11-23)

- **Restart now** (Settings → Language) brings the window back at the same size and place;
  before, it came back at the size of the last time millerX was closed.

## 0.7.7 (2026-11-23)

- **Gallery picks show in the column:** clicking a thumbnail highlights the same file in the
  folder's column (and scrolls to it), so you see where you are; the gallery stays. Asked for
  on GitHub.

- **Languages:** millerX speaks your system's language where there is a translation: German and
  Greek so far (the Greek isn't from a native speaker: corrections are very welcome). Settings → Language: System, English, Wizard (English, as a pompous old sorcerer
  would put it) or Wienerisch (Viennese slang). Changing it offers "Restart now": millerX comes
  back at once with the same windows and folders in the new language (not while files are being
  copied). Programs opened from millerX keep your system's language. Translators welcome: see po/.

- **Man page** (`man millerx`) and AppStream metainfo for software centres.

## 0.7.6 (2026-11-23)

- **Names under thumbnails on by default** (Settings → Look → Gallery). Your own choice, once saved,
  stays.

- **Search:** Ctrl+F (or Menu → Search here…) searches the current folder and every folder below
  it for names with the typed text in, upper/lower case alike. Results show up while it searches,
  nearest first, each with the folder it is in; the preview shows the one you pick, Enter or a
  double-click opens it in the columns, Escape goes back. Stops at 5000 (type more to narrow it
  down); never walks into /proc, /sys, /dev or /run from above.

- **Trash:** always the last line of the bookmarks menu (no ✕), its icon full or empty. Sorted by
  name; its buttons on top sort by size, newest or oldest deleted, and name again. It opens in a
  window of its own (close it and you are back where you were) and shows the trash of every
  disk; the preview says where an item was deleted from and when (and still shows pictures and
  text). Right-click: Put back (the folder is made again if it is gone; something new in its place
  asks skip / keep both / replace), Delete permanently (also the Delete key), Empty trash…
  Dropping files on it moves them to the trash.

- **"Show in folder":** while millerX runs it answers browsers' "Show in folder" (D-Bus
  org.freedesktop.FileManager1) when no other file manager does. It never pushes another one away;
  the README says how to make it the one D-Bus starts.

- **Thumbnail size:** Settings → Thumbnails → Size: small, medium (as before) or large, about 100,
  200 or 400 px. A narrow preview still shows them apart (at least 4, 3 or 2 per row). Large ones
  are made once at 512 px and kept in the shared cache. Asked for on LinuxQuestions.

- **Set default program:** Open with → Set default lists the same programs with the current one
  ticked; the one you pick opens that kind of file from then on. Saved in `~/.config/mimeapps.list`,
  which Thunar and the file dialogs read too. Picking a program in Open with alone changes nothing.

- **Checksums:** right-click a file → Checksums…: MD5, SHA-1 and SHA-256 in one read (with a bar for
  big files, stops when you close it), a Copy button each, and a Compare field for the checksum you
  were given (spaces, case and a file name after it don't matter). Asked for on LinuxQuestions.

- **Blender files:** `.blend` files show the picture Blender saves in them, through Blender's own
  `blender-thumbnailer`: from PATH, else `/opt/blender`, else the newest `/opt/blender-*` (by
  version: 4.10 is newer than 4.2). An optional helper like ffmpeg; without Blender nothing changes.
  Asked for on LinuxQuestions.

## 0.7.5 (2026-11-22)

- **Double-click runs programs:** a program you may run (the x bit, and really a program) starts in
  its own folder; a script asks first: Run, Run in terminal (which waits for Enter so you can read
  what it printed), or Open to read it; a launcher (.desktop with the x bit) starts what it
  describes. Before, millerX looked for "the program for this type" and found none. A file that only
  has the x bit, like everything on a FAT or NTFS stick, still just opens.

- **Disk fill bar in the highlight colour:** the Computer column's preview shows how full a disk is
  in your highlight colour (or the theme's), red above 95 %. GTK's own colours were made for
  batteries: an almost empty disk showed orange.

## 0.7.4 (2026-11-22)

- **Computer column:** the computer button (Alt+/) opens a first column with all your disks (also
  the ones from /etc/fstab), USB sticks and other removable media, network shares and GVfs places
  (sftp://, smb://), plus "Connect to server…". A disk opens to the right like any folder; the
  preview shows the disk itself (device, file system, free space, a fill bar) instead of counting it.
  One click mounts a stick, right-click ejects or unmounts it. The separate drive button is gone.

- **Thumbnails setting:** Settings → Thumbnails → Show: Not in network folders (the default now:
  NFS, Samba, sshfs get plain icons, so a slow share stays quick), Always, or Never (plain icons
  everywhere). A file you click is still shown big.

- **Settings in three tabs:** Look (dividers and highlight on the left, browsing and preview on the
  right), Thumbnails (what to show, the cache, the optional helpers) and Folders (the ones sorted their
  own way). Shorter, no scrolling; they open on the tab you used last.

- **Optional helpers can be switched off:** Settings → Thumbnails → Optional helpers has a tick box
  for each one that's installed. ffmpeg off: no video frames, music covers or AVIF/HEIC; pdftoppm off:
  no PDF pages; pkexec off: "Open as root" disappears from every menu.

## 0.7.2 (2026-11-21)

- **Column width setting:** Settings → Browsing → Column width (120–600, default 230) sets where new
  folder columns start. Open columns follow it at once; one you dragged keeps its own width, and
  double-clicking its divider brings it back to the setting.

## 0.7.1 (2026-11-21)

- **millerX remembers its window:** close it and open it again, and it comes back with the same size,
  in the same place, maximized if it was. If that place isn't on any screen any more (monitor
  unplugged), it opens centred. On Wayland only the size is kept: the compositor decides where windows go.
- **New windows** (Ctrl+N, middle-click) open centred and bigger, 1400 × 800 (at most 85 % of the
  screen), not wherever the window manager drops them. The very first start does the same.
- **Narrow preview:** when the pane gets narrow, the icon moves above the details, so they keep the
  whole width instead of breaking into a few letters per line. Wider again, it goes back beside them.
- **Settings in boxes:** every section has its own frame with its title on it, the labels line up
  across all of them, and the window scrolls if the screen is too small for it.

## 0.7 (2026-11-21)

The gallery (a folder's contents under its details) grew up:

- **Select several thumbnails**: Ctrl+click adds or removes one, Shift+click takes a range,
  Ctrl+A all of them. Right-click, Delete, Shift+Delete, Ctrl+C and Ctrl+X work on the whole
  selection; Rename and Properties on one. A plain click on one of several selected picks just
  that one.
- **Ctrl+C and Ctrl+X** on thumbnails, like in a column.
- **Drag thumbnails out**, one or the whole selection, like rows in a column: into Audacious, GIMP,
  a browser, another millerX window or a folder. The thumbnail goes along under the mouse.
  (Asked for on LinuxQuestions.)
- **Videos show a frame** (a few seconds in; very short ones their first frame) and **music files
  their embedded cover**, in the gallery and in the big preview, through ffmpeg if it is
  installed. Music without a cover keeps its icon and isn't asked again. Saved in the shared
  thumbnail cache like pictures.
- The right-click menu of thumbnails also offers **Compress…** and, for archives, **Extract**.
- **Settings → Optional helpers** shows which helper programs millerX found (ffmpeg, pdftoppm,
  pkexec), what each one gives, and what to install if one is missing. So no more wondering why
  pictures have thumbnails and videos don't.
- **A folder's details follow it:** size and item counts update when files are added, deleted
  or moved in or out while you look at it (they used to keep the old numbers).

## 0.6.5 (2026-11-21)

- **Archives: unpack and pack**, right in millerX, with libarchive (which millerX already used for
  the preview, so nothing new to install):
  - right-click an archive → **Extract here**: an archive with one folder (or file) on top puts
    just that next to it; one with several things on top gets its own folder, so nothing spills
    out. **Extract to "name/"** always makes a folder.
  - right-click files or folders → **Compress…**: pick a name and zip, tar.gz, tar.xz, tar.zst
    or 7z.
  - Runs in the background with the progress bar and Cancel; a cancelled or failed job leaves
    nothing half-done behind. Name conflicts ask, as with copying (skip, keep both, merge).
  - **Safe with hostile archives:** entries with absolute paths or `..` are left out (and you're
    told), and nothing is written through symlinks, so an archive can't put files outside the
    folder you unpack it in.
  - Reads zip, tar (gz, bz2, xz, zst, …), 7z, rar, cpio, iso, jar, cbz, epub and Slackware's
    txz/tgz packages. The preview now also lists 7z, rar and cpio contents.
- **Slackware: a ready-built .txz** on every release from now on, built on Slackware 15.0 (so it
  runs on 15.0 and -current). The SlackBuild is still there for building it yourself.

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
