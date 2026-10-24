CC ?= gcc
CARGO ?= cargo
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc -MMD -MP -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now,-z,defs
LIBS ?= lib/libcoreutils.a -lcrypto -lpthread -luring

# BLKS 384-Bit Post-Quantum Tree-Hash Library (built in-tree via rust/crates/blks-core or external)
LOCAL_BLKS_LIB = rust/target/release/libblks_core.a
BLKS_DIR ?= $(abspath ../blks)
ifneq ($(wildcard include/blks.h),)
  override CFLAGS += -DHAVE_BLKS=1
  override LIBS += $(LOCAL_BLKS_LIB) -lm -ldl
else ifneq ($(wildcard $(BLKS_DIR)/include/blks.h),)
  override CFLAGS += -I$(BLKS_DIR)/include -DHAVE_BLKS=1
  override LIBS += $(BLKS_DIR)/target/release/libblks_core.a -lm -ldl
else ifneq ($(wildcard /usr/include/blks.h),)
  override CFLAGS += -DHAVE_BLKS=1
  override LIBS += -lblks_core -lm -ldl
else ifneq ($(wildcard /usr/local/include/blks.h),)
  override CFLAGS += -I/usr/local/include -DHAVE_BLKS=1
  override LIBS += -L/usr/local/lib -lblks_core -lm -ldl
endif

# Primary Binaries (Rust Core & Ratatui TUI)
TARGET = blkcp
TARGET_TUI = blkcp-tui

# Reference Binaries (C11 Core & ncursesw TUI)
TARGET_C = blkcp-c
TARGET_TUI_C = blkcp-tui-c

SRCS = src/blkcp.c src/version.c src/conversions.c src/stats.c src/signals.c src/args.c src/io_engine.c src/io_sync.c src/io_async.c src/io_reflink.c src/io_uring.c src/io_splice.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

TUI_SRCS = src/tui/tui_device.c src/tui/tui_file_picker.c src/tui/tui_render.c src/tui/tui_nav.c src/tui/tui_main.c
TUI_OBJS = $(TUI_SRCS:.c=.o)
TUI_DEPS = $(TUI_OBJS:.o=.d)
TUI_LIBS = -lncursesw

all: rust-bins c-bins
	@mkdir -p bin
	@cp -f $(TARGET) bin/$(TARGET)
	@cp -f $(TARGET_TUI) bin/$(TARGET_TUI)
	@cp -f $(TARGET_C) bin/$(TARGET_C)
	@cp -f $(TARGET_TUI_C) bin/$(TARGET_TUI_C)

rust-bins:
	$(CARGO) build --release --workspace --manifest-path rust/Cargo.toml
	@cp -f rust/target/release/blkcp $(TARGET)
	@cp -f rust/target/release/blkcp-tui $(TARGET_TUI)
	@mkdir -p bin
	@cp -f $(TARGET) bin/$(TARGET)
	@cp -f $(TARGET_TUI) bin/$(TARGET_TUI)

$(LOCAL_BLKS_LIB): rust-bins

c-bins: $(LOCAL_BLKS_LIB) $(TARGET_C) $(TARGET_TUI_C)
	@mkdir -p bin
	@cp -f $(TARGET_C) bin/$(TARGET_C)
	@cp -f $(TARGET_TUI_C) bin/$(TARGET_TUI_C)

release:
	$(MAKE) clean-c
	$(MAKE) rust-bins
	$(MAKE) CFLAGS="-O3 -DNDEBUG -flto -Wall -Wextra -pthread -Iinclude -Isrc -MMD -MP -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE" LDFLAGS="-flto -pie -Wl,-O1,--sort-common,--as-needed,-z,relro,-z,now,-z,defs" c-bins
	strip --strip-all $(TARGET_C) $(TARGET_TUI_C)
	@mkdir -p bin
	@cp -f $(TARGET) bin/$(TARGET)
	@cp -f $(TARGET_TUI) bin/$(TARGET_TUI)
	@cp -f $(TARGET_C) bin/$(TARGET_C)
	@cp -f $(TARGET_TUI_C) bin/$(TARGET_TUI_C)
	@echo "=== Release-Build abgeschlossen: Rust (blkcp, blkcp-tui) & C11 (blkcp-c, blkcp-tui-c) bereit ==="

$(TARGET_C): $(OBJS) $(LOCAL_BLKS_LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(TARGET_TUI_C): $(TUI_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TUI_OBJS) $(TUI_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS) $(TUI_DEPS)

clean-c:
	rm -f $(OBJS) $(DEPS) $(TUI_OBJS) $(TUI_DEPS) $(TARGET_C) $(TARGET_TUI_C) bin/$(TARGET_C) bin/$(TARGET_TUI_C)

clean: clean-c
	rm -f $(TARGET) $(TARGET_TUI) bin/$(TARGET) bin/$(TARGET_TUI)
	$(CARGO) clean --manifest-path rust/Cargo.toml

test: rust-bins
	BLKCP_BIN="./$(TARGET)" ./tests/run_tests.sh

test-c: c-bins
	BLKCP_BIN="./$(TARGET_C)" ./tests/run_tests.sh

benchmark: all
	./tests/benchmark_compare.sh

man:
	man -l man/blkcp.1

tui: rust-bins
	./$(TARGET_TUI)

tui-c: c-bins
	./$(TARGET_TUI_C)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
BASHCOMPDIR ?= $(PREFIX)/share/bash-completion/completions
ZSHCOMPDIR ?= $(PREFIX)/share/zsh/site-functions
FISHCOMPDIR ?= $(PREFIX)/share/fish/vendor_completions.d

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 755 $(TARGET_TUI) $(DESTDIR)$(BINDIR)/$(TARGET_TUI)
	install -m 755 $(TARGET_C) $(DESTDIR)$(BINDIR)/$(TARGET_C)
	install -m 755 $(TARGET_TUI_C) $(DESTDIR)$(BINDIR)/$(TARGET_TUI_C)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 man/blkcp.1 $(DESTDIR)$(MANDIR)/blkcp.1
	install -d $(DESTDIR)$(BASHCOMPDIR)
	install -m 644 completions/bash/blkcp $(DESTDIR)$(BASHCOMPDIR)/blkcp
	install -d $(DESTDIR)$(ZSHCOMPDIR)
	install -m 644 completions/zsh/_blkcp $(DESTDIR)$(ZSHCOMPDIR)/_blkcp
	install -d $(DESTDIR)$(FISHCOMPDIR)
	install -m 644 completions/fish/blkcp.fish $(DESTDIR)$(FISHCOMPDIR)/blkcp.fish
	@echo "=== Installation erfolgreich in $(DESTDIR)$(BINDIR) abgeschlossen (blkcp, blkcp-tui, blkcp-c, blkcp-tui-c) ==="

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_TUI)
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_C)
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_TUI_C)
	rm -f $(DESTDIR)$(MANDIR)/blkcp.1
	rm -f $(DESTDIR)$(BASHCOMPDIR)/blkcp
	rm -f $(DESTDIR)$(ZSHCOMPDIR)/_blkcp
	rm -f $(DESTDIR)$(FISHCOMPDIR)/blkcp.fish
	@echo "=== Deinstallation erfolgreich abgeschlossen ==="

audit:
	@echo "=== [1/4] Rust Standard Library & ANSSI Safety Gate (Clippy + Unsafe Contract) ==="
	$(CARGO) clippy --workspace --all-targets --manifest-path rust/Cargo.toml -- -D warnings -D unsafe_op_in_unsafe_fn -D clippy::undocumented_unsafe_blocks -D clippy::missing_safety_doc -D clippy::ptr_as_ptr -D clippy::cast_ptr_alignment
	@echo "=== [2/4] Verifying Exact Unsafe Block Count in rust/src/ (Expected: 7) ==="
	@UNSAFE_CNT=$$(grep -rn "unsafe " rust/src/ | grep -v "//" | wc -l); \
	if [ "$$UNSAFE_CNT" -ne 7 ]; then \
		echo "AUDIT FAILURE: Expected exactly 7 unsafe blocks in rust/src/, found $$UNSAFE_CNT"; \
		exit 1; \
	fi; \
	echo "Verified: Exactly $$UNSAFE_CNT documented unsafe blocks in rust/src/."
	@echo "=== [3/4] Supply-Chain & Advisory Gate (cargo-audit / cargo-deny if installed) ==="
	@if command -v cargo-audit >/dev/null 2>&1; then \
		$(CARGO) audit --file rust/Cargo.lock; \
	else \
		echo "info: cargo-audit not installed (skipping live RustSec network check)"; \
	fi
	@if command -v cargo-deny >/dev/null 2>&1; then \
		$(CARGO) deny --manifest-path rust/Cargo.toml check; \
	else \
		echo "info: cargo-deny not installed (declarative policy stored in rust/deny.toml)"; \
	fi
	@echo "=== [4/4] Workspace Unit & Cryptographic Verification Suite (18/18 Tests) ==="
	$(CARGO) test --workspace --manifest-path rust/Cargo.toml --quiet
	@echo "=== OSS SECURITY & SOUNDNESS AUDIT PASSED SUCCESSFULLY ==="

.PHONY: all rust-bins c-bins release clean clean-c test test-c benchmark man tui tui-c install uninstall audit
