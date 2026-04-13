# Quickfish
`Quickfish` is a TUI file manager designed to replace the need for graphical alternatives. The goal for this project is to do everything a GUI file manager can do and more.

Proudly a part of the ObsidianWater project!

## How will this replace GUI file managers for me?
Most people use GUI file managers to move files to and from places easily. We try to replicate the ease of use with keybinds and motions. It's important to note that this will not be a one size fits all solution. This tool has lots of features and may take time to get used to its quirks and motions!

## Keybinds/Commands
A list of keybinds and motions for Quickfish are as follows:

### General
- "HJKL/ARROWS" Move between buffers.

### File Buffer
- **"hjkl/arrows"** Movement and simple actions. "j/k/up/down" move between items. "h/left" back one directory. "l/right" enter/open file/directory.
- **"g"** Goto mode. Type a directory then press enter.
- **":"** Quickshell mode. Run commands from the Shell Buffer whilst in the File Buffer.
- **"<index>+g"** Goto index. Example: "21+g" puts the cursor on index 21 in the buffer.
- **"p"** Preview mode. Show a preview of the folder or file (if possible). File support may vary.
- **"r"** Rename mode. Rename the file/folder under the cursor.
- **"d/D"** Delete file/folder under cursor. "d" moves to trash and "D" deletes permently (cannot be undone)
- **"u/U"** Undo/redo respectivly.
- **"s"** Select file under cursor. Can be used with Shell Buffer commands via the "sel" alias (eg, rm sel).
- **"v"** Same as "s" but sweep mode.

### Shell Buffer
The shell buffer also supports all commands available to your system.
- **"s"** Built in Stat command. Formatted for view in Quickfish.
- **"q"** Quit the program.

## Installation
Currently the only way to build and use Quickfish is via building manually. Soon when it's in a more stable position we will package it for the AUR.

## Building From Source
Building from source is fairly easy. All you need is `gcc` and `make`.

Default build (dev environment)
```bash
git clone https://github.com/Foox-dev/quickfish
cd quickfish
make
# Binary is placed in the build/ directory
```

Release build (recommended):
```bash
git clone https://github.com/Foox-dev/quickfish
cd quickfish
make rel
# Binary is placed in the build/release/ directory
```

## License
GPL 2.0
