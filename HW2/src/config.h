#ifndef GALLERY_CONFIG_H
#define GALLERY_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
#define MAX_VISITORS 1000
#define MAX_PAINTINGS 100
typedef struct {
    unsigned capacity;
    unsigned paintings;
    unsigned visitors;
    unsigned picture_capacity[MAX_PAINTINGS];
    unsigned arrival_min;
    unsigned arrival_max;
    unsigned view_min;
    unsigned view_max;
    uint32_t seed;
    bool random_order;
    bool quiet;
    char log_path[4096];
} Config;

/* 0 = успешно, 1 = справка, -1 = ошибка. */
int parse_config(int argc, char **argv, Config *config);
#endif
