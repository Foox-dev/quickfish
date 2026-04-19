#include "test.h"
#include "../src/main.h"

void test_main() {
	char path[64];
	char size[32];

	SUITE("path_join");

	path_join(path, sizeof(path), "/tmp", "alpha");
	CHECK_STR(path, "/tmp/alpha", "joins directory and name");

	path_join(path, 8, "/tmp", "alphabet");
	CHECK_STR(path, "/tmp/al", "truncates to fit destination");

	SUITE("format_size");

	format_size(999, size, sizeof(size));
	CHECK_STR(size, "999B", "formats byte-sized values");

	format_size(1536, size, sizeof(size));
	CHECK_STR(size, "1.5K", "formats kibibyte-sized values");

	format_size(2 * 1024 * 1024, size, sizeof(size));
	CHECK_STR(size, "2.0M", "formats mebibyte-sized values");
}
