#include "gallery.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void ipc_init(Ipc *c) {
    *c = (Ipc){.dirfd = -1, .readyfd = -1, .requests = -1, .plans = -1};
}

static void object_name(const Ipc *c, int index, char name[96]) {
    snprintf(name, 96, "/gallery3_%lu_%ld_%d", (unsigned long)getuid(), (long)c->owner, index);
}

static int map_shared(Ipc *c, bool create) {
    char name[96];
    object_name(c, -1, name);
    int fd = shm_open(name, create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR, 0600);
    if (fd < 0) {
        return -1;
    }
    c->owns_shm = create;
    if (create && ftruncate(fd, sizeof(Shared)) < 0) {
        close(fd);
        return -1;
    }

    void *p = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (p == MAP_FAILED) {
        errno = saved;
        return -1;
    }
    c->mem = p;
    return 0;
}

static int create_sem(Ipc *c, int index) {
    unsigned value = index == EMPTY ? 1 : 0;
    if (c->mode == UNNAMED) {
        sem_t *s = &c->mem->sems[index];
        if (sem_init(s, 1, value)) {
            return -1;
        }
        c->sems[index] = s;
    } else {
        char name[96];
        object_name(c, index, name);
        sem_t *s = sem_open(name, O_CREAT | O_EXCL, 0600, value);
        if (s == SEM_FAILED) {
            return -1;
        }
        c->sems[index] = s;
    }
    c->created_sems++;
    return 0;
}

static int byte_lock(int fd, off_t pos, short type) {
    struct flock lock = {.l_type = type, .l_whence = SEEK_SET, .l_start = pos, .l_len = 1};
    return fcntl(fd, F_SETLK, &lock);
}

static pid_t owner_lock(int fd) {
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    if (fcntl(fd, F_GETLK, &lock) < 0 || lock.l_type == F_UNLCK) {
        return 0;
    }
    return lock.l_pid;
}

int ipc_create(Ipc *c, Mode mode, const char *path, int count) {
    c->owner = getpid();
    c->mode = mode;
    c->count = count;

    if (path) {
        if (snprintf(c->path, sizeof(c->path), "%s", path) >= (int)sizeof(c->path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (mkdir(c->path, 0700)) {
            return -1;
        }
    } else {
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !*tmp) {
            tmp = "/tmp";
        }
        if (snprintf(c->path, sizeof(c->path), "%s/gallery3.XXXXXX", tmp) >=
            (int)sizeof(c->path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (!mkdtemp(c->path)) {
            return -1;
        }
    }

    c->owns_dir = true;
    c->dirfd = open(c->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (c->dirfd < 0) {
        return -1;
    }
    if (mode == FIFO) {
        if (mkfifoat(c->dirfd, "requests", 0600) || mkfifoat(c->dirfd, "plans", 0600)) {
            return -1;
        }
        /* На Linux O_RDWR держит оба конца открытыми до подключения клиентов. */
        c->requests = openat(c->dirfd, "requests", O_RDWR | O_NONBLOCK);
        c->plans = openat(c->dirfd, "plans", O_RDWR | O_NONBLOCK);
        if (c->requests < 0 || c->plans < 0) {
            return -1;
        }
    } else if (map_shared(c, true)) {
        return -1;
    }

    for (int i = 0; i < FIRST_GATE + count; i++) {
        if (stopped || create_sem(c, i)) {
            return -1;
        }
    }
    c->readyfd = openat(c->dirfd, "starting", O_RDWR | O_CREAT | O_EXCL, 0600);
    if (c->readyfd < 0 || byte_lock(c->readyfd, 0, F_WRLCK)) {
        return -1;
    }
    if (dprintf(c->readyfd, "%ld %d %d\n", (long)c->owner, count, (int)mode) < 0) {
        return -1;
    }
    return renameat(c->dirfd, "starting", c->dirfd, "ready");
}

int ipc_connect(Ipc *c, const char *path, int id) {
    c->dirfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (c->dirfd < 0) {
        return -1;
    }
    c->readyfd = openat(c->dirfd, "ready", O_RDWR);
    if (c->readyfd < 0) {
        return -1;
    }

    char header[96] = {0};
    long pid;
    int mode, count;
    if (pread(c->readyfd, header, sizeof(header) - 1, 0) <= 0 ||
        sscanf(header, "%ld %d %d", &pid, &count, &mode) != 3 ||
        pid <= 0 || pid > INT_MAX || count < 1 || count > MAX_VISITORS ||
        id < 1 || id > count || (mode != NAMED && mode != FIFO) ||
        owner_lock(c->readyfd) != (pid_t)pid) {
        errno = EINVAL;
        return -1;
    }

    c->count = count;
    c->owner = (pid_t)pid;
    c->mode = (Mode)mode;
    if (c->mode == FIFO) {
        c->requests = openat(c->dirfd, "requests", O_WRONLY | O_NONBLOCK);
        c->plans = openat(c->dirfd, "plans", O_RDONLY | O_NONBLOCK);
        if (c->requests < 0 || c->plans < 0) {
            return -1;
        }
    } else if (map_shared(c, false)) {
        return -1;
    }

    int indices[] = {EMPTY, FULL, FIRST_GATE + id - 1};
    for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
        char name[96];
        int k = indices[i];
        object_name(c, k, name);
        sem_t *s = sem_open(name, 0);
        if (s == SEM_FAILED) {
            return -1;
        }
        c->sems[k] = s;
    }

    off_t pos = 128 + id;
    if (byte_lock(c->readyfd, pos, F_WRLCK)) {
        return -1;
    }
    char claimed = 0;
    if (pread(c->readyfd, &claimed, 1, pos) < 0) {
        return -1;
    }
    if (claimed) {
        errno = EEXIST;
        return -1;
    }
    if (pwrite(c->readyfd, "x", 1, pos) != 1) {
        return -1;
    }
    return 0;
}

int ipc_alive(const Ipc *c) {
    if (stopped) {
        errno = EINTR;
        return 0;
    }
    if (getpid() != c->owner && owner_lock(c->readyfd) != c->owner) {
        errno = ECANCELED;
        return 0;
    }
    return 1;
}

void ipc_cancel_owner(const Ipc *c) {
    if (c->owner > 0 && getpid() != c->owner && owner_lock(c->readyfd) == c->owner) {
        kill(c->owner, stopped == SIGINT ? SIGINT : SIGTERM);
    }
}

void ipc_close(Ipc *c) {
    int saved = errno;
    bool owner = c->owns_dir && c->owner == getpid();
    if (owner && c->dirfd >= 0) {
        unlinkat(c->dirfd, "ready", 0);
        unlinkat(c->dirfd, "starting", 0);
    }
    if (c->readyfd >= 0) {
        close(c->readyfd);
    }

    for (int i = 0; i < FIRST_GATE + c->count && i < FIRST_GATE + MAX_VISITORS; i++) {
        if (!c->sems[i]) {
            continue;
        }
        if (c->mode == UNNAMED) {
            if (owner) {
                sem_destroy(c->sems[i]);
            }
        } else {
            sem_close(c->sems[i]);
            if (owner && i < c->created_sems) {
                char name[96];
                object_name(c, i, name);
                sem_unlink(name);
            }
        }
    }
    if (c->mem) {
        munmap(c->mem, sizeof(Shared));
    }
    if (owner && c->owns_shm) {
        char name[96];
        object_name(c, -1, name);
        shm_unlink(name);
    }

    if (c->requests >= 0) {
        close(c->requests);
    }
    if (c->plans >= 0) {
        close(c->plans);
    }
    if (c->dirfd >= 0) {
        if (owner && c->mode == FIFO) {
            unlinkat(c->dirfd, "requests", 0);
            unlinkat(c->dirfd, "plans", 0);
        }
        close(c->dirfd);
    }
    if (owner) {
        rmdir(c->path);
    }
    errno = saved;
}

static int take(Ipc *c, int k) {
    /* Обычный путь использует один sem_timedwait. Тайм-аут нужен для обнаружения
       закрытия сеанса и сигнала, пришедшего прямо перед блокировкой. */
    while (!stopped) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 100000000;
        if (t.tv_nsec >= 1000000000) {
            t.tv_sec++;
            t.tv_nsec -= 1000000000;
        }

        if (!sem_timedwait(c->sems[k], &t)) {
            return 0;
        }
        if ((errno != ETIMEDOUT && errno != EINTR) || !ipc_alive(c)) {
            return -1;
        }
    }
    errno = EINTR;
    return -1;
}

int sleep_until(Ipc *c, int64_t deadline) {
    for (;;) {
        if (!ipc_alive(c)) {
            return -1;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            return 0;
        }
        if (left > 100) {
            left = 100;
        }

        struct timespec pause = {.tv_nsec = (long)left * 1000000L};
        if (nanosleep(&pause, NULL) && errno != EINTR) {
            return -1;
        }
    }
}

/* Каждая запись короче PIPE_BUF. Неблокирующий write либо передаёт кадр
   целиком, либо ждём POLLOUT. По протоколу получатель читает после sem_post. */
static int put_frame(Ipc *c, int fd, const void *buf, size_t size) {
    while (ipc_alive(c)) {
        ssize_t n = write(fd, buf, size);
        if (n == (ssize_t)size) {
            return 0;
        }
        if (n >= 0) {
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN) {
            return -1;
        }

        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) {
            return -1;
        }
    }
    return -1;
}

static int get_frame(int fd, void *buf, size_t size) {
    ssize_t n;
    do {
        n = read(fd, buf, size);
    } while (n < 0 && errno == EINTR && !stopped);

    if (n == (ssize_t)size) {
        return 0;
    }
    if (n >= 0) {
        errno = EIO;
    }
    return -1;
}

int ipc_send(Ipc *c, Request req) {
    if (take(c, EMPTY)) {
        return -1;
    }
    if (c->mode == FIFO) {
        if (put_frame(c, c->requests, &req, sizeof(req))) {
            return -1;
        }
    } else {
        c->mem->slot = req;
    }
    return sem_post(c->sems[FULL]);
}

int ipc_receive(Ipc *c, Request *req) {
    if (take(c, FULL)) {
        return -1;
    }
    if (c->mode == FIFO) {
        if (get_frame(c->requests, req, sizeof(*req))) {
            return -1;
        }
    } else {
        *req = c->mem->slot;
    }
    return sem_post(c->sems[EMPTY]);
}

int ipc_grant(Ipc *c, int id) {
    return sem_post(c->sems[FIRST_GATE + id - 1]);
}

int ipc_await(Ipc *c, int id) {
    return take(c, FIRST_GATE + id - 1);
}

int ipc_give_plan(Ipc *c, const Plan *plan, int id) {
    if (c->mode == FIFO) {
        if (put_frame(c, c->plans, plan, sizeof(*plan))) {
            return -1;
        }
    } else if (id == 1) {
        c->mem->plan = *plan;
    }
    return ipc_grant(c, id);
}

int ipc_get_plan(Ipc *c, Plan *plan, int id) {
    if (ipc_await(c, id)) {
        return -1;
    }
    if (c->mode == FIFO) {
        return get_frame(c->plans, plan, sizeof(*plan));
    }
    *plan = c->mem->plan;
    return 0;
}
