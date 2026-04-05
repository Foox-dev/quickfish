#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "file.h"
#include "main.h"
#include "shell.h"

#define INDEX_W 4
#define NAME_W 22
#define COL_W (INDEX_W + NAME_W)
#define COL_PAD 2

static int entry_cmp(const void *a, const void *b);
static int dir_size_walk(const char *path, const struct stat *st, int type, struct FTW *ftw);
static off_t compute_dir_size(const char *path);
static int ext_color_normal(const char *name);
static int to_selected_pair(int normal_pair);

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

	use = realpath(path, resolved) ? resolved : path;

	strncpy(files->cwd, use, PATH_MAX - 1);
	files->cwd[PATH_MAX - 1] = '\0';

	dir = opendir(files->cwd);
	if (!dir) {
		files->entry_count = 0;
		files->selected = 0;
		files->col_offset = 0;
		return;
	}

	files->entry_count = 0;
	files->selected = 0;
	files->col_offset = 0;

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
	if (e->mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
		return selected ? CP_SEL_EXEC : CP_EXT_EXEC;
	}
	normal = ext_color_normal(e->name);
	return selected ? to_selected_pair(normal) : normal;
}

void files_render(FilesBuffer *files, WINDOW *win, int height, int width, int focused) {
	int rows;
	const char *cwd;
	int avail;
	char count_str[32];
	int cell_w;
	int n_cols;
	int first;
	int last;
	int visible;
	int cwdlen;
	int cx;
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

			if (is_sel) {
				wattron(win, COLOR_PAIR(sel_pair) | A_BOLD);
			} else {
				wattron(win, COLOR_PAIR(CP_INDEX));
			}
			wprintw(win, "%3d ", e->index);
			if (is_sel) {
				wattroff(win, COLOR_PAIR(sel_pair) | A_BOLD);
			} else {
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
		print_to_shell(shell, "stat: no file specified\n");
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
			print_to_shell(shell, msg);
			return;
		}
		path_join(full, sizeof full, files->cwd, e->name);
	}

	if (stat(full, &st) != 0) {
		snprintf(msg, sizeof msg, "stat: could not stat '%s'\n", e->name);
		print_to_shell(shell, msg);
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

	print_to_shell(shell, out);
}
