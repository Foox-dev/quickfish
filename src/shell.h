#pragma once

#include <ncurses.h>
#include "file.h"
#include "colors.h"

#define SHELL_MAX_INPUT 512
#define SHELL_MAX_HIST 100
#define SHELL_OUTPUT_MAX 8192
#define SHELL_RESULT_MAX 128
#define DIR_JUMPLIST_MAX 64

typedef struct ShellBuffer {
    char history[SHELL_MAX_HIST][SHELL_MAX_INPUT];
    int history_count;
    int history_index;
    char current_input[SHELL_MAX_INPUT];
    int input_pos;
    char last_output[SHELL_OUTPUT_MAX];
    char last_cmd[SHELL_MAX_INPUT];
    char last_result[SHELL_RESULT_MAX];
    int quit_requested;
    char dir_jumplist[DIR_JUMPLIST_MAX][PATH_MAX];
    int dir_jumplist_count;
    int rm_confirm_mode;
    char rm_pending_cmd[SHELL_MAX_INPUT];
} ShellBuffer;

void shell_add_history(ShellBuffer *shell, const char *cmd);
void shell_handle_char(ShellBuffer *shell, int ch);
void shell_tab_complete(ShellBuffer *shell, const char *cwd);
void shell_restore_ncurses(WINDOW *files_win, WINDOW *shell_win);
int execute_given(ShellBuffer *shell, FilesBuffer *files,
                   WINDOW *files_win, WINDOW *shell_win);
void shell_render(ShellBuffer *shell, WINDOW *win, int height,
                  int focused, const char *cwd, const FilesBuffer *files);

void compute_ghost(const char *input, int input_pos, const char *cwd,
                   const FilesBuffer *files, const ShellBuffer *shell,
                   char *ghost_out, int ghost_max);
int print_to_shell(ShellBuffer *shell, const char *given);
