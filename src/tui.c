#include "tui.h"
#include "main.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>

static volatile int needs_resize = 0;
static void handle_resize(int sig) { (void)sig; needs_resize = 1; }

static void rebuild_windows(TUI *tui) {
    getmaxyx(stdscr, tui->max_y, tui->max_x);

    tui->files_height = (tui->max_y - 1) / 2;
    tui->shell_height = tui->max_y - tui->files_height - 1;
    tui->divider_y = tui->files_height;

    if (tui->files_win) delwin(tui->files_win);
    if (tui->shell_win) delwin(tui->shell_win);

    tui->files_win = newwin(tui->files_height, tui->max_x, 0, 0);
    tui->shell_win = newwin(tui->shell_height, tui->max_x, tui->files_height + 1, 0);
    if (tui->files_win) keypad(tui->files_win, TRUE);
    if (tui->shell_win) keypad(tui->shell_win, TRUE);
}

TUI *tui_init(const char *start_dir) {
    TUI *tui = calloc(1, sizeof(TUI));
    if (!tui) return NULL;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    set_escdelay(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_WHITE, -1);
        init_pair(2, COLOR_BLACK, COLOR_WHITE);
        init_pair(3, COLOR_CYAN, -1);
        init_pair(4, COLOR_YELLOW, -1);
        init_pair(5, COLOR_GREEN, -1);
        init_pair(6, COLOR_WHITE, -1);
        init_pair(7, COLOR_BLACK, COLOR_WHITE);
        init_pair(8, COLOR_CYAN, -1);
        init_pair(9, COLOR_WHITE, -1);
        init_pair(10, COLOR_WHITE, -1);
    }

    rebuild_windows(tui);
    if (!tui->files_win || !tui->shell_win) {
        endwin();
        free(tui);
        return NULL;
    }

    tui->active_buffer = BUFFER_FILES;
    tui->running = 1;

    char resolved[PATH_MAX];
    const char *dir = start_dir;
    if (!dir || dir[0] == '\0') {
        dir = getenv("HOME");
        if (!dir || dir[0] == '\0') {
            struct passwd *pw = getpwuid(getuid());
            dir = (pw && pw->pw_dir) ? pw->pw_dir : ".";
        }
    }
    if (realpath(dir, resolved)) dir = resolved;
    files_load_directory(&tui->files, dir);

    tui->shell.history_index = -1;

    signal(SIGWINCH, handle_resize);
    return tui;
}

void tui_cleanup(TUI *tui) {
    if (!tui) return;
    if (tui->files_win) delwin(tui->files_win);
    if (tui->shell_win) delwin(tui->shell_win);
    endwin();
    free(tui);
}

void tui_resize_handler(TUI *tui) {
    if (!needs_resize) return;
    needs_resize = 0;
    endwin();
    refresh();
    rebuild_windows(tui);
    clearok(stdscr, TRUE);
    if (tui->files_win) { clearok(tui->files_win, TRUE); touchwin(tui->files_win); }
    if (tui->shell_win) { clearok(tui->shell_win, TRUE); touchwin(tui->shell_win); }
}

void tui_render(TUI *tui) {
    if (!tui) return;

    if (tui->active_buffer != BUFFER_SHELL && !tui->rename_mode && !tui->quickshell_mode)
        curs_set(0);

    erase();

    move(tui->divider_y, 0);
    for (int i = 0; i < tui->max_x; i++) addch(ACS_HLINE);

    move(tui->max_y - 1, 0);
    clrtoeol();
    wnoutrefresh(stdscr);

    files_render(&tui->files, tui->files_win,
                 tui->files_height, tui->max_x,
                 tui->active_buffer == BUFFER_FILES);

    if (tui->goto_mode && tui->files_win) {
        char label[GOTO_BUF_MAX + 16];
        snprintf(label, sizeof(label), " GOTO: %s ", tui->goto_buf);
        int llen = (int)strlen(label);
        int lx = (tui->max_x - llen) / 2;
        if (lx < 0) lx = 0;
        wattron(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
        mvwprintw(tui->files_win, 0, lx, "%s", label);
        wattroff(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
        wnoutrefresh(tui->files_win);
    }

    shell_render(&tui->shell, tui->shell_win,
                 tui->shell_height,
                 tui->active_buffer == BUFFER_SHELL,
                 tui->files.cwd);

    if (tui->rename_mode && tui->files_win && tui->files.entry_count > 0) {
        const char *prefix = " RENAME: ";
        char label[RENAME_BUF_MAX + 16];
        snprintf(label, sizeof(label), " RENAME: %s ", tui->rename_buf);
        int llen = (int)strlen(label);
        int lx = (tui->max_x - llen) / 2;
        if (lx < 0) lx = 0;
        int px = lx;
        wattron(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
        mvwprintw(tui->files_win, 0, px, "%s", prefix);
        wattroff(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
        int name_x = px + (int)strlen(prefix);
        int avail = tui->max_x - name_x - 1;
        if (avail > 0)
            mvwprintw(tui->files_win, 0, name_x, "%-*.*s", avail, avail, tui->rename_buf);
        wmove(tui->files_win, 0, name_x + tui->rename_cursor);
        leaveok(tui->files_win, FALSE);
        curs_set(2);
        wnoutrefresh(tui->files_win);
    }

    if (tui->quickshell_mode && tui->files_win) {
        int qrow = tui->files_height - 1;
        const char *prompt = ": ";
        int prompt_len = (int)strlen(prompt);
        int avail = tui->max_x - prompt_len - 1;
        wattron(tui->files_win, A_BOLD);
        mvwprintw(tui->files_win, qrow, 0, "%s", prompt);
        wattroff(tui->files_win, A_BOLD);
        if (avail > 0)
            mvwprintw(tui->files_win, qrow, prompt_len,
                      "%-*.*s", avail, avail, tui->quickshell_buf);

        /* ghost autocomplete */
        char ghost[MAX_FILENAME];
        compute_ghost(tui->quickshell_buf, tui->quickshell_cursor,
                      tui->files.cwd, ghost, sizeof(ghost));
        if (ghost[0] != '\0') {
            int gx = prompt_len + tui->quickshell_cursor;
            wmove(tui->files_win, qrow, gx);
            wclrtoeol(tui->files_win);
            wattron(tui->files_win, A_DIM);
            waddstr(tui->files_win, ghost);
            wattroff(tui->files_win, A_DIM);
        }
        wmove(tui->files_win, qrow, prompt_len + tui->quickshell_cursor);
        leaveok(tui->files_win, FALSE);
        curs_set(2);
        wnoutrefresh(tui->files_win);
    }

    doupdate();
}

// goto mode
static void goto_begin(TUI *tui) {
    tui->goto_mode = 1;
    tui->goto_len = 0;
    tui->goto_buf[0] = '\0';
}

static void goto_cancel(TUI *tui) {
    tui->goto_mode = 0;
    tui->goto_len = 0;
    tui->goto_buf[0] = '\0';
}

static void goto_confirm(TUI *tui) {
    if (tui->goto_len == 0) return;

    char target[PATH_MAX];
    if (tui->goto_buf[0] == '/') {
        strncpy(target, tui->goto_buf, PATH_MAX - 1);
        target[PATH_MAX - 1] = '\0';
    } else if (strcmp(tui->files.cwd, "/") == 0) {
        snprintf(target, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), tui->goto_buf);
    } else {
        path_join(target, PATH_MAX, tui->files.cwd, tui->goto_buf);
    }

    struct stat st;
    if (stat(target, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            files_load_directory(&tui->files, target);
        else
            shell_open_file(target, tui->files_win, tui->shell_win);
        return;
    }

    char dir_buf[PATH_MAX];
    strncpy(dir_buf, target, PATH_MAX - 1);
    dir_buf[PATH_MAX - 1] = '\0';

    char *last_slash = strrchr(dir_buf, '/');
    const char *search_name;
    char search_dir[PATH_MAX];

    if (last_slash && last_slash != dir_buf) {
        *last_slash = '\0';
        strncpy(search_dir, dir_buf, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
        search_name = last_slash + 1;
    } else {
        strncpy(search_dir, tui->files.cwd, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
        search_name = tui->goto_buf;
    }

    if (!strchr(tui->goto_buf, '/')) {
        char *endp;
        long idx = strtol(search_name, &endp, 10);
        if (*endp == '\0' && idx > 0) {
            for (int i = 0; i < tui->files.entry_count; i++) {
                if (tui->files.entries[i].index != (int)idx) continue;
                DirEntry *e = &tui->files.entries[i];
                if (e->type == ENTRY_DIR) {
                    files_change_dir(&tui->files, e->name);
                } else {
                    tui->files.selected = i;
                    char path[PATH_MAX];
                    files_get_selected_path(&tui->files, path, sizeof(path));
                    shell_open_file(path, tui->files_win, tui->shell_win);
                }
                return;
            }
            return;
        }
    }

    DIR *d = opendir(search_dir);
    if (!d) return;

    char exact[MAX_FILENAME] = {0};
    char prefix_m[MAX_FILENAME] = {0};
    int slen = (int)strlen(search_name);
    struct dirent *de;

    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (strcasecmp(de->d_name, search_name) == 0 && !exact[0])
            strncpy(exact, de->d_name, MAX_FILENAME - 1);
        else if (slen > 0 &&
                 strncasecmp(de->d_name, search_name, slen) == 0 &&
                 !prefix_m[0])
            strncpy(prefix_m, de->d_name, MAX_FILENAME - 1);
    }
    closedir(d);

    const char *match = exact[0] ? exact : prefix_m;
    if (!match[0]) return;

    char resolved[PATH_MAX];
    const char *use = realpath(search_dir, resolved) ? resolved : search_dir;
    if (strcmp(use, tui->files.cwd) != 0)
        files_load_directory(&tui->files, use);

    for (int i = 0; i < tui->files.entry_count; i++) {
        if (strcasecmp(tui->files.entries[i].name, match) != 0) continue;
        DirEntry *e = &tui->files.entries[i];
        if (e->type == ENTRY_DIR) {
            files_change_dir(&tui->files, e->name);
        } else {
            tui->files.selected = i;
            char path[PATH_MAX];
            files_get_selected_path(&tui->files, path, sizeof(path));
            shell_open_file(path, tui->files_win, tui->shell_win);
        }
        return;
    }
}

static void goto_tab_complete(TUI *tui) {
    char dir_part[PATH_MAX];
    char name_part[MAX_FILENAME];

    const char *last_slash = strrchr(tui->goto_buf, '/');
    if (last_slash) {
        int dlen = (int)(last_slash - tui->goto_buf);
        strncpy(dir_part, tui->goto_buf, dlen);
        dir_part[dlen] = '\0';
        strncpy(name_part, last_slash + 1, MAX_FILENAME - 1);
        name_part[MAX_FILENAME - 1] = '\0';
    } else {
        dir_part[0] = '\0';
        strncpy(name_part, tui->goto_buf, MAX_FILENAME - 1);
        name_part[MAX_FILENAME - 1] = '\0';
    }

    char search_dir[PATH_MAX];
    if (dir_part[0] == '\0') {
        strncpy(search_dir, tui->files.cwd, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
    } else if (dir_part[0] == '/') {
        strncpy(search_dir, dir_part, PATH_MAX - 1);
        search_dir[PATH_MAX - 1] = '\0';
    } else if (strcmp(tui->files.cwd, "/") == 0) {
        snprintf(search_dir, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), dir_part);
    } else {
        path_join(search_dir, PATH_MAX, tui->files.cwd, dir_part);
    }

    char completed[MAX_FILENAME];
    int count = complete_in_dir(search_dir, name_part, completed, sizeof(completed));
    if (count == 0 || completed[0] == '\0') return;

    if (dir_part[0] == '\0') {
        strncpy(tui->goto_buf, completed, GOTO_BUF_MAX - 1);
        tui->goto_buf[GOTO_BUF_MAX - 1] = '\0';
    } else {
        path_join(tui->goto_buf, GOTO_BUF_MAX, dir_part, completed);
    }
    tui->goto_len = (int)strlen(tui->goto_buf);
}

// rename mode
static void rename_begin(TUI *tui) {
    if (tui->files.entry_count == 0) return;
    DirEntry *e = &tui->files.entries[tui->files.selected];
    tui->rename_mode = 1;
    strncpy(tui->rename_buf, e->name, MAX_FILENAME - 1);
    tui->rename_buf[MAX_FILENAME - 1] = '\0';
    tui->rename_len = (int)strlen(tui->rename_buf);
    tui->rename_cursor = tui->rename_len;
}

static void rename_commit(TUI *tui) {
    if (tui->rename_len == 0 || tui->files.entry_count == 0) return;
    DirEntry *e = &tui->files.entries[tui->files.selected];
    if (strcmp(e->name, tui->rename_buf) == 0) return;

    char old_path[PATH_MAX], new_path[PATH_MAX];
    if (strcmp(tui->files.cwd, "/") == 0) {
        snprintf(old_path, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), e->name);
        snprintf(new_path, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), tui->rename_buf);
    } else {
        path_join(old_path, PATH_MAX, tui->files.cwd, e->name);
        path_join(new_path, PATH_MAX, tui->files.cwd, tui->rename_buf);
    }
    rename(old_path, new_path);
    files_load_directory(&tui->files, tui->files.cwd);
}

static void rename_cancel(TUI *tui) {
    tui->rename_mode = 0;
    tui->rename_len = 0;
    tui->rename_cursor = 0;
    tui->rename_buf[0] = '\0';
    curs_set(0);
}

// input
static void handle_rename_input(TUI *tui, int ch) {
    switch (ch) {
    case 27:
        rename_cancel(tui);
        break;
    case '\n': case '\r': case KEY_ENTER:
        rename_commit(tui);
        rename_cancel(tui);
        break;
    case KEY_LEFT:
        if (tui->rename_cursor > 0) tui->rename_cursor--;
        break;
    case KEY_RIGHT:
        if (tui->rename_cursor < tui->rename_len) tui->rename_cursor++;
        break;
    case KEY_HOME:
        tui->rename_cursor = 0;
        break;
    case KEY_END:
        tui->rename_cursor = tui->rename_len;
        break;
    case '\x7f': case KEY_BACKSPACE:
        if (tui->rename_cursor > 0) {
            memmove(tui->rename_buf + tui->rename_cursor - 1,
                    tui->rename_buf + tui->rename_cursor,
                    (size_t)(tui->rename_len - tui->rename_cursor + 1));
            tui->rename_cursor--;
            tui->rename_len--;
        }
        break;
    case KEY_DC:
        if (tui->rename_cursor < tui->rename_len) {
            memmove(tui->rename_buf + tui->rename_cursor,
                    tui->rename_buf + tui->rename_cursor + 1,
                    (size_t)(tui->rename_len - tui->rename_cursor));
            tui->rename_len--;
        }
        break;
    default:
        if (ch >= 32 && ch < 127 && tui->rename_len < MAX_FILENAME - 1) {
            memmove(tui->rename_buf + tui->rename_cursor + 1,
                    tui->rename_buf + tui->rename_cursor,
                    (size_t)(tui->rename_len - tui->rename_cursor + 1));
            tui->rename_buf[tui->rename_cursor] = (char)ch;
            tui->rename_cursor++;
            tui->rename_len++;
        }
        break;
    }
}

static void handle_goto_input(TUI *tui, int ch) {
    switch (ch) {
    case 27:
        goto_cancel(tui);
        break;
    case '\t':
        goto_tab_complete(tui);
        break;
    case '\n': case '\r': case KEY_ENTER:
        goto_confirm(tui);
        goto_cancel(tui);
        break;
    case '\x7f': case KEY_BACKSPACE:
        if (tui->goto_len > 0)
            tui->goto_buf[--tui->goto_len] = '\0';
        break;
    default:
        if (ch >= 32 && ch < 127 && tui->goto_len < GOTO_BUF_MAX - 1) {
            tui->goto_buf[tui->goto_len++] = (char)ch;
            tui->goto_buf[tui->goto_len] = '\0';
        }
        break;
    }
}

// quickshell mode
static void quickshell_begin(TUI *tui) {
    tui->quickshell_mode = 1;
    tui->quickshell_len = 0;
    tui->quickshell_cursor = 0;
    tui->quickshell_buf[0] = '\0';
}

static void quickshell_cancel(TUI *tui) {
    tui->quickshell_mode = 0;
    tui->quickshell_len = 0;
    tui->quickshell_cursor = 0;
    tui->quickshell_buf[0] = '\0';
    curs_set(0);
}

static void quickshell_execute(TUI *tui) {
    if (tui->quickshell_len == 0) { quickshell_cancel(tui); return; }
    strncpy(tui->shell.current_input, tui->quickshell_buf, SHELL_MAX_INPUT - 1);
    tui->shell.current_input[SHELL_MAX_INPUT - 1] = '\0';
    tui->shell.input_pos = tui->quickshell_len;
    quickshell_cancel(tui);
    execute_given(&tui->shell, &tui->files, tui->files_win, tui->shell_win);
    if (tui->shell.quit_requested) { tui->running = 0; return; }
    files_load_directory(&tui->files, tui->files.cwd);
}

static void handle_quickshell_input(TUI *tui, int ch) {
    switch (ch) {
    case 27:
        quickshell_cancel(tui);
        break;
    case '\n': case '\r': case KEY_ENTER:
        quickshell_execute(tui);
        break;
    case KEY_LEFT:
        if (tui->quickshell_cursor > 0) tui->quickshell_cursor--;
        break;
    case KEY_RIGHT:
        if (tui->quickshell_cursor < tui->quickshell_len) tui->quickshell_cursor++;
        break;
    case KEY_HOME:
        tui->quickshell_cursor = 0;
        break;
    case KEY_END:
        tui->quickshell_cursor = tui->quickshell_len;
        break;
    case '\x7f': case KEY_BACKSPACE:
        if (tui->quickshell_cursor > 0) {
            memmove(tui->quickshell_buf + tui->quickshell_cursor - 1,
                    tui->quickshell_buf + tui->quickshell_cursor,
                    (size_t)(tui->quickshell_len - tui->quickshell_cursor + 1));
            tui->quickshell_cursor--;
            tui->quickshell_len--;
        }
        break;
    case KEY_DC:
        if (tui->quickshell_cursor < tui->quickshell_len) {
            memmove(tui->quickshell_buf + tui->quickshell_cursor,
                    tui->quickshell_buf + tui->quickshell_cursor + 1,
                    (size_t)(tui->quickshell_len - tui->quickshell_cursor));
            tui->quickshell_len--;
        }
        break;
    default:
        if (ch >= 32 && ch < 127 && tui->quickshell_len < SHELL_MAX_INPUT - 1) {
            memmove(tui->quickshell_buf + tui->quickshell_cursor + 1,
                    tui->quickshell_buf + tui->quickshell_cursor,
                    (size_t)(tui->quickshell_len - tui->quickshell_cursor + 1));
            tui->quickshell_buf[tui->quickshell_cursor] = (char)ch;
            tui->quickshell_cursor++;
            tui->quickshell_len++;
        }
        break;
    }
}


void tui_handle_files_input(TUI *tui, int ch) {
    if (tui->rename_mode)     { handle_rename_input(tui, ch);     return; }
    if (tui->goto_mode)       { handle_goto_input(tui, ch);       return; }
    if (tui->quickshell_mode) { handle_quickshell_input(tui, ch); return; }

    switch (ch) {
    case 'j': case KEY_DOWN:
        files_select_next(&tui->files);
        break;

    case 'k': case KEY_UP:
        files_select_prev(&tui->files);
        break;

    case 4:
        files_select_next_n(&tui->files, 5);
        break;

    case 21:
        files_select_prev_n(&tui->files, 5);
        break;

    case 'h': case KEY_LEFT:
        files_change_dir(&tui->files, "..");
        break;

    case 'l': case KEY_RIGHT:
    case '\n': case '\r': case KEY_ENTER: {
        if (tui->files.entry_count == 0) break;
        DirEntry *e = &tui->files.entries[tui->files.selected];
        if (e->type == ENTRY_DIR) {
            files_change_dir(&tui->files, e->name);
        } else {
            char path[PATH_MAX];
            files_get_selected_path(&tui->files, path, sizeof(path));
            shell_open_file(path, tui->files_win, tui->shell_win);
        }
        break;
    }

    case 'g':
        goto_begin(tui);
        break;

    case 'r':
        rename_begin(tui);
        break;

    case ':':
        quickshell_begin(tui);
        break;
    }
}

void tui_handle_shell_input(TUI *tui, int ch) {
    if (ch == '\n' || ch == '\r') {
        execute_given(&tui->shell, &tui->files, tui->files_win, tui->shell_win);
        if (tui->shell.quit_requested) { tui->running = 0; return; }
        files_load_directory(&tui->files, tui->files.cwd);
        return;
    }
    if (ch == '\t') {
        shell_tab_complete(&tui->shell, tui->files.cwd);
        return;
    }
    shell_handle_char(&tui->shell, ch);
}

void tui_handle_input(TUI *tui, int ch) {
    if (!tui->goto_mode && !tui->rename_mode && !tui->quickshell_mode) {
        if (ch == 'J') { tui->active_buffer = BUFFER_SHELL; return; }
        if (ch == 'K') { tui->active_buffer = BUFFER_FILES; curs_set(0); return; }
    }

    if (tui->active_buffer == BUFFER_SHELL)
        tui_handle_shell_input(tui, ch);
    else
        tui_handle_files_input(tui, ch);
}
