#include "tui.h"
#include <stdio.h>
#include <unistd.h>
#include <ncurses.h>

int main(int argc, char **argv) {
    const char *start_dir = (argc > 1) ? argv[1] : NULL;

    TUI *tui = tui_init(start_dir);
    if (!tui) {
        fprintf(stderr, "Failed to initialize TUI\n");
        return 1;
    }

    while (tui->running) {
        tui_resize_handler(tui);
        tui_render(tui);

        int ch = getch();
        if (ch != ERR)
            tui_handle_input(tui, ch);
        else
            usleep(16000);
    }

    tui_cleanup(tui);

    return 0;
}
