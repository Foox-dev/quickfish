# Todo list (and ideas) Also has some feature ideas and stuff, yada yada.

## TODO
- clipboard support (+ register)
- put expensive tasks on a differant thread
- add the Operation Buffer
- add proper comments and documentation to all files
- hold down "s" to "drag select"
- Everything is a command system. We start with a command, like "rename" or "delete" then theres an alias for it that's a keybind. Like r or D

## ideas
- add image preview via some in terminal image renderer (find it out later)
- Bulk rename
- Jumplist/bookmarks
- Git integration
- Exiting the program puts you in the file directory the last where in (might be redundant since we have the Shell Buffer)
- Config file
  - Folders/files that can never be deleted
  - Custom keybinds
  - Workspaces
  - Buffer Layout
- Theme support (like swordfishs)
  - Config file support. Set the theme automaticly in the config file
- Plugin support
- Argument support
  - Default args in the config file?
- Persistent workspaces (eg "--workspace dev" would open `<dir>/build`)
  - Default workspace/quick loaded workspaces in config file?
- A second file buffer to allow multipanel managing
  - Do we set this by default, or in config file?
- Action Pipelines. Build pipelines like "compress > move > rename"
- Built in file conversion TUI. Would use ffmpeg as a backend (optinaly)
- Binary inspector via a hex view
- Rename file shell command ("r"?)
- Context-aware shell. You can use the sel alias (and "r" rename, if we add that) like "r sel.ext png" to rename all the selections file extentions to "png". We would also add "cur" for current file, "cwd" for current directory, and subsections for each (like "cwd.build/" for `<cwd>/build/`)
- Command templates "template compress = tar -czf {dir}.tar.gz {dir}" then use it with "compress sel" in the shell buffer
- Quick Fuzzy search ("/" in file buffer?)
- Quickview trash buffer (view whats been put into the trash by quickfish)
- Undo with a preview inside the file buffer
- Edit file/folder permissions via a simple tui/buffer (include adding +/-x)
- File as objects system. Files can be tagged and sorted and marked as "important" or other tags that effect actions towards them

## Buffer system
**Core Buffers:**
File Buffer: The buffer for files. It's always the default buffer you start in.
Shell Buffer: Can run commands and more complex features that the quickshell cannot easily.
No children yet.
  
**Secondary Buffers:**
Preview Buffer: Child of the File Buffer.
Operation Buffer (TODO): Will display actions like a preview for some things, batch actions, things that cant fit in the File Buffer, but need a TUI. Also a child of the File Buffer
