#include <ncurses.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tui.h"

int main(int argc, char **argv) {
	struct stat st;
	const char *start_dir;
	TUI *tui;
	int ch;

	if (argc > 1) {
		if (stat(argv[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
			fprintf(stderr, "\"%s\" is not a valid directory.\n", argv[1]);
			return 1;
		}
	}
	start_dir = (argc > 1) ? argv[1] : NULL;

	tui = tui_init(start_dir);
	if (!tui) {
		fprintf(stderr, "Failed to initialize TUI! How tf did you even achieve this??\n");
		return 1;
	}

	while (tui->running) {
		tui_resize_handler(tui);
		tui_render(tui);

		ch = getch();
		if (ch != ERR) {
			tui_handle_input(tui, ch);
		} else {
			usleep(16000);
		}
	}

	tui_cleanup(tui);

	return 0;
}
