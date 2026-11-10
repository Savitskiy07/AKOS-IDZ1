CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread -lrt
COMMON = build/common.o build/ipc.o build/model.o
BINS = bin/gallery-named bin/gallery-unnamed bin/guard bin/visitor

.PHONY: all clean test
all: $(BINS)

build/%.o: src/%.c src/gallery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/day-named.o: DAY_MODE = NAMED
build/day-unnamed.o: DAY_MODE = UNNAMED
build/day-named.o build/day-unnamed.o: src/day.c src/gallery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DDAY_MODE=$(DAY_MODE) -c $< -o $@

bin/gallery-named bin/gallery-unnamed: bin/gallery-%: build/day-%.o $(COMMON) | bin
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/guard: build/guard.o $(COMMON) | bin
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/visitor: build/visitor.o $(COMMON) | bin
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin build:
	mkdir -p $@

test: all
	python3 tests/test_gallery.py

clean:
	rm -rf bin build
