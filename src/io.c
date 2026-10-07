#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int write_all(int fd, const char *data, size_t length) {
    while (length != 0) {
        ssize_t result = write(fd, data, length);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = EIO;
            }
            return -1;
        }
        data += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

int read_line(int fd, char *buffer, size_t size) {
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        char ch;
        ssize_t result = read(fd, &ch, 1);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            return -1;
        }
        if (result == 0 || ch == '\n') {
            buffer[used] = '\0';
            if (overflow) {
                return -2;
            }
            return result == 0 && used == 0 ? 0 : 1;
        }
        if (used + 1 < size) {
            buffer[used++] = ch;
        } else {
            overflow = true;
        }
    }
}

int logger_open(Logger *logger, const char *path, const char *config_path,
                bool quiet) {
    memset(logger, 0, sizeof(*logger));
    logger->fd = -1;
    logger->quiet = quiet;
    if (clock_gettime(CLOCK_MONOTONIC, &logger->start) != 0) {
        return -1;
    }
    if (strcmp(path, "-") == 0) {
        return 0;
    }
    /* Не обрезаем файл до проверки: пути могут быть жёсткими ссылками. */
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    struct stat output_stat;
    struct stat input_stat;
    if (fstat(fd, &output_stat) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (config_path[0] != '\0' && stat(config_path, &input_stat) == 0 &&
        output_stat.st_dev == input_stat.st_dev &&
        output_stat.st_ino == input_stat.st_ino) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (S_ISREG(output_stat.st_mode) && ftruncate(fd, 0) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    logger->fd = fd;
    return 0;
}

bool logger_event(Logger *logger, bool always, const char *event,
                  int visitor, int painting, int inside, int reserved,
                  int queue, int viewers, const char *details) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        logger->failed = true;
        return false;
    }
    int64_t elapsed = (int64_t)(now.tv_sec - logger->start.tv_sec) * 1000 +
                      (now.tv_nsec - logger->start.tv_nsec) / 1000000;
    char line[4096];
    int length = snprintf(line, sizeof(line),
        "%06" PRIu64 " t_ms=%" PRId64 " event=%s visitor=%d painting=%d "
        "inside=%d reserved=%d queue=%d viewers=%d | %s\n",
        ++logger->sequence, elapsed, event, visitor, painting,
        inside, reserved, queue, viewers, details);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        logger->failed = true;
        return false;
    }
    if ((!logger->quiet || always) && !logger->stdout_failed &&
        write_all(STDOUT_FILENO, line, (size_t)length) != 0) {
        logger->stdout_failed = true;
        logger->failed = true;
        dprintf(STDERR_FILENO, "Ошибка вывода в stdout: %s\n", strerror(errno));
    }
    if (logger->fd >= 0 && !logger->file_failed &&
        write_all(logger->fd, line, (size_t)length) != 0) {
        logger->file_failed = true;
        logger->failed = true;
        dprintf(STDERR_FILENO, "Ошибка записи журнала: %s\n", strerror(errno));
    }
    return !logger->failed;
}

int logger_close(Logger *logger) {
    if (logger->fd >= 0 && close(logger->fd) != 0) {
        logger->fd = -1;
        return -1;
    }
    logger->fd = -1;
    return 0;
}
