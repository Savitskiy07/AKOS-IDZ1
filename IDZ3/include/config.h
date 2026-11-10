#ifndef GALLERY_CONFIG_H
#define GALLERY_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_VISITORS 1000
#define MAX_PAINTINGS 64
#define MAX_DELAY_MS 60000
#define PATH_SIZE 4096

typedef enum { STRATEGY_RANDOM, STRATEGY_ORDERED } Strategy;

typedef struct {
    int capacity;
    int paintings;
    int picture_capacity;
    int visitors;
    int arrival_min_ms;
    int arrival_max_ms;
    int view_min_ms;
    int view_max_ms;
    uint32_t seed;
    Strategy strategy;
    bool quiet;
    bool interactive;
    char log_path[PATH_SIZE];
    char config_path[PATH_SIZE];
} Config;

/* 0: параметры готовы; 1: справка напечатана; -1: ошибка ввода */
int config_parse(Config *config, int argc, char **argv);
const char *strategy_name(Strategy strategy);

#endif
