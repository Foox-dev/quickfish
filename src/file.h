#pragma once

#include <ncurses.h>
#include <limits.h>
#include "colors.h"

#define MAX_FILENAME 256
#define MAX_FILES 1000

typedef enum {
    ENTRY_FILE,
    ENTRY_DIR,
} EntryType;

typedef struct {
    char name[MAX_FILENAME];
    EntryType type;
    int index;
    int is_empty;
} DirEntry;

typedef struct {
    DirEntry entries[MAX_FILES];
    int entry_count;
    int selected;
    int col_offset;
    char cwd[PATH_MAX];
} FilesBuffer;

struct ShellBuffer;

void files_load_directory(FilesBuffer *files, const char *path);
void files_sort_entries(FilesBuffer *files);
void files_select_next(FilesBuffer *files);
void files_select_next_n(FilesBuffer *files, int n);
void files_select_prev(FilesBuffer *files);
void files_select_prev_n(FilesBuffer *files, int n);
void files_change_dir(FilesBuffer *files, const char *dirname);
int  files_get_selected_path(FilesBuffer *files, char *out_path, size_t out_size);
void files_render(FilesBuffer *files, WINDOW *win, int height, int width, int focused);

int complete_in_dir(const char *dir, const char *prefix, char *out, int out_size);
void files_cmd_stat(FilesBuffer *files, struct ShellBuffer *shell, const char *arg);
void refresh_files_buffer(FilesBuffer *files);
