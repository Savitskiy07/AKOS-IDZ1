#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    Config c;
    char capacities[1024];
} Parser;

static int error(const char *key) {
    dprintf(STDERR_FILENO, "Ошибка параметра: %s. Используйте --help.\n", key);
    return -1;
}

static bool number(const char *s, unsigned long max, unsigned *out) {
    if (!s || !*s) {
        return false;
    }

    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }

    errno = 0;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v > max) {
        return false;
    }

    *out = (unsigned)v;
    return true;
}

static int set_option(Parser *p, const char *key, const char *value) {
    unsigned v;
    if (!strcmp(key, "strategy")) {
        if (strcmp(value, "random") && strcmp(value, "sequential")) {
            return error(key);
        }

        p->c.random_order = !strcmp(value, "random");
    } else if (!strcmp(key, "picture-capacity")) {
        if (strlen(value) >= sizeof(p->capacities)) {
            return error(key);
        }

        strcpy(p->capacities, value);
    } else if (!strcmp(key, "log")) {
        if (!*value || strlen(value) >= sizeof(p->c.log_path)) {
            return error(key);
        }

        strcpy(p->c.log_path, value);
    } else {
        if (!number(value, UINT_MAX, &v)) {
            return error(key);
        }

        if (!strcmp(key, "capacity")) {
            p->c.capacity = v;
        } else if (!strcmp(key, "paintings")) {
            p->c.paintings = v;
        } else if (!strcmp(key, "visitors")) {
            p->c.visitors = v;
        } else if (!strcmp(key, "arrival-min")) {
            p->c.arrival_min = v;
        } else if (!strcmp(key, "arrival-max")) {
            p->c.arrival_max = v;
        } else if (!strcmp(key, "view-min")) {
            p->c.view_min = v;
        } else if (!strcmp(key, "view-max")) {
            p->c.view_max = v;
        } else if (!strcmp(key, "seed")) {
            p->c.seed = (uint32_t)v;
        } else if (!strcmp(key, "quiet") && v <= 1) {
            p->c.quiet = v != 0;
        } else {
            return error(key);
        }
    }

    return 0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') {
        s++;
    }

    size_t len = strlen(s);
    while (len && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r')) {
        len--;
        s[len] = 0;
    }

    return s;
}

static int read_config(Parser *p, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "Не открыть конфигурацию %s: %s\n", path, strerror(errno));
        return -1;
    }

    char buf[16385];
    size_t used = 0;
    while (used < sizeof(buf)-1) {
        ssize_t n = read(fd, buf+used, sizeof(buf)-1-used);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n < 0) {
            close(fd);
            return error("чтение конфигурации");
        }

        if (!n) {
            break;
        }

        used += (size_t)n;
    }

    close(fd);
    if (used == sizeof(buf)-1 || memchr(buf, 0, used)) {
        return error("размер/формат конфигурации");
    }

    buf[used] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *comment = strchr(line, '#');
        if (comment) {
            *comment = 0;
        }

        char *key = trim(line);
        if (!*key) {
            continue;
        }

        char *eq = strchr(key, '=');
        if (!eq) {
            return error(key);
        }

        *eq = 0;
        if (set_option(p, trim(key), trim(eq+1)) != 0) {
            return -1;
        }
    }

    return 0;
}

static void help(void) {
    dprintf(1,
        "Вариант 16. Картинная галерея (C11, pthread, Linux).\n"
        "Использование: ./gallery [параметры]\n"
        "  --capacity N           вместимость галереи, 1..1000 (10)\n"
        "  --paintings N          число картин, 1..100 (5)\n"
        "  --picture-capacity N   мест у каждой картины (2), или список: 1,2,3\n"
        "  --visitors N           число посетителей, 0..1000 (200)\n"
        "  --arrival-min MS       минимум интервала прибытия (1)\n"
        "  --arrival-max MS       максимум интервала прибытия (5)\n"
        "  --view-min MS          минимум времени осмотра (5)\n"
        "  --view-max MS          максимум времени осмотра (15)\n"
        "  --strategy NAME        random (по умолчанию) или sequential\n"
        "  --seed N               начальное значение ГПСЧ, 0..4294967295 (16)\n"
        "  --config FILE          key=value; CLI переопределяет файл\n"
        "  --log FILE             журнал событий и статистики (gallery.log)\n"
        "                         значение none отключает файл\n"
        "  --quiet                только итог на stdout; файл содержит все события\n"
        "  --help                 справка\n"
        "Время в миллисекундах, 0..60000; min <= max. Ctrl+C завершает корректно.\n");
}

int parse_config(int argc, char **argv, Config *config) {
    Parser p = {
        .c = {
            .capacity = 10,
            .paintings = 5,
            .visitors = 200,
            .arrival_min = 1,
            .arrival_max = 5,
            .view_min = 5,
            .view_max = 15,
            .seed = 16,
            .random_order = true,
            .log_path = "gallery.log"
        },
        .capacities = "2"
    };
    static const struct option opts[] = {
        {"capacity", 1, 0, 100},
        {"paintings", 1, 0, 101},
        {"picture-capacity", 1, 0, 102},
        {"visitors", 1, 0, 103},
        {"arrival-min", 1, 0, 104},
        {"arrival-max", 1, 0, 105},
        {"view-min", 1, 0, 106},
        {"view-max", 1, 0, 107},
        {"strategy", 1, 0, 108},
        {"seed", 1, 0, 109},
        {"log", 1, 0, 110},
        {"config", 1, 0, 111},
        {"quiet", 0, 0, 112},
        {"help", 0, 0, 113},
        {0, 0, 0, 0}
    };
    /* Два прохода: независимо от положения --config аргументы CLI важнее. */
    const char *config_path = NULL;
    opterr = 0;
    int opt;
    int index = 0;
    while ((opt = getopt_long(argc, argv, "", opts, &index)) != -1) {
        if (opt == 113) {
            help();
            return 1;
        }

        if (opt == '?') {
            return error("неизвестная опция или отсутствует значение");
        }

        if (opt == 111) {
            if (config_path) {
                return error("--config задан повторно");
            }

            config_path = optarg;
        }
    }

    if (optind != argc) {
        return error("лишние позиционные аргументы");
    }

    if (config_path && read_config(&p, config_path)) {
        return -1;
    }

    optind = 0;
    while ((opt = getopt_long(argc, argv, "", opts, &index)) != -1) {
        if (opt == 111) {
            continue;
        }

        if (set_option(&p, opts[index].name, opt == 112 ? "1" : optarg)) {
            return -1;
        }
    }
    Config *c = &p.c;
    if (!c->capacity || c->capacity > MAX_VISITORS) {
        return error("capacity");
    }

    if (!c->paintings || c->paintings > MAX_PAINTINGS) {
        return error("paintings");
    }

    if (c->visitors > MAX_VISITORS) {
        return error("visitors");
    }

    if (c->arrival_min > c->arrival_max || c->arrival_max > 60000) {
        return error("arrival-min/max");
    }

    if (c->view_min > c->view_max || c->view_max > 60000) {
        return error("view-min/max");
    }

    unsigned count = 0;
    char *s = p.capacities;
    do {
        char *comma = strchr(s, ',');
        if (comma) {
            *comma = 0;
        }

        unsigned v;
        if (count >= c->paintings || !number(s, MAX_VISITORS, &v) || v == 0) {
            return error("picture-capacity");
        }

        c->picture_capacity[count] = v;
        count++;
        if (!comma) {
            break;
        }

        s = comma+1;
    } while (true);
    if (count == 1) {
        for (unsigned i = 1; i < c->paintings; i++) {
            c->picture_capacity[i] = c->picture_capacity[0];
        }
    } else if (count != c->paintings) {
        return error("длина списка picture-capacity");
    }

    *config = *c;
    return 0;
}
