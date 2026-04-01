CC ?= gcc
override CFLAGS_DEV = -Wall -Wextra -g -std=gnu11 -MMD -MP
override CFLAGS_REL = -Wall -Wextra -Werror -O3 -std=gnu11 -MMD -MP
LDLIBS = -lncurses -lpanel
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
DEV_BUILDDIR = build
REL_BUILDDIR = build/release
SRC = src/main.c src/tui.c src/file.c src/shell.c
OBJ_DEV = $(SRC:src/%.c=$(DEV_BUILDDIR)/%.o)
OBJ_REL = $(SRC:src/%.c=$(REL_BUILDDIR)/%.o)
DEP_DEV = $(OBJ_DEV:.o=.d)
DEP_REL = $(OBJ_REL:.o=.d)
THEMES = $(wildcard themes/*.swt)
THEME_OBJ_DEV = $(THEMES:themes/%.swt=$(DEV_BUILDDIR)/themes_%.o)
THEME_OBJ_REL = $(THEMES:themes/%.swt=$(REL_BUILDDIR)/themes_%.o)
TOTAL = $(shell echo $$(($(words $(SRC)) + $(words $(THEMES)) + 2)))
WIDTH = $(shell echo $$(($(TOTAL)<10?1:$(TOTAL)<100?2:3)))

.PHONY: dev rel clean install uninstall format

dev: $(DEV_BUILDDIR)/sourcefish

$(DEV_BUILDDIR):
	@mkdir -p $(DEV_BUILDDIR)

$(DEV_BUILDDIR)/themes_%.o: themes/%.swt | $(DEV_BUILDDIR)
	@mkdir -p $(@D)
	@$(eval COUNT=$(shell echo $$(($(COUNT)+1))))
	@printf "[%*d/%d] Embedding %s (dev)\n" $(WIDTH) $(COUNT) $(TOTAL) $<
	@ld -r -b binary $< -o $@

$(DEV_BUILDDIR)/%.o: src/%.c | $(DEV_BUILDDIR)
	@$(eval COUNT=$(shell echo $$(($(COUNT)+1))))
	@printf "[%*d/%d] Compiling %s (dev)\n" $(WIDTH) $(COUNT) $(TOTAL) $<
	@$(CC) $(CFLAGS_DEV) -c $< -o $@

$(DEV_BUILDDIR)/sourcefish: $(OBJ_DEV) $(THEME_OBJ_DEV)
	@$(eval COUNT=$(shell echo $$(($(COUNT)+1))))
	@printf "[%*d/%d] Linking %s (dev)\n" $(WIDTH) $(COUNT) $(TOTAL) $@
	@$(CC) $(OBJ_DEV) $(THEME_OBJ_DEV) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "Dev build complete. Binary located in $(DEV_BUILDDIR)/"

-include $(DEP_DEV)

rel: $(REL_BUILDDIR)/sourcefish

$(REL_BUILDDIR):
	@mkdir -p $(REL_BUILDDIR)

$(REL_BUILDDIR)/themes_%.o: themes/%.swt | $(REL_BUILDDIR)
	@mkdir -p $(@D)
	@$(eval COUNT=$(shell echo $$(($(COUNT)+1))))
	@printf "[%*d/%d] Embedding %s (release)\n" $(WIDTH) $(COUNT) $(TOTAL) $<
	@ld -r -b binary $< -o $@

$(REL_BUILDDIR)/%.o: src/%.c | $(REL_BUILDDIR)
	@$(eval COUNT=$(shell echo $$(($(COUNT)+1))))
	@printf "[%*d/%d] Compiling %s (release)\n" $(WIDTH) $(COUNT) $(TOTAL) $<
	@$(CC) $(CFLAGS_REL) -c $< -o $@

$(REL_BUILDDIR)/sourcefish: $(OBJ_REL) $(THEME_OBJ_REL)
	@$(eval COUNT=$(shell echo $$(($(COUNT)+1))))
	@printf "[%*d/%d] Linking %s (release)\n" $(WIDTH) $(COUNT) $(TOTAL) $@
	@$(CC) $(OBJ_REL) $(THEME_OBJ_REL) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "NOTE: Please increment package version in main.h"
	@echo "Release build complete. Binary located in $(REL_BUILDDIR)/"

-include $(DEP_REL)

install:
	install -Dm755 $(REL_BUILDDIR)/sourcefish $(DESTDIR)$(BINDIR)/sourcefish
	install -Dm644 LICENSE $(DESTDIR)/usr/share/licenses/sourcefish/LICENSE
	install -Dm644 README.md $(DESTDIR)/usr/share/doc/sourcefish/README.md

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/sourcefish

clean:
	rm -rf $(DEV_BUILDDIR) $(REL_BUILDDIR)

format:
	clang-format -i src/*.c
