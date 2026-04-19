/**
 * @file tui.h
 * @brief Public interface for the Quickfish TUI layer.
 *
 * Declares the TUI struct and all functions for initialisation, rendering,
 * input handling, and cleanup. Include this wherever the TUI needs to be
 * driven from outside tui.c.
 */
#pragma once

#include <ncurses.h>
#include <panel.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "shell.h"

#define GOTO_BUF_MAX 512 ///< Maximum length of the goto input buffer including null terminator
#define RENAME_BUF_MAX 512 ///< Maximum length of the rename input buffer including null terminator
#define FILE_CMD_BUF_MAX 32 ///< Maximum length of the file command preview buffer including null terminator

/**
 * @brief Identifies which pane currently has focus.
 */
typedef enum {
	BUFFER_FILES, ///< File list pane
	BUFFER_SHELL, ///< Shell pane
	BUFFER_PREVIEW, ///< Preview pane
	BUFFER_INFO, ///< F1 help overlay
} BufferType;

/**
 * @brief Controls how a delete confirmation dialog handles the selected entry.
 */
typedef enum {
	DEL_NONE, ///< No delete operation pending
	DEL_TRASH, ///< Move to trash
	DEL_PERM, ///< Permanently delete (unrecoverable)
} DeleteMode;

/**
 * @brief Top-level state for the quickfish TUI.
 *
 * Owns all ncurses windows, overlay buffers, and references to the files
 * and shell subsystems. Allocated and initialised by tui_init().
 */
typedef struct {
	char file_cmd_buf[FILE_CMD_BUF_MAX];
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
	int file_cmd_len;
	int visual_mode;
	DeleteMode delete_confirm;
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
void tui_resize_handler(TUI *tui);
void tui_render(TUI *tui);
void tui_handle_input(TUI *tui, int ch);
void tui_handle_files_input(TUI *tui, int ch);
void tui_handle_preview_input(TUI *tui, int ch);
void tui_handle_shell_input(TUI *tui, int ch);

extern volatile int needs_resize;
