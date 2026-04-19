#define _XOPEN_SOURCE 700

#include "test.h"
#include "../src/file.h"
#include "../src/main.h"
#include "../src/shell.h"

#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int unlink_cb(const char *path, const struct stat *st, int type, struct FTW *ftwbuf);
static void remove_tree(const char *path);
static void write_file(const char *path);
static void load_fixture(const char *root);

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

static void load_fixture(const char *root) {
	char path[PATH_MAX];

	path_join(path, sizeof(path), root, "alpha");
	mkdir(path, 0755);

	path_join(path, sizeof(path), root, "alpha/alchemy.txt");
	write_file(path);

	path_join(path, sizeof(path), root, "beta");
	mkdir(path, 0755);

	path_join(path, sizeof(path), root, "alpine.txt");
	write_file(path);
}

void test_shell() {
	char root[] = "/tmp/quickfish-test-shell-XXXXXX";
	char path[PATH_MAX];
	char alpha_dir[PATH_MAX];
	char expected[PATH_MAX * 5];
	char renamed[PATH_MAX];
	char moved_src[PATH_MAX];
	char moved_dst[PATH_MAX];
	char trashed[PATH_MAX];
	char ghost[128];
	FilesBuffer files;
	ShellBuffer shell;
	UndoOp rename_op;
	UndoOp move_op;
	UndoOp trash_op;
	int rc;

	memset(&files, 0, sizeof(files));
	memset(&shell, 0, sizeof(shell));

	SUITE("shell_add_history");

	shell_add_history(&shell, "ls");
	shell_add_history(&shell, "ls");
	shell_add_history(&shell, "pwd");
	CHECK(shell.history_count == 2, "deduplicates consecutive history entries");
	CHECK_STR(shell.history[0], "ls", "stores first history entry");
	CHECK_STR(shell.history[1], "pwd", "stores second history entry");

	SUITE("shell_handle_char");

	memset(&shell, 0, sizeof(shell));
	shell_handle_char(&shell, 'a');
	shell_handle_char(&shell, 'c');
	shell_handle_char(&shell, KEY_LEFT);
	shell_handle_char(&shell, 'b');
	CHECK_STR(shell.current_input, "abc", "inserts at cursor position");
	CHECK(shell.input_pos == 2, "tracks cursor after insertion");

	shell_handle_char(&shell, KEY_BACKSPACE);
	CHECK_STR(shell.current_input, "ac", "backspace removes previous character");
	CHECK(shell.input_pos == 1, "backspace updates cursor");

	SUITE("shell completion and ghost");

	CHECK(mkdtemp(root) != NULL, "creates shell temp directory");
	load_fixture(root);
	files_load_directory(&files, root);

	memset(&shell, 0, sizeof(shell));
	strncpy(shell.current_input, "al", SHELL_MAX_INPUT - 1);
	shell.input_pos = 2;
	shell_tab_complete(&shell, root);
	CHECK_STR(shell.current_input, "alp", "tab-completes to common prefix");

	compute_ghost("q", 1, root, &files, &shell, ghost, sizeof(ghost));
	CHECK_STR(ghost, " > quit", "shows command ghost");

	compute_ghost("alp/ha", 6, root, &files, &shell, ghost, sizeof(ghost));
	CHECK_STR(ghost, "", "does not invent completion without a match");

	compute_ghost("alp", 3, root, &files, &shell, ghost, sizeof(ghost));
	CHECK_STR(ghost, "", "does not show file ghost for bare first token");

	compute_ghost("alpha/al", 8, root, &files, &shell, ghost, sizeof(ghost));
	CHECK_STR(ghost, "chemy.txt", "shows suffix for path completion");

	strncpy(shell.dir_jumplist[0], "/tmp/origin", PATH_MAX - 1);
	shell.dir_jumplist_count = 1;
	compute_ghost("..", 2, root, &files, &shell, ghost, sizeof(ghost));
	CHECK_STR(ghost, " > origin", "shows previous directory ghost");

	compute_ghost("1", 1, root, &files, &shell, ghost, sizeof(ghost));
	CHECK_STR(ghost, " > alpha", "shows directory name for numeric jump");

	SUITE("execute_given");

	memset(&shell, 0, sizeof(shell));
	strncpy(shell.current_input, "q", SHELL_MAX_INPUT - 1);
	rc = execute_given(&shell, &files, NULL, NULL);
	CHECK(rc == 0, "quit command returns shell status");
	CHECK(shell.quit_requested == 1, "quit command sets quit flag");

	memset(&shell, 0, sizeof(shell));
	strncpy(shell.current_input, "cd alpha", SHELL_MAX_INPUT - 1);
	rc = execute_given(&shell, &files, NULL, NULL);
	path_join(path, sizeof(path), root, "alpha");
	CHECK(rc == 1, "cd command reports directory change");
	CHECK_STR(files.cwd, path, "cd command updates current directory");
	CHECK(shell.dir_jumplist_count == 1, "cd command records previous directory");

	strncpy(shell.current_input, "..", SHELL_MAX_INPUT - 1);
	rc = execute_given(&shell, &files, NULL, NULL);
	CHECK(rc == 1, "back command reports directory change");
	CHECK_STR(files.cwd, root, "back command restores previous directory");

	strncpy(shell.current_input, "1", SHELL_MAX_INPUT - 1);
	rc = execute_given(&shell, &files, NULL, NULL);
	CHECK(rc == 1, "numeric jump reports directory change");
	CHECK_STR(files.cwd, path, "numeric jump enters indexed directory");

	SUITE("debug commands");

	files_load_directory(&files, root);
	memset(&shell, 0, sizeof(shell));
	path_join(files.move_reg.paths[0], PATH_MAX, root, "alpine.txt");
	path_join(files.move_reg.paths[1], PATH_MAX, root, "beta");
	files.move_reg.count = 2;

	strncpy(shell.current_input, "$move", SHELL_MAX_INPUT - 1);
	rc = execute_given(&shell, &files, NULL, NULL);
	snprintf(expected, sizeof(expected),
	         "[move register]\n"
	         "  %s\n"
	         "  %s\n",
	         files.move_reg.paths[0], files.move_reg.paths[1]);
	CHECK(rc == 0, "$move returns shell status");
	CHECK_STR(shell.last_output + 1, expected, "$move prints move register contents");

	files.undo_top = 0;
	files.redo_top = 0;
	memset(&rename_op, 0, sizeof(rename_op));
	rename_op.type = OP_RENAME;
	strncpy(rename_op.cwd, root, PATH_MAX - 1);
	strncpy(rename_op.old_name, "alpine.txt", MAX_FILENAME - 1);
	strncpy(rename_op.new_name, "alpine-renamed.txt", MAX_FILENAME - 1);
	push_undo(&files, rename_op);

	path_join(alpha_dir, sizeof(alpha_dir), root, "alpha");
	path_join(moved_src, sizeof(moved_src), root, "alpine.txt");
	path_join(moved_dst, sizeof(moved_dst), alpha_dir, "alpine.txt");
	memset(&move_op, 0, sizeof(move_op));
	move_op.type = OP_MOVE;
	move_op.batch_id = 1;
	strncpy(move_op.cwd, alpha_dir, PATH_MAX - 1);
	strncpy(move_op.old_name, "alpine.txt", MAX_FILENAME - 1);
	strncpy(move_op.trash_path, moved_src, PATH_MAX - 1);
	push_undo(&files, move_op);

	memset(&shell, 0, sizeof(shell));
	strncpy(shell.current_input, "$undo", SHELL_MAX_INPUT - 1);
	rc = execute_given(&shell, &files, NULL, NULL);
	path_join(renamed, sizeof(renamed), root, "alpine-renamed.txt");
	snprintf(expected, sizeof(expected),
	         "[undo stack]\n"
	         " 1. move %s -> %s\n"
	         " 2. rename %s -> %s\n",
	         moved_src, moved_dst, moved_src, renamed);
	CHECK(rc == 0, "$undo returns shell status");
	CHECK_STR(shell.last_output + 1, expected, "$undo prints top-to-bottom");

	memset(&trash_op, 0, sizeof(trash_op));
	trash_op.type = OP_TRASH;
	strncpy(trash_op.cwd, root, PATH_MAX - 1);
	strncpy(trash_op.old_name, "beta", MAX_FILENAME - 1);
	strncpy(trash_op.trash_path, "/tmp/trash/beta", PATH_MAX - 1);
	files.redo_top = 0;
	files.redo_stack[files.redo_top++] = rename_op;
	files.redo_stack[files.redo_top++] = trash_op;

	memset(&shell, 0, sizeof(shell));
	strncpy(shell.current_input, "$redo", SHELL_MAX_INPUT - 1);
	rc = execute_given(&shell, &files, NULL, NULL);
	path_join(trashed, sizeof(trashed), root, "beta");
	snprintf(expected, sizeof(expected),
	         "[redo stack]\n"
	         " 1. trash %s -> %s\n"
	         " 2. rename %s -> %s\n",
	         trashed, trash_op.trash_path, moved_src, renamed);
	CHECK(rc == 0, "$redo returns shell status");
	CHECK_STR(shell.last_output + 1, expected, "$redo prints top-to-bottom");

	remove_tree(root);
}
