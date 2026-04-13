#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "main.h"
#include "shell.h"

static const char *lookup_label(const char *cmd);
static void jumplist_push(ShellBuffer *shell, const char *path);
static void do_spawn(const char *cwd, WINDOW *files_win, WINDOW *shell_win);
static void run_shell(const char *cmd, const char *cwd, ShellBuffer *shell);
static const char *parse_cd(const char *cmd);

typedef struct {
	const char *cmd;
	const char *label;
} CmdLabel;

static const CmdLabel cmd_labels[] = {
	{ "q", "quit" },
	{ "swordfish", "it's my brother, no way!" },
	{ "s", "stat <file/index>" },
	{ NULL, NULL }
};

static const char *lookup_label(const char *cmd) {
	const char *c;
	size_t len;

	while (*cmd == ' ' || *cmd == '\t') { cmd++; }
	for (int i = 0; cmd_labels[i].cmd != NULL; i++) {
		c = cmd_labels[i].cmd;
		len = strlen(c);
		if (strncmp(cmd, c, len) == 0 &&
		    (cmd[len] == '\0' || cmd[len] == ' ' || cmd[len] == '\t')) {
			return cmd_labels[i].label;
		}
	}
	return NULL;
}

static void jumplist_push(ShellBuffer *shell, const char *path) {
	if (shell->dir_jumplist_count < DIR_JUMPLIST_MAX) {
		strncpy(shell->dir_jumplist[shell->dir_jumplist_count], path, PATH_MAX - 1);
		shell->dir_jumplist[shell->dir_jumplist_count][PATH_MAX - 1] = '\0';
		shell->dir_jumplist_count++;
	}
}

void shell_add_history(ShellBuffer *shell, const char *cmd) {
	if (shell->history_count > 0 &&
	    strcmp(shell->history[shell->history_count - 1], cmd) == 0) {
		return;
	}
	if (shell->history_count >= SHELL_MAX_HIST) {
		memmove(shell->history[0], shell->history[1],
		        (size_t)(SHELL_MAX_HIST - 1) * SHELL_MAX_INPUT);
		shell->history_count = SHELL_MAX_HIST - 1;
	}
	strncpy(shell->history[shell->history_count], cmd, SHELL_MAX_INPUT - 1);
	shell->history[shell->history_count][SHELL_MAX_INPUT - 1] = '\0';
	shell->history_count++;
}

void shell_handle_char(ShellBuffer *shell, int ch) {
	int len;

	len = (int)strlen(shell->current_input);

	if (ch == '\x7f' || ch == KEY_BACKSPACE) {
		if (shell->input_pos > 0) {
			memmove(shell->current_input + shell->input_pos - 1,
			        shell->current_input + shell->input_pos,
			        (size_t)(len - shell->input_pos + 1));
			shell->input_pos--;
		}
		return;
	}
	if (ch == KEY_DC) {
		if (shell->input_pos < len) {
			memmove(shell->current_input + shell->input_pos,
			        shell->current_input + shell->input_pos + 1,
			        (size_t)(len - shell->input_pos));
		}
		return;
	}
	if (ch == KEY_LEFT) {
		if (shell->input_pos > 0) { shell->input_pos--; }
		return;
	}
	if (ch == KEY_RIGHT) {
		if (shell->input_pos < len) { shell->input_pos++; }
		return;
	}
	if (ch == KEY_HOME) {
		shell->input_pos = 0;
		return;
	}
	if (ch == KEY_END) {
		shell->input_pos = len;
		return;
	}
	if (ch == KEY_UP) {
		if (shell->history_index < shell->history_count - 1) {
			shell->history_index++;
			strncpy(shell->current_input,
			        shell->history[shell->history_count - 1 - shell->history_index],
			        SHELL_MAX_INPUT - 1);
			shell->input_pos = (int)strlen(shell->current_input);
		}
		return;
	}
	if (ch == KEY_DOWN) {
		if (shell->history_index > 0) {
			shell->history_index--;
			strncpy(shell->current_input,
			        shell->history[shell->history_count - 1 - shell->history_index],
			        SHELL_MAX_INPUT - 1);
			shell->input_pos = (int)strlen(shell->current_input);
		} else if (shell->history_index == 0) {
			shell->history_index = -1;
			memset(shell->current_input, 0, SHELL_MAX_INPUT);
			shell->input_pos = 0;
		}
		return;
	}
	if (ch >= 32 && ch < 127 && len < SHELL_MAX_INPUT - 1) {
		memmove(shell->current_input + shell->input_pos + 1,
		        shell->current_input + shell->input_pos,
		        (size_t)(len - shell->input_pos + 1));
		shell->current_input[shell->input_pos++] = (char)ch;
	}
}

void shell_tab_complete(ShellBuffer *shell, const char *cwd) {
	int tok_start;
	char token[SHELL_MAX_INPUT];
	int tok_len;
	char dir_part[PATH_MAX];
	char name_part[MAX_FILENAME];
	const char *last_slash;
	char search_dir[PATH_MAX];
	char completed[MAX_FILENAME];
	int count;
	char new_token[PATH_MAX];
	char new_input[SHELL_MAX_INPUT];

	tok_start = shell->input_pos;
	while (tok_start > 0 && shell->current_input[tok_start - 1] != ' ') { tok_start--; }

	tok_len = shell->input_pos - tok_start;
	strncpy(token, shell->current_input + tok_start, tok_len);
	token[tok_len] = '\0';

	last_slash = strrchr(token, '/');

	if (last_slash) {
		int dlen = (int)(last_slash - token);
		strncpy(dir_part, token, dlen);
		dir_part[dlen] = '\0';
		strncpy(name_part, last_slash + 1, MAX_FILENAME - 1);
		name_part[MAX_FILENAME - 1] = '\0';
	} else {
		dir_part[0] = '\0';
		strncpy(name_part, token, MAX_FILENAME - 1);
		name_part[MAX_FILENAME - 1] = '\0';
	}

	if (dir_part[0] == '\0') {
		strncpy(search_dir, cwd, PATH_MAX - 1);
		search_dir[PATH_MAX - 1] = '\0';
	} else if (dir_part[0] == '/') {
		strncpy(search_dir, dir_part, PATH_MAX - 1);
		search_dir[PATH_MAX - 1] = '\0';
	} else {
		path_join(search_dir, PATH_MAX, cwd, dir_part);
	}

	count = complete_in_dir(search_dir, name_part, completed, sizeof(completed));
	if (count == 0 || completed[0] == '\0') { return; }

	if (dir_part[0] == '\0') {
		strncpy(new_token, completed, sizeof(new_token) - 1);
		new_token[sizeof(new_token) - 1] = '\0';
	} else {
		path_join(new_token, sizeof(new_token), dir_part, completed);
	}

	strncpy(new_input, shell->current_input, tok_start);
	new_input[tok_start] = '\0';
	strncat(new_input, new_token, SHELL_MAX_INPUT - 1 - tok_start);

	strncpy(shell->current_input, new_input, SHELL_MAX_INPUT - 1);
	shell->current_input[SHELL_MAX_INPUT - 1] = '\0';
	shell->input_pos = (int)strlen(shell->current_input);
}

void shell_restore_ncurses(WINDOW *files_win, WINDOW *shell_win) {
	reset_prog_mode();
	keypad(stdscr, TRUE);
	nodelay(stdscr, TRUE);

	if (files_win) { keypad(files_win, TRUE); }
	if (shell_win) { keypad(shell_win, TRUE); }

	curs_set(0);
	clearok(stdscr, TRUE);
	refresh();

	if (files_win) { clearok(files_win, TRUE); touchwin(files_win); }
	if (shell_win) { clearok(shell_win, TRUE); touchwin(shell_win); }
}

static void do_spawn(const char *cwd, WINDOW *files_win, WINDOW *shell_win) {
	const char *sh;
	pid_t pid;
	int status;

	def_prog_mode();
	endwin();

	sh = getenv("SHELL");
	if (!sh || sh[0] == '\0') { sh = "/bin/sh"; }

	pid = fork();
	if (pid == 0) {
		if (cwd && cwd[0]) { chdir(cwd); }
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGTSTP, SIG_DFL);
		execlp(sh, sh, (char *)NULL);
		_exit(127);
	} else if (pid > 0) {
		waitpid(pid, &status, 0);
	}

	shell_restore_ncurses(files_win, shell_win);
}

static void run_shell(const char *cmd, const char *cwd, ShellBuffer *shell) {
	int pfd[2];
	const char *sh;
	pid_t pid;
	int total;
	char buf[512];
	int n;
	int status;

	shell->last_output[0] = '\0';

	if (pipe(pfd) != 0) { return; }

	sh = getenv("SHELL");
	if (!sh || sh[0] == '\0') { sh = "/bin/sh"; }

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		dup2(pfd[1], STDERR_FILENO);
		close(pfd[1]);
		if (cwd && cwd[0]) { chdir(cwd); }
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGTSTP, SIG_DFL);
		execlp(sh, sh, "-c", cmd, (char *)NULL);
		_exit(127);
	}

	close(pfd[1]);

	if (pid > 0) {
		total = 0;
		while ((n = (int)read(pfd[0], buf, sizeof(buf))) > 0) {
			if (total + n < SHELL_OUTPUT_MAX - 1) {
				memcpy(shell->last_output + total, buf, (size_t)n);
				total += n;
			}
		}
		shell->last_output[total] = '\0';
		close(pfd[0]);
		waitpid(pid, &status, 0);
	} else {
		close(pfd[0]);
	}
}

static const char *parse_cd(const char *cmd) {
	const char *after;

	while (*cmd == ' ' || *cmd == '\t') { cmd++; }
	if (strncmp(cmd, "cd", 2) != 0) { return NULL; }
	after = cmd + 2;
	if (*after != '\0' && *after != ' ' && *after != '\t') { return NULL; }
	while (*after == ' ' || *after == '\t') { after++; }
	return after;
}

int print_to_shell(ShellBuffer *shell, const char *text, int type) {
	char formatted[SHELL_OUTPUT_MAX];

	if (type == SHELL_MSG_ERROR) {
		snprintf(formatted, sizeof(formatted), "ERROR: %s", text);
	} else if (type == SHELL_MSG_WARN) {
		snprintf(formatted, sizeof(formatted), "WARN: %s", text);
	} else {
		strncpy(formatted, text, sizeof(formatted) - 1);
		formatted[sizeof(formatted) - 1] = '\0';
	}

	shell->last_output[0] = (char)type;
	strncpy(shell->last_output + 1, formatted, SHELL_OUTPUT_MAX - 2);
	shell->last_output[SHELL_OUTPUT_MAX - 1] = '\0';
	return 0;
}

int execute_given(ShellBuffer *shell, FilesBuffer *files, WINDOW *files_win, WINDOW *shell_win) {
	char cmd[SHELL_MAX_INPUT];
	char expanded[SHELL_MAX_INPUT * 4];
	int len;
	const char *label;
	char *endp;
	long idx;
	char target[PATH_MAX];
	char resolved[PATH_MAX];
	const char *use;
	const char *cd_arg;
	int found;
	const char *p;
	int is_rm;
	int is_r;
	const char *arg;
	char display[SHELL_OUTPUT_MAX];
	int dlen;
	int any;
	const char *scan;
	const char *tok_end;
	char token[MAX_FILENAME];
	int tok_len;
	int matched;
	int was_quoted;
	char entry_path[PATH_MAX];
	char rm_cmd[PATH_MAX + 16];
	char sel_args[SHELL_MAX_INPUT * 3];
	const char *srch;
	char *dst;
	int rem;
	int n;

	strncpy(cmd, shell->current_input, SHELL_MAX_INPUT - 1);
	cmd[SHELL_MAX_INPUT - 1] = '\0';

	if (files->sel_count > 0) {
		files_build_sel_args(files, sel_args, sizeof(sel_args));
		srch = cmd;
		dst = expanded;
		rem = (int)sizeof(expanded) - 1;
		while (*srch && rem > 0) {
			if (strncmp(srch, "sel", 3) == 0 &&
			    (srch == cmd || srch[-1] == ' ') &&
			    (srch[3] == '\0' || srch[3] == ' ')) {
				n = snprintf(dst, rem, "%s", sel_args);
				if (n > 0 && n < rem) { dst += n; rem -= n; }
				srch += 3;
			} else {
				*dst++ = *srch++;
				rem--;
			}
		}
		*dst = '\0';
		strncpy(cmd, expanded, SHELL_MAX_INPUT - 1);
		cmd[SHELL_MAX_INPUT - 1] = '\0';
	}

	len = (int)strlen(cmd);
	while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) { cmd[--len] = '\0'; }

	memset(shell->current_input, 0, SHELL_MAX_INPUT);
	shell->input_pos = 0;
	shell->history_index = -1;

	if (len == 0) {
		do_spawn(files->cwd, files_win, shell_win);
		return 0;
	}

	strncpy(shell->last_cmd, cmd, SHELL_MAX_INPUT - 1);
	shell->last_cmd[SHELL_MAX_INPUT - 1] = '\0';

	label = lookup_label(cmd);
	if (label) {
		strncpy(shell->last_result, label, SHELL_RESULT_MAX - 1);
		shell->last_result[SHELL_RESULT_MAX - 1] = '\0';
	} else {
		shell->last_result[0] = '\0';
	}

	shell_add_history(shell, cmd);

	if (strcmp(cmd, "q") == 0) {
		shell->quit_requested = 1;
		return 0;
	}

	if (cmd[0] == 's' && (cmd[1] == '\0' || cmd[1] == ' ')) {
		files_cmd_stat(files, shell, cmd[1] == ' ' ? cmd + 2 : "");
		return 0;
	}

	if (strcmp(cmd, "..") == 0) {
		if (shell->dir_jumplist_count > 0) {
			const char *prev = shell->dir_jumplist[--shell->dir_jumplist_count];
			const char *slash = strrchr(prev, '/');
			const char *dname = (slash && slash[1] != '\0') ? slash + 1 : prev;
			strncpy(shell->last_result, dname, SHELL_RESULT_MAX - 1);
			shell->last_result[SHELL_RESULT_MAX - 1] = '\0';
			files_load_directory(files, prev);
		}
		return 1;
	}

	idx = strtol(cmd, &endp, 10);
	if (*endp == '\0' && idx > 0) {
		for (int i = 0; i < files->entry_count; i++) {
			if (files->entries[i].index == (int)idx &&
			    files->entries[i].type == ENTRY_DIR) {
				if (strcmp(files->cwd, "/") == 0) {
					snprintf(target, PATH_MAX, "/%.*s",
					         (int)(PATH_MAX - 2), files->entries[i].name);
				} else {
					path_join(target, PATH_MAX, files->cwd, files->entries[i].name);
				}
				jumplist_push(shell, files->cwd);
				strncpy(shell->last_result, files->entries[i].name, SHELL_RESULT_MAX - 1);
				shell->last_result[SHELL_RESULT_MAX - 1] = '\0';
				use = realpath(target, resolved) ? resolved : target;
				files_load_directory(files, use);
				return 1;
			}
		}
	}

	cd_arg = parse_cd(cmd);
	if (cd_arg != NULL) {
		if (*cd_arg == '\0') {
			const char *home = getenv("HOME");
			snprintf(target, PATH_MAX, "%s", (home && home[0]) ? home : "/");
		} else {
			idx = strtol(cd_arg, &endp, 10);
			if (*endp == '\0' && idx > 0) {
				found = 0;
				for (int i = 0; i < files->entry_count; i++) {
					if (files->entries[i].index == (int)idx &&
					    files->entries[i].type == ENTRY_DIR) {
						if (strcmp(files->cwd, "/") == 0) {
							snprintf(target, PATH_MAX, "/%.*s",
							         (int)(PATH_MAX - 2), files->entries[i].name);
						} else {
							path_join(target, PATH_MAX, files->cwd, files->entries[i].name);
						}
						found = 1;
						break;
					}
				}
				if (!found) { return 0; }
			} else if (cd_arg[0] == '~') {
				const char *home = getenv("HOME");
				if (!home) { home = "/"; }
				snprintf(target, PATH_MAX, "%s%s", home, cd_arg + 1);
			} else if (cd_arg[0] != '/') {
				path_join(target, PATH_MAX, files->cwd, cd_arg);
			} else {
				snprintf(target, PATH_MAX, "%s", cd_arg);
			}
		}

		use = realpath(target, resolved) ? resolved : target;
		jumplist_push(shell, files->cwd);
		files_load_directory(files, use);
		return 1;
	}

	p = cmd;
	while (*p == ' ' || *p == '\t') { p++; }

	// Match both "rm <args>" and "r <args>" as remove shorthand
	is_rm = (strncmp(p, "rm", 2) == 0 && (p[2] == ' ' || p[2] == '\t'));
	is_r  = (p[0] == 'r' && p[1] == ' ');

	if (is_rm || is_r) {
		arg = p + (is_rm ? 2 : 1);
		while (*arg == ' ' || *arg == '\t') { arg++; }

		if (!shell->rm_confirm_mode) {
			// Phase 1 collect targets and ask for confirmation
			dlen = 0;
			any = 0;
			scan = arg;

			dlen += snprintf(display + dlen, sizeof(display) - dlen, "File(s) to be removed:\n");

			while (*scan != '\0' && dlen < (int)sizeof(display) - 1) {
				tok_end = scan;
				while (*tok_end && *tok_end != ' ' && *tok_end != '\t') { tok_end++; }

				tok_len = (int)(tok_end - scan);
				if (tok_len >= MAX_FILENAME) { tok_len = MAX_FILENAME - 1; }
				strncpy(token, scan, tok_len);
				token[tok_len] = '\0';

				was_quoted = 0;
				if (tok_len >= 2 && token[0] == '\'' && token[tok_len - 1] == '\'') {
					memmove(token, token + 1, tok_len - 2);
					token[tok_len - 2] = '\0';
					was_quoted = 1;
				}

				scan = tok_end;
				while (*scan == ' ' || *scan == '\t') { scan++; }

				matched = 0;
				if (!was_quoted) {
					idx = strtol(token, &endp, 10);
					if (*endp == '\0' && idx > 0) {
						for (int i = 0; i < files->entry_count; i++) {
							if (files->entries[i].index != (int)idx) { continue; }
							dlen += snprintf(display + dlen, sizeof(display) - dlen, "  %s\n", files->entries[i].name);
							any = 1;
							matched = 1;
							break;
						}
						if (!matched) {
							print_to_shell(shell, "no entry at that index", SHELL_MSG_ERROR);
							return 0;
						}
					}
				}
				if (!matched) {
					for (int i = 0; i < files->entry_count; i++) {
						if (strcmp(files->entries[i].name, token) != 0) { continue; }
						dlen += snprintf(display + dlen, sizeof(display) - dlen, "  %s\n", files->entries[i].name);
						any = 1;
						break;
					}
				}
			}

			if (any) {
				snprintf(display + dlen, sizeof(display) - dlen, "Continue? [y/N]");
				print_to_shell(shell, display, 1);
				shell->rm_confirm_mode = 1;
				strncpy(shell->rm_pending_cmd, cmd, SHELL_MAX_INPUT - 1);
				shell->rm_pending_cmd[SHELL_MAX_INPUT - 1] = '\0';
				return 0;
			}
		} else {
			// Phase 2 execute the deletions
			shell->rm_confirm_mode = 0;
			any = 0;
			while (*arg != '\0') {
				tok_end = arg;
				while (*tok_end && *tok_end != ' ' && *tok_end != '\t') { tok_end++; }

				tok_len = (int)(tok_end - arg);
				if (tok_len >= MAX_FILENAME) { tok_len = MAX_FILENAME - 1; }
				strncpy(token, arg, tok_len);
				token[tok_len] = '\0';

				was_quoted = 0;
				if (tok_len >= 2 && token[0] == '\'' && token[tok_len - 1] == '\'') {
					memmove(token, token + 1, tok_len - 2);
					token[tok_len - 2] = '\0';
					was_quoted = 1;
				}

				arg = tok_end;
				while (*arg == ' ' || *arg == '\t') { arg++; }

				matched = 0;
				if (!was_quoted) {
					idx = strtol(token, &endp, 10);
					if (*endp == '\0' && idx > 0) {
						for (int i = 0; i < files->entry_count; i++) {
							if (files->entries[i].index != (int)idx) { continue; }
							if (strcmp(files->cwd, "/") == 0) {
								snprintf(entry_path, PATH_MAX, "/%s", files->entries[i].name);
							} else {
								path_join(entry_path, PATH_MAX, files->cwd, files->entries[i].name);
							}
							if (files->entries[i].type == ENTRY_DIR) {
								snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", entry_path);
							} else {
								snprintf(rm_cmd, sizeof(rm_cmd), "rm '%s'", entry_path);
							}
							run_shell(rm_cmd, files->cwd, shell);
							any = 1;
							matched = 1;
							break;
						}
					}
				}
				if (!matched) {
					for (int i = 0; i < files->entry_count; i++) {
						if (strcmp(files->entries[i].name, token) != 0) { continue; }
						if (strcmp(files->cwd, "/") == 0) {
							snprintf(entry_path, PATH_MAX, "/%s", files->entries[i].name);
						} else {
							path_join(entry_path, PATH_MAX, files->cwd, files->entries[i].name);
						}
						if (files->entries[i].type == ENTRY_DIR) {
							snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", entry_path);
						} else {
							snprintf(rm_cmd, sizeof(rm_cmd), "rm '%s'", entry_path);
						}
						run_shell(rm_cmd, files->cwd, shell);
						any = 1;
						break;
					}
				}
			}
			if (any) { return 0; }
		}
	}

	run_shell(cmd, files->cwd, shell);
	return 0;
}

void compute_ghost(const char *input, int input_pos, const char *cwd,
                   const FilesBuffer *files, const ShellBuffer *shell,
                   char *ghost_out, int ghost_max) {
	int tok_start;
	int tok_len;
	char token[SHELL_MAX_INPUT];
	int name_len;
	int is_first_token;
	int has_slash;
	char *endp;
	long idx;
	const char *slash;
	const char *dname;
	char dir_part[PATH_MAX];
	char name_part[MAX_FILENAME];
	const char *last_slash;
	char search_dir[PATH_MAX];
	char completed[MAX_FILENAME];
	int count;
	int comp_len;

	ghost_out[0] = '\0';
	if (input_pos <= 0 || !cwd) { return; }

	tok_start = input_pos;
	while (tok_start > 0 && input[tok_start - 1] != ' ') { tok_start--; }

	tok_len = input_pos - tok_start;
	if (tok_len <= 0) { return; }

	strncpy(token, input + tok_start, tok_len);
	token[tok_len] = '\0';

	name_len = tok_len;
	is_first_token = (tok_start == 0);
	has_slash = (strchr(token, '/') != NULL);

	if (!is_first_token) { return; }

	if (is_first_token && !has_slash) {
		if (strcmp(token, "..") == 0) {
			if (shell && shell->dir_jumplist_count > 0) {
				const char *prev = shell->dir_jumplist[shell->dir_jumplist_count - 1];
				slash = strrchr(prev, '/');
				dname = (slash && slash[1] != '\0') ? slash + 1 : prev;
				snprintf(ghost_out, ghost_max, " > %s", dname);
			}
			return;
		}

		idx = strtol(token, &endp, 10);
		if (*endp == '\0' && idx > 0 && files) {
			for (int i = 0; i < files->entry_count; i++) {
				if (files->entries[i].index == (int)idx &&
				    files->entries[i].type == ENTRY_DIR) {
					snprintf(ghost_out, ghost_max, " > %s", files->entries[i].name);
					return;
				}
			}
		}

		for (int i = 0; cmd_labels[i].cmd != NULL; i++) {
			if (strcmp(cmd_labels[i].cmd, token) == 0) {
				snprintf(ghost_out, ghost_max, " > %s", cmd_labels[i].label);
				return;
			}
		}
		return;
	}

	last_slash = strrchr(token, '/');
	if (last_slash) {
		int dlen = (int)(last_slash - token);
		strncpy(dir_part, token, dlen);
		dir_part[dlen] = '\0';
		strncpy(name_part, last_slash + 1, MAX_FILENAME - 1);
		name_part[MAX_FILENAME - 1] = '\0';
		name_len = (int)strlen(name_part);
	} else {
		dir_part[0] = '\0';
		strncpy(name_part, token, MAX_FILENAME - 1);
		name_part[MAX_FILENAME - 1] = '\0';
	}

	if (name_len <= 0) { return; }

	if (dir_part[0] == '\0') {
		strncpy(search_dir, cwd, PATH_MAX - 1);
		search_dir[PATH_MAX - 1] = '\0';
	} else if (dir_part[0] == '/') {
		strncpy(search_dir, dir_part, PATH_MAX - 1);
		search_dir[PATH_MAX - 1] = '\0';
	} else {
		path_join(search_dir, PATH_MAX, cwd, dir_part);
	}

	count = complete_in_dir(search_dir, name_part, completed, sizeof(completed));
	if (count == 0 || completed[0] == '\0') { return; }

	comp_len = (int)strlen(completed);
	if (comp_len <= name_len) { return; }

	strncpy(ghost_out, completed + name_len, ghost_max - 1);
	ghost_out[ghost_max - 1] = '\0';
}

void shell_render(ShellBuffer *shell, WINDOW *win, int height, int focused,
                  const char *cwd, const FilesBuffer *files) {
	int width;
	int out_rows;
	char buf[SHELL_OUTPUT_MAX];
	const char *line_ptrs[1024];
	int nlines;
	int start;
	int result_row;
	int cmd_cols;
	int x;
	int input_row;
	char ghost[MAX_FILENAME];
	int cx;

	width = getmaxx(win);
	werase(win);

	if (focused) {
		wattron(win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
		mvwprintw(win, 0, 0, " SHELL ");
		wattroff(win, COLOR_PAIR(CP_TITLE_F) | A_BOLD);
		mvwprintw(win, 0, 8, " Enter: spawn shell q: quit");
	} else {
		wattron(win, COLOR_PAIR(CP_TITLE_UF) | A_BOLD);
		mvwprintw(win, 0, 0, " SHELL ");
		wattroff(win, COLOR_PAIR(CP_TITLE_UF) | A_BOLD);
	}
	wclrtoeol(win);

	out_rows = height - 3;
	if (out_rows > 0 && shell->last_output[0] != '\0') {
		strncpy(buf, shell->last_output, SHELL_OUTPUT_MAX - 1);
		buf[SHELL_OUTPUT_MAX - 1] = '\0';

		nlines = 0;
		line_ptrs[nlines++] = buf;
		for (char *p = buf; *p && nlines < 1024; p++) {
			if (*p == '\n') {
				*p = '\0';
				if (*(p + 1) != '\0') { line_ptrs[nlines++] = p + 1; }
			}
		}
		start = (nlines > out_rows) ? nlines - out_rows : 0;
		for (int i = start; i < nlines; i++) {
			int row = 1 + (i - start);
			const char *text = line_ptrs[i];
			int pair = 0;
			int bold = 0;

			if (row >= height - 2) { break; }

			wstandend(win);

			if (text[0] == SHELL_MSG_ERROR) {
				pair = CP_EXT_VIDEO;
				bold = 1;
				text++;
			} else if (text[0] == SHELL_MSG_WARN) {
				pair = CP_EXT_ARCHIVE;
				bold = 1;
				text++;
			} else if (text[0] == SHELL_MSG_NORMAL) {
				text++;
			}

			if (pair) { wattron(win, COLOR_PAIR(pair) | (bold ? A_BOLD : A_NORMAL)); }
			mvwprintw(win, row, 1, "%.*s", width - 2, text);
			if (pair) { wattroff(win, COLOR_PAIR(pair) | (bold ? A_BOLD : A_NORMAL)); }
			wstandend(win);
		}
	}

	result_row = height - 2;
	if (result_row >= 1 && shell->last_cmd[0] != '\0') {
		wattron(win, A_DIM);
		mvwprintw(win, result_row, 1, "%.*s", width / 2, shell->last_cmd);
		if (shell->last_result[0] != '\0') {
			cmd_cols = (int)strlen(shell->last_cmd);
			if (cmd_cols > width / 2) { cmd_cols = width / 2; }
			x = 1 + cmd_cols;
			if (x + 3 < width) {
				mvwprintw(win, result_row, x, " > %.*s", width - x - 4, shell->last_result);
			}
		}
		wattroff(win, A_DIM);
		wclrtoeol(win);
	}

	input_row = height - 1;
	wattron(win, COLOR_PAIR(CP_PROMPT) | A_BOLD);
	mvwaddstr(win, input_row, 1, "> ");
	wattroff(win, COLOR_PAIR(CP_PROMPT) | A_BOLD);

	if (shell->rm_confirm_mode) {
		wattron(win, A_BOLD);
		waddstr(win, "[y/N]: ");
		wattroff(win, A_BOLD);
		waddstr(win, shell->current_input);
		wclrtoeol(win);
		if (focused) {
			cx = 3 + 7 + shell->input_pos;
			wmove(win, input_row, cx);
			leaveok(win, FALSE);
			curs_set(2);
		} else {
			leaveok(win, TRUE);
		}
		wnoutrefresh(win);
		return;
	}

	waddstr(win, shell->current_input);

	if (focused) {
		compute_ghost(shell->current_input, shell->input_pos, cwd,
		              files, shell, ghost, sizeof(ghost));
		if (ghost[0] != '\0') {
			wclrtoeol(win);
			wattron(win, A_DIM);
			waddstr(win, ghost);
			wattroff(win, A_DIM);
		}
	}

	wclrtoeol(win);

	if (focused) {
		cx = 3 + shell->input_pos;
		wmove(win, input_row, cx);
		leaveok(win, FALSE);
		curs_set(2);
	} else {
		leaveok(win, TRUE);
	}

	wnoutrefresh(win);
}
