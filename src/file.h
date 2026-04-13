#pragma once

#include <limits.h>
#include <ncurses.h>
#include <sys/stat.h>

#include "colors.h"

#define MAX_FILENAME 256
#define MAX_FILES 1000
#define UNDO_MAX 10
#define MOVE_REGISTER_MAX MAX_FILES

typedef struct {
	int count;
	char paths[MOVE_REGISTER_MAX][PATH_MAX];
} MoveRegister;

typedef enum {
	ENTRY_FILE,
	ENTRY_DIR,
} EntryType;

typedef enum {
	OP_RENAME,
	OP_TRASH,
	OP_DELETE_PERM,
} OpType;

typedef struct {
	int index;
	int is_empty;
	EntryType type;
	mode_t mode;
	char name[MAX_FILENAME];
} DirEntry;

typedef struct {
	OpType type;
	char cwd[PATH_MAX];
	char old_name[MAX_FILENAME];
	char new_name[MAX_FILENAME];
	char trash_path[PATH_MAX];
} UndoOp;

typedef struct {
	int entry_count;
	int selected;
	int col_offset;
	int sel_count;
	int undo_top;
	int redo_top;
	char cwd[PATH_MAX];
	DirEntry entries[MAX_FILES];
	int multi_sel[MAX_FILES];
	int in_move_reg[MAX_FILES];
	UndoOp undo_stack[UNDO_MAX];
	UndoOp redo_stack[UNDO_MAX];
	MoveRegister move_reg;
} FilesBuffer;

struct ShellBuffer;

int dir_is_empty(const char *path);
void files_load_directory(FilesBuffer *files, const char *path);
void files_sort_entries(FilesBuffer *files);
void files_select_next(FilesBuffer *files);
void files_select_next_n(FilesBuffer *files, int n);
void files_select_prev(FilesBuffer *files);
void files_select_prev_n(FilesBuffer *files, int n);
void files_change_dir(FilesBuffer *files, const char *dirname);
int files_get_selected_path(FilesBuffer *files, char *out_path, size_t out_size);
void files_render(FilesBuffer *files, WINDOW *win, int height, int width, int focused);

int complete_in_dir(const char *dir, const char *prefix, char *out, int out_size);

int entry_color_pair(const DirEntry *e, int selected);
void files_cmd_stat(FilesBuffer *files, struct ShellBuffer *shell, const char *arg);
void refresh_files_buffer(FilesBuffer *files);

void files_open_selected(FilesBuffer *files, struct ShellBuffer *shell, WINDOW *files_win, WINDOW *shell_win);

void files_toggle_select(FilesBuffer *files, int idx);
void files_clear_selection(FilesBuffer *files);
void files_delete_to_trash(FilesBuffer *files, struct ShellBuffer *shell);
void files_delete_permanent(FilesBuffer *files, struct ShellBuffer *shell);
void files_undo(FilesBuffer *files, struct ShellBuffer *shell);
void files_redo(FilesBuffer *files, struct ShellBuffer *shell);
int files_build_sel_args(FilesBuffer *files, char *out, int out_size);

void files_move_mark(FilesBuffer *files, struct ShellBuffer *shell, int idx);
void files_move_mark_all(FilesBuffer *files, struct ShellBuffer *shell);
void files_move_clear(FilesBuffer *files, struct ShellBuffer *shell);
void files_move_paste(FilesBuffer *files, struct ShellBuffer *shell, const char *dest);
