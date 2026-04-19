#pragma once

#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>

static inline void path_join(char *dst, size_t dsz, const char *dir, const char *name) {
	size_t dlen;
	size_t nmax;
	size_t nlen;

	if (dsz == 0) { return; }
	dlen = strlen(dir);
	if (dlen >= dsz - 1) { dlen = dsz - 2; }
	memcpy(dst, dir, dlen);
	dst[dlen] = '/';
	nmax = dsz - dlen - 2;
	nlen = strlen(name);
	if (nlen > nmax) { nlen = nmax; }
	memcpy(dst + dlen + 1, name, nlen);
	dst[dlen + 1 + nlen] = '\0';
}

static inline void format_size(off_t size, char *buf, int bufsize) {
			if (size >= 1024 * 1024 * 1024) {
				snprintf(buf, bufsize, "%.1fG", (double)size / (1024 * 1024 * 1024));
			} else if (size >= 1024 * 1024) {
				snprintf(buf, bufsize, "%.1fM", (double)size / (1024 * 1024));
			} else if (size >= 1024) {
				snprintf(buf, bufsize, "%.1fK", (double)size / 1024);
			} else {
				snprintf(buf, bufsize, "%ldB", (long)size);
			}

}
