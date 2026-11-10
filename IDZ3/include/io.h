#ifndef GALLERY_IO_H
#define GALLERY_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    int fd;
    bool quiet;
    bool failed;
    bool stdout_failed;
    bool file_failed;
    uint64_t sequence;
    struct timespec start;
} Logger;

int write_all(int fd, const char *data, size_t length);
/* 1: строка прочитана; 0: EOF; -1: read; -2: строка слишком длинная */
int read_line(int fd, char *buffer, size_t size);
/* config_path защищён от случайного совпадения с выходным файлом */
int logger_open(Logger *logger, const char *path, const char *config_path,
                bool quiet);
/* вызывается из последовательного цикла модели; отдельная блокировка не нужна */
bool logger_event(Logger *logger, bool always, const char *event,
                  int visitor, int painting, int inside, int reserved,
                  int queue, int viewers, const char *details);
int logger_close(Logger *logger);

#endif
