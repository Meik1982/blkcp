CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc -MMD -MP
LDFLAGS ?= 
LIBS ?= lib/libcoreutils.a -lcrypto -lpthread

TARGET = dd
TARGET_TUI = dd-tui

SRCS = src/dd.c src/version.c src/conversions.c src/stats.c src/signals.c src/args.c src/io_engine.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

TUI_SRCS = src/tui/tui_device.c src/tui/tui_file_picker.c src/tui/tui_render.c src/tui/tui_main.c
TUI_OBJS = $(TUI_SRCS:.c=.o)
TUI_DEPS = $(TUI_OBJS:.o=.d)
TUI_LIBS = -lncursesw

all: $(TARGET) $(TARGET_TUI)

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
	man -l man/dd.1

tui: $(TARGET_TUI)
	./$(TARGET_TUI)

.PHONY: all clean test benchmark man tui
