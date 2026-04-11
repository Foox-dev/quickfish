#pragma once

#include <ncurses.h>
#include <panel.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "shell.h"

#define GOTO_BUF_MAX 512
#define RENAME_BUF_MAX 512

typedef enum {
	BUFFER_FILES,
	BUFFER_SHELL,
	BUFFER_PREVIEW,
	BUFFER_INFO,
} BufferType;

typedef struct {
	char index_jump_buf[8];
	char goto_buf[GOTO_BUF_MAX];
	char rename_buf[RENAME_BUF_MAX];
	char quickshell_buf[SHELL_MAX_INPUT];
	int info_mode;
	int max_x, max_y;
	int files_height, shell_height, divider_y;
	int running;
	int goto_mode;
	int goto_len;
	int rename_mode;
	int rename_len;
	int rename_cursor;
	int quickshell_mode;
	int quickshell_len;
	int quickshell_cursor;
	int preview_mode;
	int preview_scroll;
	int preview_last_selected;
	int preview_saved_col_offset;
	int index_jump_len;
	BufferType active_buffer;
	BufferType prev_buffer;
	WINDOW *files_win;
	WINDOW *shell_win;
	WINDOW *info_win;
	PANEL *info_panel;
	FilesBuffer files;
	ShellBuffer shell;
} TUI;

TUI *tui_init(const char *start_dir);
void tui_cleanup(TUI *tui);
void tui_run(TUI *tui);
void tui_resize_handler(TUI *tui);
void tui_render(TUI *tui);
void tui_handle_input(TUI *tui, int ch);
void tui_handle_files_input(TUI *tui, int ch);
void tui_handle_preview_input(TUI *tui, int ch);
void tui_handle_shell_input(TUI *tui, int ch);

extern volatile int needs_resize;
