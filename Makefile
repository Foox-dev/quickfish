CC ?= gcc
override CFLAGS_DEV = -Wall -Wextra -g -std=gnu11 -MMD -MP
override CFLAGS_REL = -Wall -Wextra -Werror -O3 -std=gnu11 -MMD -MP
LDLIBS = -lncurses -lpanel -ltinfo
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
DEV_BUILDDIR = build
REL_BUILDDIR = build/release
TEST_BUILDDIR = build/test
SRC = src/main.c src/tui.c src/file.c src/shell.c
OBJ_DEV = $(SRC:src/%.c=$(DEV_BUILDDIR)/%.o)
OBJ_REL = $(SRC:src/%.c=$(REL_BUILDDIR)/%.o)
DEP_DEV = $(OBJ_DEV:.o=.d)
DEP_REL = $(OBJ_REL:.o=.d)
TEST_SRC = $(wildcard tests/*.c)
TEST_OBJ = $(TEST_SRC:tests/%.c=$(TEST_BUILDDIR)/%.o)
TEST_OBJ_SRC = $(filter-out $(DEV_BUILDDIR)/main.o, $(OBJ_DEV))
THEMES = $(wildcard themes/*.swt)
THEME_OBJ_DEV = $(THEMES:themes/%.swt=$(DEV_BUILDDIR)/themes_%.o)
THEME_OBJ_REL = $(THEMES:themes/%.swt=$(REL_BUILDDIR)/themes_%.o)
TOTAL = $(shell echo $$(($(words $(SRC)) + $(words $(THEMES)) + 2)))
WIDTH = $(shell echo $$(($(TOTAL)<10?1:$(TOTAL)<100?2:3)))

.PHONY: dev rel test clean install uninstall docs

dev: $(DEV_BUILDDIR)/quickfish

$(DEV_BUILDDIR):
	@mkdir -p $(DEV_BUILDDIR)

$(DEV_BUILDDIR)/themes_%.o: themes/%.swt | $(DEV_BUILDDIR)
	@mkdir -p $(@D)
	@$(eval COUNT_DEV=$(shell echo $$(($(COUNT_DEV)+1))))
	@printf "[%*d/%d] Embedding %s (dev)\n" $(WIDTH) $(COUNT_DEV) $(TOTAL) $<
	@ld -r -b binary $< -o $@

$(DEV_BUILDDIR)/%.o: src/%.c | $(DEV_BUILDDIR)
	@$(eval COUNT_DEV=$(shell echo $$(($(COUNT_DEV)+1))))
	@printf "[%*d/%d] Compiling %s (dev)\n" $(WIDTH) $(COUNT_DEV) $(TOTAL) $<
	@$(CC) $(CFLAGS_DEV) -c $< -o $@

$(DEV_BUILDDIR)/quickfish: $(OBJ_DEV) $(THEME_OBJ_DEV)
	@$(eval COUNT_DEV=$(shell echo $$(($(COUNT_DEV)+1))))
	@printf "[%*d/%d] Linking %s (dev)\n" $(WIDTH) $(COUNT_DEV) $(TOTAL) $@
	@$(CC) $(OBJ_DEV) $(THEME_OBJ_DEV) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "Dev build complete. Binary located in $(DEV_BUILDDIR)/"

-include $(DEP_DEV)

rel: test $(REL_BUILDDIR)/quickfish

$(REL_BUILDDIR):
	@mkdir -p $(REL_BUILDDIR)

$(REL_BUILDDIR)/themes_%.o: themes/%.swt | $(REL_BUILDDIR)
	@mkdir -p $(@D)
	@$(eval COUNT_REL=$(shell echo $$(($(COUNT_REL)+1))))
	@printf "[%*d/%d] Embedding %s (release)\n" $(WIDTH) $(COUNT_REL) $(TOTAL) $<
	@ld -r -b binary $< -o $@

$(REL_BUILDDIR)/%.o: src/%.c | $(REL_BUILDDIR)
	@$(eval COUNT_REL=$(shell echo $$(($(COUNT_REL)+1))))
	@printf "[%*d/%d] Compiling %s (release)\n" $(WIDTH) $(COUNT_REL) $(TOTAL) $<
	@$(CC) $(CFLAGS_REL) -c $< -o $@

$(REL_BUILDDIR)/quickfish: $(OBJ_REL) $(THEME_OBJ_REL)
	@$(eval COUNT_REL=$(shell echo $$(($(COUNT_REL)+1))))
	@printf "[%*d/%d] Linking %s (release)\n" $(WIDTH) $(COUNT_REL) $(TOTAL) $@
	@$(CC) $(OBJ_REL) $(THEME_OBJ_REL) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "NOTE: Please increment package version in main.h"
	@echo "Release build complete. Binary located in $(REL_BUILDDIR)/"

-include $(DEP_REL)

test: $(OBJ_DEV) $(THEME_OBJ_DEV) $(TEST_BUILDDIR)/test_runner
	@$(TEST_BUILDDIR)/test_runner

$(TEST_BUILDDIR):
	@mkdir -p $(TEST_BUILDDIR)

$(TEST_BUILDDIR)/%.o: tests/%.c tests/test.h | $(TEST_BUILDDIR)
	@printf "[test] Compiling %s\n" $<
	@$(CC) $(CFLAGS_DEV) -Isrc -Itests -c $< -o $@

$(TEST_BUILDDIR)/test_runner: $(TEST_OBJ) $(TEST_OBJ_SRC) $(THEME_OBJ_DEV)
	@printf "[test] Linking test runner\n"
	@$(CC) $(TEST_OBJ) $(TEST_OBJ_SRC) $(THEME_OBJ_DEV) $(LDFLAGS) $(LDLIBS) -o $@

install:
	install -Dm755 $(REL_BUILDDIR)/quickfish $(DESTDIR)$(BINDIR)/quickfish
	install -Dm644 LICENSE $(DESTDIR)/usr/share/licenses/quickfish/LICENSE
	install -Dm644 README.md $(DESTDIR)/usr/share/doc/quickfish/README.md

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/quickfish

clean:
	rm -rf $(DEV_BUILDDIR) $(REL_BUILDDIR) $(TEST_BUILDDIR)

docs:
	@echo "Building doxygen docs..."
	doxygen Doxyfile
	@echo "Documentation generated in docs/doxygen"
