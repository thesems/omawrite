# Omawrite v2

Local fork of Omawrite with a themed folder/file explorer. The editor, settings,
draft recovery, and unsaved-change prompts are retained.

- Click the folder name to choose a workspace; the last folder is remembered.
- Right-click a file, folder, or empty explorer space to create files and folders.
- Right-click an entry to rename it. Delete file and Delete folder move entries
  (including a folder's contents) to the system Trash.
- Drag entries onto folders to move them. Drop onto **Move to top level**, the
  workspace button, or empty space to move them back to the workspace root. The
  top-level drop area appears only while dragging.
- Moves and renames refuse name collisions. Open documents follow their new paths.
- Deleting an open file or its containing folder keeps its text as an unsaved draft.
- Ctrl+E or the footer menu button toggles the explorer.
- The explorer shows Markdown (`.md`) and text (`.txt`) files. Creating or renaming
  a file requires one of these extensions; folder names are unrestricted.

## Local build

```sh
mkdir -p build
cd build
qmake6 ../omawrite.pro
make -j4
```

The executable is `build/omawrite-v2`; it accepts a file or folder argument.
The installed copy is `~/.local/bin/omawrite-v2`. The user desktop entry
`~/.local/share/applications/omawrite.desktop` overrides the packaged launcher.
Super+Shift+W is overridden in `~/.config/hypr/bindings.lua` to launch v2.

Original integration files were backed up under
`~/.local/share/omawrite-v2/backups/20261012-103929/`. To roll back, restore that
`bindings.lua`, remove the user `omawrite.desktop` override, and reload Hyprland.
The original executable is still available as `/usr/bin/omawrite`.

## Checks

```sh
mkdir -p build-tests
cd build-tests
qmake6 ../tests/tests.pro
make -j4
QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=basic QT_QUICK_BACKEND=software ./tst_omawrite
```

The checks cover file creation, duplicate protection, renames, moves, trashing an
open document, drag-and-drop into and out of a folder, the empty-space context
menu, unsaved changes when switching files, and row mapping after file insertion.

---

## Upstream Omawrite

A dead-simple Markdown writing app built with Qt Quick and C++ that automatically follows system dark/light mode.

<img width="2948" height="3227" alt="screenshot-2026-06-23_15-24-08" src="https://github.com/user-attachments/assets/4e930c0d-edda-4046-b444-a59eff523329" />
<img width="2948" height="3227" alt="screenshot-2026-06-23_15-23-23" src="https://github.com/user-attachments/assets/8ced7c26-961b-4ded-b263-84403001a951" />


## Install

Install via the Omarchy Package Repository via the `omawrite` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Shortcuts

- `Ctrl+S` saves. Unsaved documents use the XDG desktop portal file picker.
- `Ctrl+Shift+S` saves as.
- `Ctrl+O` opens a Markdown file through the portal picker.
- `Ctrl+P` opens the system print dialog.
- `Ctrl+N` opens a new Omawrite window.
- `Ctrl+Z`, `Ctrl+Shift+Z`, and `Ctrl+Y` handle undo and redo.
- `Super+F` toggles fullscreen. Qt maps this key as `Meta+F`.
- `Ctrl+F` searches the document. Use `Enter` or `Ctrl+G` for the next match and `Shift+Enter` for the previous match.
- `Ctrl+H` opens find and replace.
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+K` insert bold, italic, and link Markdown.
- `Ctrl+?` shows the keyboard shortcut reference.

Unsaved drafts are recovered after an abnormal exit. Omawrite also watches open files
and warns before an external change can replace local work.

The font button in the footer picks the writing font from any installed text font,
and Omawrite remembers the choice. IBM Plex Mono is the default.

Text follows the desktop text size — `omarchy display text size`, or GNOME's
`text-scaling-factor` — and re-flows without a restart. The default of 12px leaves
Omawrite at the size it is designed around; larger and smaller sizes scale from there.

## Requirements

- Qt 6: `qt6-base`, `qt6-declarative`, `qt6-quickcontrols2`
- `xdg-desktop-portal` and a portal backend

The IBM Plex Mono font is bundled under the SIL Open Font License 1.1; see
`fonts/OFL.txt`. The font is copyright IBM Corp.
