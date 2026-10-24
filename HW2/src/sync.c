#define _POSIX_C_SOURCE 200809L
#include "sync.h"
#include <errno.h>
#include <stdlib.h>
#include <time.h>

static void require(int result) {
    if (result != 0) {
        abort();
    }
}

int gate_init(Gate *g, unsigned count) {
#ifdef USE_CONDVAR
    int rc = pthread_mutex_init(&g->mutex, NULL);
    if (rc) {
        return rc;
    }

    rc = pthread_cond_init(&g->changed, NULL);
    if (rc) {
        pthread_mutex_destroy(&g->mutex);
        return rc;
    }

    g->count = count;
    return 0;
#else
    return sem_init(&g->sem, 0, count) == 0 ? 0 : errno;
#endif
}

void gate_destroy(Gate *g) {
#ifdef USE_CONDVAR
    require(pthread_cond_destroy(&g->changed));
    require(pthread_mutex_destroy(&g->mutex));
#else
    require(sem_destroy(&g->sem));
#endif
}

void gate_post(Gate *g) {
#ifdef USE_CONDVAR
    require(pthread_mutex_lock(&g->mutex));
    g->count++;
    require(pthread_cond_signal(&g->changed));
    require(pthread_mutex_unlock(&g->mutex));
#else
    require(sem_post(&g->sem));
#endif
}

bool gate_try_wait(Gate *g) {
#ifdef USE_CONDVAR
    require(pthread_mutex_lock(&g->mutex));
    bool acquired = g->count != 0;
    if (acquired) {
        g->count--;
    }

    require(pthread_mutex_unlock(&g->mutex));
    return acquired;
#else
    while (sem_trywait(&g->sem) != 0) {
        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN) {
            return false;
        }

        abort();
    }

    return true;
#endif
}

bool gate_wait(Gate *g, const atomic_bool *stop) {
    while (!atomic_load(stop)) {
        struct timespec until;
        if (clock_gettime(CLOCK_REALTIME, &until) != 0) {
            abort();
        }

        until.tv_nsec += 50000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }

#ifdef USE_CONDVAR
        require(pthread_mutex_lock(&g->mutex));
        int rc = 0;
        while (g->count == 0 && !atomic_load(stop) && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&g->changed, &g->mutex, &until);
            if (rc != 0 && rc != ETIMEDOUT) {
                abort();
            }
        }

        bool acquired = g->count != 0 && !atomic_load(stop);
        if (acquired) {
            g->count--;
        }

        require(pthread_mutex_unlock(&g->mutex));
        if (acquired) {
            return true;
        }
#else
        if (sem_timedwait(&g->sem, &until) == 0) {
            return true;
        }

        if (errno != EINTR && errno != ETIMEDOUT) {
            abort();
        }
#endif
    }

    return false;
}

const char *sync_backend(void) {
#ifdef USE_CONDVAR
    return "condition_variables";
#else
    return "semaphores";
#endif
}
