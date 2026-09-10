CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Iinclude -Isrc
LDFLAGS ?= 
LIBS ?= lib/libcoreutils.a

TARGET = dd
SRCS = src/dd.c src/version.c src/conversions.c src/stats.c src/signals.c src/args.c src/io_engine.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
