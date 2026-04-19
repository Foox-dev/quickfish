/**
 * @file tui.c
 * @brief TUI rendering, input handling, and lifecycle for Quickfish.
 *
 * Manages the ncurses windows (file buffer, shell buffer, preview buffer, info
 * overlay), dispatches keyboard input to the appropriate subsystem, and
 * owns the resize signal handler.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <ncurses.h>
#include <panel.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "main.h"
#include "tui.h"

#define MIN_COLS 51 ///< Minimum terminal width to run the TUI
#define MIN_ROWS 25 ///< Minimum terminal height to run the TUI

static void handle_resize(int sig);
static void info_draw(TUI *tui);
static void info_toggle(TUI *tui);
static int preview_entry_cmp(const void *a, const void *b);
static void edit_handle_ch(char *buf, int *len, int *cursor, int max, int ch);
static void rebuild_windows(TUI *tui);
static void preview_render_dir(TUI *tui, const char *path, int col, int avail, int max_rows);
static void preview_render_file(TUI *tui, const char *path, DirEntry *e, int col, int avail, int max_rows);
static void preview_render(TUI *tui, int list_w);
static void render_goto_overlay(TUI *tui);
static void render_rename_overlay(TUI *tui);
static void render_visual_indicator(TUI *tui);
static void render_quickshell_overlay(TUI *tui, int files_render_w);
static void render_delete_confirm(TUI *tui, DeleteMode mode);
static void goto_begin(TUI *tui);
static void goto_cancel(TUI *tui);
static void goto_confirm(TUI *tui);
static void goto_tab_complete(TUI *tui);
static void rename_begin(TUI *tui);
static void rename_commit(TUI *tui);
static void rename_cancel(TUI *tui);
static void handle_rename_input(TUI *tui, int ch);
static void handle_goto_input(TUI *tui, int ch);
static void quickshell_begin(TUI *tui);
static void quickshell_cancel(TUI *tui);
static void quickshell_execute(TUI *tui);
static void handle_quickshell_input(TUI *tui, int ch);
static void move_paste_confirm(TUI *tui, const char *dest);
static void file_cmd_clear(TUI *tui);
static void files_load_directory_tracked(TUI *tui, const char *path);
static void files_change_dir_tracked(TUI *tui, const char *dirname);
static int handle_file_cmd_input(TUI *tui, int ch);

volatile int needs_resize = 0; ///< Flag set by the SIGWINCH handler to indicate that the terminal has been resized and the TUI needs to adjust its layout accordingly

static void handle_resize(int sig) { (void)sig; needs_resize = 1; }

typedef struct {
	int is_empty;
	int is_dir;
	mode_t mode;
	char name[MAX_FILENAME];
} PreviewEntry;

static void file_cmd_clear(TUI *tui) {
	tui->file_cmd_len = 0;
	tui->file_cmd_buf[0] = '\0';
}

static void files_load_directory_tracked(TUI *tui, const char *path) {
	if (strcmp(path, tui->files.cwd) == 0) {
		return;
	}
	shell_push_dir(&tui->shell, tui->files.cwd);
	files_load_directory(&tui->files, path);
}

static void files_change_dir_tracked(TUI *tui, const char *dirname) {
	char old_cwd[PATH_MAX];

	snprintf(old_cwd, sizeof(old_cwd), "%s", tui->files.cwd);
	files_change_dir(&tui->files, dirname);
	if (strcmp(old_cwd, tui->files.cwd) != 0) {
		shell_push_dir(&tui->shell, old_cwd);
	}
}

static int handle_file_cmd_input(TUI *tui, int ch) {
	if (tui->file_cmd_len > 0 && tui->file_cmd_buf[0] == '.') {
		switch (ch) {
		case 27:
		case '\n':
		case '\r':
		case KEY_ENTER:
		case KEY_DC:
			file_cmd_clear(tui);
			return 1;
		case '\x7f':
		case KEY_BACKSPACE:
			if (tui->file_cmd_len > 0) {
				tui->file_cmd_len--;
				tui->file_cmd_buf[tui->file_cmd_len] = '\0';
			}
			return 1;
		default:
			if (ch >= 32 && ch < 127 && tui->file_cmd_len < FILE_CMD_BUF_MAX - 1) {
				tui->file_cmd_buf[tui->file_cmd_len++] = (unsigned char)ch;
				tui->file_cmd_buf[tui->file_cmd_len] = '\0';
			}
			if (strcmp(tui->file_cmd_buf, "..") == 0) {
				shell_jump_prev_dir(&tui->shell, &tui->files);
				file_cmd_clear(tui);
			}
			return 1;
		}
	}

	if (ch >= '0' && ch <= '9') {
		if (tui->file_cmd_len < FILE_CMD_BUF_MAX - 1) {
			tui->file_cmd_buf[tui->file_cmd_len++] = (unsigned char)ch;
			tui->file_cmd_buf[tui->file_cmd_len] = '\0';
		}
		return 1;
	}

	if (ch == '.') {
		if (tui->file_cmd_len > 0 && tui->file_cmd_buf[0] != '.') {
			file_cmd_clear(tui);
		}
		if (tui->file_cmd_len < FILE_CMD_BUF_MAX - 1) {
			tui->file_cmd_buf[tui->file_cmd_len++] = '.';
			tui->file_cmd_buf[tui->file_cmd_len] = '\0';
		}
		if (strcmp(tui->file_cmd_buf, "..") == 0) {
			shell_jump_prev_dir(&tui->shell, &tui->files);
			file_cmd_clear(tui);
		}
		return 1;
	}

	return 0;
}

/**
 * @brief Comparator for sorting preview entries.
 *
 * Directories are sorted before files, and within each group
 * entries are sorted alphabetically (case-insensitive).
 *
 * @param a Pointer to the first PreviewEntry.
 * @param b Pointer to the second PreviewEntry.
 * @return Negative if "a" comes first, positive if "b" comes first, "ero if equal.
 */
static int preview_entry_cmp(const void *a, const void *b) {
	const PreviewEntry *ea;
	const PreviewEntry *eb;

	ea = (const PreviewEntry *)a;
	eb = (const PreviewEntry *)b;
	if (ea->is_dir != eb->is_dir) {
		return (ea->is_dir == 0) ? 1 : -1;
	}
	return strcasecmp(ea->name, eb->name);
}

/**
 * @brief Applies a single keypress to an editable text buffer.
 *
 * Handles cursor movement, insertion, backspace, and delete-forward.
 * Used by the goto, rename, and quickshell overlays.
 *
 * @param buf 	 The text buffer (null-terminated, caller-owned).
 * @param len 	 Current length of text in buf (updated in place).
 * @param cursor Current cursor position within buf (updated in place).
 * @param max 	 Maximum capacity of buf including the null terminator.
 * @param ch 		 ncurses key value to process.
 */
static void edit_handle_ch(char *buf, int *len, int *cursor, int max, int ch) {
	if (ch == KEY_LEFT) {
		if (*cursor > 0) { (*cursor)--; }
	} else if (ch == KEY_RIGHT) {
		if (*cursor < *len) { (*cursor)++; }
	} else if (ch == KEY_HOME) {
		*cursor = 0;
	} else if (ch == KEY_END) {
		*cursor = *len;
	} else if (ch == '\x7f' || ch == KEY_BACKSPACE) {
		if (*cursor > 0) {
			memmove(buf + *cursor - 1, buf + *cursor, (size_t)(*len - *cursor + 1));
			(*cursor)--;
			(*len)--;
		}
	} else if (ch == KEY_DC) {
		if (*cursor < *len) {
			memmove(buf + *cursor, buf + *cursor + 1, (size_t)(*len - *cursor));
			(*len)--;
		}
	} else if (ch >= 32 && ch < 127 && *len < max - 1) {
		memmove(buf + *cursor + 1, buf + *cursor, (size_t)(*len - *cursor + 1));
		buf[(*cursor)++] = (unsigned char)ch;
		(*len)++;
	}
}

/* Tears down and recreates all ncurses windows to match the current terminal size.
   Called once on init and again after every SIGWINCH. */
static void rebuild_windows(TUI *tui) {
	getmaxyx(stdscr, tui->max_y, tui->max_x);

	tui->files_height = (tui->max_y - 1) / 2;
	tui->shell_height = tui->max_y - tui->files_height - 1;
	tui->divider_y = tui->files_height;

	if (tui->files_win) { delwin(tui->files_win); }
	if (tui->shell_win) { delwin(tui->shell_win); }

	tui->files_win = newwin(tui->files_height, tui->max_x, 0, 0);
	tui->shell_win = newwin(tui->shell_height, tui->max_x, tui->files_height + 1, 0);
	if (tui->files_win) { keypad(tui->files_win, TRUE); }
	if (tui->shell_win) { keypad(tui->shell_win, TRUE); }

	if (tui->info_win) {
		wresize(tui->info_win, tui->max_y, tui->max_x);
		replace_panel(tui->info_panel, tui->info_win);
		if (!tui->info_mode) { hide_panel(tui->info_panel); }
	}
}

/**
 * @brief Initialises ncurses and allocates a TUI instance.
 *
 * Sets up color pairs, creates the files and shell windows, loads the
 * starting directory, and installs the SIGWINCH handler.
 *
 * @param start_dir Initial directory to display. If NULL or empty the function falls back to $HOME, then getpwuid, then ".".
 * @return Heap-allocated TUI on success, NULL on allocation or window failure.
 */
TUI *tui_init(const char *start_dir) {
	TUI *tui;
	char resolved[PATH_MAX];
	const char *dir;
	struct passwd *pw;

	tui = calloc(1, sizeof(TUI));
	if (!tui) { return NULL; }

	initscr();
	cbreak();
	timeout(50);
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	set_escdelay(0);

	if (has_colors()) {
		start_color();
		use_default_colors();

		init_pair(1, COLOR_WHITE, -1);
		init_pair(2, COLOR_WHITE, COLOR_BLUE);
		init_pair(3, COLOR_CYAN, -1);
		init_pair(4, COLOR_YELLOW, -1);
		init_pair(5, COLOR_GREEN, -1);
		init_pair(6, COLOR_WHITE, -1);
		init_pair(7, COLOR_BLACK, COLOR_WHITE);
		init_pair(8, COLOR_CYAN, -1);
		init_pair(9, COLOR_CYAN, -1);
		init_pair(10, COLOR_WHITE, -1);

		init_pair(CP_EXT_IMAGE, COLOR_MAGENTA, -1);
		init_pair(CP_EXT_VIDEO, COLOR_RED, -1);
		init_pair(CP_EXT_AUDIO, COLOR_CYAN, -1);
		init_pair(CP_EXT_ARCHIVE, COLOR_YELLOW, -1);
		init_pair(CP_EXT_CODE, COLOR_GREEN, -1);
		init_pair(CP_EXT_DOC, COLOR_WHITE, -1);
		init_pair(CP_EXT_EXEC, COLOR_YELLOW, -1);
		init_pair(CP_EXT_DATA, COLOR_BLUE, -1);

		init_pair(CP_SEL_IMAGE, COLOR_BLACK, COLOR_MAGENTA);
		init_pair(CP_SEL_VIDEO, COLOR_BLACK, COLOR_RED);
		init_pair(CP_SEL_AUDIO, COLOR_BLACK, COLOR_CYAN);
		init_pair(CP_SEL_ARCHIVE, COLOR_BLACK, COLOR_YELLOW);
		init_pair(CP_SEL_CODE, COLOR_BLACK, COLOR_GREEN);
		init_pair(CP_SEL_DOC, COLOR_BLACK, COLOR_WHITE);
		init_pair(CP_SEL_EXEC, COLOR_BLACK, COLOR_YELLOW);
		init_pair(CP_SEL_DATA, COLOR_BLACK, COLOR_BLUE);
		init_pair(CP_SEL_DIR, COLOR_BLACK, COLOR_CYAN);
		init_pair(CP_SEL_FILE, COLOR_BLACK, COLOR_WHITE);
		init_pair(CP_MULTI_SEL, COLOR_BLACK, COLOR_MAGENTA);
		init_pair(CP_DELETE_CONFIRM, COLOR_BLACK, COLOR_RED);
		init_pair(CP_TRASH_CONFIRM, COLOR_BLACK, COLOR_YELLOW);
	}

	rebuild_windows(tui);
	if (!tui->files_win || !tui->shell_win) {
		endwin();
		free(tui);
		return NULL;
	}

	tui->active_buffer = BUFFER_FILES;
	tui->running = 1;

	dir = start_dir;
	if (!dir || dir[0] == '\0') {
		dir = getenv("HOME");
		if (!dir || dir[0] == '\0') {
			pw = getpwuid(getuid());
			dir = (pw && pw->pw_dir) ? pw->pw_dir : ".";
		}
	}
	if (realpath(dir, resolved)) { dir = resolved; }
	files_load_directory(&tui->files, dir);

	tui->shell.history_index = -1;

	signal(SIGWINCH, handle_resize);

	return tui;
}

/**
 * @brief Tears down ncurses and frees the TUI.
 *
 * Safe to call with a NULL pointer.
 *
 * @param tui The TUI instance to destroy.
 */
void tui_cleanup(TUI *tui) {
	if (!tui) { return; }
	if (tui->files_win) { delwin(tui->files_win); }
	if (tui->shell_win) { delwin(tui->shell_win); }
	if (tui->info_panel) { del_panel(tui->info_panel); }
	if (tui->info_win) { delwin(tui->info_win); }
	endwin();
	free(tui);
}

/**
 * @brief Responds to a pending terminal resize.
 *
 * Should be called at the top of the main loop whenever needs_resize is set.
 * Resets the flag, rebuilds all windows, and forces a full redraw.
 *
 * @param tui The TUI instance to resize.
 */
void tui_resize_handler(TUI *tui) {
	if (!needs_resize) { return; }
	needs_resize = 0;

	endwin();
	refresh();
	clear();
	rebuild_windows(tui);

	clearok(stdscr, TRUE);
	if (tui->files_win) { clearok(tui->files_win, TRUE); touchwin(tui->files_win); }
	if (tui->shell_win) { clearok(tui->shell_win, TRUE); touchwin(tui->shell_win); }
	if (tui->info_win) { clearok(tui->info_win, TRUE); touchwin(tui->info_win); }

	wrefresh(stdscr);
}

/**
 * @brief Renders a directory listing into the preview buffer.
 *
 * Reads up to 4096 entries, sorts them (dirs first, then alphabetically),
 * and draws them with appropriate color pairs. Respects tui->preview_scroll.
 *
 * @param tui			 The TUI instance.
 * @param path		 Absolute path of the directory to preview.
 * @param col			 Starting column in files_win to draw into.
 * @param avail		 Available column width for entry names.
 * @param max_rows Maximum number of rows to render.
 */
static void preview_render_dir(TUI *tui, const char *path, int col, int avail, int max_rows) {
	DIR *d;
	PreviewEntry *names;
	struct dirent *de;
	struct stat st;
	DirEntry tmp_e;
	char entry_path[PATH_MAX];
	int total;
	int max_scroll;
	int rendered;
	int cp;
	attr_t at;

	d = opendir(path);
	if (!d) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(cannot open dir)");
		wattroff(tui->files_win, A_DIM);
		return;
	}

	names = malloc(4096 * sizeof(PreviewEntry));
	if (!names) { closedir(d); return; }
	total = 0;

	while ((de = readdir(d)) && total < 4096) {
		if (de->d_name[0] == '.') { continue; }
		snprintf(names[total].name, sizeof(names[total].name), "%s", de->d_name);
		path_join(entry_path, PATH_MAX, path, de->d_name);
		if (stat(entry_path, &st) == 0) {
			names[total].is_dir = S_ISDIR(st.st_mode);
			names[total].is_empty = names[total].is_dir ? dir_is_empty(entry_path) : 0;
			names[total].mode = st.st_mode;
		} else {
			names[total].is_dir = 0;
			names[total].is_empty = 0;
			names[total].mode = 0;
		}
		total++;
	}
	closedir(d);

	qsort(names, (size_t)total, sizeof(PreviewEntry), preview_entry_cmp);

	max_scroll = total - max_rows;
	if (max_scroll < 0) { max_scroll = 0; }
	if (tui->preview_scroll > max_scroll) { tui->preview_scroll = max_scroll; }

	rendered = 0;
	for (int i = tui->preview_scroll; i < total && rendered < max_rows; i++, rendered++) {
		if (names[i].is_dir) {
			if (names[i].is_empty) {
				wattron(tui->files_win, COLOR_PAIR(CP_DIR_EMPTY) | A_DIM);
				mvwprintw(tui->files_win, 1 + rendered, col, "%-*.*s", avail, avail, names[i].name);
				wattroff(tui->files_win, COLOR_PAIR(CP_DIR_EMPTY) | A_DIM);
			} else {
				wattron(tui->files_win, COLOR_PAIR(CP_DIR_FULL) | A_BOLD);
				mvwprintw(tui->files_win, 1 + rendered, col, "%-*.*s", avail, avail, names[i].name);
				wattroff(tui->files_win, COLOR_PAIR(CP_DIR_FULL) | A_BOLD);
			}
		} else {
			strncpy(tmp_e.name, names[i].name, MAX_FILENAME - 1);
			tmp_e.name[MAX_FILENAME - 1] = '\0';
			tmp_e.type = ENTRY_FILE;
			tmp_e.is_empty = 0;
			tmp_e.mode = names[i].mode;
			cp = entry_color_pair(&tmp_e, 0);
			at = (cp == CP_EXT_EXEC) ? A_BOLD : A_NORMAL;
			wattron(tui->files_win, COLOR_PAIR(cp) | at);
			mvwprintw(tui->files_win, 1 + rendered, col, "%-*.*s", avail, avail, names[i].name);
			wattroff(tui->files_win, COLOR_PAIR(cp) | at);
		}
	}

	if (total == 0) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(empty)");
		wattroff(tui->files_win, A_DIM);
	}

	free(names);
}

#define BAT_CACHE_BUF (512 * 1024) ///< Maximum bytes buffered from a bat(1) subprocess
#define BAT_CACHE_LMAX 4096 ///< Maximum number of lines indexed in the bat cache

static char bat_path_cache[PATH_MAX]; ///< Path of the file currently held in the bat cache
static char bat_buf[BAT_CACHE_BUF]; ///< Raw output buffer from the last bat(1) run
static char *bat_lines[BAT_CACHE_LMAX]; ///< Pointers into bat_buf, one per line
static int bat_nlines = 0; ///< Number of valid entries in bat_lines
static int bat_valid = 0; ///< Non-zero when bat_buf holds usable content for bat_path_cache

/* Returns 1 if bat(1) is on PATH, 0 otherwise. Result is cached after the first call. */
static int bat_available(void) {
	static int result = -1;
	FILE *f;
	char buf[4];
	if (result != -1) { return result; }
	f = popen("command -v bat 2>/dev/null", "r");
	result = (f && fgets(buf, sizeof(buf), f)) ? 1 : 0;
	if (f) { pclose(f); }
	return result;
}

/* Maps 24-bit RGB to the nearest xterm-256 color index. */
static short rgb_to_256(int r, int g, int b) {
	return (short)(16 + 36 * ((r * 5 + 127) / 255) + 6 * ((g * 5 + 127) / 255) + ((b * 5 + 127) / 255));
}

/* Returns an ncurses color-pair number for the given xterm-256 foreground index,
	 allocating a new pair on first use. Returns 0 if the terminal has too few colors
	 or if the dynamic pair pool is exhausted. */
static short bat_color_pair(short fg) {
	static short cache[256];
	static int ready = 0;
	static int used = 0;
	short pair;

	if (!ready) { memset(cache, -1, sizeof(cache)); ready = 1; }
	if (COLORS < 256 || COLOR_PAIRS < 256 || fg < 0 || fg > 255) { return 0; }
	if (cache[fg] < 0) {
		if (64 + used >= COLOR_PAIRS || used >= 192) { return 0; }
		pair = (short)(64 + used++);
		init_pair(pair, fg, -1);
		cache[fg] = pair;
	}
	return cache[fg];
}

/**
 * @brief Renders one ANSI-escaped line from bat(1) into an ncurses window.
 *
 * Parses SGR escape sequences inline, mapping colors and attributes to
 * ncurses equivalents. Non-printable or unrecognised sequences are skipped.
 *
 * @param win   Target ncurses window.
 * @param row   Row to write into.
 * @param col   Starting column.
 * @param avail Maximum number of visible characters to write.
 * @param s     Null-terminated ANSI-escaped string to render.
 */
static void render_bat_line(WINDOW *win, int row, int col, int avail, const char *s) {
	attr_t attrs = A_NORMAL;
	short fg = -1;
	int budget = avail;
	int params[16];
	int np;
	int i;

	wmove(win, row, col);

	while (*s && budget > 0) {
		if (s[0] != '\033' || s[1] != '[') {
			short pr = (fg >= 0) ? bat_color_pair(fg) : 0;

			if (pr) {
				wattron(win, COLOR_PAIR(pr) | attrs);
			} else if (attrs) {
				wattron(win, attrs);
			}

			waddch(win, (unsigned char)*s);
			if (pr) {
				wattroff(win, COLOR_PAIR(pr) | attrs);
			} else if (attrs) {
				wattroff(win, attrs);
			}
			s++;
			budget--;
			continue;
		}

		s += 2;
		np = 0;
		while (np < 16) {
			char *end;
			long pv = strtol(s, &end, 10);
			params[np] = (end == s) ? 0 : (pv < 0 ? 0 : pv > 255 ? 255 : (int)pv);
			np++;
			s = end;
			if (*s == ';') { s++; } else { break; }
		}

		if (*s) { s++; }
		for (i = 0; i < np; i++) {
			switch (params[i]) {
			case 0: attrs = A_NORMAL; fg = -1; break;
			case 1: attrs |= A_BOLD; break;
			case 2: attrs |= A_DIM; break;
			case 4: attrs |= A_UNDERLINE; break;
			case 22: attrs &= ~(A_BOLD | A_DIM); break;
			case 24: attrs &= ~A_UNDERLINE; break;
			case 39: fg = -1; break;
			case 30: case 31: case 32: case 33:
			case 34: case 35: case 36: case 37:
				fg = (short)(params[i] - 30); break;
			case 90: case 91: case 92: case 93:
			case 94: case 95: case 96: case 97:
				fg = (short)(params[i] - 90 + 8); break;
			case 38:
				if (i + 2 < np && params[i + 1] == 5) {
					int c = params[i + 2];
					fg = (c >= 0 && c <= 255) ? (short)c : -1; i += 2;
				} else if (i + 4 < np && params[i + 1] == 2) {
					fg = rgb_to_256(params[i+2], params[i+3], params[i+4]);
					i += 4;
				}
				break;
			default: break;
			}
		}
	}
	wstandend(win);
}

/**
 * @brief Populates the bat cache by running bat(1) on the given file.
 *
 * Shell-escapes the path, spawns bat with plain style and no pager, reads
 * up to BAT_CACHE_BUF bytes, then splits the output into lines stored in
 * bat_lines[]. Sets bat_valid on success.
 *
 * @param path Absolute path of the file to render.
 */
static void bat_fill_cache(const char *path) {
	char cmd[PATH_MAX * 2 + 256];
	char esc[PATH_MAX * 2];
	const char *color_flag, *p;
	FILE *f;
	int ei, total, n, nl;
	char *c;

	bat_valid = 0;
	bat_nlines = 0;

	ei = 0;
	for (p = path; *p && ei < (int)sizeof(esc) - 4; p++) {
		if (*p == '\'') {
			esc[ei++] = '\''; esc[ei++] = '\\';
			esc[ei++] = '\''; esc[ei++] = '\'';
		} else {
			esc[ei++] = *p;
		}
	}
	esc[ei] = '\0';

	strncpy(bat_path_cache, path, PATH_MAX - 1);
	bat_path_cache[PATH_MAX - 1] = '\0';

	color_flag = (COLORS >= 256) ? "always" : "never";
	snprintf(cmd, sizeof(cmd), "bat --color=%s --style=plain --pager=never --tabs=4 --wrap=never '%s' 2>/dev/null", color_flag, esc);

	f = popen(cmd, "r");
	if (!f) { return; }

	total = 0;
	while (total < BAT_CACHE_BUF - 1) {
		n = (int)fread(bat_buf + total, 1, BAT_CACHE_BUF - 1 - total, f);
		if (n <= 0) { break; }
		total += n;
	}

	bat_buf[total] = '\0';
	pclose(f);

	nl = 0;
	bat_lines[nl++] = bat_buf;
	for (c = bat_buf; *c && nl < BAT_CACHE_LMAX; c++) {
		if (*c == '\n') {
			*c = '\0';
			if (c > bat_buf && *(c - 1) == '\r') { *(c - 1) = '\0'; }
			if (*(c + 1) != '\0') { bat_lines[nl++] = c + 1; }
		}
	}
	bat_nlines = nl;
	bat_valid = (total > 0) ? 1 : 0;
}

/**
 * @brief Renders a file's contents into the preview column.
 *
 * Uses bat(1) with ANSI color if available, falling back to plain fread.
 * Binary files and media types (image, video, audio, archive) display a
 * short label instead of content. Respects tui->preview_scroll.
 *
 * @param tui      The TUI instance.
 * @param path     Absolute path of the file to preview.
 * @param e        DirEntry for the file (used for extension-based color).
 * @param col      Starting column in files_win to draw into.
 * @param avail    Available column width.
 * @param max_rows Maximum number of rows to render.
 */
static void preview_render_file(TUI *tui, const char *path, DirEntry *e, int col, int avail, int max_rows) {
	FILE *f;
	char peek[512];
	char line[256];
	size_t n;
	int cp;
	int binary;
	int row;
	int len;

	cp = entry_color_pair(e, 0);

	if (cp == CP_EXT_IMAGE) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(image)");
		wattroff(tui->files_win, A_DIM);
		return;
	}
	if (cp == CP_EXT_VIDEO) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(video)");
		wattroff(tui->files_win, A_DIM);
		return;
	}
	if (cp == CP_EXT_AUDIO) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(audio)");
		wattroff(tui->files_win, A_DIM);
		return;
	}
	if (cp == CP_EXT_ARCHIVE) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(archive)");
		wattroff(tui->files_win, A_DIM);
		return;
	}

	if (bat_available()) {
		if (!bat_valid || strcmp(bat_path_cache, path) != 0) {
			bat_fill_cache(path);
		}
		if (bat_valid && bat_nlines > 0) {
			int max_scroll = bat_nlines - max_rows;
			if (max_scroll < 0) { max_scroll = 0; }
			if (tui->preview_scroll > max_scroll) {
				tui->preview_scroll = max_scroll;
			}
			for (row = 0; row < max_rows; row++) {
				int li = tui->preview_scroll + row;
				if (li >= bat_nlines) { break; }
				mvwprintw(tui->files_win, 1 + row, col, "%-*.*s", avail, avail, "");
				if (COLORS >= 256) {
					render_bat_line(tui->files_win, 1 + row, col, avail, bat_lines[li]);
				} else {
					const char *src = bat_lines[li];
					char plain[512];
					int pi = 0;
					while (*src && pi < (int)sizeof(plain) - 1) {
						if (src[0] == '\033' && src[1] == '[') {
							src += 2;
							while (*src && *src != 'm') { src++; }
							if (*src) { src++; }
						} else {
							plain[pi++] = *src++;
						}
					}
					plain[pi] = '\0';
					mvwprintw(tui->files_win, 1 + row, col, "%-*.*s", avail, avail, plain);
				}
			}
			return;
		}
	}

	f = fopen(path, "r");
	if (!f) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(cannot read)");
		wattroff(tui->files_win, A_DIM);
		return;
	}

	n = fread(peek, 1, sizeof(peek), f);
	rewind(f);
	binary = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)peek[i];
		if (c == 0 || (c < 7 && c != '\n' && c != '\r' && c != '\t')) {
			binary = 1;
			break;
		}
	}

	if (n == 0) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(empty)");
		wattroff(tui->files_win, A_DIM);
	} else if (binary) {
		wattron(tui->files_win, A_DIM);
		mvwprintw(tui->files_win, 1, col, "(binary)");
		wattroff(tui->files_win, A_DIM);
	} else {
		rewind(f);
		row = 0;
		while (row < max_rows && fgets(line, sizeof(line), f)) {
			len = (int)strlen(line);
			if (len > 0 && line[len - 1] == '\n') { line[--len] = '\0'; }
			if (len > 0 && line[len - 1] == '\r') { line[--len] = '\0'; }
			mvwprintw(tui->files_win, 1 + row, col, "%-*.*s", avail, avail, line);
			row++;
		}
	}

	fclose(f);
}

/**
 * @brief Draws the preview pane for the currently selected entry.
 *
 * Renders a vertical divider, a title bar, and delegates to either
 * preview_render_dir() or preview_render_file() depending on entry type.
 * Resets preview_scroll when the selection changes.
 *
 * @param tui    The TUI instance.
 * @param list_w Width in columns occupied by the file list pane.
 */
static void preview_render(TUI *tui, int list_w) {
	int preview_w;
	int height;
	int focused;
	int pair;
	int col;
	int avail;
	int max_rows;
	DirEntry *e;
	char preview_title[MAX_FILENAME + 16];
	char path[PATH_MAX];

	preview_w = tui->max_x - list_w - 1;
	height = tui->files_height;
	focused = (tui->active_buffer == BUFFER_PREVIEW);

	if (tui->files.selected != tui->preview_last_selected) {
		tui->preview_scroll = 0;
		tui->preview_last_selected = tui->files.selected;
	}

	for (int r = 0; r < height; r++) {
		mvwaddch(tui->files_win, r, list_w, ACS_VLINE);
	}

	if (tui->files.entry_count == 0) { return; }

	e = &tui->files.entries[tui->files.selected];

	pair = focused ? CP_TITLE_F : CP_TITLE_UF;
	snprintf(preview_title, sizeof(preview_title), "PREVIEW - %s", e->name);
	wattron(tui->files_win, COLOR_PAIR(pair) | A_BOLD);
	mvwprintw(tui->files_win, 0, list_w + 1, "%-*.*s", preview_w - 1, preview_w - 1, preview_title);
	wattroff(tui->files_win, COLOR_PAIR(pair) | A_BOLD);

	files_get_selected_path(&tui->files, path, sizeof(path));

	col = list_w + 2;
	avail = preview_w - 3;
	max_rows = height - 1;
	if (avail <= 0 || max_rows <= 0) { return; }

	if (e->type == ENTRY_DIR) {
		preview_render_dir(tui, path, col, avail, max_rows);
	} else {
		preview_render_file(tui, path, e, col, avail, max_rows);
	}

	wnoutrefresh(tui->files_win);
}

/* Draws the GOTO prompt centred on the title row and positions the cursor. */
static void render_goto_overlay(TUI *tui) {
	char label[GOTO_BUF_MAX + 16];
	int llen;
	int lx;

	snprintf(label, sizeof(label), " GOTO: %s ", tui->goto_buf);
	llen = (int)strlen(label);
	lx = (tui->max_x - llen) / 2;
	if (lx < 0) { lx = 0; }
	wattron(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
	mvwprintw(tui->files_win, 0, lx, "%s", label);
	wattroff(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
	wmove(tui->files_win, 0, lx + 7 + tui->goto_len);
	leaveok(tui->files_win, FALSE);
	curs_set(2);
	wnoutrefresh(tui->files_win);
}

/* Draws the RENAME prompt on the title row with an inline editable field. */
static void render_rename_overlay(TUI *tui) {
	const char *prefix;
	char label[RENAME_BUF_MAX + 16];
	int llen;
	int lx;
	int px;
	int name_x;
	int avail;

	prefix = " RENAME: ";
	snprintf(label, sizeof(label), " RENAME: %s ", tui->rename_buf);
	llen = (int)strlen(label);
	lx = (tui->max_x - llen) / 2;
	if (lx < 0) { lx = 0; }
	px = lx;
	wattron(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
	mvwprintw(tui->files_win, 0, px, "%s", prefix);
	wattroff(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
	name_x = px + (int)strlen(prefix);
	avail = tui->max_x - name_x - 1;
	if (avail > 0) {
		mvwprintw(tui->files_win, 0, name_x, "%-*.*s", avail, avail, tui->rename_buf);
	}
	wmove(tui->files_win, 0, name_x + tui->rename_cursor);
	leaveok(tui->files_win, FALSE);
	curs_set(2);
	wnoutrefresh(tui->files_win);
}

static void render_visual_indicator(TUI *tui) {
	int llen;
	int lx;

	llen = (int)strlen(" VISUAL ");
	lx = (tui->max_x - llen) / 2;
	if (lx < 0) { lx = 0; }
	wattron(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
	mvwprintw(tui->files_win, 0, lx, " VISUAL ");
	wattroff(tui->files_win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
	wnoutrefresh(tui->files_win);
}

/**
 * @brief Draws the quickshell input bar at the bottom of the files pane.
 *
 * Renders the ":" prompt, the current input buffer, and a ghost completion
 * hint (dimmed) if one is available.
 *
 * @param tui            The TUI instance.
 * @param files_render_w Width of the files pane in columns.
 */
static void render_quickshell_overlay(TUI *tui, int files_render_w) {
	int qrow;
	int avail;
	char ghost[MAX_FILENAME];
	int gx;
	int max_input_w;

	qrow = tui->files_height - 1;
	max_input_w = files_render_w / 2;
	avail = max_input_w - 2;
	wattron(tui->files_win, COLOR_PAIR(1));
	mvwprintw(tui->files_win, qrow, 0, "%s", ":");
	wattroff(tui->files_win, COLOR_PAIR(1));
	if (avail > 0) {
		mvwprintw(tui->files_win, qrow, 1, "%-*.*s", avail, avail, tui->quickshell_buf);
	}

	compute_ghost(tui->quickshell_buf, tui->quickshell_cursor,
	              tui->files.cwd, &tui->files, &tui->shell, ghost, sizeof(ghost));
	if (ghost[0] != '\0') {
		gx = 1 + tui->quickshell_cursor;
		if (gx < max_input_w) {
			wmove(tui->files_win, qrow, gx);
			wattron(tui->files_win, A_DIM);
			waddnstr(tui->files_win, ghost, max_input_w - gx);
			wattroff(tui->files_win, A_DIM);
		}
	}
	wmove(tui->files_win, qrow, 1 + tui->quickshell_cursor);
	leaveok(tui->files_win, FALSE);
	curs_set(2);
	wnoutrefresh(tui->files_win);
}

/**
 * @brief Renders a complete frame of the TUI.
 *
 * Draws the horizontal divider, file buffer, shell buffer, and any active
 * overlay (preview, goto, rename, quickshell, info). Falls back to a
 * "terminal too small" message when dimensions are below the minimum.
 *
 * @param tui The TUI instance.
 */
void tui_render(TUI *tui) {
	int files_render_w;
	int preview_w;
	int cwdlen;
	int cwd_avail;
	int jump_x;

	if (!tui) { return; }

	if (tui->max_x < MIN_COLS || tui->max_y < MIN_ROWS) {
		erase();
		attron(A_BOLD);
		mvprintw(0, 0, "Terminal size is too small!  Requires >= %dx%d", MIN_COLS, MIN_ROWS);
		attroff(A_BOLD);
		mvprintw(1, 0, "Current: %dx%d", tui->max_x, tui->max_y);
		refresh();
		return;
	}

	if (tui->active_buffer != BUFFER_SHELL &&
	    !tui->rename_mode && !tui->quickshell_mode && !tui->goto_mode) {
		curs_set(0);
	}

	erase();

	move(tui->divider_y, 0);
	for (int i = 0; i < tui->max_x; i++) { addch(ACS_HLINE); }

	move(tui->max_y - 1, 0);
	clrtoeol();
	wnoutrefresh(stdscr);

	files_render_w = tui->max_x;
	if (tui->preview_mode && tui->max_x > 8) {
		preview_w = tui->max_x / 2;
		files_render_w = tui->max_x - preview_w - 1;
	}

	files_render(&tui->files, tui->files_win, tui->files_height, files_render_w,
	             tui->active_buffer == BUFFER_FILES);

	if (tui->file_cmd_len > 0 && tui->files_win) {
		cwd_avail = files_render_w - 9;
		cwdlen = (int)strlen(tui->files.cwd);
		if (cwdlen > cwd_avail) { cwdlen = cwd_avail; }
		jump_x = 9 + cwdlen + 1;
		if (jump_x < files_render_w) {
			wattron(tui->files_win, COLOR_PAIR(CP_PROMPT) | A_BOLD);
			mvwprintw(tui->files_win, 0, jump_x, "%s", tui->file_cmd_buf);
			wattroff(tui->files_win, COLOR_PAIR(CP_PROMPT) | A_BOLD);
			wnoutrefresh(tui->files_win);
		}
	}

	if (tui->preview_mode && tui->files_win) { preview_render(tui, files_render_w); }
	if (tui->goto_mode && tui->files_win) { render_goto_overlay(tui); }

	shell_render(&tui->shell, tui->shell_win,
	             tui->shell_height,
	             tui->active_buffer == BUFFER_SHELL,
	             tui->files.cwd, &tui->files);

	if (tui->rename_mode && tui->files_win && tui->files.entry_count > 0) {
		render_rename_overlay(tui);
	}
	if (tui->quickshell_mode && tui->files_win) {
		render_quickshell_overlay(tui, files_render_w);
	}
	if (tui->info_mode && tui->info_win) {
		info_draw(tui);
		show_panel(tui->info_panel);
	}
	if (tui->visual_mode && tui->files_win) {
		render_visual_indicator(tui);
	}
	update_panels();

	if (tui->rename_mode && tui->files_win && tui->files.entry_count > 0) {
		wnoutrefresh(tui->files_win);
	} else if (tui->quickshell_mode && tui->files_win) {
		wnoutrefresh(tui->files_win);
	} else if (tui->goto_mode && tui->files_win) {
		wnoutrefresh(tui->files_win);
	} else if (tui->active_buffer == BUFFER_SHELL && tui->shell_win) {
		wnoutrefresh(tui->shell_win);
	}

	doupdate();
}

/* Renders the F1 help overlay into info_win. */
static void info_draw(TUI *tui) {
	int w;
	int row;
	int mid;

	werase(tui->info_win);
	box(tui->info_win, 0, 0);

	w = tui->max_x - 4;
	row = 1;
	mid = (tui->max_x / 4 < 50) ? 50 : tui->max_x / 4;

#define IROW(l, r) \
	mvwprintw(tui->info_win, row, 2, "%s", (l)); \
	if ((r)[0]) { mvwprintw(tui->info_win, row, 2 + mid, "%s", (r)); } \
	row++;

	wattron(tui->info_win, A_BOLD);
	mvwprintw(tui->info_win, row++, 2, "Quickfish Help");
	wattroff(tui->info_win, A_BOLD);

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "Navigation");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  j / Down   : move down",                "  k / Up     : move up");
	IROW("  h / Left   : go to parent dir",         "  l / Right  : enter dir or open file");
	IROW("  Enter      : enter dir or open file",   "  Ctrl+D / U : half-page down / up");

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "Files");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  g          : go to path or name",       "  <num>g     : go to dir by index");
	IROW("  ..         : previous directory",       "  r          : rename selected entry");
	IROW("  :          : quickshell (inline cmd)",  "  F1         : open this help");
	IROW("  s          : toggle-select item",       "  v          : sweep/visual select mode");
	IROW("  d          : trash selected / selection", "  D          : permanently delete");
	IROW("  u / U      : undo / redo last op",      "  m          : mark file for move");
	IROW("  M          : mark selection for move",  "  Ctrl+C     : clear move register");
	IROW("  p          : paste register here",      "  P          : paste into selected dir");

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "Preview / Panes");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  Ctrl+P     : open preview pane",        "  p (preview): close preview pane");
	IROW("  K / S-Down : focus files pane",         "  J / S-Up   : focus shell pane");
	IROW("  L / S-Right: focus next pane",          "  H / S-Left : focus previous pane");

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "Shell Commands");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  q          : quit",                      "  cd <path>  : change directory");
	IROW("  cd <num>/<num>: cd to dir by index",    "  ..         : jump to previous dir");
	IROW("  s <file>   : stat a file or index",     "  r / rm <n> : remove by index or name");

#undef IROW

	wattron(tui->info_win, A_DIM);
	mvwprintw(tui->info_win, tui->max_y - 2, 2, "%-*.*s", w, w, "Press any key to close.");
	wattroff(tui->info_win, A_DIM);

	wnoutrefresh(tui->info_win);
}

/* Toggles the F1 info overlay */
static void info_toggle(TUI *tui) {
	if (tui->info_mode) {
		tui->info_mode = 0;
		tui->active_buffer = tui->prev_buffer;
		hide_panel(tui->info_panel);
		return;
	}
	tui->prev_buffer = tui->active_buffer;
	tui->active_buffer = BUFFER_INFO;
	tui->info_mode = 1;
	if (!tui->info_win) {
		tui->info_win = newwin(tui->max_y, tui->max_x, 0, 0);
		tui->info_panel = new_panel(tui->info_win);
	}
}

/* Activates goto mode and clears the input buffer. */
static void goto_begin(TUI *tui) {
	tui->goto_mode = 1;
	tui->goto_len = 0;
	tui->goto_buf[0] = '\0';
}

/* Cancels goto mode and clears the input buffer. */
static void goto_cancel(TUI *tui) {
	tui->goto_mode = 0;
	tui->goto_len = 0;
	tui->goto_buf[0] = '\0';
}

/**
 * @brief Resolves and navigates to the path in the goto buffer.
 *
 * Resolution order: exact absolute/relative path, numeric index jump,
 * exact name match in the current (or specified) directory, then prefix
 * match. Navigates into directories. Selecting files is not supported.
 *
 * @param tui The TUI instance.
 */
static void goto_confirm(TUI *tui) {
	char target[PATH_MAX];
	struct stat st;
	char dir_buf[PATH_MAX];
	char *last_slash;
	const char *search_name;
	char search_dir[PATH_MAX];
	char *endp;
	long idx;
	DIR *d;
	char exact[MAX_FILENAME];
	char prefix_m[MAX_FILENAME];
	int slen;
	struct dirent *de;
	const char *match;
	char resolved[PATH_MAX];
	const char *use;
	DirEntry *e;

	if (tui->goto_len == 0) { return; }

	if (tui->goto_buf[0] == '/') {
		strncpy(target, tui->goto_buf, PATH_MAX - 1);
		target[PATH_MAX - 1] = '\0';
	} else if (strcmp(tui->files.cwd, "/") == 0) {
		snprintf(target, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), tui->goto_buf);
	} else {
		path_join(target, PATH_MAX, tui->files.cwd, tui->goto_buf);
	}

	if (stat(target, &st) == 0) {
		if (S_ISDIR(st.st_mode)) { files_load_directory_tracked(tui, target); }
		return;
	}

	strncpy(dir_buf, target, PATH_MAX - 1);
	dir_buf[PATH_MAX - 1] = '\0';

	last_slash = strrchr(dir_buf, '/');

	if (last_slash && last_slash != dir_buf) {
		*last_slash = '\0';
		snprintf(search_dir, sizeof(search_dir), "%s", dir_buf);
		search_name = last_slash + 1;
	} else {
		snprintf(search_dir, sizeof(search_dir), "%s", tui->files.cwd);
		search_name = tui->goto_buf;
	}

	if (!strchr(tui->goto_buf, '/')) {
		idx = strtol(search_name, &endp, 10);
		if (*endp == '\0' && idx > 0) {
			for (int i = 0; i < tui->files.entry_count; i++) {
				if (tui->files.entries[i].index != (int)idx) { continue; }
				e = &tui->files.entries[i];
				if (e->type == ENTRY_DIR) { files_change_dir_tracked(tui, e->name); }
				return;
			}
			return;
		}
	}

	d = opendir(search_dir);
	if (!d) { return; }

	exact[0] = '\0';
	prefix_m[0] = '\0';
	slen = (int)strlen(search_name);

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') { continue; }
		if (strcasecmp(de->d_name, search_name) == 0 && !exact[0]) {
			snprintf(exact, sizeof(exact), "%s", de->d_name);
		} else if (slen > 0 &&
		           strncasecmp(de->d_name, search_name, slen) == 0 &&
		           !prefix_m[0]) {
			snprintf(prefix_m, sizeof(prefix_m), "%s", de->d_name);
		}
	}
	closedir(d);

	match = exact[0] ? exact : prefix_m;
	if (!match[0]) { return; }

	use = realpath(search_dir, resolved) ? resolved : search_dir;
	if (strcmp(use, tui->files.cwd) != 0) { files_load_directory_tracked(tui, use); }

	for (int i = 0; i < tui->files.entry_count; i++) {
		if (strcasecmp(tui->files.entries[i].name, match) != 0) { continue; }
		e = &tui->files.entries[i];
		if (e->type == ENTRY_DIR) { files_change_dir_tracked(tui, e->name); }
		return;
	}
}

/**
 * @brief Tab-completes the current goto buffer against the filesystem.
 *
 * Splits the buffer into a directory part and a name prefix, then calls
 * complete_in_dir() to find a match. On success the buffer is updated in
 * place with the completed name.
 *
 * @param tui The TUI instance.
 */
static void goto_tab_complete(TUI *tui) {
	char dir_part[PATH_MAX];
	char name_part[MAX_FILENAME];
	const char *last_slash;
	char search_dir[PATH_MAX];
	char completed[MAX_FILENAME];
	int count;

	last_slash = strrchr(tui->goto_buf, '/');
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

	count = complete_in_dir(search_dir, name_part, completed, sizeof(completed));
	if (count == 0 || completed[0] == '\0') { return; }

	if (dir_part[0] == '\0') {
		strncpy(tui->goto_buf, completed, GOTO_BUF_MAX - 1);
		tui->goto_buf[GOTO_BUF_MAX - 1] = '\0';
	} else {
		path_join(tui->goto_buf, GOTO_BUF_MAX, dir_part, completed);
	}
	tui->goto_len = (int)strlen(tui->goto_buf);
}

/* Activates rename mode, pre-filling the buffer with the selected entry's name. */
static void rename_begin(TUI *tui) {
	DirEntry *e;

	if (tui->files.entry_count == 0) { return; }
	e = &tui->files.entries[tui->files.selected];
	tui->rename_mode = 1;
	strncpy(tui->rename_buf, e->name, RENAME_BUF_MAX - 1);
	tui->rename_buf[RENAME_BUF_MAX - 1] = '\0';
	tui->rename_len = (int)strlen(tui->rename_buf);
	tui->rename_cursor = tui->rename_len;
}

/**
 * @brief Applies the rename buffer to the selected entry and records an undo op.
 *
 * No-ops if the name is unchanged or the buffer is empty. Pushes an OP_RENAME
 * onto the undo stack, evicting the oldest entry if the stack is full.
 *
 * @param tui The TUI instance.
 */
static void rename_commit(TUI *tui) {
	DirEntry *e;
	char old_path[PATH_MAX];
	char new_path[PATH_MAX];
	UndoOp op;

	if (tui->rename_len == 0 || tui->files.entry_count == 0) { return; }
	e = &tui->files.entries[tui->files.selected];
	if (strcmp(e->name, tui->rename_buf) == 0) { return; }

	if (strcmp(tui->files.cwd, "/") == 0) {
		snprintf(old_path, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), e->name);
		snprintf(new_path, PATH_MAX, "/%.*s", (int)(PATH_MAX - 2), tui->rename_buf);
	} else {
		path_join(old_path, PATH_MAX, tui->files.cwd, e->name);
		path_join(new_path, PATH_MAX, tui->files.cwd, tui->rename_buf);
	}
	rename(old_path, new_path);

	op.type = OP_RENAME;
	strncpy(op.cwd, tui->files.cwd, PATH_MAX - 1);
	op.cwd[PATH_MAX - 1] = '\0';
	strncpy(op.old_name, e->name, MAX_FILENAME - 1);
	op.old_name[MAX_FILENAME - 1] = '\0';
	strncpy(op.new_name, tui->rename_buf, MAX_FILENAME - 1);
	op.new_name[MAX_FILENAME - 1] = '\0';
	op.trash_path[0] = '\0';

	push_undo(&tui->files, op);
	
	files_load_directory(&tui->files, tui->files.cwd);
}

/* Cancels rename mode and hides the cursor. */
static void rename_cancel(TUI *tui) {
	tui->rename_mode = 0;
	tui->rename_len = 0;
	tui->rename_cursor = 0;
	tui->rename_buf[0] = '\0';
	curs_set(0);
}

/* Dispatches a keypress while rename mode is active. */
static void handle_rename_input(TUI *tui, int ch) {
	switch (ch) {
	case 27:
		rename_cancel(tui);
		break;
	case '\n': case '\r': case KEY_ENTER:
		rename_commit(tui);
		rename_cancel(tui);
		break;
	default:
		edit_handle_ch(tui->rename_buf, &tui->rename_len, &tui->rename_cursor, RENAME_BUF_MAX, ch);
		break;
	}
}

/* Dispatches a keypress while goto mode is active. */
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
		if (tui->goto_len > 0) { tui->goto_buf[--tui->goto_len] = '\0'; }
		break;
	default:
		if (ch >= 32 && ch < 127 && tui->goto_len < GOTO_BUF_MAX - 1) {
			tui->goto_buf[tui->goto_len++] = (unsigned char)ch;
			tui->goto_buf[tui->goto_len] = '\0';
		}
		break;
	}
}

/* Activates quickshell mode and clears the input buffer. */
static void quickshell_begin(TUI *tui) {
	tui->quickshell_mode = 1;
	tui->quickshell_len = 0;
	tui->quickshell_cursor = 0;
	tui->quickshell_buf[0] = '\0';
}

/* Cancels quickshell mode, clears the buffer, and hides the cursor. */
static void quickshell_cancel(TUI *tui) {
	tui->quickshell_mode = 0;
	tui->quickshell_len = 0;
	tui->quickshell_cursor = 0;
	tui->quickshell_buf[0] = '\0';
	curs_set(0);
}

/**
 * @brief Commits the quickshell buffer to the shell and executes it.
 *
 * Copies the buffer into shell.current_input, cancels quickshell mode,
 * then calls execute_given(). If the command requests rm confirmation the
 * focus is moved to the shell pane so the user can answer the prompt.
 *
 * @param tui The TUI instance.
 */
static void quickshell_execute(TUI *tui) {
	if (tui->quickshell_len == 0) { quickshell_cancel(tui); return; }
	strncpy(tui->shell.current_input, tui->quickshell_buf, SHELL_MAX_INPUT - 1);
	tui->shell.current_input[SHELL_MAX_INPUT - 1] = '\0';
	tui->shell.input_pos = tui->quickshell_len;
	quickshell_cancel(tui);
	execute_given(&tui->shell, &tui->files, tui->files_win, tui->shell_win);
	if (tui->shell.quit_requested) { tui->running = 0; return; }
	if (tui->shell.rm_confirm_mode) {
		tui->active_buffer = BUFFER_SHELL;
		return;
	}
	refresh_files_buffer(&tui->files);
}

/* Dispatches a keypress while quickshell mode is active. */
static void handle_quickshell_input(TUI *tui, int ch) {
	switch (ch) {
	case 27:
		quickshell_cancel(tui);
		break;
	case '\n': case '\r': case KEY_ENTER:
		quickshell_execute(tui);
		break;
	default:
		edit_handle_ch(tui->quickshell_buf, &tui->quickshell_len, &tui->quickshell_cursor, SHELL_MAX_INPUT, ch);
		break;
	}
}

/**
 * @brief Shows a modal confirmation dialog for delete or trash operations.
 *
 * Builds a popup sized to the action description, waits for y/n/Esc,
 * then calls the appropriate files_delete_* function if confirmed.
 * Forces a full redraw after the popup is dismissed.
 *
 * @param tui  The TUI instance.
 * @param mode DEL_TRASH to move to trash, DEL_PERM to permanently delete.
 */
static void render_delete_confirm(TUI *tui, DeleteMode mode) {
	const char *action;
	const char *warning;
	char target[52];
	char body[80];
	int body_len;
	int warn_len;
	int w_width;
	int w_height;
	int w_y;
	int w_x;
	int confirmed;
	int warn_cp;
	int ch;
	DirEntry *e;
	WINDOW *popup;

	e = tui->files.entry_count > 0 ? &tui->files.entries[tui->files.selected] : NULL;

	if (mode == DEL_TRASH) {
		action = "Trash";
	} else {
		action = "Permanently delete";
	}

	if (tui->files.sel_count > 1) {
		snprintf(target, sizeof(target), "%d items", tui->files.sel_count);
	} else if (e) {
		const char *kind = (e->type == ENTRY_DIR) ? "directory" : "file";
		snprintf(target, sizeof(target), "'%.30s' (%s)", e->name, kind);
	} else {
		return;
	}

	int has_dir = (tui->files.sel_count > 1) || (e && e->type == ENTRY_DIR);

	if (mode == DEL_TRASH) {
		warning = has_dir ? "Directories will be deleted recursively." : NULL;
	} else {
		warning = has_dir ? "Recursive! This cannot be undone." : "This cannot be undone.";
	}

	snprintf(body, sizeof(body), "%s %s?", action, target);
	body_len = (int)strlen(body);
	warn_len = warning ? (int)strlen(warning) : 0;
	w_width = body_len + 6;
	if (warn_len + 6 > w_width) { w_width = warn_len + 6; }
	if (w_width < 46) { w_width = 46; }
	w_height = warning ? 6 : 5;
	w_y = (tui->max_y - w_height) / 2;
	w_x = (tui->max_x - w_width) / 2;
	if (w_y < 0) { w_y = 0; }
	if (w_x < 0) { w_x = 0; }

	popup = newwin(w_height, w_width, w_y, w_x);
	if (!popup) { return; }

	warn_cp = (mode == DEL_PERM) ? CP_DELETE_CONFIRM : CP_TRASH_CONFIRM;

	wbkgd(popup, COLOR_PAIR(1));
	wattron(popup, COLOR_PAIR(1) | A_BOLD);
	box(popup, 0, 0);
	wattroff(popup, COLOR_PAIR(1) | A_BOLD);

	wattron(popup, COLOR_PAIR(1) | A_BOLD);
	mvwprintw(popup, 1, 2, "%s", body);
	wattroff(popup, COLOR_PAIR(1) | A_BOLD);

	if (warning) {
		wattron(popup, COLOR_PAIR(warn_cp) | A_BOLD);
		mvwprintw(popup, 2, 2, "%s", warning);
		wattroff(popup, COLOR_PAIR(warn_cp) | A_BOLD);
	}

	wattron(popup, COLOR_PAIR(1) | A_DIM);
	mvwaddstr(popup, w_height - 2, 2, "[y] confirm   [n / Esc] cancel");
	wattroff(popup, COLOR_PAIR(1) | A_DIM);

	wnoutrefresh(popup);
	doupdate();

	keypad(popup, TRUE);
	set_escdelay(25);
	confirmed = 0;
	while (1) {
		ch = wgetch(popup);
		if (ch == 'y' || ch == 'Y') {
			confirmed = 1;
			break;
		}
		if (ch == 'n' || ch == 'N' || ch == 27) {
			break;
		}
	}

	delwin(popup);

	if (confirmed) {
		if (mode == DEL_TRASH) {
			files_delete_to_trash(&tui->files, &tui->shell);
		} else {
			files_delete_permanent(&tui->files, &tui->shell);
		}
		refresh_files_buffer(&tui->files);
	}

	clearok(curscr, TRUE);
	touchwin(tui->files_win);
	touchwin(tui->shell_win);
}

/**
 * @brief Shows a modal confirmation dialog before pasting the move register.
 *
 * Pastes into dest if confirmed. Forces a full redraw on dismissal.
 *
 * @param tui  The TUI instance.
 * @param dest Destination path for the pending move batch.
 */
static void move_paste_confirm(TUI *tui, const char *dest) {
	char body[80];
	int body_len;
	int w_width;
	int w_height;
	int w_y;
	int w_x;
	int confirmed;
	int ch;
	WINDOW *popup;

	snprintf(body, sizeof(body), "Move %d file(s) to '%.*s'?",
	         tui->files.move_reg.count, 40, dest);
	body_len = (int)strlen(body);
	w_width = body_len + 6;
	if (w_width < 46) { w_width = 46; }
	w_height = 5;
	w_y = (tui->max_y - w_height) / 2;
	w_x = (tui->max_x - w_width) / 2;
	if (w_y < 0) { w_y = 0; }
	if (w_x < 0) { w_x = 0; }

	popup = newwin(w_height, w_width, w_y, w_x);
	if (!popup) { return; }

	wbkgd(popup, COLOR_PAIR(1));
	wattron(popup, COLOR_PAIR(1) | A_BOLD);
	box(popup, 0, 0);
	wattroff(popup, COLOR_PAIR(1) | A_BOLD);
	wattron(popup, COLOR_PAIR(1) | A_BOLD);
	mvwprintw(popup, 1, 2, "%s", body);
	wattroff(popup, COLOR_PAIR(1) | A_BOLD);
	wattron(popup, COLOR_PAIR(1) | A_DIM);
	mvwaddstr(popup, w_height - 2, 2, "[y] confirm   [n / Esc] cancel");
	wattroff(popup, COLOR_PAIR(1) | A_DIM);
	wnoutrefresh(popup);
	doupdate();

	keypad(popup, TRUE);
	set_escdelay(25);
	confirmed = 0;
	while (1) {
		ch = wgetch(popup);
		if (ch == 'y' || ch == 'Y') { confirmed = 1; break; }
		if (ch == 'n' || ch == 'N' || ch == 27) { break; }
	}
	delwin(popup);

	if (confirmed) {
		files_move_paste(&tui->files, &tui->shell, dest);
	}

	clearok(curscr, TRUE);
	touchwin(tui->files_win);
	touchwin(tui->shell_win);
}

/**
 * @brief Handles input while the preview pane is focused.
 *
 * Supports scrolling (j/k, Ctrl+D/U), opening quickshell (':'), and
 * toggling the preview pane off ('p'). Quickshell keypresses are forwarded
 * immediately if quickshell mode is already active.
 *
 * @param tui The TUI instance.
 * @param ch  ncurses key value.
 */
void tui_handle_preview_input(TUI *tui, int ch) {
	if (tui->quickshell_mode) { handle_quickshell_input(tui, ch); return; }

	switch (ch) {
	case 'j': case KEY_DOWN:
		tui->preview_scroll++;
		break;
	case 'k': case KEY_UP:
		if (tui->preview_scroll > 0) { tui->preview_scroll--; }
		break;
	case 4:
		tui->preview_scroll += tui->files_height / 2;
		break;
	case 21:
		tui->preview_scroll -= tui->files_height / 2;
		if (tui->preview_scroll < 0) { tui->preview_scroll = 0; }
		break;
	case ':':
		quickshell_begin(tui);
		break;
	case 'p':
		tui->preview_mode = 0;
		tui->active_buffer = BUFFER_FILES;
		tui->files.col_offset = tui->preview_saved_col_offset;
		break;
	}
}

/**
 * @brief Handles input while the files pane is focused.
 *
 * Forwards to the active overlay handler (rename, goto, quickshell) if one
 * is open. Otherwise processes navigation, selection, deletion, move/paste,
 * undo/redo, and mode-toggle keys. Digits accumulate in file_cmd_buf for
 * index-based jumping, and ".." jumps to the previous directory.
 *
 * @param tui The TUI instance.
 * @param ch  ncurses key value.
 */
void tui_handle_files_input(TUI *tui, int ch) {
	DirEntry *e;
	long idx;
	char *endp;
	char dest[PATH_MAX];

	if (tui->rename_mode) { handle_rename_input(tui, ch); return; }
	if (tui->goto_mode) { handle_goto_input(tui, ch); return; }
	if (tui->quickshell_mode) { handle_quickshell_input(tui, ch); return; }

	if (handle_file_cmd_input(tui, ch)) {
		return;
	}

	if (tui->file_cmd_len > 0) {
		if (tui->file_cmd_buf[0] == '.') {
			file_cmd_clear(tui);
		} else if (ch != 'g') {
			file_cmd_clear(tui);
		}
	}

	switch (ch) {
	case 27:
		if (tui->visual_mode) {
			tui->visual_mode = 0;
		}
		break;

	case 'j': case KEY_DOWN:
		files_select_next(&tui->files);
		if (tui->visual_mode && tui->files.entry_count > 0) {
			int vi = tui->files.selected;
			if (!tui->files.multi_sel[vi]) {
				tui->files.multi_sel[vi] = 1;
				tui->files.sel_count++;
			}
		}
		break;

	case 'k': case KEY_UP:
		files_select_prev(&tui->files);
		if (tui->visual_mode && tui->files.entry_count > 0) {
			int vi = tui->files.selected;
			if (!tui->files.multi_sel[vi]) {
				tui->files.multi_sel[vi] = 1;
				tui->files.sel_count++;
			}
		}
		break;

	case 4:
		files_select_next_n(&tui->files, tui->files_height / 2);
		if (tui->visual_mode && tui->files.entry_count > 0) {
			int vi = tui->files.selected;
			if (!tui->files.multi_sel[vi]) {
				tui->files.multi_sel[vi] = 1;
				tui->files.sel_count++;
			}
		}
		break;

	case 21:
		files_select_prev_n(&tui->files, tui->files_height / 2);
		if (tui->visual_mode && tui->files.entry_count > 0) {
			int vi = tui->files.selected;
			if (!tui->files.multi_sel[vi]) {
				tui->files.multi_sel[vi] = 1;
				tui->files.sel_count++;
			}
		}
		break;

	case 'h': case KEY_LEFT:
		files_change_dir_tracked(tui, "..");
		break;

	case 'l': case KEY_RIGHT:
	case '\n': case '\r': case KEY_ENTER:
		if (tui->files.entry_count == 0) { break; }
		e = &tui->files.entries[tui->files.selected];
		if (e->type == ENTRY_DIR) {
			files_change_dir_tracked(tui, e->name);
		} else {
			files_open_selected(&tui->files, &tui->shell, tui->files_win, tui->shell_win);
		}
		break;

	case 'g':
		if (tui->file_cmd_len > 0 && tui->file_cmd_buf[0] != '.') {
			errno = 0;
			idx = strtol(tui->file_cmd_buf, &endp, 10);
			file_cmd_clear(tui);
			if (errno == 0 && *endp == '\0' && idx > 0 && idx <= tui->files.entry_count) {
				for (int i = 0; i < tui->files.entry_count; i++) {
					if (tui->files.entries[i].index != idx) { continue; }
					tui->files.selected = i;
					break;
				}
			} else if (idx > tui->files.entry_count) {
				print_to_shell(&tui->shell, "Jump index is higher then the file buffer!\n", 3);
			}
		} else {
			file_cmd_clear(tui);
			goto_begin(tui);
		}
		break;

	case 'r':
		rename_begin(tui);
		break;

	case 's':
		if (tui->files.entry_count > 0) {
			files_toggle_select(&tui->files, tui->files.selected);
		}
		break;

	case 'v':
		if (tui->files.entry_count > 0) {
			tui->visual_mode = !tui->visual_mode;
			if (tui->visual_mode) {
				int vi = tui->files.selected;
				if (!tui->files.multi_sel[vi]) {
					tui->files.multi_sel[vi] = 1;
					tui->files.sel_count++;
				}
			}
		}
		break;

	case 'd':
		if (tui->files.entry_count > 0) {
			render_delete_confirm(tui, DEL_TRASH);
		}
		break;

	case 'D':
		if (tui->files.entry_count > 0) {
			render_delete_confirm(tui, DEL_PERM);
		}
		break;

	case 'm':
		if (tui->files.entry_count > 0) {
			files_move_mark(&tui->files, &tui->shell, tui->files.selected);
		}
		break;

	case 'M':
		if (tui->files.entry_count > 0) {
			files_move_mark_all(&tui->files, &tui->shell);
		}
		break;

	case 'u':
		files_undo(&tui->files, &tui->shell);
		refresh_files_buffer(&tui->files);
		break;

	case 'U':
		files_redo(&tui->files, &tui->shell);
		refresh_files_buffer(&tui->files);
		break;

	case ':':
		quickshell_begin(tui);
		break;

	case 'p':
		move_paste_confirm(tui, tui->files.cwd);
		refresh_files_buffer(&tui->files);
		break;

	case 0x10:
		tui->preview_mode = !tui->preview_mode;
		if (tui->preview_mode) {
			tui->preview_saved_col_offset = tui->files.col_offset;
		} else {
			tui->files.col_offset = tui->preview_saved_col_offset;
			if (tui->active_buffer == BUFFER_PREVIEW) {
				tui->active_buffer = BUFFER_FILES;
			}
		}
		break;

	case 'P':
		if (tui->files.entry_count > 0 && tui->files.move_reg.count > 0) {
			e = &tui->files.entries[tui->files.selected];
			if (e->type == ENTRY_DIR) {
				path_join(dest, PATH_MAX, tui->files.cwd, e->name);
				move_paste_confirm(tui, dest);
				refresh_files_buffer(&tui->files);
			}
		}
		break;
	}
}

/**
 * @brief Handles input while the shell pane is focused.
 *
 * When rm_confirm_mode is active, Enter commits the typed answer and
 * executes the pending command If confirmed.
 * Tab triggers tab-completion, and all other
 * keys are forwarded to shell_handle_char().
 *
 * @param tui The TUI instance.
 * @param ch  ncurses key value.
 */
void tui_handle_shell_input(TUI *tui, int ch) {
	char ans;

	if (tui->shell.rm_confirm_mode) {
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			ans = tui->shell.current_input[0];
			memset(tui->shell.current_input, 0, SHELL_MAX_INPUT);
			tui->shell.input_pos = 0;
			if (ans == 'y' || ans == 'Y') {
				snprintf(tui->shell.current_input, sizeof(tui->shell.current_input), "%s",
				         tui->shell.rm_pending_cmd);
				tui->shell.input_pos = (int)strlen(tui->shell.current_input);
				execute_given(&tui->shell, &tui->files, tui->files_win, tui->shell_win);
				if (tui->shell.quit_requested) { tui->running = 0; return; }
				refresh_files_buffer(&tui->files);
			} else {
				tui->shell.rm_confirm_mode = 0;
				print_to_shell(&tui->shell, "Aborted.", 1);
			}
		} else if (ch == 27) {
			tui->shell.rm_confirm_mode = 0;
			memset(tui->shell.current_input, 0, SHELL_MAX_INPUT);
			tui->shell.input_pos = 0;
			print_to_shell(&tui->shell, "Aborted.", 1);
		} else {
			shell_handle_char(&tui->shell, ch);
		}
		return;
	}

	if (ch == '\n' || ch == '\r') {
		execute_given(&tui->shell, &tui->files, tui->files_win, tui->shell_win);
		if (tui->shell.quit_requested) { tui->running = 0; return; }
		if (!tui->shell.rm_confirm_mode) {
			refresh_files_buffer(&tui->files);
		}
		return;
	}
	if (ch == '\t') {
		shell_tab_complete(&tui->shell, tui->files.cwd);
		return;
	}
	shell_handle_char(&tui->shell, ch);
}

/**
 * @brief Top-level input dispatcher.
 *
 * Handles global keys.
 * Delegates to the focused pane's handler for everything else. Input is
 * silently discarded if the terminal is below the minimum size.
 *
 * @param tui The TUI instance.
 * @param ch  ncurses key value.
 */
void tui_handle_input(TUI *tui, int ch) {
	if (tui->max_x < MIN_COLS || tui->max_y < MIN_ROWS) {
		if (ch == 'q' || ch == 'Q') { tui->running = 0; }
		return;
	}

	if (ch == '\f') {
		tui->shell.last_output[0] = '\0';
		tui->shell.last_cmd[0] = '\0';
		tui->shell.last_result[0] = '\0';
		return;
	}

	if (ch == 3) { // Ctrl+C
		if (tui->files.move_reg.count > 0) {
			files_move_clear(&tui->files, &tui->shell);
		}
		return;
	}

	if (tui->active_buffer == BUFFER_INFO) {
		info_toggle(tui);
		return;
	}

	if (!tui->goto_mode && !tui->rename_mode && !tui->quickshell_mode) {
		if (ch == 'J' || ch == KEY_SF) { tui->visual_mode = 0; tui->active_buffer = BUFFER_SHELL; return; }
		if (ch == 'K' || ch == KEY_SR) { tui->active_buffer = BUFFER_FILES; curs_set(0); return; }
		if ((ch == 'L' || ch == KEY_SRIGHT) && tui->preview_mode && tui->active_buffer == BUFFER_FILES) {
			tui->active_buffer = BUFFER_PREVIEW;
			return;
		}
		if ((ch == 'L' || ch == KEY_SRIGHT) && tui->active_buffer == BUFFER_PREVIEW) {
			tui->active_buffer = BUFFER_SHELL;
			return;
		}
		if ((ch == 'H' || ch == KEY_SLEFT) &&
		    (tui->active_buffer == BUFFER_PREVIEW || tui->active_buffer == BUFFER_SHELL)) {
			tui->active_buffer = BUFFER_FILES;
			curs_set(0);
			return;
		}
		if (ch == KEY_F(1)) { info_toggle(tui); return; }
	}

	if (tui->active_buffer == BUFFER_SHELL) {
		tui_handle_shell_input(tui, ch);
	} else if (tui->active_buffer == BUFFER_PREVIEW) {
		tui_handle_preview_input(tui, ch);
	} else {
		tui_handle_files_input(tui, ch);
	}
}
