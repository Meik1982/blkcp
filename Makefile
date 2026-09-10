CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Iinclude -Isrc -MMD -MP
LDFLAGS ?= 
LIBS ?= lib/libcoreutils.a -lcrypto

TARGET = dd
SRCS = src/dd.c src/version.c src/conversions.c src/stats.c src/signals.c src/args.c src/io_engine.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

test: $(TARGET)
	./tests/run_tests.sh

benchmark: $(TARGET)
	./tests/benchmark_compare.sh

man:
	man -l man/dd.1

.PHONY: all clean test benchmark man
