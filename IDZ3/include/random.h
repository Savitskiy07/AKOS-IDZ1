#ifndef GALLERY_RANDOM_H
#define GALLERY_RANDOM_H

#include <stdint.h>

/* состояние генератора: общей переменной rand() нет. */
uint64_t random_next(uint64_t *state);
int random_between(uint64_t *state, int minimum, int maximum);

#endif
