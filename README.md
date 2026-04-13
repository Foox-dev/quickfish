# Quickfish
`Quickfish` is a TUI file manager designed to replace the need for graphical alternatives. The goal of this project is to do everything a GUI file manager can do, and more.

Proudly a part of the ObsidianWater project!

## Features
- Keyboard-driven navigation with vim-style motions
- Inline shell buffer with full system command support
- File selection, move marking, trash, and permanent delete
- Undo/redo support
- File and folder preview

## Keybinds

See [`keybinds.md`](docs/keybinds.md) for the full keybind reference, or press `F1` in-app.

## Dependencies
- `gcc`
- `make`
- `ncurses` (with panel support)

## Building From Source

**Development build:**
```bash
git clone https://github.com/Foox-dev/quickfish
cd quickfish
make
# Binary placed in build/
```

**Release build (recommended):**
```bash
git clone https://github.com/Foox-dev/quickfish
cd quickfish
make rel
# Binary placed in build/release/
```

## Installation

After a release build, install the binary and supporting files to your system:
```bash
sudo make install
```

To uninstall:
```bash
sudo make uninstall
```

The install prefix defaults to `/usr/local`. Override it with `PREFIX`:
```bash
sudo make install PREFIX=/usr
```

## Building the Documentation

Code documentation is generated with [Doxygen](https://www.doxygen.nl/):
```bash
make docs
# Output placed in docs/doxygen/
```

## Cleaning Build Artifacts

```bash
make clean
```

## Packaging
Quickfish is not yet packaged for any distribution. Once the project reaches a stable state, an AUR package is planned.

## License
[GPL 2.0](LICENSE)
