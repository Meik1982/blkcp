CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc -MMD -MP
LDFLAGS ?= 
LIBS ?= lib/libcoreutils.a -lcrypto -lpthread -luring

TARGET = blkcp
TARGET_TUI = blkcp-tui

SRCS = src/blkcp.c src/version.c src/conversions.c src/stats.c src/signals.c src/args.c src/io_engine.c src/io_sync.c src/io_async.c src/io_reflink.c src/io_uring.c src/io_splice.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

TUI_SRCS = src/tui/tui_device.c src/tui/tui_file_picker.c src/tui/tui_render.c src/tui/tui_nav.c src/tui/tui_main.c
TUI_OBJS = $(TUI_SRCS:.c=.o)
TUI_DEPS = $(TUI_OBJS:.o=.d)
TUI_LIBS = -lncursesw

all: $(TARGET) $(TARGET_TUI)
	@mkdir -p bin
	@cp -f $(TARGET) bin/$(TARGET)
	@cp -f $(TARGET_TUI) bin/$(TARGET_TUI)

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O3 -DNDEBUG -flto -Wall -Wextra -pthread -Iinclude -Isrc -MMD -MP" LDFLAGS="-flto -Wl,-O1,--sort-common,--as-needed,-z,relro,-z,now" all
	strip --strip-all $(TARGET) $(TARGET_TUI)
	@mkdir -p bin
	@cp -f $(TARGET) bin/$(TARGET)
	@cp -f $(TARGET_TUI) bin/$(TARGET_TUI)
	@echo "=== Release-Build abgeschlossen: Binaries vollständig gestrippt und optimiert (-O3, -flto) ==="

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(TARGET_TUI): $(TUI_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TUI_OBJS) $(TUI_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS) $(TUI_DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TUI_OBJS) $(TUI_DEPS) $(TARGET) $(TARGET_TUI)

test: $(TARGET)
	./tests/run_tests.sh

benchmark: $(TARGET)
	./tests/benchmark_compare.sh

man:
	man -l man/blkcp.1

tui: $(TARGET_TUI)
	./$(TARGET_TUI)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
BASHCOMPDIR ?= $(PREFIX)/share/bash-completion/completions
ZSHCOMPDIR ?= $(PREFIX)/share/zsh/site-functions
FISHCOMPDIR ?= $(PREFIX)/share/fish/vendor_completions.d

install: $(TARGET) $(TARGET_TUI)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 755 $(TARGET_TUI) $(DESTDIR)$(BINDIR)/$(TARGET_TUI)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 man/blkcp.1 $(DESTDIR)$(MANDIR)/blkcp.1
	install -d $(DESTDIR)$(BASHCOMPDIR)
	install -m 644 completions/bash/blkcp $(DESTDIR)$(BASHCOMPDIR)/blkcp
	install -d $(DESTDIR)$(ZSHCOMPDIR)
	install -m 644 completions/zsh/_blkcp $(DESTDIR)$(ZSHCOMPDIR)/_blkcp
	install -d $(DESTDIR)$(FISHCOMPDIR)
	install -m 644 completions/fish/blkcp.fish $(DESTDIR)$(FISHCOMPDIR)/blkcp.fish
	@echo "=== Installation erfolgreich in $(DESTDIR)$(BINDIR) abgeschlossen ==="

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_TUI)
	rm -f $(DESTDIR)$(MANDIR)/blkcp.1
	rm -f $(DESTDIR)$(BASHCOMPDIR)/blkcp
	rm -f $(DESTDIR)$(ZSHCOMPDIR)/_blkcp
	rm -f $(DESTDIR)$(FISHCOMPDIR)/blkcp.fish
	@echo "=== Deinstallation erfolgreich abgeschlossen ==="

.PHONY: all release clean test benchmark man tui install uninstall
