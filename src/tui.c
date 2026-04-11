#include <dirent.h>
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
static void render_quickshell_overlay(TUI *tui, int files_render_w);
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

volatile int needs_resize = 0;

static void handle_resize(int sig) { (void)sig; needs_resize = 1; }

typedef struct {
	int is_empty;
	int is_dir;
	mode_t mode;
	char name[MAX_FILENAME];
} PreviewEntry;

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
		buf[(*cursor)++] = (char)ch;
		(*len)++;
	}
}

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

void tui_cleanup(TUI *tui) {
	if (!tui) { return; }
	if (tui->files_win) { delwin(tui->files_win); }
	if (tui->shell_win) { delwin(tui->shell_win); }
	if (tui->info_panel) { del_panel(tui->info_panel); }
	if (tui->info_win) { delwin(tui->info_win); }
	endwin();
	free(tui);
}

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
	if (tui->info_win)  { clearok(tui->info_win,  TRUE); touchwin(tui->info_win);  }

	wrefresh(stdscr);
}

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
		strncpy(names[total].name, de->d_name, MAX_FILENAME - 1);
		names[total].name[MAX_FILENAME - 1] = '\0';
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
	mvwprintw(tui->files_win, 0, list_w + 1, "%-*.*s",
	          preview_w - 1, preview_w - 1, preview_title);
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

void tui_render(TUI *tui) {
	int files_render_w;
	int preview_w;
	int cwdlen;
	int cwd_avail;
	int jump_x;

	if (!tui) { return; }

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

	files_render(&tui->files, tui->files_win,
	             tui->files_height, files_render_w,
	             tui->active_buffer == BUFFER_FILES);

	if (tui->index_jump_len > 0 && tui->files_win) {
		cwd_avail = files_render_w - 9;
		cwdlen = (int)strlen(tui->files.cwd);
		if (cwdlen > cwd_avail) { cwdlen = cwd_avail; }
		jump_x = 9 + cwdlen + 1;
		if (jump_x < files_render_w) {
			wattron(tui->files_win, COLOR_PAIR(CP_PROMPT) | A_BOLD);
			mvwprintw(tui->files_win, 0, jump_x, "%s", tui->index_jump_buf);
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

	update_panels();
	doupdate();
}

/* Draws the help screen content into info_win. Window must already exist. */
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
	mvwprintw(tui->info_win, row++, 2, "Sourcefish - a tui filemanager designed to remove the need for GUI alternatives ");
	wattroff(tui->info_win, A_BOLD);
	mvwprintw(tui->info_win, row++, 2, "Released under GPL-2.0 license.");
	row++;

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "Navigation");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  j / Down   : move down",                "  k / Up     : move up");
	IROW("  h / Left   : go to parent dir",         "  l / Right  : enter dir or open file");
	IROW("  Enter      : enter dir or open file",   "  Ctrl+D / U : half-page down / up");
	row++;

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "File Manager");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  g          : go to path or name",       "  r          : rename selected entry");
	IROW("  p          : toggle preview pane",      "  :          : quickshell (inline cmd)");
	IROW("  F1         : this help screen",         "");
	row++;

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "Pane Switching");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  K / S-Down : focus files pane",         "  J / S-Up   : focus shell pane");
	IROW("  L / S-Right: focus next pane",          "  H / S-Left : focus previous pane");
	row++;

	wattron(tui->info_win, A_BOLD | A_UNDERLINE);
	mvwprintw(tui->info_win, row++, 2, "Shell Commands");
	wattroff(tui->info_win, A_BOLD | A_UNDERLINE);
	IROW("  <Enter>    : spawn interactive shell",  "  q          : quit");
	IROW("  cd <path>  : change directory",         "  cd <num>   : cd to dir by index");
	IROW("  <num>      : cd to dir by index",       "  ..         : jump to previous dir");
	IROW("  s <file>   : stat a file or index",     "  r / rm <n> : remove by index or name");

#undef IROW

	row++;
	wattron(tui->info_win, A_DIM);
	mvwprintw(tui->info_win, row++, 2, "Note: this page (and sourcefish itself) is still in heavy development.");
	mvwprintw(tui->info_win, row++, 2, "Things are subject to change!");
	wattroff(tui->info_win, A_DIM);

	wattron(tui->info_win, A_DIM);
	mvwprintw(tui->info_win, tui->max_y - 2, 2, "%-*.*s", w, w, "Press any key to return.");
	wattroff(tui->info_win, A_DIM);

	wnoutrefresh(tui->info_win);
}

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

void tui_run(TUI *tui) {
	int ch;
	while (tui->running) {
		if (needs_resize) {
			tui_resize_handler(tui);
			continue;
		}
		tui_render(tui);
		ch = wgetch(stdscr);
		if (ch == KEY_RESIZE) {
			needs_resize = 1;
			continue;
    }
    if (ch != ERR) {
      tui_handle_input(tui, ch);
    }
  }
}

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
		if (S_ISDIR(st.st_mode)) { files_load_directory(&tui->files, target); }
		return;
	}

	strncpy(dir_buf, target, PATH_MAX - 1);
	dir_buf[PATH_MAX - 1] = '\0';

	last_slash = strrchr(dir_buf, '/');

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
		idx = strtol(search_name, &endp, 10);
		if (*endp == '\0' && idx > 0) {
			for (int i = 0; i < tui->files.entry_count; i++) {
				if (tui->files.entries[i].index != (int)idx) { continue; }
				e = &tui->files.entries[i];
				if (e->type == ENTRY_DIR) { files_change_dir(&tui->files, e->name); }
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
			strncpy(exact, de->d_name, MAX_FILENAME - 1);
		} else if (slen > 0 &&
		           strncasecmp(de->d_name, search_name, slen) == 0 &&
		           !prefix_m[0]) {
			strncpy(prefix_m, de->d_name, MAX_FILENAME - 1);
		}
	}
	closedir(d);

	match = exact[0] ? exact : prefix_m;
	if (!match[0]) { return; }

	use = realpath(search_dir, resolved) ? resolved : search_dir;
	if (strcmp(use, tui->files.cwd) != 0) { files_load_directory(&tui->files, use); }

	for (int i = 0; i < tui->files.entry_count; i++) {
		if (strcasecmp(tui->files.entries[i].name, match) != 0) { continue; }
		e = &tui->files.entries[i];
		if (e->type == ENTRY_DIR) { files_change_dir(&tui->files, e->name); }
		return;
	}
}

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

static void rename_commit(TUI *tui) {
	DirEntry *e;
	char old_path[PATH_MAX];
	char new_path[PATH_MAX];

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
	files_load_directory(&tui->files, tui->files.cwd);
}

static void rename_cancel(TUI *tui) {
	tui->rename_mode = 0;
	tui->rename_len = 0;
	tui->rename_cursor = 0;
	tui->rename_buf[0] = '\0';
	curs_set(0);
}

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
			tui->goto_buf[tui->goto_len++] = (char)ch;
			tui->goto_buf[tui->goto_len] = '\0';
		}
		break;
	}
}

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
	if (tui->shell.rm_confirm_mode) {
		tui->active_buffer = BUFFER_SHELL;
		return;
	}
	refresh_files_buffer(&tui->files);
}

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

void tui_handle_files_input(TUI *tui, int ch) {
	DirEntry *e;
	int idx;
	char *endp;

	if (tui->rename_mode) { handle_rename_input(tui, ch); return; }
	if (tui->goto_mode) { handle_goto_input(tui, ch); return; }
	if (tui->quickshell_mode) { handle_quickshell_input(tui, ch); return; }

	if (ch >= '0' && ch <= '9') {
		if (tui->index_jump_len < (int)sizeof(tui->index_jump_buf) - 1) {
			tui->index_jump_buf[tui->index_jump_len++] = (char)ch;
			tui->index_jump_buf[tui->index_jump_len] = '\0';
		}
		return;
	}

	if (ch != 'g' && tui->index_jump_len > 0) {
		tui->index_jump_len = 0;
		tui->index_jump_buf[0] = '\0';
	}

	switch (ch) {
	case 'j': case KEY_DOWN:
		files_select_next(&tui->files);
		break;

	case 'k': case KEY_UP:
		files_select_prev(&tui->files);
		break;

	case 4:
		files_select_next_n(&tui->files, tui->files_height / 2);
		break;

	case 21:
		files_select_prev_n(&tui->files, tui->files_height / 2);
		break;

	case 'h': case KEY_LEFT:
		files_change_dir(&tui->files, "..");
		break;

	case 'l': case KEY_RIGHT:
	case '\n': case '\r': case KEY_ENTER:
		if (tui->files.entry_count == 0) { break; }
		e = &tui->files.entries[tui->files.selected];
		if (e->type == ENTRY_DIR) {
			files_change_dir(&tui->files, e->name);
		} else {
			files_open_selected(&tui->files, &tui->shell, tui->files_win, tui->shell_win);
		}
		break;

	case 'g':
		if (tui->index_jump_len > 0) {
			idx = (int)strtol(tui->index_jump_buf, &endp, 10);
			if (idx > tui->files.entry_count) {
				print_to_shell(&tui->shell, "Jump index is higher then the file buffer!\n", 3);
			}
			tui->index_jump_len = 0;
			tui->index_jump_buf[0] = '\0';
			if (*endp == '\0' && idx > 0) {
				for (int i = 0; i < tui->files.entry_count; i++) {
					if (tui->files.entries[i].index != idx) { continue; }
					tui->files.selected = i;
					break;
				}
			}
		} else {
			goto_begin(tui);
		}
		break;

	case 'r':
		rename_begin(tui);
		break;

	case ':':
		quickshell_begin(tui);
		break;

	case 'p':
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
	}
}

void tui_handle_shell_input(TUI *tui, int ch) {
	char ans;

	if (tui->shell.rm_confirm_mode) {
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			ans = tui->shell.current_input[0];
			memset(tui->shell.current_input, 0, SHELL_MAX_INPUT);
			tui->shell.input_pos = 0;
			if (ans == 'y' || ans == 'Y') {
				strncpy(tui->shell.current_input, tui->shell.rm_pending_cmd, SHELL_MAX_INPUT - 1);
				tui->shell.current_input[SHELL_MAX_INPUT - 1] = '\0';
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
		refresh_files_buffer(&tui->files);
		return;
	}
	if (ch == '\t') {
		shell_tab_complete(&tui->shell, tui->files.cwd);
		return;
	}
	shell_handle_char(&tui->shell, ch);
}

void tui_handle_input(TUI *tui, int ch) {
	if (tui->active_buffer == BUFFER_INFO) {
		info_toggle(tui);
		return;
	}

	if (!tui->goto_mode && !tui->rename_mode && !tui->quickshell_mode) {
		if (ch == 'J' || ch == KEY_SF) { tui->active_buffer = BUFFER_SHELL; return; }
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
