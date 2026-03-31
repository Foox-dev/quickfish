#ifndef TUI_H
#define TUI_H

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include "file.h"
#include "shell.h"

#define GOTO_BUF_MAX 512
#define RENAME_BUF_MAX 512

typedef enum {
    BUFFER_FILES,
    BUFFER_SHELL,
} BufferType;

typedef struct {
    FilesBuffer files;
    ShellBuffer shell;
    BufferType  active_buffer;
    WINDOW *files_win;
    WINDOW *shell_win;
    int max_x, max_y;
    int files_height, shell_height, divider_y;
    int running;
    int goto_mode;
    char goto_buf[GOTO_BUF_MAX];
    int goto_len;
    int rename_mode;
    char rename_buf[MAX_FILENAME];
    int rename_len;
    int rename_cursor;

    int quickshell_mode;
    char quickshell_buf[SHELL_MAX_INPUT];
    int quickshell_len;
    int quickshell_cursor;
} TUI;

TUI *tui_init(const char *start_dir);
void tui_cleanup(TUI *tui);
void tui_resize_handler(TUI *tui);
void tui_render(TUI *tui);
void tui_handle_input(TUI *tui, int ch);
void tui_handle_files_input(TUI *tui, int ch);
void tui_handle_shell_input(TUI *tui, int ch);

#endif
