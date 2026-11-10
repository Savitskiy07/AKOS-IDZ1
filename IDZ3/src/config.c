#include "config.h"
#include "io.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *key;
    const char *prompt;
    size_t offset;
    int minimum;
    int maximum;
} NumberField;

#define FIELD(member, prompt, min, max) \
    {#member, prompt, offsetof(Config, member), min, max}
static const NumberField number_fields[] = {
    FIELD(capacity, "Вместимость галереи", 1, MAX_VISITORS),
    FIELD(paintings, "Количество картин", 1, MAX_PAINTINGS),
    FIELD(picture_capacity, "Мест возле каждой картины", 1, MAX_VISITORS),
    FIELD(visitors, "Количество посетителей", 0, MAX_VISITORS),
    FIELD(arrival_min_ms, "Минимальный интервал прибытия, мс", 0, MAX_DELAY_MS),
    FIELD(arrival_max_ms, "Максимальный интервал прибытия, мс", 0, MAX_DELAY_MS),
    FIELD(view_min_ms, "Минимальное время осмотра, мс", 0, MAX_DELAY_MS),
    FIELD(view_max_ms, "Максимальное время осмотра, мс", 0, MAX_DELAY_MS)
};
#undef FIELD

enum {
    OPT_CAPACITY = 1000, OPT_PAINTINGS, OPT_PICTURE_CAPACITY, OPT_VISITORS,
    OPT_ARRIVAL_MIN, OPT_ARRIVAL_MAX, OPT_VIEW_MIN, OPT_VIEW_MAX,
    OPT_SEED, OPT_STRATEGY
};

static const struct option options[] = {
    {"capacity", required_argument, NULL, OPT_CAPACITY},
    {"paintings", required_argument, NULL, OPT_PAINTINGS},
    {"picture-capacity", required_argument, NULL, OPT_PICTURE_CAPACITY},
    {"visitors", required_argument, NULL, OPT_VISITORS},
    {"arrival-min-ms", required_argument, NULL, OPT_ARRIVAL_MIN},
    {"arrival-max-ms", required_argument, NULL, OPT_ARRIVAL_MAX},
    {"view-min-ms", required_argument, NULL, OPT_VIEW_MIN},
    {"view-max-ms", required_argument, NULL, OPT_VIEW_MAX},
    {"seed", required_argument, NULL, OPT_SEED},
    {"strategy", required_argument, NULL, OPT_STRATEGY},
    {"config", required_argument, NULL, 'c'},
    {"log", required_argument, NULL, 'l'},
    {"quiet", no_argument, NULL, 'q'},
    {"interactive", no_argument, NULL, 'i'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
};

static const char *option_keys[] = {
    "capacity", "paintings", "picture_capacity", "visitors",
    "arrival_min_ms", "arrival_max_ms", "view_min_ms", "view_max_ms",
    "seed", "strategy"
};

const char *strategy_name(Strategy strategy) {
    return strategy == STRATEGY_RANDOM ? "random" : "ordered";
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }
    return text;
}

static bool parse_number(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value) {
    if (*text == '\0') {
        return false;
    }
    for (const char *p = text; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static int copy_path(char *destination, const char *source) {
    size_t length = strlen(source);
    if (length == 0 || length >= PATH_SIZE) {
        dprintf(STDERR_FILENO, "Путь должен содержать 1..%d байт.\n", PATH_SIZE - 1);
        return -1;
    }
    memcpy(destination, source, length + 1);
    return 0;
}

/* возвращает индекс ключа для обнаружения повторов в конфигурации */
static int set_field(Config *config, const char *key, const char *value) {
    size_t count = sizeof(number_fields) / sizeof(number_fields[0]);
    for (size_t i = 0; i < count; i++) {
        const NumberField *field = &number_fields[i];
        if (strcmp(key, field->key) == 0) {
            uint64_t number;
            if (!parse_number(value, (uint64_t)field->minimum,
                              (uint64_t)field->maximum, &number)) {
                dprintf(STDERR_FILENO, "%s: ожидается целое число %d..%d.\n",
                        key, field->minimum, field->maximum);
                return -1;
            }
            *(int *)((char *)config + field->offset) = (int)number;
            return (int)i;
        }
    }
    if (strcmp(key, "seed") == 0) {
        uint64_t number;
        if (!parse_number(value, 0, UINT32_MAX, &number)) {
            dprintf(STDERR_FILENO, "seed: ожидается целое число 0..4294967295.\n");
            return -1;
        }
        config->seed = (uint32_t)number;
        return 8;
    }
    if (strcmp(key, "strategy") == 0) {
        if (strcmp(value, "random") == 0) {
            config->strategy = STRATEGY_RANDOM;
        } else if (strcmp(value, "ordered") == 0) {
            config->strategy = STRATEGY_ORDERED;
        } else {
            dprintf(STDERR_FILENO, "strategy: допустимы random и ordered.\n");
            return -1;
        }
        return 9;
    }
    if (strcmp(key, "log") == 0) {
        return copy_path(config->log_path, value) == 0 ? 10 : -1;
    }
    if (strcmp(key, "quiet") == 0) {
        uint64_t number;
        if (!parse_number(value, 0, 1, &number)) {
            dprintf(STDERR_FILENO, "quiet: допустимы 0 и 1.\n");
            return -1;
        }
        config->quiet = number != 0;
        return 11;
    }
    dprintf(STDERR_FILENO, "Неизвестный параметр: %s.\n", key);
    return -1;
}

static int read_config(Config *config) {
    int fd = open(config->config_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть конфигурацию: %s\n", strerror(errno));
        return -1;
    }
    char buffer[PATH_SIZE + 128];
    unsigned int line = 0;
    unsigned int seen = 0;
    int status;
    while ((status = read_line(fd, buffer, sizeof(buffer))) > 0) {
        line++;
        char *comment = strchr(buffer, '#');
        if (comment != NULL) {
            *comment = '\0';
        }
        char *key = trim(buffer);
        if (*key == '\0') {
            continue;
        }
        char *separator = strchr(key, '=');
        if (separator == NULL) {
            status = -1;
            break;
        }
        *separator = '\0';
        int index = set_field(config, trim(key), trim(separator + 1));
        if (index < 0 || (seen & (1U << index)) != 0) {
            status = -1;
            break;
        }
        seen |= 1U << index;
    }
    if (status < 0) {
        dprintf(STDERR_FILENO, "Ошибка конфигурации около строки %u "
                "(формат key=value, ключи не повторяются).\n", line + (status == -2));
    }
    if (close(fd) != 0) {
        dprintf(STDERR_FILENO, "Ошибка закрытия конфигурации: %s\n", strerror(errno));
        return -1;
    }
    return status < 0 ? -1 : 0;
}

static int prompt_field(Config *config, const char *key, const char *label,
                         const char *current) {
    for (;;) {
        if (dprintf(STDOUT_FILENO, "%s [%s]: ", label, current) < 0) {
            return -1;
        }
        char buffer[128];
        int result = read_line(STDIN_FILENO, buffer, sizeof(buffer));
        if (result == -2) {
            dprintf(STDERR_FILENO, "Слишком длинная строка. Повторите ввод.\n");
            continue;
        }
        if (result <= 0) {
            dprintf(STDERR_FILENO, "Ввод прерван: параметры не заданы полностью.\n");
            return -1;
        }
        char *value = trim(buffer);
        if (*value == '\0' || set_field(config, key, value) >= 0) {
            return 0;
        }
    }
}

static int interactive_input(Config *config) {
    for (size_t i = 0; i < sizeof(number_fields) / sizeof(number_fields[0]); i++) {
        const NumberField *field = &number_fields[i];
        char current[32];
        snprintf(current, sizeof(current), "%d",
                 *(int *)((char *)config + field->offset));
        if (prompt_field(config, field->key, field->prompt, current) != 0) {
            return -1;
        }
    }
    char current[32];
    snprintf(current, sizeof(current), "%" PRIu32, config->seed);
    if (prompt_field(config, "seed", "Начальное значение генератора", current) != 0) {
        return -1;
    }
    return prompt_field(config, "strategy", "Стратегия (random/ordered)",
                        strategy_name(config->strategy));
}

static void print_help(void) {
    const char *help =
        "Картинная галерея. Вариант 16. Последовательная модель на C.\n"
        "Сборка: cmake -S . -B build-cmake && cmake --build build-cmake.\n"
        "Запуск: ./build-cmake/gallery [параметры]\n\n"
        "  --capacity N          Мест в галерее: 1..1000 (10)\n"
        "  --paintings N         Картин: 1..64 (5)\n"
        "  --picture-capacity N  Мест у каждой картины: 1..1000 (2)\n"
        "  --visitors N          Посетителей: 0..1000 (200)\n"
        "  --arrival-min-ms N    Нижняя граница интервала прибытия (10)\n"
        "  --arrival-max-ms N    Верхняя граница интервала прибытия (30)\n"
        "  --view-min-ms N       Нижняя граница времени осмотра (40)\n"
        "  --view-max-ms N       Верхняя граница времени осмотра (100)\n"
        "                        Все задержки: 0..60000 мс, min <= max\n"
        "  --strategy NAME       random (по умолчанию) или ordered\n"
        "  --seed N              0..4294967295 (42)\n"
        "  -c, --config FILE     Конфигурация key=value\n"
        "  -l, --log FILE        Журнал (gallery.log), '-' отключает файл\n"
        "  -q, --quiet           Только параметры и итоги в консоли\n"
        "  -i, --interactive     Ввести параметры через stdin\n"
        "  -h, --help            Эта справка\n\n"
        "Приоритет: значения по умолчанию < файл < CLI < интерактивный ввод.\n"
        "Ctrl+C / SIGTERM: остановка и освобождение занятых мест.\n"
        "Существующий журнал перезаписывается. Пути относительны к cwd.\n";
    (void)write_all(STDOUT_FILENO, help, strlen(help));
}

int config_parse(Config *config, int argc, char **argv) {
    *config = (Config){
        .capacity = 10, .paintings = 5, .picture_capacity = 2, .visitors = 200,
        .arrival_min_ms = 10, .arrival_max_ms = 30,
        .view_min_ms = 40, .view_max_ms = 100, .seed = 42,
        .strategy = STRATEGY_RANDOM, .log_path = "gallery.log"
    };
    opterr = 0;
    int option;
    while ((option = getopt_long(argc, argv, "c:l:qih", options, NULL)) != -1) {
        if (option == '?') {
            dprintf(STDERR_FILENO, "Неизвестный или неполный параметр. См. --help.\n");
            return -1;
        }
        if (option == 'h') {
            print_help();
            return 1;
        }
        if (option == 'c') {
            if (config->config_path[0] != '\0') {
                dprintf(STDERR_FILENO, "Допускается только один --config.\n");
                return -1;
            }
            if (copy_path(config->config_path, optarg) != 0) {
                return -1;
            }
        }
    }
    if (optind != argc) {
        dprintf(STDERR_FILENO, "Лишний позиционный аргумент: %s\n", argv[optind]);
        return -1;
    }
    if (config->config_path[0] != '\0' && read_config(config) != 0) {
        return -1;
    }
    optind = 0;
    while ((option = getopt_long(argc, argv, "c:l:qih", options, NULL)) != -1) {
        if (option >= OPT_CAPACITY && option <= OPT_STRATEGY) {
            if (set_field(config, option_keys[option - OPT_CAPACITY], optarg) < 0) {
                return -1;
            }
        } else if (option == 'l') {
            if (copy_path(config->log_path, optarg) != 0) {
                return -1;
            }
        } else if (option == 'q') {
            config->quiet = true;
        } else if (option == 'i') {
            config->interactive = true;
        }
    }
    if (config->interactive && interactive_input(config) != 0) {
        return -1;
    }
    if (config->arrival_min_ms > config->arrival_max_ms ||
        config->view_min_ms > config->view_max_ms) {
        dprintf(STDERR_FILENO, "Минимальная задержка не может быть больше максимальной.\n");
        return -1;
    }
    return 0;
}
