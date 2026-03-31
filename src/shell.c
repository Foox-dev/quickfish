#include "shell.h"
#include "main.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdio.h>
#include <limits.h>

typedef struct {
    const char *cmd;
    const char *label;
} CmdLabel;

// result ghosts that show the result of a command.
// We do this since we will create custom commands that are
// non standerd and this will be a good way to show the user
// what something does (plus it's cool!). I got this idea
// from clifm
static const CmdLabel cmd_labels[] = {
    { "q", "quit" },
    { "ls", "list" },
    { "pwd", "working dir" },
    { "mkdir", "create dir" },
    { "rmdir", "remove dir" },
    { "rm", "remove file" },
    { "cp", "copy" },
    { "mv", "move" },
    { "touch", "touch" },
    { "cat", "meow" },
    { "echo", "echo" },
    { "chmod", "change perm" },
    { "curl", "curl" },
    { "wget", "wget" },
    { "swordfish", "no way!" },
    { NULL, NULL }
};

static const char *lookup_label(const char *cmd) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    for (int i = 0; cmd_labels[i].cmd != NULL; i++) {
        const char *c = cmd_labels[i].cmd;
        size_t len = strlen(c);
        if (strncmp(cmd, c, len) == 0 &&
            (cmd[len] == '\0' || cmd[len] == ' ' || cmd[len] == '\t'))
            return cmd_labels[i].label;
    }
    return NULL;
}

void shell_add_history(ShellBuffer *shell, const char *cmd) {
    if (shell->history_count >= SHELL_MAX_HIST) return;
    strncpy(shell->history[shell->history_count], cmd, SHELL_MAX_INPUT - 1);
    shell->history[shell->history_count][SHELL_MAX_INPUT - 1] = '\0';
    shell->history_count++;
}

void shell_handle_char(ShellBuffer *shell, int ch) {
    if (ch == '\x7f' || ch == KEY_BACKSPACE) {
        if (shell->input_pos > 0) {
            shell->input_pos--;
            shell->current_input[shell->input_pos] = '\0';
        }
        return;
    }
    if (ch == KEY_UP) {
        if (shell->history_index < shell->history_count - 1) {
            shell->history_index++;
            strncpy(shell->current_input,
                    shell->history[shell->history_count - 1 - shell->history_index],
                    SHELL_MAX_INPUT - 1);
            shell->input_pos = (int)strlen(shell->current_input);
        }
        return;
    }
    if (ch == KEY_DOWN) {
        if (shell->history_index > 0) {
            shell->history_index--;
            strncpy(shell->current_input,
                    shell->history[shell->history_count - 1 - shell->history_index],
                    SHELL_MAX_INPUT - 1);
            shell->input_pos = (int)strlen(shell->current_input);
        } else if (shell->history_index == 0) {
            shell->history_index = -1;
            memset(shell->current_input, 0, SHELL_MAX_INPUT);
            shell->input_pos = 0;
        }
        return;
    }
    if (ch >= 32 && ch < 127 && shell->input_pos < SHELL_MAX_INPUT - 1) {
        shell->current_input[shell->input_pos++] = (char)ch;
        shell->current_input[shell->input_pos] = '\0';
    }
}

void shell_tab_complete(ShellBuffer *shell, const char *cwd) {
    int tok_start = shell->input_pos;
    while (tok_start > 0 && shell->current_input[tok_start - 1] != ' ')
        tok_start--;

    char token[SHELL_MAX_INPUT];
    int tok_len = shell->input_pos - tok_start;
    strncpy(token, shell->current_input + tok_start, tok_len);
    token[tok_len] = '\0';

    char dir_part[PATH_MAX];
    char name_part[MAX_FILENAME];
    const char *last_slash = strrchr(token, '/');

    if (last_slash) {
        int dlen = (int)(last_slash - token);
        strncpy(dir_part, token, dlen);
        dir_part[dlen] = '\0';
        strncpy(name_part, last_slash + 1, MAX_FILENAME - 1);
        name_part[MAX_FILENAME - 1] = '\0';
    } else {
        dir_part[0] = '\0';
        strncpy(name_part, token, MAX_FILENAME - 1);
        name_part[MAX_FILENAME - 1] = '\0';
    }

    char search_dir[PATH_MAX];

    if (dir_part[0] == '\0') {
        strncpy(search_dir, cwd, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
    } else if (dir_part[0] == '/') {
        strncpy(search_dir, dir_part, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
    } else {
        path_join(search_dir, PATH_MAX, cwd, dir_part);
    }

    char completed[MAX_FILENAME];
    int count = complete_in_dir(search_dir, name_part, completed, sizeof(completed));
    if (count == 0 || completed[0] == '\0') return;

    char new_token[PATH_MAX];

    if (dir_part[0] == '\0') {
        strncpy(new_token, completed, sizeof(new_token) - 1);
        new_token[sizeof(new_token) - 1] = '\0';
    } else {
        path_join(new_token, sizeof(new_token), dir_part, completed);
    }

    char new_input[SHELL_MAX_INPUT];
    strncpy(new_input, shell->current_input, tok_start);
    new_input[tok_start] = '\0';
    strncat(new_input, new_token, SHELL_MAX_INPUT - 1 - tok_start);

    strncpy(shell->current_input, new_input, SHELL_MAX_INPUT - 1);
    shell->current_input[SHELL_MAX_INPUT - 1] = '\0';
    shell->input_pos = (int)strlen(shell->current_input);
}

void shell_restore_ncurses(WINDOW *files_win, WINDOW *shell_win) {
    reset_prog_mode();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    if (files_win) keypad(files_win, TRUE);
    if (shell_win)  keypad(shell_win, TRUE);

    curs_set(0);
    clearok(stdscr, TRUE);
    refresh();

    if (files_win) { clearok(files_win, TRUE); touchwin(files_win); }
    if (shell_win)  { clearok(shell_win, TRUE); touchwin(shell_win); }
}

static void do_spawn(const char *cwd, WINDOW *files_win, WINDOW *shell_win) {
    def_prog_mode();
    endwin();

    const char *sh = getenv("SHELL");
    if (!sh || sh[0] == '\0') sh = "/bin/sh";

    pid_t pid = fork();
    if (pid == 0) {
        if (cwd && cwd[0]) chdir(cwd);
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        execlp(sh, sh, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }

    shell_restore_ncurses(files_win, shell_win);
}

static void run_shell(const char *cmd, const char *cwd, ShellBuffer *shell) {
    shell->last_output[0] = '\0';

    int pfd[2];
    if (pipe(pfd) != 0) return;

    const char *sh = getenv("SHELL");
    if (!sh || sh[0] == '\0') sh = "/bin/sh";

    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        if (cwd && cwd[0]) chdir(cwd);
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        execlp(sh, sh, "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);

    if (pid > 0) {
        int total = 0;
        char buf[512];
        int n;
        while ((n = (int)read(pfd[0], buf, sizeof(buf))) > 0) {
            if (total + n < SHELL_OUTPUT_MAX - 1) {
                memcpy(shell->last_output + total, buf, (size_t)n);
                total += n;
            }
        }
        shell->last_output[total] = '\0';
        close(pfd[0]);
        int status;
        waitpid(pid, &status, 0);
    } else {
        close(pfd[0]);
    }
}

void shell_open_file(const char *path, WINDOW *files_win, WINDOW *shell_win) {
    const char *editor = getenv("VISUAL");
    if (!editor || editor[0] == '\0') editor = getenv("EDITOR");
    if (editor && editor[0] != '\0') {
        def_prog_mode();
        endwin();
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            execlp(editor, editor, path, (char *)NULL);
            _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        }
        shell_restore_ncurses(files_win, shell_win);
        return;
    }

    const char *pager = getenv("PAGER");
    if (!pager || pager[0] == '\0') pager = "less";
    def_prog_mode();
    endwin();
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        execlp(pager, pager, path, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
    shell_restore_ncurses(files_win, shell_win);
}

static const char *parse_cd(const char *cmd) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (strncmp(cmd, "cd", 2) != 0) return NULL;
    const char *after = cmd + 2;
    if (*after != '\0' && *after != ' ' && *after != '\t') return NULL;
    while (*after == ' ' || *after == '\t') after++;
    return after;
}

int execute_given(ShellBuffer *shell, FilesBuffer *files,
                  WINDOW *files_win, WINDOW *shell_win) {
    char cmd[SHELL_MAX_INPUT];
    strncpy(cmd, shell->current_input, SHELL_MAX_INPUT - 1);
    cmd[SHELL_MAX_INPUT - 1] = '\0';

    int len = (int)strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t'))
        cmd[--len] = '\0';

    memset(shell->current_input, 0, SHELL_MAX_INPUT);
    shell->input_pos     = 0;
    shell->history_index = -1;

    if (len == 0) {
        do_spawn(files->cwd, files_win, shell_win);
        return 0;
    }

    strncpy(shell->last_cmd, cmd, SHELL_MAX_INPUT - 1);
    shell->last_cmd[SHELL_MAX_INPUT - 1] = '\0';

    const char *label = lookup_label(cmd);
    if (label) {
        strncpy(shell->last_result, label, SHELL_RESULT_MAX - 1);
        shell->last_result[SHELL_RESULT_MAX - 1] = '\0';
    } else {
        shell->last_result[0] = '\0';
    }

    shell_add_history(shell, cmd);

    if (strcmp(cmd, "q") == 0) {
        shell->quit_requested = 1;
        return 0;
    }

    const char *cd_arg = parse_cd(cmd);
    if (cd_arg != NULL) {
        char target[PATH_MAX];

        if (*cd_arg == '\0') {
            const char *home = getenv("HOME");
            snprintf(target, PATH_MAX, "%s", (home && home[0]) ? home : "/");
        } else {
            char *endp;
            long idx = strtol(cd_arg, &endp, 10);
            if (*endp == '\0' && idx > 0) {
                int found = 0;
                for (int i = 0; i < files->entry_count; i++) {
                    if (files->entries[i].index == (int)idx &&
                        files->entries[i].type == ENTRY_DIR) {
                        if (strcmp(files->cwd, "/") == 0)
                            snprintf(target, PATH_MAX, "/%.*s",
                                     (int)(PATH_MAX - 2), files->entries[i].name);
                        else
                            path_join(target, PATH_MAX, files->cwd, files->entries[i].name);
                        found = 1;
                        break;
                    }
                }
                if (!found) return 0;
            } else if (cd_arg[0] == '~') {
                const char *home = getenv("HOME");
                if (!home) home = "/";
                snprintf(target, PATH_MAX, "%s%s", home, cd_arg + 1);
            } else if (cd_arg[0] != '/') {
                path_join(target, PATH_MAX, files->cwd, cd_arg);
            } else {
                snprintf(target, PATH_MAX, "%s", cd_arg);
            }
        }

        char resolved[PATH_MAX];
        const char *use = realpath(target, resolved) ? resolved : target;
        files_load_directory(files, use);
        return 1;
    }

    run_shell(cmd, files->cwd, shell);
    return 0;
}

void compute_ghost(const char *input, int input_pos, const char *cwd,
                           char *ghost_out, int ghost_max) {
    ghost_out[0] = '\0';
    if (input_pos <= 0 || !cwd) return;

    int tok_start = input_pos;
    while (tok_start > 0 && input[tok_start - 1] != ' ')
        tok_start--;

    int tok_len = input_pos - tok_start;
    if (tok_len <= 0) return;

    char token[SHELL_MAX_INPUT];
    strncpy(token, input + tok_start, tok_len);
    token[tok_len] = '\0';

    int name_len = tok_len;
    int is_first_token = (tok_start == 0);
    int has_slash = (strchr(token, '/') != NULL);

    if (is_first_token && !has_slash) {
        for (int i = 0; cmd_labels[i].cmd != NULL; i++) {
            if (strcmp(cmd_labels[i].cmd, token) == 0) {
                snprintf(ghost_out, ghost_max, " > %s", cmd_labels[i].label);
                return;
            }
        }
        return;
    }

    if (!is_first_token) return;

    char dir_part[PATH_MAX];
    char name_part[MAX_FILENAME];
    const char *last_slash = strrchr(token, '/');
    if (last_slash) {
        int dlen = (int)(last_slash - token);
        strncpy(dir_part, token, dlen);
        dir_part[dlen] = '\0';
        strncpy(name_part, last_slash + 1, MAX_FILENAME - 1);
        name_part[MAX_FILENAME - 1] = '\0';
        name_len = (int)strlen(name_part);
    } else {
        dir_part[0] = '\0';
        strncpy(name_part, token, MAX_FILENAME - 1);
        name_part[MAX_FILENAME - 1] = '\0';
    }

    if (name_len <= 0) return;

    char search_dir[PATH_MAX];
    if (dir_part[0] == '\0') {
        strncpy(search_dir, cwd, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
    } else if (dir_part[0] == '/') {
        strncpy(search_dir, dir_part, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
    } else {
        path_join(search_dir, PATH_MAX, cwd, dir_part);
    }

    char completed[MAX_FILENAME];
    int count = complete_in_dir(search_dir, name_part, completed, sizeof(completed));
    if (count == 0 || completed[0] == '\0') return;

    int comp_len = (int)strlen(completed);
    if (comp_len <= name_len) return;

    strncpy(ghost_out, completed + name_len, ghost_max - 1);
    ghost_out[ghost_max - 1] = '\0';
}

void shell_render(ShellBuffer *shell, WINDOW *win, int height, int focused, const char *cwd) {
    int width = getmaxx(win);
    werase(win);

    if (focused) {
        wattron(win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
        mvwprintw(win, 0, 0, " SHELL ");
        wattroff(win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
        mvwprintw(win, 0, 8,
            " Enter: spawn shell q: quit");
    } else {
        wattron(win, COLOR_PAIR(CP_TITLE_UF) | A_BOLD);
        mvwprintw(win, 0, 0, " SHELL ");
        wattroff(win, COLOR_PAIR(CP_TITLE_UF) | A_BOLD);
    }
    wclrtoeol(win);

    int out_rows = height - 3;
    if (out_rows > 0 && shell->last_output[0] != '\0') {
        char buf[SHELL_OUTPUT_MAX];
        strncpy(buf, shell->last_output, SHELL_OUTPUT_MAX - 1);
        buf[SHELL_OUTPUT_MAX - 1] = '\0';

        const char *line_ptrs[1024];
        int nlines = 0;
        line_ptrs[nlines++] = buf;
        for (char *p = buf; *p && nlines < 1024; p++) {
            if (*p == '\n') {
                *p = '\0';
                if (*(p + 1) != '\0')
                    line_ptrs[nlines++] = p + 1;
            }
        }
        int start = (nlines > out_rows) ? nlines - out_rows : 0;
        for (int i = start; i < nlines; i++) {
            int row = 1 + (i - start);
            if (row >= height - 2) break;
            mvwprintw(win, row, 1, "%.*s", width - 2, line_ptrs[i]);
        }
    }

    int result_row = height - 2;
    if (result_row >= 1 && shell->last_cmd[0] != '\0') {
        wattron(win, A_DIM);
        mvwprintw(win, result_row, 1, "%.*s", width / 2, shell->last_cmd);
        if (shell->last_result[0] != '\0') {
            int cmd_cols = (int)strlen(shell->last_cmd);
            if (cmd_cols > width / 2) cmd_cols = width / 2;
            int x = 1 + cmd_cols;
            if (x + 3 < width)
                mvwprintw(win, result_row, x, " > %.*s", width - x - 4, shell->last_result);
        }
        wattroff(win, A_DIM);
        wclrtoeol(win);
    }

    int input_row = height - 1;
    wattron(win, COLOR_PAIR(CP_PROMPT) | A_BOLD);
    mvwaddstr(win, input_row, 1, "> ");
    wattroff(win, COLOR_PAIR(CP_PROMPT) | A_BOLD);

    waddstr(win, shell->current_input);

    if (focused) {
        char ghost[MAX_FILENAME];
        compute_ghost(shell->current_input, shell->input_pos, cwd,
                      ghost, sizeof(ghost));
        if (ghost[0] != '\0') {
            wclrtoeol(win);
            wattron(win, A_DIM);
            waddstr(win, ghost);
            wattroff(win, A_DIM);
        }
    }

    wclrtoeol(win);

    if (focused) {
        int cx = 3 + shell->input_pos;
        wmove(win, input_row, cx);
        leaveok(win, FALSE);
        curs_set(2);
    } else {
        leaveok(win, TRUE);
    }

    wnoutrefresh(win);
}
