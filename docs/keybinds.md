# Quickfish Keybinds

This document lists the default keybindings for Quickfish.

## Navigation
- `j` / Down Arrow: Move down
- `k` / Up Arrow: Move up
- `h` / Left Arrow: Go to parent directory
- `l` / Right Arrow / Enter: Enter directory or open file
- `Ctrl+D` / `Ctrl+U`: Half-page down / up

## File Manager
- `g`: Go to path or name
- `<num>g`: Go to directory by index
- `..`: Jump to previous directory
- `r`: Rename selected entry
- `:`: Quickshell (inline command)
- `F1`: Toggle help overlay
- `s`: Toggle-select item
- `v`: Sweep/visual select mode
- `d`: Trash selected/selection
- `D`: Permanently delete
- `u` / `U`: Undo / redo last operation
- `m`: Mark file for move
- `M`: Mark selection for move
- `p`: Paste move register into current directory
- `P`: Paste move register into selected directory
- `Ctrl+C`: Clear move register

## Preview / Pane Switching
- `Ctrl+P`: Open preview pane
- `p` in preview pane: Close preview pane
- `K` / Shift+Down: Focus files pane
- `J` / Shift+Up: Focus shell pane
- `L` / Shift+Right: Focus next pane
- `H` / Shift+Left: Focus previous pane

## Shell Commands
- `q`: Quit
- `cd <path>`: Change directory
- `cd <num>` / `<num>`: CD to dir by index
- `..`: Jump to previous dir
- `s <file>`: Stat a file or index
- `r` / `rm <n>`: Remove by index or name

## Debug
- `$jumplist`: Print the current directory jumplist
- `$move`: Print the move register contents
- `$undo`: Print the undo stack top-to-bottom
- `$redo`: Print the redo stack top-to-bottom

---

For more details, see the in-app help (F1) or the main documentation.
