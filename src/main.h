#pragma once

#include <stddef.h>
#include <string.h>

static inline void path_join(char *dst, size_t dsz, const char *dir, const char *name) {
	size_t dlen;
	size_t nmax;

	dlen = strlen(dir);
	if (dlen >= dsz - 1) { dlen = dsz - 2; }
	memcpy(dst, dir, dlen);
	dst[dlen] = '/';
	nmax = dsz - dlen - 2;
	strncpy(dst + dlen + 1, name, nmax);
	dst[dsz - 1] = '\0';
}
