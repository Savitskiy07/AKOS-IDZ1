#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdlib.h>

static int selected(const char *name, unsigned *count) {
    const char *value = getenv(name);
    (*count)++;
    return value && strtoul(value, NULL, 10) == *count;
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    void *(*function)(void *), void *arg) {
    static unsigned count;
    if (selected("GALLERY_FAIL_THREAD", &count)) {
        return EAGAIN;
    }

    return __real_pthread_create(thread, attr, function, arg);
}

void *__real_calloc(size_t, size_t);

void *__wrap_calloc(size_t count, size_t size) {
    if (getenv("GALLERY_FAIL_ALLOC")) {
        errno = ENOMEM;
        return NULL;
    }

    return __real_calloc(count, size);
}

#ifndef USE_CONDVAR
int __real_sem_init(sem_t *, int, unsigned);

int __wrap_sem_init(sem_t *sem, int shared, unsigned value) {
    static unsigned count;
    if (selected("GALLERY_FAIL_SYNC", &count)) {
        errno = ENOSPC;
        return -1;
    }

    return __real_sem_init(sem, shared, value);
}
#else
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);

int __wrap_pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    static unsigned count;
    if (selected("GALLERY_FAIL_SYNC", &count)) {
        return ENOMEM;
    }

    return __real_pthread_cond_init(cond, attr);
}
#endif
