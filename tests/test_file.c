#define _XOPEN_SOURCE 700

#include "test.h"
#include "../src/file.h"
#include "../src/main.h"
#include "../src/shell.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int unlink_cb(const char *path, const struct stat *st, int type, struct FTW *ftwbuf);
static void remove_tree(const char *path);
static void write_file(const char *path);
static void init_shell(ShellBuffer *shell);

static int unlink_cb(const char *path, const struct stat *st, int type, struct FTW *ftwbuf) {
	(void)st;
	(void)type;
	(void)ftwbuf;
	return remove(path);
}

static void remove_tree(const char *path) {
	nftw(path, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static void write_file(const char *path) {
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd >= 0) {
		close(fd);
	}
}

static void init_shell(ShellBuffer *shell) {
	memset(shell, 0, sizeof(*shell));
}

void test_file() {
	char root[] = "/tmp/quickfish-test-file-XXXXXX";
	char path[PATH_MAX];
	char src[PATH_MAX];
	char dst[PATH_MAX];
	char out[MAX_FILENAME];
	char selected[PATH_MAX];
	FilesBuffer files;
	ShellBuffer shell;
	struct stat st;
	int count;

	SUITE("complete_in_dir");

	CHECK(mkdtemp(root) != NULL, "creates temp directory");
	if (root[0] == '\0') {
		return;
	}

	path_join(path, sizeof(path), root, "alpha");
	CHECK(mkdir(path, 0755) == 0, "creates alpha directory");

	path_join(path, sizeof(path), root, "alphabet");
	write_file(path);

	path_join(path, sizeof(path), root, "alps");
	write_file(path);

	path_join(path, sizeof(path), root, "beta");
	write_file(path);

	count = complete_in_dir(root, "al", out, sizeof(out));
	CHECK(count == 3, "counts all matching entries");
	CHECK_STR(out, "alp", "reduces matches to common prefix");

	memset(&files, 0, sizeof(files));

	SUITE("files_load_directory");

	files_load_directory(&files, root);
	CHECK(files.entry_count == 4, "loads visible directory entries");
	CHECK(files.entries[0].type == ENTRY_DIR, "sorts directories before files");
	CHECK_STR(files.entries[0].name, "alpha", "sorts entries alphabetically");

	CHECK(files_get_selected_path(&files, selected, sizeof(selected)) == 0,
	      "selected directory reports directory type");
	snprintf(path, sizeof(path), "%s/alpha", root);
	CHECK_STR(selected, path, "selected path resolves from current directory");

	SUITE("files_change_dir");

	files_change_dir(&files, "alpha");
	CHECK_STR(files.cwd, path, "changes into named directory");
	CHECK(files.entry_count == 0, "empty child directory loads cleanly");

	files_change_dir(&files, "..");
	CHECK_STR(files.cwd, root, "changes back to parent directory");

	SUITE("files_build_sel_args");

	files_load_directory(&files, root);
	for (int i = 0; i < files.entry_count; i++) {
		if (strcmp(files.entries[i].name, "alphabet") == 0 ||
		    strcmp(files.entries[i].name, "beta") == 0) {
			files_toggle_select(&files, i);
		}
	}
	CHECK(files.sel_count == 2, "tracks multiple selections");
	files_build_sel_args(&files, out, sizeof(out));
	CHECK_STR(out, "'alphabet' 'beta'", "formats selected entries as shell args");

	SUITE("move register");

	init_shell(&shell);
	files_load_directory(&files, root);
	for (int i = 0; i < files.entry_count; i++) {
		if (strcmp(files.entries[i].name, "alphabet") == 0) {
			files_move_mark(&files, &shell, i);
			break;
		}
	}
	CHECK(files.move_reg.count == 1, "marks an entry into move register");

	for (int i = 0; i < files.entry_count; i++) {
		if (strcmp(files.entries[i].name, "alphabet") == 0) {
			files_move_mark(&files, &shell, i);
			break;
		}
	}
	CHECK(files.move_reg.count == 0, "toggle unmarks existing register entry");

	SUITE("move paste and undo");

	files_load_directory(&files, root);
	init_shell(&shell);
	for (int i = 0; i < files.entry_count; i++) {
		if (strcmp(files.entries[i].name, "alphabet") == 0) {
			files_move_mark(&files, &shell, i);
			break;
		}
	}

	path_join(dst, sizeof(dst), root, "alpha");
	files_move_paste(&files, &shell, dst);
	path_join(src, sizeof(src), root, "alphabet");
	path_join(dst, sizeof(dst), root, "alpha/alphabet");
	CHECK(stat(src, &st) != 0, "move removes source entry from old directory");
	CHECK(stat(dst, &st) == 0, "move places entry in destination directory");
	CHECK(files.move_reg.count == 0, "successful paste clears move register");

	files_undo(&files, &shell);
	path_join(src, sizeof(src), root, "alphabet");
	path_join(dst, sizeof(dst), root, "alpha/alphabet");
	CHECK(stat(src, &st) == 0, "undo move restores source entry");
	CHECK(stat(dst, &st) != 0, "undo move removes destination entry");

	files_redo(&files, &shell);
	CHECK(stat(src, &st) != 0, "redo move removes restored source entry");
	CHECK(stat(dst, &st) == 0, "redo move reapplies destination entry");

	SUITE("move paste rejection");

	files_load_directory(&files, root);
	init_shell(&shell);
	for (int i = 0; i < files.entry_count; i++) {
		if (strcmp(files.entries[i].name, "alpha") == 0) {
			files_move_mark(&files, &shell, i);
			break;
		}
	}
	path_join(dst, sizeof(dst), root, "alpha");
	files_move_paste(&files, &shell, dst);
	CHECK(files.move_reg.count == 1, "rejected self-move stays in register");
	CHECK(strstr(shell.last_output + 1, "can't move 'alpha' into itself") != NULL,
	      "self-move prints clear error");

	remove_tree(root);
}
