#include "gallery.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t stopped;

static void on_stop(int sig) {
    if (!stopped || sig == SIGINT) {
        stopped = sig;
    }
}

int install_signals(void) {
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    action.sa_handler = on_stop;
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        return -1;
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        struct pollfd output = {.fd = fd, .events = POLLOUT};
        int ready = poll(&output, 1, stopped ? 0 : 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (!ready) {
            if (!stopped) {
                continue;
            }
            errno = EINTR;
            return -1;
        }
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN && !stopped) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int number(const char *s, int min, int max, int *out) {
    if (!s || !isdigit((unsigned char)*s)) {
        return -1;
    }
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno || *end || value < min || value > max) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int interval(const char *s, int *lo, int *hi) {
    const char *colon = strchr(s, ':');
    if (!colon) {
        if (number(s, 0, 60000, lo)) {
            return -1;
        }
        *hi = *lo;
        return 0;
    }
    char left[16];
    size_t n = (size_t)(colon - s);
    if (n >= sizeof(left)) {
        return -1;
    }
    memcpy(left, s, n);
    left[n] = '\0';
    return number(left, 0, 60000, lo) || number(colon + 1, *lo, 60000, hi);
}

void usage(const char *name) {
    dprintf(
        STDOUT_FILENO,
        "Запуск: %s [параметры]\n"
        "  --capacity N    вместимость галереи, 1..300 (12)\n"
        "  --paintings N   число картин, 1..64 (5)\n"
        "  --limit N|A,B   места у картин: одно число или список по числу картин (1..300, по умолчанию 3)\n"
        "  --visitors N    число посетителей, 1..300 (120)\n"
        "  --arrival A:B   пауза между прибытиями в мс (2:8)\n"
        "  --view A:B      длительность одного осмотра в мс (10:30)\n"
        "  --strategy S    random или ordered (random)\n"
        "  --seed N        зерно, 0..2147483647 (по времени и PID)\n"
        "  --log FILE      копия журнала в файл (по умолчанию только stdout)\n"
        "Для интервала допустимы 0..60000 мс, одно число задаёт постоянную паузу.\n",
        name
    );
}

static int read_limits(Config *c, const char *s) {
    int common;
    if (!strchr(s, ',')) {
        if (number(s, 1, MAX_VISITORS, &common)) {
            return -1;
        }
        for (int i = 0; i < c->paintings; i++) {
            c->limits[i] = common;
        }
        return 0;
    }
    for (int i = 0; i < c->paintings; i++) {
        if (!isdigit((unsigned char)*s)) {
            return -1;
        }
        char *end;
        errno = 0;
        long value = strtol(s, &end, 10);
        if (errno || value < 1 || value > MAX_VISITORS) {
            return -1;
        }
        c->limits[i] = (int)value;
        if (i + 1 == c->paintings) {
            return *end ? -1 : 0;
        }
        if (*end != ',') {
            return -1;
        }
        s = end + 1;
    }
    return -1;
}

int config_read(Config *c, int argc, char **argv) {
    const char *limits = "3";
    *c = (Config){
        .capacity = 12,
        .paintings = 5,
        .visitors = 120,
        .arrival_min = 2,
        .arrival_max = 8,
        .view_min = 10,
        .view_max = 30,
        .random_order = true,
        .seed = ((uint32_t)time(NULL) ^ (uint32_t)getpid()) & INT_MAX
    };

    for (int i = 0; i < argc; i += 2) {
        const char *opt = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        int bad = 1, seed;
        if (!v) {
            goto invalid;
        }

        if (!strcmp(opt, "--capacity")) {
            bad = number(v, 1, MAX_VISITORS, &c->capacity);
        } else if (!strcmp(opt, "--paintings")) {
            bad = number(v, 1, MAX_PAINTINGS, &c->paintings);
        } else if (!strcmp(opt, "--limit")) {
            limits = v;
            bad = 0;
        } else if (!strcmp(opt, "--visitors")) {
            bad = number(v, 1, MAX_VISITORS, &c->visitors);
        } else if (!strcmp(opt, "--arrival")) {
            bad = interval(v, &c->arrival_min, &c->arrival_max);
        } else if (!strcmp(opt, "--view")) {
            bad = interval(v, &c->view_min, &c->view_max);
        } else if (!strcmp(opt, "--strategy")) {
            c->random_order = !strcmp(v, "random");
            bad = !c->random_order && strcmp(v, "ordered");
        } else if (!strcmp(opt, "--seed")) {
            bad = number(v, 0, INT_MAX, &seed);
            if (!bad) {
                c->seed = (uint32_t)seed;
            }
        } else if (!strcmp(opt, "--log") && *v) {
            c->log_path = v;
            bad = 0;
        }
        if (!bad) {
            continue;
        }

invalid:
        dprintf(STDERR_FILENO, "Ошибка параметра %s%s%s\n", opt, v ? " " : "", v ? v : "");
        return -1;
    }
    if (read_limits(c, limits)) {
        dprintf(STDERR_FILENO, "Ошибка --limit: задайте число или по одному числу на картину.\n");
        return -1;
    }
    return 0;
}

uint32_t random_next(uint32_t *s) {
    uint32_t x = (*s += UINT32_C(0x9e3779b9));
    x = (x ^ (x >> 16)) * UINT32_C(0x21f0aaad);
    x = (x ^ (x >> 15)) * UINT32_C(0x735a2d97);
    return x ^ (x >> 15);
}

int sample(uint32_t *s, int lo, int hi) {
    return lo + (int)(random_next(s) % (uint32_t)(hi - lo + 1));
}
