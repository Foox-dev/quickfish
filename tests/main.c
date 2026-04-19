#include "test.h"

#include <stdio.h>

int tests_passed = 0;
int tests_failed = 0;

void test_main();
void test_file();
void test_shell();

int main() {
	int total;

	printf("\nrunning quickfish tests\n");

	test_main();
	test_file();
	test_shell();

	total = tests_passed + tests_failed;
	if (tests_failed == 0) {
		printf("ok -- %d/%d tests passed\n", tests_passed, total);
		return 0;
	}

	printf("FAILED -- %d/%d tests passed, %d failed\n",
	       tests_passed, total, tests_failed);
	return 1;
}
