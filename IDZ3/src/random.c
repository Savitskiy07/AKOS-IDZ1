#include "random.h"

/* беззнаковое переполнение здесь является частью алгоритма */
uint64_t random_next(uint64_t *state) {
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

int random_between(uint64_t *state, int minimum, int maximum) {
    uint64_t bound = (uint64_t)(maximum - minimum) + 1;
    uint64_t threshold = (UINT64_C(0) - bound) % bound;
    uint64_t value;
    do {
        value = random_next(state);
    } while (value < threshold);
    return minimum + (int)(value % bound);
}
