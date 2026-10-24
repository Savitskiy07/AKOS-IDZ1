CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
LDLIBS = -pthread
SOURCES = src/main.c src/config.c src/sync.c
HEADERS = src/config.h src/sync.h
SANITIZER_EXTRA ?=

.PHONY: all test audit clean sanitize thread-sanitize
all: gallery gallery_cv

gallery: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

gallery_cv: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) -DUSE_CONDVAR $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

test: all
	python3 tests/test_gallery.py

audit: all
	python3 tests/audit_gallery.py

sanitize:
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(SOURCES) $(SANITIZER_EXTRA) -o gallery_asan $(LDLIBS)
	$(CC) $(CPPFLAGS) -DUSE_CONDVAR $(CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(SOURCES) $(SANITIZER_EXTRA) -o gallery_cv_asan $(LDLIBS)
	python3 tests/test_gallery.py ./gallery_asan ./gallery_cv_asan

thread-sanitize:
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -fsanitize=thread -fno-omit-frame-pointer $(SOURCES) -o gallery_tsan $(LDLIBS)
	$(CC) $(CPPFLAGS) -DUSE_CONDVAR $(CFLAGS) -O1 -fsanitize=thread -fno-omit-frame-pointer $(SOURCES) -o gallery_cv_tsan $(LDLIBS)
	python3 tests/test_gallery.py ./gallery_tsan ./gallery_cv_tsan

clean:
	rm -f gallery gallery_cv gallery_asan gallery_cv_asan gallery_tsan gallery_cv_tsan gallery.log
