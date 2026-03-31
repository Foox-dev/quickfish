#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "file.h"
#include "main.h"

#define INDEX_W 4
#define NAME_W 22
#define COL_W (INDEX_W + NAME_W)
#define COL_PAD 2

static int dir_is_empty(const char *path) {
    int found = 0;
    struct dirent *de;
    DIR *d = opendir(path);

    if (!d) return 1;

    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        found = 1;
        break;
    }

    closedir(d);
    return !found;
}

int complete_in_dir(const char *dir, const char *prefix, char *out, int out_size) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int plen = (int)strlen(prefix);
    int found = 0;
    out[0] = '\0';

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (plen > 0 && strncasecmp(de->d_name, prefix, plen) != 0) continue;
        if (found == 0) {
            strncpy(out, de->d_name, out_size - 1);
            out[out_size - 1] = '\0';
        } else {
            int i = 0;
            while (out[i] && de->d_name[i] &&
                   tolower((unsigned char)out[i]) == tolower((unsigned char)de->d_name[i]))
                i++;
            out[i] = '\0';
        }
        found++;
    }

    closedir(d);
    return found;
}

void files_load_directory(FilesBuffer *files, const char *path) {
    char resolved[PATH_MAX];
    const char *use = realpath(path, resolved) ? resolved : path;

    strncpy(files->cwd, use, PATH_MAX - 1);
    files->cwd[PATH_MAX - 1] = '\0';

    DIR *dir = opendir(files->cwd);
    if (!dir) {
        files->entry_count = 0;
        files->selected = 0;
        files->col_offset = 0;
        return;
    }

    files->entry_count = 0;
    files->selected = 0;
    files->col_offset = 0;

    struct dirent *de;
    while ((de = readdir(dir)) && files->entry_count < MAX_FILES) {
        if (de->d_name[0] == '.') continue;

        char full[PATH_MAX];
        path_join(full, sizeof(full), files->cwd, de->d_name);

        struct stat st;
        if (stat(full, &st) == -1) continue;

        DirEntry *e = &files->entries[files->entry_count];
        strncpy(e->name, de->d_name, MAX_FILENAME - 1);
        e->name[MAX_FILENAME - 1] = '\0';
        e->type = S_ISDIR(st.st_mode) ? ENTRY_DIR : ENTRY_FILE;
        e->is_empty = (e->type == ENTRY_DIR) ? dir_is_empty(full) : 0;
        e->index = files->entry_count + 1;
        files->entry_count++;
    }
    closedir(dir);
    files_sort_entries(files);
}

void files_sort_entries(FilesBuffer *files) {
    for (int i = 0; i < files->entry_count - 1; i++) {
        for (int j = i + 1; j < files->entry_count; j++) {
            DirEntry *a = &files->entries[i];
            DirEntry *b = &files->entries[j];
            int swap = (a->type != b->type) ? (a->type == ENTRY_FILE) : (strcasecmp(a->name, b->name) > 0);
            if (swap) { DirEntry t = *a; *a = *b; *b = t; }
        }
    }

    for (int i = 0; i < files->entry_count; i++)
        files->entries[i].index = i + 1;
}

void files_select_next(FilesBuffer *files) {
    if (files->selected < files->entry_count - 1) files->selected++;
}

void files_select_next_n(FilesBuffer *files, int n) {
    files->selected += n;
    if (files->selected >= files->entry_count) files->selected = files->entry_count - 1;
}

void files_select_prev(FilesBuffer *files) {
    if (files->selected > 0) files->selected--;
}

void files_select_prev_n(FilesBuffer *files, int n) {
    files->selected -= n;
    if (files->selected < 0) files->selected = 0;
}

void files_change_dir(FilesBuffer *files, const char *dirname) {
    char new_path[PATH_MAX];

    if (strcmp(dirname, "..") == 0) {
        strncpy(new_path, files->cwd, PATH_MAX - 1);
        char *slash = strrchr(new_path, '/');
        if (slash && slash != new_path) {
            *slash = '\0';
        } else strcpy(new_path, "/");
    } else if (strcmp(files->cwd, "/") == 0) {
        snprintf(new_path, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), dirname);
    } else {
        path_join(new_path, PATH_MAX, files->cwd, dirname);
    }

    files_load_directory(files, new_path);
}

int files_get_selected_path(FilesBuffer *files, char *out_path, size_t out_size) {
    if (files->selected < 0 || files->selected >= files->entry_count)
        return -1;

    DirEntry *e = &files->entries[files->selected];

    if (strcmp(files->cwd, "/") == 0)
        snprintf(out_path, out_size, "/%s", e->name);
    else
        snprintf(out_path, out_size, "%s/%s", files->cwd, e->name);

    return (e->type == ENTRY_DIR) ? 0 : 1;
}

void files_render(FilesBuffer *files, WINDOW *win, int height, int width, int focused) {
    int rows = height - 1;
    if (rows <= 0 || width <= 0) return;

    werase(win);

    if (focused) {
        wattron(win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
        mvwprintw(win, 0, 0, " FILES ");
        wattroff(win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
    } else {
        wattron(win, COLOR_PAIR(CP_TITLE_UF) | A_BOLD);
        mvwprintw(win, 0, 0, " FILES ");
        wattroff(win, COLOR_PAIR(CP_TITLE_UF) | A_BOLD);
    }

    {
        const char *cwd = files->cwd;
        int avail = width - 9;
        if (avail > 0) {
            int cwdlen = (int)strlen(cwd);
            if (cwdlen > avail) cwd += cwdlen - avail;
            wattron(win, A_BOLD);
            mvwprintw(win, 0, 8, " %.*s", avail, cwd);
            wattroff(win, A_BOLD);
        }
    }

    if (files->entry_count == 0) {
        mvwprintw(win, 2, 2, "(empty directory)");
        wnoutrefresh(win);
        return;
    }

    int cell_w = COL_W + COL_PAD;
    int n_cols = (width / cell_w > 0) ? width / cell_w : 1;
    int total_cols = (files->entry_count + rows - 1) / rows;
    if (total_cols < 1) total_cols = 1;

    int sel_col = files->selected / rows;
    if (sel_col < files->col_offset)
        files->col_offset = sel_col;
    if (sel_col >= files->col_offset + n_cols)
        files->col_offset = sel_col - n_cols + 1;
    if (files->col_offset < 0)
        files->col_offset = 0;

    for (int vc = 0; vc < n_cols; vc++) {
        int ac = files->col_offset + vc;
        if (ac >= total_cols) break;
        int x = vc * cell_w;

        for (int row = 0; row < rows; row++) {
            int idx = ac * rows + row;
            if (idx >= files->entry_count) break;

            DirEntry *e = &files->entries[idx];
            int y   = row + 1;
            int sel = (idx == files->selected);

            wmove(win, y, x);

            if (sel)
                wattron(win, COLOR_PAIR(CP_SELECTED) | A_BOLD);

            if (!sel) wattron(win, COLOR_PAIR(CP_INDEX));
            wprintw(win, "%3d ", e->index);
            if (!sel) wattroff(win, COLOR_PAIR(CP_INDEX));

            if (!sel) {
                if (e->type == ENTRY_DIR) {
                    if (e->is_empty)
                        wattron(win, COLOR_PAIR(CP_DIR_EMPTY) | A_DIM);
                    else
                        wattron(win, COLOR_PAIR(CP_DIR_FULL) | A_BOLD);
                } else {
                    wattron(win, COLOR_PAIR(CP_FILE));
                }
            }

            wprintw(win, "%-*.*s", NAME_W, NAME_W, e->name);

            if (!sel) {
                if (e->type == ENTRY_DIR) {
                    if (e->is_empty)
                        wattroff(win, COLOR_PAIR(CP_DIR_EMPTY) | A_DIM);
                    else
                        wattroff(win, COLOR_PAIR(CP_DIR_FULL) | A_BOLD);
                } else {
                    wattroff(win, COLOR_PAIR(CP_FILE));
                }
            }

            if (sel)
                wattroff(win, COLOR_PAIR(CP_SELECTED) | A_BOLD);
        }
    }

    if (files->col_offset > 0)
        mvwprintw(win, 1, 0, "<");
    if (files->col_offset + n_cols < total_cols)
        mvwprintw(win, 1, width - 1, ">");

    wnoutrefresh(win);
}
