#ifndef GALLERY_SYNC_H
#define GALLERY_SYNC_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#ifndef USE_CONDVAR
#include <semaphore.h>
#endif

typedef struct {
#ifdef USE_CONDVAR
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned count;
#else
    sem_t sem;
#endif
} Gate;

int gate_init(Gate *gate, unsigned count);

void gate_destroy(Gate *gate);

void gate_post(Gate *gate);

bool gate_try_wait(Gate *gate);

bool gate_wait(Gate *gate, const atomic_bool *stop);

const char *sync_backend(void);
#endif
