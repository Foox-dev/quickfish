#pragma once

#include <string.h>
#include <stddef.h>

static inline void path_join(char *dst, size_t dsz, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    if (dlen >= dsz - 1) dlen = dsz - 2;
    memcpy(dst, dir, dlen);
    dst[dlen] = '/';
    size_t nmax = dsz - dlen - 2;
    strncpy(dst + dlen + 1, name, nmax);
    dst[dsz - 1] = '\0';
}
