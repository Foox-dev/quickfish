#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>

#include "file.h"
#include "main.h"
#include "shell.h"
#include "tui.h"

#define INDEX_W 4
#define NAME_W 22
#define COL_W (INDEX_W + NAME_W)
#define COL_PAD 2

static int entry_cmp(const void *a, const void *b);
static int dir_size_walk(const char *path, const struct stat *st, int type, struct FTW *ftw);
static off_t compute_dir_size(const char *path);
static int ext_color_normal(const char *name);
static int to_selected_pair(int normal_pair);
static void push_undo(FilesBuffer *files, UndoOp op);
static void do_trash_entry(FilesBuffer *files, struct ShellBuffer *shell, int idx);
static void do_delete_entry(FilesBuffer *files, struct ShellBuffer *shell, int idx);
static void files_move_reindex(FilesBuffer *files);
static void files_move_print_register(FilesBuffer *files, struct ShellBuffer *shell);

int dir_is_empty(const char *path) {
	int found;
	struct dirent *de;
	DIR *d;

	found = 0;
	d = opendir(path);

	if (!d) { return 1; }

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') { continue; }
		found = 1;
		break;
	}

	closedir(d);
	return !found;
}

int complete_in_dir(const char *dir, const char *prefix, char *out, int out_size) {
	DIR *d;
	int plen;
	int found;
	struct dirent *de;

	d = opendir(dir);
	if (!d) { return 0; }

	plen = (int)strlen(prefix);
	found = 0;
	out[0] = '\0';

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') { continue; }
		if (plen > 0 && strncasecmp(de->d_name, prefix, plen) != 0) { continue; }
		if (found == 0) {
			strncpy(out, de->d_name, out_size - 1);
			out[out_size - 1] = '\0';
		} else {
			int i = 0;
			while (out[i] && de->d_name[i] &&
			       tolower((unsigned char)out[i]) == tolower((unsigned char)de->d_name[i])) {
				i++;
			}
			out[i] = '\0';
		}
		found++;
	}

	closedir(d);
	return found;
}

void files_load_directory(FilesBuffer *files, const char *path) {
	char resolved[PATH_MAX];
	const char *use;
	DIR *dir;
	struct dirent *de;
	char full[PATH_MAX];
	struct stat st;
	DirEntry *e;
	int saved_undo_top;
	int saved_redo_top;
	UndoOp saved_undo[UNDO_MAX];
	UndoOp saved_redo[UNDO_MAX];

	saved_undo_top = files->undo_top;
	saved_redo_top = files->redo_top;
	memcpy(saved_undo, files->undo_stack, sizeof(saved_undo));
	memcpy(saved_redo, files->redo_stack, sizeof(saved_redo));

	use = realpath(path, resolved) ? resolved : path;

	strncpy(files->cwd, use, PATH_MAX - 1);
	files->cwd[PATH_MAX - 1] = '\0';

	dir = opendir(files->cwd);
	if (!dir) {
		files->entry_count = 0;
		files->selected = 0;
		files->col_offset = 0;
		files->sel_count = 0;
		memset(files->multi_sel, 0, sizeof(files->multi_sel));
		memcpy(files->undo_stack, saved_undo, sizeof(saved_undo));
		memcpy(files->redo_stack, saved_redo, sizeof(saved_redo));
		files->undo_top = saved_undo_top;
		files->redo_top = saved_redo_top;
		return;
	}

	files->entry_count = 0;
	files->selected = 0;
	files->col_offset = 0;
	files->sel_count = 0;
	memset(files->multi_sel, 0, sizeof(files->multi_sel));

	while ((de = readdir(dir)) && files->entry_count < MAX_FILES) {
		if (de->d_name[0] == '.') { continue; }

		path_join(full, sizeof(full), files->cwd, de->d_name);

		if (stat(full, &st) == -1) { continue; }

		e = &files->entries[files->entry_count];
		strncpy(e->name, de->d_name, MAX_FILENAME - 1);
		e->name[MAX_FILENAME - 1] = '\0';
		e->type = S_ISDIR(st.st_mode) ? ENTRY_DIR : ENTRY_FILE;
		e->mode = st.st_mode;
		e->is_empty = (e->type == ENTRY_DIR) ? dir_is_empty(full) : 0;
		e->index = files->entry_count + 1;
		files->entry_count++;
	}
	closedir(dir);
	files_sort_entries(files);

	memcpy(files->undo_stack, saved_undo, sizeof(saved_undo));
	memcpy(files->redo_stack, saved_redo, sizeof(saved_redo));
	files->undo_top = saved_undo_top;
	files->redo_top = saved_redo_top;
	files_move_reindex(files);
}

static int entry_cmp(const void *a, const void *b) {
	const DirEntry *ea;
	const DirEntry *eb;

	ea = (const DirEntry *)a;
	eb = (const DirEntry *)b;
	if (ea->type != eb->type) {
		return (ea->type == ENTRY_DIR) ? -1 : 1;
	}
	return strcasecmp(ea->name, eb->name);
}

void files_sort_entries(FilesBuffer *files) {
	qsort(files->entries, (size_t)files->entry_count, sizeof(DirEntry), entry_cmp);
	for (int i = 0; i < files->entry_count; i++) {
		files->entries[i].index = i + 1;
	}
}

void files_select_next(FilesBuffer *files) {
	if (files->selected < files->entry_count - 1) { files->selected++; }
}

void files_select_next_n(FilesBuffer *files, int n) {
	files->selected += n;
	if (files->selected >= files->entry_count) { files->selected = files->entry_count - 1; }
}

void files_select_prev(FilesBuffer *files) {
	if (files->selected > 0) { files->selected--; }
}

void files_select_prev_n(FilesBuffer *files, int n) {
	files->selected -= n;
	if (files->selected < 0) { files->selected = 0; }
}

void files_change_dir(FilesBuffer *files, const char *dirname) {
	char new_path[PATH_MAX];
	char return_to[MAX_FILENAME];
	char *slash;

	return_to[0] = '\0';

	if (strcmp(dirname, "..") == 0) {
		strncpy(new_path, files->cwd, PATH_MAX - 1);
		new_path[PATH_MAX - 1] = '\0';
		slash = strrchr(new_path, '/');
		if (slash && slash != new_path) {
			strncpy(return_to, slash + 1, MAX_FILENAME - 1);
			*slash = '\0';
		} else {
			strncpy(return_to, new_path + 1, MAX_FILENAME - 1);
			strcpy(new_path, "/");
		}
	} else if (strcmp(files->cwd, "/") == 0) {
		snprintf(new_path, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), dirname);
	} else {
		path_join(new_path, PATH_MAX, files->cwd, dirname);
	}

	files_load_directory(files, new_path);

	if (return_to[0] != '\0') {
		for (int i = 0; i < files->entry_count; i++) {
			if (strcmp(files->entries[i].name, return_to) == 0) {
				files->selected = i;
				break;
			}
		}
	}
}

int files_get_selected_path(FilesBuffer *files, char *out_path, size_t out_size) {
	DirEntry *e;

	if (files->selected < 0 || files->selected >= files->entry_count) { return -1; }

	e = &files->entries[files->selected];

	if (strcmp(files->cwd, "/") == 0) {
		snprintf(out_path, out_size, "/%s", e->name);
	} else {
		snprintf(out_path, out_size, "%s/%s", files->cwd, e->name);
	}

	return (e->type == ENTRY_DIR) ? 0 : 1;
}

static off_t dir_size_total;

static int dir_size_walk(const char *path, const struct stat *st, int type, struct FTW *ftw) {
	(void)path; (void)type; (void)ftw;
	dir_size_total += st->st_blocks * 512;
	return 0;
}

static off_t compute_dir_size(const char *path) {
	dir_size_total = 0;
	nftw(path, dir_size_walk, 64, FTW_PHYS);
	return dir_size_total;
}

void refresh_files_buffer(FilesBuffer *files) {
	int saved_sel;
	int saved_offset;

	saved_sel = files->selected;
	saved_offset = files->col_offset;
	files_load_directory(files, files->cwd);
	files->selected = (saved_sel < files->entry_count) ? saved_sel : 0;
	files->col_offset = saved_offset;
}

static int ext_color_normal(const char *name) {
	const char *base;
	const char *dot;
	const char *ext;

	base = strrchr(name, '/');
	base = base ? base + 1 : name;

	if (!strcasecmp(base, "Makefile") || !strcasecmp(base, "makefile") ||
	    !strcasecmp(base, "GNUmakefile") || !strcasecmp(base, "Rakefile") ||
	    !strcasecmp(base, "Justfile") || !strcasecmp(base, "justfile") ||
	    !strcasecmp(base, "Dockerfile") || !strcasecmp(base, "dockerfile") ||
	    !strcasecmp(base, "Containerfile") ||
	    !strcasecmp(base, "CMakeLists.txt") ||
	    !strcasecmp(base, "meson.build") || !strcasecmp(base, "BUILD") ||
	    !strcasecmp(base, "BUILD.bazel") || !strcasecmp(base, "WORKSPACE") ||
	    !strcasecmp(base, "Vagrantfile") || !strcasecmp(base, "Guardfile") ||
	    !strcasecmp(base, "Gemfile") || !strcasecmp(base, "Podfile")) {
		return CP_EXT_CODE;
	}

	if (!strcasecmp(base, "README") || !strcasecmp(base, "CHANGELOG") ||
	    !strcasecmp(base, "CHANGES") || !strcasecmp(base, "AUTHORS") ||
	    !strcasecmp(base, "CONTRIBUTORS") || !strcasecmp(base, "COPYING") ||
	    !strcasecmp(base, "LICENSE") || !strcasecmp(base, "LICENCE") ||
	    !strcasecmp(base, "NOTICE") || !strcasecmp(base, "PATENTS") ||
	    !strcasecmp(base, "TODO") || !strcasecmp(base, "HACKING")) {
		return CP_EXT_DOC;
	}

	if (!strcasecmp(base, ".gitignore") || !strcasecmp(base, ".gitattributes") ||
	    !strcasecmp(base, ".gitmodules") || !strcasecmp(base, ".editorconfig") ||
	    !strcasecmp(base, ".env") || !strcasecmp(base, ".envrc") ||
	    !strcasecmp(base, ".npmrc") || !strcasecmp(base, ".yarnrc") ||
	    !strcasecmp(base, ".babelrc") || !strcasecmp(base, ".eslintrc") ||
	    !strcasecmp(base, ".prettierrc") || !strcasecmp(base, ".stylelintrc") ||
	    !strcasecmp(base, ".dockerignore") || !strcasecmp(base, ".htaccess") ||
	    !strcasecmp(base, "Pipfile") || !strcasecmp(base, "Procfile") ||
	    !strcasecmp(base, "requirements.txt") || !strcasecmp(base, "go.sum") ||
	    !strcasecmp(base, "go.mod") || !strcasecmp(base, "Cargo.lock") ||
	    !strcasecmp(base, "package-lock.json") || !strcasecmp(base, "yarn.lock") ||
	    !strcasecmp(base, "composer.lock") || !strcasecmp(base, "Gemfile.lock")) {
		return CP_EXT_DATA;
	}

	dot = strrchr(base, '.');
	if (!dot || dot == base) { return CP_FILE; }
	ext = dot + 1;

	if (!strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg") ||
	    !strcasecmp(ext, "gif") || !strcasecmp(ext, "bmp") || !strcasecmp(ext, "tiff") ||
	    !strcasecmp(ext, "tif") || !strcasecmp(ext, "webp") || !strcasecmp(ext, "svg") ||
	    !strcasecmp(ext, "ico") || !strcasecmp(ext, "heic") || !strcasecmp(ext, "avif")) {
		return CP_EXT_IMAGE;
	}

	if (!strcasecmp(ext, "mp4") || !strcasecmp(ext, "mkv") || !strcasecmp(ext, "avi") ||
	    !strcasecmp(ext, "mov") || !strcasecmp(ext, "wmv") || !strcasecmp(ext, "flv") ||
	    !strcasecmp(ext, "webm") || !strcasecmp(ext, "m4v") || !strcasecmp(ext, "ts") ||
	    !strcasecmp(ext, "mpg") || !strcasecmp(ext, "mpeg")) {
		return CP_EXT_VIDEO;
	}

	if (!strcasecmp(ext, "mp3") || !strcasecmp(ext, "flac") || !strcasecmp(ext, "ogg") ||
	    !strcasecmp(ext, "wav") || !strcasecmp(ext, "aac") || !strcasecmp(ext, "m4a") ||
	    !strcasecmp(ext, "opus") || !strcasecmp(ext, "wma") || !strcasecmp(ext, "aiff")) {
		return CP_EXT_AUDIO;
	}

	if (!strcasecmp(ext, "zip") || !strcasecmp(ext, "tar") || !strcasecmp(ext, "gz") ||
	    !strcasecmp(ext, "bz2") || !strcasecmp(ext, "xz") || !strcasecmp(ext, "zst") ||
	    !strcasecmp(ext, "7z") || !strcasecmp(ext, "rar") || !strcasecmp(ext, "deb") ||
	    !strcasecmp(ext, "rpm") || !strcasecmp(ext, "pkg") || !strcasecmp(ext, "dmg") ||
	    !strcasecmp(ext, "iso") || !strcasecmp(ext, "tgz") || !strcasecmp(ext, "tbz2")) {
		return CP_EXT_ARCHIVE;
	}

	if (!strcasecmp(ext, "c") || !strcasecmp(ext, "h") || !strcasecmp(ext, "cc") ||
	    !strcasecmp(ext, "cpp") || !strcasecmp(ext, "cxx") || !strcasecmp(ext, "hh") ||
	    !strcasecmp(ext, "hpp") || !strcasecmp(ext, "rs") || !strcasecmp(ext, "go") ||
	    !strcasecmp(ext, "py") || !strcasecmp(ext, "rb") || !strcasecmp(ext, "java") ||
	    !strcasecmp(ext, "js") || !strcasecmp(ext, "ts") || !strcasecmp(ext, "jsx") ||
	    !strcasecmp(ext, "tsx") || !strcasecmp(ext, "lua") || !strcasecmp(ext, "zig") ||
	    !strcasecmp(ext, "swift") || !strcasecmp(ext, "kt") || !strcasecmp(ext, "cs") ||
	    !strcasecmp(ext, "php") || !strcasecmp(ext, "pl") || !strcasecmp(ext, "hs") ||
	    !strcasecmp(ext, "el") || !strcasecmp(ext, "vim") || !strcasecmp(ext, "S") ||
	    !strcasecmp(ext, "asm") || !strcasecmp(ext, "ex") || !strcasecmp(ext, "exs") ||
	    !strcasecmp(ext, "clj") || !strcasecmp(ext, "scala") || !strcasecmp(ext, "ml")) {
		return CP_EXT_CODE;
	}

	if (!strcasecmp(ext, "sh") || !strcasecmp(ext, "bash") || !strcasecmp(ext, "zsh") ||
	    !strcasecmp(ext, "fish") || !strcasecmp(ext, "ksh") || !strcasecmp(ext, "ps1") ||
	    !strcasecmp(ext, "bat") || !strcasecmp(ext, "cmd") || !strcasecmp(ext, "out") ||
	    !strcasecmp(ext, "run") || !strcasecmp(ext, "AppImage")) {
		return CP_EXT_EXEC;
	}

	if (!strcasecmp(ext, "pdf") || !strcasecmp(ext, "doc") || !strcasecmp(ext, "docx") ||
	    !strcasecmp(ext, "odt") || !strcasecmp(ext, "rtf") || !strcasecmp(ext, "tex") ||
	    !strcasecmp(ext, "md") || !strcasecmp(ext, "txt") || !strcasecmp(ext, "rst") ||
	    !strcasecmp(ext, "org") || !strcasecmp(ext, "epub") || !strcasecmp(ext, "mobi") ||
	    !strcasecmp(ext, "xls") || !strcasecmp(ext, "xlsx") || !strcasecmp(ext, "ods") ||
	    !strcasecmp(ext, "ppt") || !strcasecmp(ext, "pptx") || !strcasecmp(ext, "odp")) {
		return CP_EXT_DOC;
	}

	if (!strcasecmp(ext, "json") || !strcasecmp(ext, "yaml") || !strcasecmp(ext, "yml") ||
	    !strcasecmp(ext, "toml") || !strcasecmp(ext, "ini") || !strcasecmp(ext, "cfg") ||
	    !strcasecmp(ext, "conf") || !strcasecmp(ext, "xml") || !strcasecmp(ext, "csv") ||
	    !strcasecmp(ext, "tsv") || !strcasecmp(ext, "sql") || !strcasecmp(ext, "db") ||
	    !strcasecmp(ext, "sqlite") || !strcasecmp(ext, "env") || !strcasecmp(ext, "lock") ||
	    !strcasecmp(ext, "log") || !strcasecmp(ext, "pid")) {
		return CP_EXT_DATA;
	}

	return CP_FILE;
}

static int to_selected_pair(int normal_pair) {
	switch (normal_pair) {
	case CP_EXT_IMAGE:   return CP_SEL_IMAGE;
	case CP_EXT_VIDEO:   return CP_SEL_VIDEO;
	case CP_EXT_AUDIO:   return CP_SEL_AUDIO;
	case CP_EXT_ARCHIVE: return CP_SEL_ARCHIVE;
	case CP_EXT_CODE:    return CP_SEL_CODE;
	case CP_EXT_DOC:     return CP_SEL_DOC;
	case CP_EXT_EXEC:    return CP_SEL_EXEC;
	case CP_EXT_DATA:    return CP_SEL_DATA;
	default:             return CP_SEL_FILE;
	}
}

int entry_color_pair(const DirEntry *e, int selected) {
	int normal;

	if (e->type == ENTRY_DIR) {
		return selected ? CP_SEL_DIR : (e->is_empty ? CP_DIR_EMPTY : CP_DIR_FULL);
	}
	normal = ext_color_normal(e->name);
	if (normal == CP_FILE && (e->mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
		return selected ? CP_SEL_EXEC : CP_EXT_EXEC;
	}
	return selected ? to_selected_pair(normal) : normal;
}

void files_render(FilesBuffer *files, WINDOW *win, int height, int width, int focused) {
	int rows;
	const char *cwd;
	int avail;
	char count_str[32];
	char sel_str[32];
	int cell_w;
	int n_cols;
	int first;
	int last;
	int visible;
	int cwdlen;
	int cx;
	int sx;
	DirEntry *selected_entry;
	char full[PATH_MAX];
	struct stat st;
	char perms[11];
	char size_str[32];
	off_t sz;
	char info[64];
	int ix;
	int total_cols;
	int sel_col;
	int ac;
	int x;
	int idx;
	DirEntry *e;
	int y;
	int is_sel;
	int name_pair;
	int sel_pair;
	attr_t name_attrs;
	char display[NAME_W + 1];

	rows = height - 1;
	if (rows <= 0 || width <= 0) { return; }

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

	cwd = files->cwd;
	avail = width - 9;
	if (avail > 0) {
		cwdlen = (int)strlen(cwd);
		if (cwdlen > avail) { cwd += cwdlen - avail; }
		wattron(win, A_BOLD);
		mvwprintw(win, 0, 8, " %.*s", avail, cwd);
		wattroff(win, A_BOLD);
	}

	cell_w = COL_W + COL_PAD;
	n_cols = (width / cell_w > 0) ? width / cell_w : 1;
	first = files->col_offset * rows;
	last = (files->col_offset + n_cols) * rows;
	if (last > files->entry_count) { last = files->entry_count; }
	visible = (files->entry_count > 0) ? (last - first) : 0;
	snprintf(count_str, sizeof(count_str), " %d/%d i ", visible, files->entry_count);
	cx = width - (int)strlen(count_str);
	if (files->sel_count > 0 && cx > 0) {
		snprintf(sel_str, sizeof(sel_str), " [%d sel] ", files->sel_count);
		sx = cx - (int)strlen(sel_str);
		if (sx > 0) {
			wattron(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
			mvwprintw(win, 0, sx, "%s", sel_str);
			wattroff(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
		} else {
			/* Doesn't fit beside the item counter — render below it on row 1. */
			int sel_len = (int)strlen(sel_str);
			int sel_x   = width - sel_len;
			if (sel_x < 0) { sel_x = 0; }
			wattron(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
			mvwprintw(win, 1, sel_x, "%s", sel_str);
			wattroff(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
		}
	}
	if (cx > 0) { mvwprintw(win, 0, cx, "%s", count_str); }

	if (files->entry_count > 0) {
		selected_entry = &files->entries[files->selected];
		path_join(full, sizeof(full), files->cwd, selected_entry->name);

		if (stat(full, &st) == 0) {
			perms[0] = S_ISDIR(st.st_mode) ? 'd' : '-';
			perms[1] = (st.st_mode & S_IRUSR) ? 'r' : '-';
			perms[2] = (st.st_mode & S_IWUSR) ? 'w' : '-';
			perms[3] = (st.st_mode & S_IXUSR) ? 'x' : '-';
			perms[4] = (st.st_mode & S_IRGRP) ? 'r' : '-';
			perms[5] = (st.st_mode & S_IWGRP) ? 'w' : '-';
			perms[6] = (st.st_mode & S_IXGRP) ? 'x' : '-';
			perms[7] = (st.st_mode & S_IROTH) ? 'r' : '-';
			perms[8] = (st.st_mode & S_IWOTH) ? 'w' : '-';
			perms[9] = (st.st_mode & S_IXOTH) ? 'x' : '-';
			perms[10] = '\0';

			sz = st.st_size;
			if (S_ISDIR(st.st_mode)) {
				snprintf(size_str, sizeof(size_str), "-");
			} else if (sz >= 1024 * 1024 * 1024) {
				snprintf(size_str, sizeof(size_str), "%.1fG", (double)sz / (1024 * 1024 * 1024));
			} else if (sz >= 1024 * 1024) {
				snprintf(size_str, sizeof(size_str), "%.1fM", (double)sz / (1024 * 1024));
			} else if (sz >= 1024) {
				snprintf(size_str, sizeof(size_str), "%.1fK", (double)sz / 1024);
			} else {
				snprintf(size_str, sizeof(size_str), "%ldB", (long)sz);
			}

			snprintf(info, sizeof(info), " %s %s ", perms, size_str);
			ix = width - (int)strlen(info);
			if (ix > 0) {
				wattron(win, A_DIM);
				mvwprintw(win, rows, ix, "%s", info);
				wattroff(win, A_DIM);
			}
		}
	}

	if (files->entry_count == 0) {
		mvwprintw(win, 2, 2, "(empty directory)");
		wnoutrefresh(win);
		return;
	}

	total_cols = (files->entry_count + rows - 1) / rows;
	if (total_cols < 1) { total_cols = 1; }

	sel_col = files->selected / rows;
	if (sel_col < files->col_offset) { files->col_offset = sel_col; }
	if (sel_col >= files->col_offset + n_cols) { files->col_offset = sel_col - n_cols + 1; }
	if (files->col_offset < 0) { files->col_offset = 0; }

	for (int vc = 0; vc < n_cols; vc++) {
		ac = files->col_offset + vc;
		if (ac >= total_cols) { break; }
		x = vc * cell_w;

		for (int row = 0; row < rows; row++) {
			idx = ac * rows + row;
			if (idx >= files->entry_count) { break; }

			e = &files->entries[idx];
			y = row + 1;
			is_sel = (idx == files->selected);

			wmove(win, y, x + 1);

			name_pair = entry_color_pair(e, 0);
			sel_pair = entry_color_pair(e, 1);

			if (files->in_move_reg[idx]) {
				wattron(win, COLOR_PAIR(CP_EXT_ARCHIVE) | A_BOLD);
				wprintw(win, "mv> ");
				wattroff(win, COLOR_PAIR(CP_EXT_ARCHIVE) | A_BOLD);
			} else if (is_sel) {
				wattron(win, COLOR_PAIR(sel_pair) | A_BOLD);
				wprintw(win, "%3d ", e->index);
				wattroff(win, COLOR_PAIR(sel_pair) | A_BOLD);
			} else if (files->multi_sel[idx]) {
				wattron(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
				wprintw(win, "%3d ", e->index);
				wattroff(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
			} else {
				wattron(win, COLOR_PAIR(CP_INDEX));
				wprintw(win, "%3d ", e->index);
				wattroff(win, COLOR_PAIR(CP_INDEX));
			}

			name_attrs = A_NORMAL;
			if (e->type == ENTRY_DIR) {
				name_attrs = e->is_empty ? A_DIM : A_BOLD;
			} else if (name_pair == CP_EXT_EXEC) {
				name_attrs = A_BOLD;
			}

			if (is_sel) {
				wattron(win, COLOR_PAIR(sel_pair) | A_BOLD);
			} else if (files->multi_sel[idx]) {
				wattron(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
			} else {
				wattron(win, COLOR_PAIR(name_pair) | name_attrs);
			}

			if ((int)strlen(e->name) > NAME_W) {
				strncpy(display, e->name, NAME_W - 2);
				display[NAME_W - 2] = '.';
				display[NAME_W - 1] = '.';
				display[NAME_W] = '\0';
			} else {
				strncpy(display, e->name, NAME_W);
				display[NAME_W] = '\0';
			}
			wprintw(win, "%-*.*s", NAME_W, NAME_W, display);

			if (is_sel) {
				wattroff(win, COLOR_PAIR(sel_pair) | A_BOLD);
			} else if (files->multi_sel[idx]) {
				wattroff(win, COLOR_PAIR(CP_MULTI_SEL) | A_BOLD);
			} else {
				wattroff(win, COLOR_PAIR(name_pair) | name_attrs);
			}
		}
	}

	if (files->col_offset > 0) { mvwprintw(win, 1, 0, "<"); }
	if (files->col_offset + n_cols < total_cols) { mvwprintw(win, 1, width - 1, ">"); }

	wnoutrefresh(win);
}

void files_cmd_stat(FilesBuffer *files, struct ShellBuffer *shell, const char *arg) {
	char full[PATH_MAX];
	DirEntry *e;
	DirEntry synthetic;
	const char *base;
	char *end;
	long idx;
	struct stat st;
	char perms[11];
	off_t sz;
	char size_str[32];
	char time_str[32];
	struct tm *tm;
	char out[512];
	char msg[MAX_FILENAME + 32];

	if (!arg || arg[0] == '\0') {
		print_to_shell(shell, "stat: no file specified\n", 3);
		return;
	}

	e = NULL;
	memset(&synthetic, 0, sizeof(synthetic));

	if (arg[0] == '~') {
		const char *home = getenv("HOME");
		if (!home) { home = "/"; }
		if (arg[1] == '/' || arg[1] == '\0') {
			snprintf(full, sizeof full, "%s%s", home, arg + 1);
		} else {
			snprintf(full, sizeof full, "%s", arg);
		}
		base = strrchr(full, '/');
		strncpy(synthetic.name, base ? base + 1 : full, MAX_FILENAME - 1);
		e = &synthetic;
	} else if (strchr(arg, '/')) {
		if (arg[0] == '/') {
			snprintf(full, sizeof full, "%s", arg);
		} else {
			path_join(full, sizeof full, files->cwd, arg);
		}
		base = strrchr(full, '/');
		strncpy(synthetic.name, base ? base + 1 : full, MAX_FILENAME - 1);
		e = &synthetic;
	} else {
		idx = strtol(arg, &end, 10);
		if (*end == '\0') {
			for (int i = 0; i < files->entry_count; i++) {
				if (files->entries[i].index == (int)idx) { e = &files->entries[i]; break; }
			}
		} else {
			for (int i = 0; i < files->entry_count; i++) {
				if (strcasecmp(files->entries[i].name, arg) == 0) { e = &files->entries[i]; break; }
			}
		}

		if (!e) {
			snprintf(msg, sizeof msg, "stat: '%s' not found in current directory\n", arg);
			print_to_shell(shell, msg, 3);
			return;
		}
		path_join(full, sizeof full, files->cwd, e->name);
	}

	if (stat(full, &st) != 0) {
		snprintf(msg, sizeof msg, "stat: could not stat '%s'\n", e->name);
		print_to_shell(shell, msg, 3);
		return;
	}

	perms[0] = S_ISDIR(st.st_mode) ? 'd' : '-';
	perms[1] = (st.st_mode & S_IRUSR) ? 'r' : '-';
	perms[2] = (st.st_mode & S_IWUSR) ? 'w' : '-';
	perms[3] = (st.st_mode & S_IXUSR) ? 'x' : '-';
	perms[4] = (st.st_mode & S_IRGRP) ? 'r' : '-';
	perms[5] = (st.st_mode & S_IWGRP) ? 'w' : '-';
	perms[6] = (st.st_mode & S_IXGRP) ? 'x' : '-';
	perms[7] = (st.st_mode & S_IROTH) ? 'r' : '-';
	perms[8] = (st.st_mode & S_IWOTH) ? 'w' : '-';
	perms[9] = (st.st_mode & S_IXOTH) ? 'x' : '-';
	perms[10] = '\0';

	sz = S_ISDIR(st.st_mode) ? compute_dir_size(full) : st.st_size;

	if (sz >= 1024 * 1024 * 1024) {
		snprintf(size_str, sizeof size_str, "%.1fG", (double)sz / (1024 * 1024 * 1024));
	} else if (sz >= 1024 * 1024) {
		snprintf(size_str, sizeof size_str, "%.1fM", (double)sz / (1024 * 1024));
	} else if (sz >= 1024) {
		snprintf(size_str, sizeof size_str, "%.1fK", (double)sz / 1024);
	} else {
		snprintf(size_str, sizeof size_str, "%ldB", (long)sz);
	}

	tm = localtime(&st.st_mtime);
	strftime(time_str, sizeof time_str, "%Y-%m-%d %H:%M", tm);

	snprintf(out, sizeof out,
	    "name: %s\n"
	    "type: %s\n"
	    "perms: %s\n"
	    "size: %s\n"
	    "modified: %s\n"
	    "inode: %lu\n"
	    "links: %lu\n",
	    e->name,
	    S_ISDIR(st.st_mode) ? "directory" : "file",
	    perms,
	    size_str,
	    time_str,
	    (unsigned long)st.st_ino,
	    (unsigned long)st.st_nlink
	);

	print_to_shell(shell, out, 1);
}

static void push_undo(FilesBuffer *files, UndoOp op) {
	if (files->undo_top < UNDO_MAX) {
		files->undo_stack[files->undo_top++] = op;
	} else {
		memmove(&files->undo_stack[0], &files->undo_stack[1],
		        (UNDO_MAX - 1) * sizeof(UndoOp));
		files->undo_stack[UNDO_MAX - 1] = op;
	}
	files->redo_top = 0;
}

static void do_trash_entry(FilesBuffer *files, struct ShellBuffer *shell, int idx) {
	char entry_path[PATH_MAX];
	char trash_dir[PATH_MAX];
	char trash_dest[PATH_MAX];
	char mkdir_cmd[PATH_MAX + 16];
	char mv_cmd[PATH_MAX * 2 + 8];
	const char *home;
	struct stat st;
	UndoOp op;

	if (idx < 0 || idx >= files->entry_count) { return; }

	if (strcmp(files->cwd, "/") == 0) {
		snprintf(entry_path, PATH_MAX, "/%s", files->entries[idx].name);
	} else {
		path_join(entry_path, PATH_MAX, files->cwd, files->entries[idx].name);
	}

	home = getenv("HOME");
	if (!home || home[0] == '\0') { home = "/tmp"; }
	snprintf(trash_dir, sizeof(trash_dir), "%s/.local/share/Trash/files", home);
	snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s'", trash_dir);
	system(mkdir_cmd);

	path_join(trash_dest, sizeof(trash_dest), trash_dir, files->entries[idx].name);
	if (stat(trash_dest, &st) == 0) {
		char ts_suffix[24];
		snprintf(ts_suffix, sizeof(ts_suffix), ".%ld", (long)time(NULL));
		strncat(trash_dest, ts_suffix, sizeof(trash_dest) - strlen(trash_dest) - 1);
	}

	snprintf(mv_cmd, sizeof(mv_cmd), "mv '%s' '%s'", entry_path, trash_dest);
	system(mv_cmd);

	op.type = OP_TRASH;
	strncpy(op.cwd, files->cwd, PATH_MAX - 1);
	op.cwd[PATH_MAX - 1] = '\0';
	strncpy(op.old_name, files->entries[idx].name, MAX_FILENAME - 1);
	op.old_name[MAX_FILENAME - 1] = '\0';
	op.new_name[0] = '\0';
	strncpy(op.trash_path, trash_dest, PATH_MAX - 1);
	op.trash_path[PATH_MAX - 1] = '\0';

	push_undo(files, op);
	(void)shell;
}

static void do_delete_entry(FilesBuffer *files, struct ShellBuffer *shell, int idx) {
	char entry_path[PATH_MAX];
	char cmd[PATH_MAX + 16];
	UndoOp op;

	if (idx < 0 || idx >= files->entry_count) { return; }

	if (strcmp(files->cwd, "/") == 0) {
		snprintf(entry_path, PATH_MAX, "/%s", files->entries[idx].name);
	} else {
		path_join(entry_path, PATH_MAX, files->cwd, files->entries[idx].name);
	}

	op.type = OP_DELETE_PERM;
	strncpy(op.cwd, files->cwd, PATH_MAX - 1);
	op.cwd[PATH_MAX - 1] = '\0';
	strncpy(op.old_name, files->entries[idx].name, MAX_FILENAME - 1);
	op.old_name[MAX_FILENAME - 1] = '\0';
	op.new_name[0] = '\0';
	op.trash_path[0] = '\0';

	if (files->entries[idx].type == ENTRY_DIR) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", entry_path);
	} else {
		snprintf(cmd, sizeof(cmd), "rm -f '%s'", entry_path);
	}
	system(cmd);

	push_undo(files, op);
	(void)shell;
}

void files_toggle_select(FilesBuffer *files, int idx) {
	if (idx < 0 || idx >= files->entry_count) { return; }
	if (files->multi_sel[idx]) {
		files->multi_sel[idx] = 0;
		files->sel_count--;
	} else {
		files->multi_sel[idx] = 1;
		files->sel_count++;
	}
}

void files_clear_selection(FilesBuffer *files) {
	memset(files->multi_sel, 0, sizeof(files->multi_sel));
	files->sel_count = 0;
}

void files_delete_to_trash(FilesBuffer *files, struct ShellBuffer *shell) {
	if (files->sel_count > 0) {
		for (int i = 0; i < files->entry_count; i++) {
			if (files->multi_sel[i]) {
				do_trash_entry(files, shell, i);
			}
		}
		files_clear_selection(files);
	} else {
		do_trash_entry(files, shell, files->selected);
	}
}

void files_delete_permanent(FilesBuffer *files, struct ShellBuffer *shell) {
	if (files->sel_count > 0) {
		for (int i = 0; i < files->entry_count; i++) {
			if (files->multi_sel[i]) {
				do_delete_entry(files, shell, i);
			}
		}
		files_clear_selection(files);
	} else {
		do_delete_entry(files, shell, files->selected);
	}
}

int files_build_sel_args(FilesBuffer *files, char *out, int out_size) {
	int written;
	int n;

	written = 0;
	for (int i = 0; i < files->entry_count; i++) {
		if (!files->multi_sel[i]) { continue; }
		if (written > 0 && written < out_size - 1) {
			out[written++] = ' ';
		}
		n = snprintf(out + written, out_size - written, "'%s'", files->entries[i].name);
		if (n > 0) { written += n; }
		if (written >= out_size - 1) { break; }
	}
	out[written] = '\0';
	return written;
}

void files_undo(FilesBuffer *files, struct ShellBuffer *shell) {
	UndoOp *op;
	char old_path[PATH_MAX];
	char new_path[PATH_MAX];
	char restore_cmd[PATH_MAX * 2 + 16];
	char msg[MAX_FILENAME * 2 + 64];

	if (files->undo_top == 0) {
		print_to_shell(shell, "Nothing to undo.\n", SHELL_MSG_WARN);
		return;
	}

	op = &files->undo_stack[--files->undo_top];

	if (files->redo_top < UNDO_MAX) {
		files->redo_stack[files->redo_top++] = *op;
	}

	switch (op->type) {
	case OP_RENAME:
		if (strcmp(op->cwd, "/") == 0) {
			snprintf(old_path, PATH_MAX, "/%s", op->new_name);
			snprintf(new_path, PATH_MAX, "/%s", op->old_name);
		} else {
			path_join(old_path, PATH_MAX, op->cwd, op->new_name);
			path_join(new_path, PATH_MAX, op->cwd, op->old_name);
		}
		if (rename(old_path, new_path) == 0) {
			snprintf(msg, sizeof(msg), "Undo rename: '%.255s' -> '%.255s'\n", op->new_name, op->old_name);
			print_to_shell(shell, msg, SHELL_MSG_NORMAL);
		}
		break;
	case OP_TRASH:
		if (op->trash_path[0] == '\0') {
			print_to_shell(shell, "Undo trash: path unknown.\n", SHELL_MSG_WARN);
			break;
		}
		if (strcmp(op->cwd, "/") == 0) {
			snprintf(old_path, PATH_MAX, "/%s", op->old_name);
		} else {
			path_join(old_path, PATH_MAX, op->cwd, op->old_name);
		}
		snprintf(restore_cmd, sizeof(restore_cmd), "mv '%s' '%s'", op->trash_path, old_path);
		if (system(restore_cmd) == 0) {
			snprintf(msg, sizeof(msg), "Restored '%s' from trash.\n", op->old_name);
			print_to_shell(shell, msg, SHELL_MSG_NORMAL);
		} else {
			snprintf(msg, sizeof(msg), "Restore failed for '%s'.\n", op->old_name);
			print_to_shell(shell, msg, SHELL_MSG_ERROR);
		}
		break;
	case OP_DELETE_PERM:
		snprintf(msg, sizeof(msg), "Undo perm-delete: '%s' is gone forever.\n", op->old_name);
		print_to_shell(shell, msg, SHELL_MSG_ERROR);
		break;
	}
}

void files_redo(FilesBuffer *files, struct ShellBuffer *shell) {
	UndoOp *op;
	char old_path[PATH_MAX];
	char new_path[PATH_MAX];
	char msg[MAX_FILENAME * 2 + 64];
	if (files->redo_top == 0) {
		print_to_shell(shell, "Nothing to redo.\n", SHELL_MSG_WARN);
		return;
	}

	op = &files->redo_stack[--files->redo_top];

	if (files->undo_top < UNDO_MAX) {
		files->undo_stack[files->undo_top++] = *op;
	}

	switch (op->type) {
	case OP_RENAME:
		if (strcmp(op->cwd, "/") == 0) {
			snprintf(old_path, PATH_MAX, "/%s", op->old_name);
			snprintf(new_path, PATH_MAX, "/%s", op->new_name);
		} else {
			path_join(old_path, PATH_MAX, op->cwd, op->old_name);
			path_join(new_path, PATH_MAX, op->cwd, op->new_name);
		}
		if (rename(old_path, new_path) == 0) {
			snprintf(msg, sizeof(msg), "Redo rename: '%.255s' -> '%.255s'\n", op->old_name, op->new_name);
			print_to_shell(shell, msg, SHELL_MSG_NORMAL);
		}
		break;
	case OP_TRASH:
		snprintf(msg, sizeof(msg), "Redo trash not supported.\n");
		print_to_shell(shell, msg, SHELL_MSG_WARN);
		break;
	case OP_DELETE_PERM:
		snprintf(msg, sizeof(msg), "Redo perm-delete not supported.\n");
		print_to_shell(shell, msg, SHELL_MSG_WARN);
		break;
	}
}

void files_open_selected(FilesBuffer *files, struct ShellBuffer *shell, WINDOW *files_win, WINDOW *shell_win) {
	char path[PATH_MAX];
	int pfd[2];
	pid_t pid;
	char mime[256];
	char desktop_id[256];
	char desktop_path[PATH_MAX];
	char line[512];
	char exec_cmd[512];
	int n;
	int status;
	int is_terminal_app;
	int found;
	const char *editor;
	const char *home;
	FILE *f;
	const char *search_dirs[] = {
		"/usr/share/applications",
		"/usr/local/share/applications",
		NULL,
	};
	char home_apps[PATH_MAX];

	(void)shell;

	if (files_get_selected_path(files, path, sizeof(path)) != 1) { return; }

	mime[0] = '\0';
	if (pipe(pfd) == 0) {
		pid = fork();
		if (pid == 0) {
			close(pfd[0]);
			dup2(pfd[1], STDOUT_FILENO);
			close(pfd[1]);
			execlp("xdg-mime", "xdg-mime", "query", "filetype", path, (char *)NULL);
			_exit(1);
		}
		close(pfd[1]);
		n = (pid > 0) ? (int)read(pfd[0], mime, sizeof(mime) - 1) : 0;
		close(pfd[0]);
		if (pid > 0) { waitpid(pid, &status, 0); }
		if (n > 0) {
			mime[n] = '\0';
			if (mime[n - 1] == '\n') { mime[--n] = '\0'; }
		}
	}

	desktop_id[0] = '\0';
	if (mime[0] != '\0' && pipe(pfd) == 0) {
		pid = fork();
		if (pid == 0) {
			close(pfd[0]);
			dup2(pfd[1], STDOUT_FILENO);
			close(pfd[1]);
			execlp("xdg-mime", "xdg-mime", "query", "default", mime, (char *)NULL);
			_exit(1);
		}
		close(pfd[1]);
		n = (pid > 0) ? (int)read(pfd[0], desktop_id, sizeof(desktop_id) - 1) : 0;
		close(pfd[0]);
		if (pid > 0) { waitpid(pid, &status, 0); }
		if (n > 0) {
			desktop_id[n] = '\0';
			if (desktop_id[n - 1] == '\n') { desktop_id[--n] = '\0'; }
		}
	}

	is_terminal_app = 0;
	exec_cmd[0] = '\0';
	if (desktop_id[0] != '\0') {
		home = getenv("HOME");
		if (home) {
			snprintf(home_apps, sizeof(home_apps), "%s/.local/share/applications", home);
		} else {
			home_apps[0] = '\0';
		}

		found = 0;
		for (int i = 0; !found; i++) {
			const char *dir;
			if (search_dirs[i] != NULL) {
				dir = search_dirs[i];
			} else if (home_apps[0] != '\0') {
				dir = home_apps;
				home_apps[0] = '\0';
			} else {
				break;
			}
			path_join(desktop_path, sizeof(desktop_path), dir, desktop_id);
			f = fopen(desktop_path, "r");
			if (!f) { continue; }
			found = 1;
			while (fgets(line, sizeof(line), f)) {
				if (strncmp(line, "Terminal=true", 13) == 0) {
					is_terminal_app = 1;
				}
				if (strncmp(line, "Exec=", 5) == 0 && exec_cmd[0] == '\0') {
					strncpy(exec_cmd, line + 5, sizeof(exec_cmd) - 1);
					exec_cmd[sizeof(exec_cmd) - 1] = '\0';
					n = (int)strlen(exec_cmd);
					if (n > 0 && exec_cmd[n - 1] == '\n') { exec_cmd[--n] = '\0'; }
					char *pct = strchr(exec_cmd, ' ');
					if (pct) { *pct = '\0'; }
					pct = strchr(exec_cmd, '%');
					if (pct) { *pct = '\0'; }
				}
			}
			fclose(f);
		}
	}

	if (is_terminal_app) {
		if (exec_cmd[0] == '\0') {
			editor = getenv("VISUAL");
			if (!editor || editor[0] == '\0') { editor = getenv("EDITOR"); }
			if (!editor || editor[0] == '\0') { editor = "vi"; }
			strncpy(exec_cmd, editor, sizeof(exec_cmd) - 1);
		}
		def_prog_mode();
		endwin();
		pid = fork();
		if (pid == 0) {
			signal(SIGINT, SIG_DFL);
			signal(SIGQUIT, SIG_DFL);
			signal(SIGTSTP, SIG_DFL);
			execlp(exec_cmd, exec_cmd, path, (char *)NULL);
			_exit(1);
		} else if (pid > 0) {
			waitpid(pid, &status, 0);
		}
		reset_prog_mode();
		keypad(stdscr, TRUE);
		nodelay(stdscr, TRUE);
		curs_set(0);
		if (files_win) { keypad(files_win, TRUE); touchwin(files_win); }
		if (shell_win) { keypad(shell_win, TRUE); touchwin(shell_win); }
		refresh();
	} else {
		pid = fork();
		if (pid == 0) {
			setsid();
			if (fork() != 0) { _exit(0); }
			signal(SIGINT, SIG_DFL);
			signal(SIGQUIT, SIG_DFL);
			signal(SIGTSTP, SIG_DFL);
			close(STDIN_FILENO);
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			execlp("xdg-open", "xdg-open", path, (char *)NULL);
			_exit(1);
		} else if (pid > 0) {
			waitpid(pid, NULL, WNOHANG);
		}
		needs_resize = 1;
	}
}

static void files_move_reindex(FilesBuffer *files) {
	char full[PATH_MAX];

	for (int i = 0; i < files->entry_count; i++) {
		path_join(full, sizeof(full), files->cwd, files->entries[i].name);
		files->in_move_reg[i] = 0;
		for (int j = 0; j < files->move_reg.count; j++) {
			if (strcmp(full, files->move_reg.paths[j]) == 0) {
				files->in_move_reg[i] = 1;
				break;
			}
		}
	}
}

static void files_move_print_register(FilesBuffer *files, struct ShellBuffer *shell) {
	char buf[SHELL_OUTPUT_MAX];
	int pos;

	if (files->move_reg.count == 0) {
		print_to_shell(shell, "[move register] (empty)\n", SHELL_MSG_NORMAL);
		return;
	}
	pos = 0;
	pos += snprintf(buf + pos, sizeof(buf) - pos, "[move register]\n");
	for (int i = 0; i < files->move_reg.count && pos < (int)sizeof(buf) - 1; i++) {
		pos += snprintf(buf + pos, sizeof(buf) - pos, "  %s\n", files->move_reg.paths[i]);
	}
	print_to_shell(shell, buf, SHELL_MSG_NORMAL);
}

void files_move_mark(FilesBuffer *files, struct ShellBuffer *shell, int idx) {
	char full[PATH_MAX];

	if (idx < 0 || idx >= files->entry_count) { return; }

	path_join(full, sizeof(full), files->cwd, files->entries[idx].name);

	for (int j = 0; j < files->move_reg.count; j++) {
		if (strcmp(files->move_reg.paths[j], full) != 0) { continue; }
		memmove(files->move_reg.paths[j], files->move_reg.paths[j + 1],
		        (size_t)(files->move_reg.count - j - 1) * PATH_MAX);
		files->move_reg.count--;
		files_move_reindex(files);
		files_move_print_register(files, shell);
		return;
	}

	if (files->move_reg.count >= MOVE_REGISTER_MAX) {
		print_to_shell(shell, "Move register is full.\n", SHELL_MSG_WARN);
		return;
	}

	strncpy(files->move_reg.paths[files->move_reg.count], full, PATH_MAX - 1);
	files->move_reg.paths[files->move_reg.count][PATH_MAX - 1] = '\0';
	files->move_reg.count++;
	files_move_reindex(files);
	files_move_print_register(files, shell);
}

void files_move_mark_all(FilesBuffer *files, struct ShellBuffer *shell) {
	char full[PATH_MAX];
	int already;

	for (int i = 0; i < files->entry_count; i++) {
		if (files->sel_count > 0 && !files->multi_sel[i]) { continue; }
		if (files->move_reg.count >= MOVE_REGISTER_MAX) { break; }

		path_join(full, sizeof(full), files->cwd, files->entries[i].name);

		already = 0;
		for (int j = 0; j < files->move_reg.count; j++) {
			if (strcmp(files->move_reg.paths[j], full) == 0) { already = 1; break; }
		}
		if (already) { continue; }

		strncpy(files->move_reg.paths[files->move_reg.count], full, PATH_MAX - 1);
		files->move_reg.paths[files->move_reg.count][PATH_MAX - 1] = '\0';
		files->move_reg.count++;
	}

	files_move_reindex(files);
	files_move_print_register(files, shell);
}

void files_move_clear(FilesBuffer *files, struct ShellBuffer *shell) {
	files->move_reg.count = 0;
	memset(files->in_move_reg, 0, sizeof(files->in_move_reg));
	print_to_shell(shell, "[move register] cleared\n", SHELL_MSG_NORMAL);
}

void files_move_paste(FilesBuffer *files, struct ShellBuffer *shell, const char *dest) {
	char src[PATH_MAX];
	char dst[PATH_MAX];
	char msg[PATH_MAX + 64];
	char mv_cmd[PATH_MAX * 2 + 16];
	int keep[MOVE_REGISTER_MAX];
	int any_kept;
	int new_count;
	const char *base;
	struct stat st;
	size_t srclen;

	if (files->move_reg.count == 0) { return; }

	memset(keep, 0, sizeof(int) * files->move_reg.count);
	any_kept = 0;

	for (int i = 0; i < files->move_reg.count; i++) {
		strncpy(src, files->move_reg.paths[i], PATH_MAX - 1);
		src[PATH_MAX - 1] = '\0';

		base = strrchr(src, '/');
		base = base ? base + 1 : src;

		if (strcmp(dest, "/") == 0) {
			snprintf(dst, sizeof(dst), "/%s", base);
		} else {
			path_join(dst, sizeof(dst), dest, base);
		}

		if (strcmp(src, dst) == 0) {
			snprintf(msg, sizeof(msg), "Move: '%s' is already here.\n", base);
			print_to_shell(shell, msg, SHELL_MSG_WARN);
			continue;
		}

		srclen = strlen(src);
		if (strncmp(dest, src, srclen) == 0 && (dest[srclen] == '/' || dest[srclen] == '\0')) {
			snprintf(msg, sizeof(msg), "Move: can't move '%s' into itself.\n", base);
			print_to_shell(shell, msg, SHELL_MSG_ERROR);
			keep[i] = 1;
			any_kept = 1;
			continue;
		}

		if (stat(dst, &st) == 0) {
			snprintf(msg, sizeof(msg), "Move: '%s' already exists at destination.\n", base);
			print_to_shell(shell, msg, SHELL_MSG_ERROR);
			keep[i] = 1;
			any_kept = 1;
			continue;
		}

		snprintf(mv_cmd, sizeof(mv_cmd), "mv '%s' '%s'", src, dst);
		if (system(mv_cmd) == 0) {
			snprintf(msg, sizeof(msg), "Moved '%s'\n", base);
			print_to_shell(shell, msg, SHELL_MSG_NORMAL);
		} else {
			snprintf(msg, sizeof(msg), "Move failed: '%s'\n", base);
			print_to_shell(shell, msg, SHELL_MSG_ERROR);
			keep[i] = 1;
			any_kept = 1;
		}
	}

	if (!any_kept) {
		files->move_reg.count = 0;
	} else {
		new_count = 0;
		for (int i = 0; i < files->move_reg.count; i++) {
			if (!keep[i]) { continue; }
			if (new_count != i) {
				memcpy(files->move_reg.paths[new_count], files->move_reg.paths[i], PATH_MAX);
			}
			new_count++;
		}
		files->move_reg.count = new_count;
		files_move_print_register(files, shell);
	}

	files_move_reindex(files);
}
